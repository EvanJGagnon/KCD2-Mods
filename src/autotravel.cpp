// autotravel.cpp -- Horse Route Follow for Kingdom Come: Deliverance II (kcd2_autotravel.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// The game's own horse auto-follow ("road magnetism") keeps doing all the riding.  The mod only
// decides which road it takes, so that it follows a route to the custom map marker:
//   * The auto-follow's road chooser asks an angle filter about every candidate road segment it is
//     considering.  Both chooser call sites (fork chooser and initial snap) are redirected through
//     small stubs that record the candidate's end points and call FilterHook, which accepts or
//     refuses the candidate depending on whether it leads along the planned route.
//   * Routes are planned on road graphs (<level>.amg, generated from the game's ubernav road data:
//     only roads the game can auto-follow).  Planning is direction-aware ("GPS style"): a route
//     always starts the way the horse is travelling.
//   * The marker is read through the map screen (the checkpoint getter call is redirected to
//     CpHook), which also draws the route with the game's own fast-travel path line.
//   * Arrival is measured from the rider's real position; the horse is stopped by holding S briefly.
// The mod never leaves a moving horse without a road: if the game keeps offering only roads that
// the route refuses, the game's own choice wins for a moment and the route is replanned.
//
// Game code is located by signature/RTTI at startup (kcd2_common.h); if anything does not match,
// nothing is patched.  Settings: kcd2_autotravel.ini; log: kcd2_autotravel.log (next to the DLL).

#include "kcd2_common.h"
#include <cmath>
#include <vector>
#include <queue>
#include <unordered_map>
#include <algorithm>

// ---------------- signatures (WHGame build 15693 addresses in comments) ----------------
// The bytes after each call pin down the registers the stubs read (r14/r15, rbx/rsi) and the
// marker record layout (+0x50).
static const char* SIG_FORK = "48 8D 54 24 40 48 8D 4D 9F E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 47 68 48 8D 55 AF 4D 8B CF 48 89 45 E7 4D 8B C6 F3 0F 11 7C 24 20"; // +9 = 0xA09944
static const char* SIG_SNAP = "48 8D 54 24 38 F3 44 0F 11 5C 24 38 48 8D 4C 24 50 F3 44 0F 11 54 24 3C C7 44 24 40 00 00 00 00 E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 48 8B 44 24 50 48 8D 55 88 48 8B 4D 60 4C 8B CE 4C 8B C3 48 89 44 24 30"; // +0x20 = 0xA09D7E
static const char* SIG_CP   = "48 8D 54 24 20 48 8B CB E8 ?? ?? ?? ?? 48 8B 4C 24 20 48 8D B3 08 02 00 00 48 85 C9 BA 18 00 00 00 0F 95 C0 84 C0 65 48 8B 04 25 58 00 00 00 0F 84 ?? ?? ?? ?? 48 8D 79 50 8B 0D ?? ?? ?? ??"; // +8 = 0xDCB1E9
static const char* SIG_CPGET= "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B DA 48 8B F1 E8 ?? ?? ?? ?? 48 8B 88 C8 00 00 00 48 8B 01 FF 10 48 85 C0 74 ?? 48 8B 10 48 8B C8 FF 12 8B E8 48 8B BE 88 05 00 00 48 8B B6 90 05 00 00"; // 0xDCB460 (marker vector +0x588/+0x590)
static const char* SIG_FRAMEWORK_REF = "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 84 C0";                                    // mov rcx,[CCryAction*] (0x549D328)
static const char* SIG_GETCLIENTENT  = "48 83 EC 28 48 8B 01 FF 90 18 02 00 00 48 8B 0D ?? ?? ?? ?? 48 8B 11 4C 8B 42 70 8B D0 48 83 C4 28"; // CCryAction vtbl[66]
static const char* SIG_GETWORLDPOS   = "8B 41 64 89 02 8B 41 74 89 42 04 8B 81 84 00 00 00 89 42 08 48 8B C2 C3";        // CEntity vtbl[46]: position at +0x64/+0x74/+0x84
// C_UIMap::ShowFastTravelPath(this, const std::vector<Vec3>&): pushes x,y of each point into the map's
// "FastTravelPath" array and fires ShowFastTravelPath -- the dotted route line.  (0x2B016B0)
static const char* SIG_SHOWPATH = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B F1 48 8B DA 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B 7B 08 48 8B 1B EB ?? 48 8B D3 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8D 53 04 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 83 C3 0C 48 3B DF 75 ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4C 24 60 E8 ?? ?? ?? ?? 48 8D 9E 08 02 00 00 48 8B CB";
// The game hiding that line: string ctor, SendEvent(element, "HideFastTravelPath", bool&), string dtor.  (0x1F4D2CE)
static const char* SIG_HIDEPATH = "48 8D 15 ?? ?? ?? ?? 48 8B 59 10 48 8D 4C 24 40 C6 44 24 38 01 E8 ?? ?? ?? ?? 4C 8D 44 24 38 48 8D 54 24 40 48 8D 8B 08 02 00 00 E8 ?? ?? ?? ?? 48 8D 4C 24 40 E8 ?? ?? ?? ?? 48 8B 5C 24 20";

// On-screen notice: CScriptSystem::Update (vtbl[1], every frame, game thread) is hooked so the mod can
// run one line of game script there: Game.SendInfoText("@hrf_wrong_way") (text in the mod's Localization).
static const char* SIG_SS_UPDATE = "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 48 8B F1 33 D2 0F 29 74 24 20";   // 0xA26B94
static const char* SIG_SS_EXEC   = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 48 8B F9 48 89 50 E8"; // vtbl[6] ExecuteBuffer 0x4D4484
// Hardcore mode hides custom markers: the map's marker refresh returns early when the game mode is 2.
// A marker placed earlier (e.g. with the Hardcore Map Markers add-on) can still be stored, invisible,
// so the mod ignores markers in hardcore unless that check has been removed (the add-on is active).
// Read only: the game-mode object and the "je" (+0x1C) after cmp eax,2.   (0xDCB1BF)
static const char* SIG_GAMEMODE = "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 18 01 00 00 48 8B C8 48 8B 10 FF 52 10 83 F8 02 ?? ?? ?? ?? ?? ?? 48 8D 54 24 20 48 8B CB E8";
static const char* SIG_APSE_REF  = "E8 ?? ?? ?? ?? 48 8B 88 E8 00 00 00 E8 ?? ?? ?? ?? 48 8D 54 24 78 48 8D 88 08 15 00 00"; // 0x2BA782B: game->UI module->map
static const char* SIG_GETMODULE = "48 89 5C 24 08 56 57 41 56 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 48 85 C0 75";   // UI module getter 0x5677CC

// ---------------- tunables ----------------
static const float    NODE_TOL      = 3.0f;    // candidate point -> graph node
static const float    EXACT_TOL    = 1.5f;    // candidate point is this graph node (the graph merges points closer than this)
static const float    MARKER_SNAP   = 5000.0f; // marker -> nearest road node (off-road markers route to the closest road point)
static const float    CORRIDOR      = 6.0f;    // metres around the route that count as on it (parallel streets/alleys)
static const float    FIRST_BAN     = 1e6f;    // cost of starting a route backwards (effectively never)
static const uint64_t STUCK_MS      = 400;     // standing: every road ahead refused this long -> let the game choose
static const uint64_t STUCK_MOVE_MS = 150;     // moving:   every road ahead refused this long -> let the game choose
static const uint64_t STUCK_NONE_MS = 900;     // moving:   no road accepted at all this long -> let the game choose
static const float    SHOW_SPEED    = 6.5f;    // wrong-way notice only above this speed (m/s): riding, not walking about
static const uint64_t FOLLOW_MS     = 20000;   // auto-follow asked about roads this recently = riding on auto-follow
static const float    WRONG_WAY_M   = 60.0f;   // riding on the wrong way this far from the route -> recalculate
static const float    DEVIATE       = 15.0f;   // rider this far from the route ...
static const uint64_t DEVIATE_MS    = 1000;    // ... for this long = left the route: recalculate
static const float    BRAKE_LEAD_S  = 0.4f;    // brake earlier by this many seconds of travel
static const DWORD    BRAKE_MS      = 1200;    // hold S this long on arrival
static float g_arriveDist = 3.0f;              // ini ArriveDistance
static float g_uturnCost  = 400.0f;            // ini UTurnPenalty
static bool  g_brakeOn = true, g_mapLine = true;

// ---------------- game functions / data ----------------
struct Vec3f { float x,y,z; };
struct VecView { Vec3f* b; Vec3f* e; Vec3f* c; };      // std::vector<Vec3> layout (begin/end/capacity)
typedef bool  (*Filter_t)(float*, float*);
typedef void* (*CpGetter_t)(void*, void*);
typedef void  (*ShowPath_t)(void*, const VecView*);
typedef void  (*StrCtor_t)(void*, const char*);
typedef void  (*StrDtor_t)(void*);
typedef void  (*SendEvent_t)(void*, void*, const bool*);
static Filter_t    g_filter=nullptr;
static CpGetter_t  g_cpget=nullptr;
static ShowPath_t  g_showPath=nullptr;
static StrCtor_t   g_strCtor=nullptr;
static StrDtor_t   g_strDtor=nullptr;
static SendEvent_t g_sendEvent=nullptr;
static const char* g_hideName=nullptr;
static void**      g_fwGlobal=nullptr;
static uintptr_t   g_vtCryAction=0, g_vtEntity=0, g_vtUIMap=0;

// data slots written by the asm stubs (live in the stub page)
static volatile uintptr_t* g_p0 = nullptr;     // candidate start point
static volatile uintptr_t* g_p1 = nullptr;     // candidate end point
static volatile uintptr_t* g_map = nullptr;    // map controller (C_UIMap)

// ---------------- shared state ----------------
static HANDLE g_brakeEvt = nullptr;
// rider (player entity; the horse is under him), tracked every 20 ms off the game thread
static volatile uintptr_t g_ent=0; static uint64_t g_entT=0;
static volatile float g_px=0,g_py=0,g_speed=0; static volatile bool g_posOk=false;
static volatile float g_mhx=0,g_mhy=0; static volatile bool g_headOk=false;             // movement heading
static volatile float g_fx=0,g_fy=0;   static volatile bool g_faceOk=false;             // facing (rider's forward axis)
static volatile bool g_wrongWay=false, g_showWrong=false; static uint64_t g_wrongT=0;               // orientation stage: facing away
static volatile uint64_t g_moveT=0;                                                      // when the horse last started moving
static volatile float g_endX=0,g_endY=0,g_endMk=0; static volatile bool g_endOk=false;  // route end, its distance to the marker
static volatile uint64_t g_hookT=0;                                                     // last time auto-follow asked us
static volatile bool g_arrived=false;

using kc::logf;
static bool rdn(uintptr_t a, void* out, size_t n){
    __try { memcpy(out,(const void*)a,n); return true; } __except(EXCEPTION_EXECUTE_HANDLER){ return false; }
}
template<class T> static bool rd(uintptr_t a, T* out){ return rdn(a,out,sizeof(T)); }
static bool sane(float x,float y){ return std::isfinite(x) && std::isfinite(y) && fabsf(x)<100000.f && fabsf(y)<100000.f; }

// ---------------- road graph ----------------
struct Graph {
    const char* name=nullptr;
    std::vector<float> x, y;
    std::vector<uint32_t> off, to; std::vector<float> cost;   // CSR adjacency
    std::unordered_map<int64_t, std::vector<int>> grid;       // 8 m buckets
    static int64_t key(int gx,int gy){ return ((int64_t)gx<<32) ^ (uint32_t)gy; }
    bool load(const char* lvl){
        char fn[64], p[MAX_PATH]; sprintf_s(fn,"%s.amg",lvl); kc::path(p,sizeof p,fn);
        FILE* f=nullptr; fopen_s(&f,p,"rb"); if(!f) return false;
        char mg[4]={0}; uint32_t nn=0,ne=0; fread(mg,1,4,f); fread(&nn,4,1,f); fread(&ne,4,1,f);
        if(nn==0||nn>2000000||ne>4000000){ fclose(f); logf("{\"ev\":\"bad_graph\",\"level\":\"%s\"}",lvl); return false; }
        x.resize(nn); y.resize(nn);
        for(uint32_t i=0;i<nn;i++){ fread(&x[i],4,1,f); fread(&y[i],4,1,f); }
        std::vector<uint32_t> ea(ne), eb(ne); std::vector<float> ec(ne), el(ne);
        for(uint32_t i=0;i<ne;i++){ fread(&ea[i],4,1,f); fread(&eb[i],4,1,f); fread(&el[i],4,1,f); fread(&ec[i],4,1,f); }
        fclose(f);
        off.assign(nn+1,0);
        for(uint32_t i=0;i<ne;i++){ off[ea[i]+1]++; off[eb[i]+1]++; }
        for(uint32_t i=0;i<nn;i++) off[i+1]+=off[i];
        to.resize(off[nn]); cost.resize(off[nn]); std::vector<uint32_t> fill(off.begin(),off.end()-1);
        for(uint32_t i=0;i<ne;i++){
            to[fill[ea[i]]]=eb[i]; cost[fill[ea[i]]++]=ec[i];
            to[fill[eb[i]]]=ea[i]; cost[fill[eb[i]]++]=ec[i];
        }
        for(uint32_t i=0;i<nn;i++) grid[key((int)floorf(x[i]/8),(int)floorf(y[i]/8))].push_back(i);
        name=lvl; return true;
    }
    int nearest(float px,float py,float maxd) const {
        if(maxd>200){                                   // far lookups: flat scan
            int best=-1; float bd=maxd*maxd;
            for(size_t i=0;i<x.size();i++){ float d=(x[i]-px)*(x[i]-px)+(y[i]-py)*(y[i]-py); if(d<bd){bd=d;best=(int)i;} }
            return best;
        }
        int best=-1; float bd=maxd*maxd; int r=(int)ceilf(maxd/8);
        int gx=(int)floorf(px/8), gy=(int)floorf(py/8);
        for(int dx=-r;dx<=r;dx++) for(int dy=-r;dy<=r;dy++){
            auto it=grid.find(key(gx+dx,gy+dy)); if(it==grid.end()) continue;
            for(int i:it->second){ float d=(x[i]-px)*(x[i]-px)+(y[i]-py)*(y[i]-py); if(d<bd){bd=d;best=i;} }
        }
        return best;
    }
    // Is u->v a reversal relative to direction (ax,ay)?  (turn sharper than ~100 degrees)
    bool reverses(float ax,float ay,int u,int v) const {
        float dx=x[v]-x[u], dy=y[v]-y[u], l=hypotf(dx,dy);
        return l>0.01f && (ax!=0||ay!=0) && (dx*ax+dy*ay)/l < -0.2f;
    }
    // Closest point to (px,py) on any road segment: the point (qx,qy) on segment a-b.
    bool closest_on_road(float px,float py,float& qx,float& qy,int& a,int& b) const {
        float bd=1e30f; a=b=-1;
        for(int u=0;u<(int)x.size();u++) for(uint32_t e=off[u];e<off[u+1];e++){
            int v=to[e]; if(v<u) continue;
            float ex=x[v]-x[u], ey=y[v]-y[u], l2=ex*ex+ey*ey;
            float t= l2>1e-6f ? ((px-x[u])*ex+(py-y[u])*ey)/l2 : 0.f; t=std::min(1.f,std::max(0.f,t));
            float cx=x[u]+t*ex, cy=y[u]+t*ey, d=(cx-px)*(cx-px)+(cy-py)*(cy-py);
            if(d<bd){ bd=d; qx=cx; qy=cy; a=u; b=v; }
        }
        return a>=0;
    }
    void dir(uint32_t e,int u,float& dx,float& dy) const {
        dx=x[to[e]]-x[u]; dy=y[to[e]]-y[u]; float l=hypotf(dx,dy);
        if(l>0.01f){ dx/=l; dy/=l; } else dx=dy=0;
    }
    // Shortest route s->t over directed road segments.  A first step against the heading (hx,hy)
    // costs `first`; any later reversal costs `uturn` -- so the route keeps going the way the horse
    // is travelling unless turning around saves more than that.
    bool route(int s,int t,std::vector<int>& path,float hx,float hy,float uturn,float first) const {
        path.clear();
        if(s==t){ path.push_back(s); return true; }
        size_t E=to.size();
        std::vector<float> dist(E,1e30f); std::vector<int32_t> prev(E,-1), from(E,-1);
        typedef std::pair<float,uint32_t> P; std::priority_queue<P,std::vector<P>,std::greater<P>> q;
        for(uint32_t e=off[s];e<off[s+1];e++){
            float d=cost[e]+(reverses(hx,hy,s,to[e])?first:0.f);
            from[e]=s; if(d<dist[e]){ dist[e]=d; q.push({d,e}); }
        }
        int64_t hit=-1;
        while(!q.empty()){
            auto [d,e]=q.top(); q.pop();
            if(d>dist[e]) continue;
            int u=to[e];
            if(u==t){ hit=e; break; }
            float ax,ay; dir(e,from[e],ax,ay);
            for(uint32_t f=off[u];f<off[u+1];f++){
                float nd=d+cost[f]+(reverses(ax,ay,u,to[f])?uturn:0.f);
                if(nd<dist[f]){ dist[f]=nd; prev[f]=(int32_t)e; from[f]=u; q.push({nd,f}); }
            }
        }
        if(hit<0) return false;
        for(int64_t e=hit;e>=0;e=prev[e]) path.push_back(to[e]);
        path.push_back(s);
        std::reverse(path.begin(),path.end()); return true;
    }
};
static Graph g_graphs[3]; static int g_ng=0;
static Graph* g_cur=nullptr;

// ---------------- route ----------------
static std::vector<int>   g_route;              // graph nodes, start -> marker
static std::vector<float> g_cum;                // route distance at each index
static std::vector<char>  g_rev;                // route turns back at this index
static float g_hx=0,g_hy=0;                     // heading used for planning (unit, 0 = unknown)
static int   g_prog=0;                          // route index nearest the rider
static float g_mx=0,g_my=0; static bool g_haveMarker=false;
static uint64_t g_lastPlan=0, g_fwdRejectT=0, g_yieldUntil=0;

static bool read_marker(float& mx,float& my){
    uintptr_t m=g_map?*g_map:0; if(!m) return false;
    uintptr_t b=0,e=0; if(!rd(m+0x588,&b)||!rd(m+0x590,&e)||!b||e<=b) return false;
    uintptr_t rec=0; if(!rd(b,&rec)||!rec) return false;
    if(!rd(rec+0x50,&mx) || !rd(rec+0x54,&my)) return false;
    return sane(mx,my);
}
static void clear_route(){ g_route.clear(); }   // the destination (g_end*) stays: it does not depend on the route
static volatile float g_dmMin=1e9f;               // closest the rider has been to the marker
static int g_endA=-1, g_endB=-1;                  // road segment holding the destination
// The destination is the point of road closest to the marker (anywhere along a segment, not just
// at a graph node), independent of any route.  Recomputed when the marker or the level changes.
static void set_dest(){
    g_endOk=false; g_endA=g_endB=-1;
    float qx,qy; int a,b;
    if(g_cur && g_haveMarker && g_cur->closest_on_road(g_mx,g_my,qx,qy,a,b)){
        g_endX=qx; g_endY=qy; g_endMk=hypotf(qx-g_mx,qy-g_my); g_endA=a; g_endB=b; g_endOk=true;
    }
}
static void set_marker(float mx,float my){
    g_mx=mx; g_my=my; g_haveMarker=true; clear_route(); g_arrived=false; g_dmMin=1e9f;
    set_dest();
}
static void set_level(Graph* g){
    if(g==g_cur) return;
    g_cur=g; clear_route(); g_lastPlan=0; g_dmMin=1e9f; set_dest();
    if(g) logf("{\"ev\":\"level\",\"name\":\"%s\"}",g->name);
}
// ---------------- hardcore: markers the map does not show ----------------
static void** g_gmGlobal=nullptr; static uintptr_t g_hcJump=0;
typedef void* (*GetObj_t)(void*); typedef int (*GetMode_t)(void*);
static int game_mode(){                           // as the map does: [global]->vtbl[0x118/8]() ->vtbl[2]()
    void* g=*g_gmGlobal; if(!g) return -1;
    void* o=((GetObj_t)(*(void***)g)[0x118/8])(g); if(!o) return -1;
    return ((GetMode_t)(*(void***)o)[2])(o);
}
static bool markers_hidden(){                     // game thread
    if(!g_gmGlobal || !g_hcJump) return false;
    if(*(uint16_t*)g_hcJump==0x9090) return false; // hardcore check removed (Hardcore Map Markers add-on)
    int m=-1; __try { m=game_mode(); } __except(EXCEPTION_EXECUTE_HANDLER){ g_gmGlobal=nullptr; return false; }
    return m==2;
}

// Applies the current marker state; true if there is a destination to route to.
static bool sync_destination(bool mk,float mx,float my){
    if(mk && markers_hidden()){                   // hardcore without the add-on: a stored marker is invisible
        static bool said=false; if(!said){ said=true; logf("{\"ev\":\"hardcore_marker_ignored\"}"); }
        mk=false;
    }
    if(mk){ if(!g_haveMarker || hypotf(mx-g_mx,my-g_my)>5) set_marker(mx,my); return true; }
    if(g_haveMarker && g_map && *g_map){ g_haveMarker=false; clear_route(); g_endOk=false; logf("{\"ev\":\"marker_removed\"}"); }
    return false;
}
static bool g_planFree=false;                     // plan_from: may the route start behind the horse?
static bool route_to(int n0,int t,std::vector<int>& p){
    if(g_planFree) return g_cur->route(n0,t,p,g_hx,g_hy,g_uturnCost,0.f);
    return g_cur->route(n0,t,p,g_hx,g_hy,g_uturnCost,FIRST_BAN) || g_cur->route(n0,t,p,g_hx,g_hy,g_uturnCost,g_uturnCost);
}
static float path_len(const std::vector<int>& p){
    float l=0; for(size_t i=1;i<p.size();i++) l+=hypotf(g_cur->x[p[i]]-g_cur->x[p[i-1]],g_cur->y[p[i]]-g_cur->y[p[i-1]]); return l;
}

// Plans from graph node n0.  free=true (horse standing): the truly shortest route, even if it starts
// behind the horse -- the orientation stage then turns the horse around.  free=false (moving): the
// shortest route that starts the way the horse is going.
static void plan_from(int n0,bool free=false){
    g_planFree=free; g_wrongWay=false; g_showWrong=false;
    clear_route(); g_prog=0; g_lastPlan=GetTickCount64();
    if(!g_endOk) set_dest();
    if(!g_endOk){ logf("{\"ev\":\"plan_fail\",\"why\":\"no_destination\",\"mx\":%.1f,\"my\":%.1f}",g_mx,g_my); return; }
    // Route onto the destination's segment and across it, so the rider passes the exact closest point.
    std::vector<int> pa,pb; bool oa=route_to(n0,g_endA,pa), ob=route_to(n0,g_endB,pb);
    if(!oa && !ob){ logf("{\"ev\":\"plan_fail\",\"why\":\"no_route\"}"); return; }
    bool useA = oa && (!ob || path_len(pa)<=path_len(pb));
    g_route = useA? pa : pb;
    int other = useA? g_endB : g_endA;
    if(g_route.size()<2 || g_route[g_route.size()-2]!=other) g_route.push_back(other);
    size_t n=g_route.size();
    g_cum.assign(n,0.f); g_rev.assign(n,0);
    for(size_t i=1;i<n;i++){ int a=g_route[i-1],b=g_route[i]; g_cum[i]=g_cum[i-1]+hypotf(g_cur->x[a]-g_cur->x[b],g_cur->y[a]-g_cur->y[b]); }
    for(size_t i=1;i+1<n;i++){
        int p=g_route[i-1],c=g_route[i],nx=g_route[i+1]; float dx=g_cur->x[c]-g_cur->x[p], dy=g_cur->y[c]-g_cur->y[p], l=hypotf(dx,dy);
        if(l>0.01f) g_rev[i]=g_cur->reverses(dx/l,dy/l,c,nx);
    }
    logf("{\"ev\":\"plan\",\"level\":\"%s\",\"from\":[%.1f,%.1f],\"to\":[%.1f,%.1f],\"len\":%.0f}",
         g_cur->name,g_cur->x[n0],g_cur->y[n0],g_mx,g_my,g_cum.back());
}
// Route index nearest (x,y) within the corridor, searched around the rider's progress.
static int route_index_near(float x,float y){
    int lo=std::max(0,g_prog-10), hi=std::min((int)g_route.size(),g_prog+150), best=-1; float bd=CORRIDOR*CORRIDOR;
    for(int k=lo;k<hi;k++){ int n=g_route[k]; float dx=g_cur->x[n]-x, dy=g_cur->y[n]-y, d=dx*dx+dy*dy; if(d<bd){ bd=d; best=k; } }
    return best;
}
// Route index of graph node n (first match at or after `from`, around the rider's progress), or -1.
static int route_index_of(int n,int from){
    int lo=std::max(0,from), hi=std::min((int)g_route.size(),g_prog+150);
    for(int k=lo;k<hi;k++) if(g_route[k]==n) return k;
    return -1;
}
static void update_progress(){
    if(!g_posOk) return;
    int lo=std::max(0,g_prog-10), hi=std::min((int)g_route.size(),g_prog+150), best=-1; float bd=25.f*25.f;
    for(int k=lo;k<hi;k++){ int n=g_route[k]; float dx=g_cur->x[n]-g_px, dy=g_cur->y[n]-g_py, d=dx*dx+dy*dy; if(d<bd){ bd=d; best=k; } }
    if(best>=0) g_prog=best;
}
// Does direction (dx,dy) point away from the route ahead of the rider (the point ~20 m further along)?
static bool against_route(float dx,float dy){
    if(g_route.size()<2 || !g_posOk) return false;
    int k=g_prog; while(k+1<(int)g_route.size() && g_cum[k]-g_cum[g_prog]<20.f) k++;
    float tx=g_cur->x[g_route[k]]-g_px, ty=g_cur->y[g_route[k]]-g_py, l=hypotf(tx,ty);
    return l>3.f && (tx*dx+ty*dy)/l < -0.3f;
}
// Distance from (x,y) to the route (its segments around the rider's progress).
static float dist_to_route(float x,float y){
    int lo=std::max(0,g_prog-10), hi=std::min((int)g_route.size()-1,g_prog+150); float bd=1e30f;
    for(int k=lo;k<hi;k++){
        int u=g_route[k], v=g_route[k+1]; float ax=g_cur->x[u], ay=g_cur->y[u], ex=g_cur->x[v]-ax, ey=g_cur->y[v]-ay, l2=ex*ex+ey*ey;
        float t= l2>1e-6f ? ((x-ax)*ex+(y-ay)*ey)/l2 : 0.f; t=std::min(1.f,std::max(0.f,t));
        float dx=ax+t*ex-x, dy=ay+t*ey-y; bd=std::min(bd,dx*dx+dy*dy);
    }
    return sqrtf(bd);
}

// ---------------- rider position (game thread for the entity lookup) ----------------
static void refresh_entity(bool force){
    uint64_t now=GetTickCount64();
    if(!g_fwGlobal || (!force && now-g_entT<=250)) return;
    g_entT=now; uintptr_t e=0;
    void* fw=*g_fwGlobal;
    if(fw && kc::is_a((uintptr_t)fw,g_vtCryAction)){
        typedef void* (*GetEnt_t)(void*); e=(uintptr_t)((GetEnt_t)(*(void***)fw)[66])(fw);   // GetClientEntity
        if(e && !kc::is_a(e,g_vtEntity)) e=0;
    }
    g_ent=e;
}
static bool player_pos(float& x,float& y){
    uintptr_t e=g_ent; if(!e || !kc::is_a(e,g_vtEntity)) return false;
    return rd(e+0x64,&x) && rd(e+0x74,&y) && sane(x,y);
}

// ---------------- on-screen notice ----------------
typedef void* (*SsUpdate_t)(void*,void*,void*,void*);
typedef bool  (*SsExec_t)(void*,const char*,size_t,const char*,void*);
static SsUpdate_t g_ssUpdate=nullptr; static SsExec_t g_ssExec=nullptr;
static const char* volatile g_notice=nullptr;    // script line to run on the next frame
static void notify(const char* line){ if(g_ssExec) g_notice=line; }
static const char* const WRONG_WAY_LINE = "Game.SendInfoText(\"@hrf_wrong_way\", true)";
static volatile uint64_t g_sayT=0;               // last time the wrong-way notice was shown

// ---------------- road choice ----------------
static void find_map();
// Replacement for the angle filter at both chooser call sites (game thread).
static bool FilterHook(float* dir, float* cur){
    bool vanilla=g_filter(dir,cur);
    uint64_t now=GetTickCount64(); g_hookT=now;
    if(kc::g_debug){                              // diagnostics: is the chooser asking while the horse starts off?
        static uint64_t dt=0;
        if(now-dt>500){ dt=now; KC_DLOG("{\"ev\":\"ask\",\"speed\":%.1f,\"since_move\":%lld,\"face\":[%.2f,%.2f],\"route\":%d,\"prog\":%d,\"turn\":%d}",
            (float)g_speed,(long long)(g_headOk? (long long)(now-g_moveT) : -1),(float)g_fx,(float)g_fy,(int)g_route.size(),g_prog,(int)g_wrongWay); }
    }
    refresh_entity(false);
    find_map();
    float a[3],b[3];
    if(!rdn(*g_p0,a,12) || !rdn(*g_p1,b,12)) return vanilla;
    // heading: the rider's movement; standing still, the way the horse faces
    if(g_posOk && g_headOk){ g_hx=g_mhx; g_hy=g_mhy; } else if(g_faceOk){ g_hx=g_fx; g_hy=g_fy; } else { g_hx=0; g_hy=0; }
    // Which level are we on?  The game's road points are nodes of exactly one of our graphs.  Level
    // coordinates overlap, so a guess made from the map (CpHook) can be wrong; the first few road
    // points that match another level's graph but not the current one switch to it.
    static Graph* other=nullptr; static int otherHits=0;
    if(!g_cur || g_cur->nearest(a[0],a[1],1.0f)<0){
        Graph* hit=nullptr;
        for(int i=0;i<g_ng;i++) if(&g_graphs[i]!=g_cur && g_graphs[i].nearest(a[0],a[1],1.0f)>=0){ hit=&g_graphs[i]; break; }
        if(hit){ otherHits = hit==other? otherHits+1 : 1; other=hit; }
        if(hit && (!g_cur || otherHits>=3)){ set_level(hit); otherHits=0; }
        if(!g_cur) return vanilla;
    } else otherHits=0;
    float mx=0,my=0; bool mk=read_marker(mx,my);
    if(!sync_destination(mk,mx,my)) return vanilla;   // no destination: vanilla auto-follow
    if(g_arrived) return vanilla;                 // done until the marker moves
    int n0=g_cur->nearest(a[0],a[1],NODE_TOL);
    if(n0<0) return vanilla;
    bool moving=g_posOk && g_speed>1.5f;
    if(now<g_yieldUntil) return vanilla;          // letting the game choose for a moment (see below)
    // Is this candidate heading the way the horse is going?  The chooser also offers the roads
    // behind each point; those are refused and never count as being stuck.
    float cdx=b[0]-a[0], cdy=b[1]-a[1], cl=hypotf(cdx,cdy);
    bool ahead = !(g_hx||g_hy) || cl<0.01f || (cdx*g_hx+cdy*g_hy)/cl > 0.2f;
    // The route is planned once (shortest road route, starting the way the horse is moving) and
    // kept.  It is recalculated only when the rider has actually left it: more than DEVIATE metres
    // from the route for DEVIATE_MS (a wrong turn, a manual detour, pushed off by traffic).
    int nr = g_posOk ? g_cur->nearest(g_px,g_py,50.f) : -1;
    if(g_route.empty()){
        if(now-g_lastPlan>1500) plan_from(nr>=0? nr : n0, !moving || (g_speed<6.f && now-g_moveT<3000));
        if(g_route.empty()) return vanilla;
    } else if(g_posOk && !(g_wrongWay && dist_to_route(g_px,g_py)<WRONG_WAY_M)){   // (not while waiting for a turn-around)
        static uint64_t offT=0;
        float dr=dist_to_route(g_px,g_py);
        if(dr<=DEVIATE) offT=0;
        else if(!offT) offT=now;
        else if(now-offT>DEVIATE_MS && nr>=0){
            logf("{\"ev\":\"left_route\",\"at\":[%.1f,%.1f],\"dist\":%.1f}",(float)g_px,(float)g_py,dr);
            offT=0; plan_from(nr,!moving); if(g_route.empty()) return vanilla;
        }
    }
    update_progress();
    int ia=route_index_near(a[0],a[1]);
    if(ia<0) return vanilla;                      // a road not on the route (e.g. behind a junction): not ours to judge
    if(!g_posOk && g_cum.back()-g_cum[ia] <= g_arriveDist){     // fallback without a rider position
        if(!g_arrived){ g_arrived=true; logf("{\"ev\":\"arrived\",\"left\":%.1f}",g_cum.back()-g_cum[ia]); }   // speed unknown: no brake
        return vanilla;
    }
    if(ia>=(int)g_route.size()-1) return vanilla;
    // The route turns back just ahead (a U-turn auto-follow cannot make, e.g. at a dead end):
    // keep riding with the game's choice; leaving the route triggers the recalculation.
    for(int k=ia;k<(int)g_route.size() && k<=ia+3;k++) if(g_rev[k]) return vanilla;
    // Accept a candidate that stays within the route corridor and makes progress along it.
    // The game's road points are our graph nodes: when the candidate starts on a route node, only
    // the route's own next road counts (a short side leg a few metres off it, e.g. the connectors
    // of a triangular junction, would still be inside the corridor).
    int na=g_cur->nearest(a[0],a[1],EXACT_TOL), nb=g_cur->nearest(b[0],b[1],EXACT_TOL);
    int ea= na>=0 ? route_index_of(na,g_prog-10) : -1;
    bool follow;
    if(ea>=0 && nb>=0 && nb==na){
        follow=vanilla;                           // the game's points are denser than ours: a step within one node
    } else if(ea>=0 && nb>=0){
        int eb=route_index_of(nb,ea+1);
        follow = eb>ea && eb-ea<=15;
    } else {
        int ib=route_index_near(b[0],b[1]);
        follow = ib>ia && ib-ia<=15;
        if(ib==ia) follow=vanilla;                // tiny segment around one route point
    }
    // Never push a standing horse (just mounted) onto a road the game itself refused.
    if(follow && !vanilla && !moving) follow=false;
    // Never leave the horse without a road: if every road ahead that the game offers keeps being
    // refused, let the game choose for a moment.  The route is kept; if the game's choice takes the
    // rider off it, the deviation check above recalculates.
    // The same when nothing at all (ahead or not) has been accepted for a while as the horse moves.
    static uint64_t noneT=0, lastCall=0;
    if(follow || now-lastCall>300) noneT=0;
    lastCall=now;
    bool stuck=false;
    if(!follow && vanilla && moving){
        if(!noneT) noneT=now;
        stuck = now-noneT > STUCK_NONE_MS;
    }
    if(ahead && vanilla){
        if(follow) g_fwdRejectT=0;
        else {
            if(!g_fwdRejectT) g_fwdRejectT=now;
            stuck = stuck || now-g_fwdRejectT > (moving? STUCK_MOVE_MS : STUCK_MS);
        }
    }
    if(stuck){
        g_fwdRejectT=0; noneT=0; g_yieldUntil=now+(moving?1200:2000);
        KC_DLOG("{\"ev\":\"yield\",\"at\":[%.1f,%.1f],\"to\":[%.1f,%.1f],\"speed\":%.1f}",a[0],a[1],b[0],b[1],(float)g_speed);
        return vanilla;
    }
    static uint64_t lastLog=0;
    if(follow!=vanilla && now-lastLog>200){
        lastLog=now; KC_DLOG("{\"ev\":\"override\",\"at\":[%.1f,%.1f],\"to\":[%.1f,%.1f],\"vanilla\":%d,\"ours\":%d}",a[0],a[1],b[0],b[1],vanilla,follow);
    }
    return follow;
}

// ---------------- route line on the map ----------------
static Graph* graph_at(float x,float y){
    // Level coordinates overlap, so once a level is known (set exactly from the road the horse is
    // on) keep it.  Otherwise guess: the level whose roads are closest to both the rider and the
    // marker (FilterHook corrects a wrong guess as soon as the horse is on a road).
    if(g_cur) return g_cur;
    Graph* best=nullptr; float bd=1e30f;
    for(int i=0;i<g_ng;i++){
        const Graph& g=g_graphs[i]; int n=g.nearest(x,y,MARKER_SNAP), m=g.nearest(g_mx,g_my,MARKER_SNAP); if(n<0||m<0) continue;
        float d=hypotf(g.x[n]-x,g.y[n]-y)+hypotf(g.x[m]-g_mx,g.y[m]-g_my); if(d<bd){ bd=d; best=&g_graphs[i]; }
    }
    return best;
}
static void hide_route(void* map){               // HideFastTravelPath(Animation=false), as the game does
    if(!g_sendEvent) return;
    alignas(16) uint8_t str[32]={0}; bool animate=false;
    g_strCtor(str,g_hideName); g_sendEvent((char*)map+0x208,str,&animate); g_strDtor(str);
}
static void draw_route(void* map,bool havePos,float px,float py){
    if(!g_showPath || !g_mapLine || !kc::is_a((uintptr_t)map,g_vtUIMap)) return;
    std::vector<Vec3f> pts;
    if(!g_arrived && !g_route.empty() && g_cur){
        if(havePos) pts.push_back({px,py,0});
        float lx=1e9f,ly=1e9f;
        for(size_t k=(size_t)std::max(0,g_prog);k<g_route.size();k++){
            int n=g_route[k]; float x=g_cur->x[n], y=g_cur->y[n];
            if(k+1<g_route.size() && hypotf(x-lx,y-ly)<8.f) continue;       // ~8 m spacing
            pts.push_back({x,y,0}); lx=x; ly=y;
        }
    }
    hide_route(map);                                                        // replace any line already shown
    if(pts.size()<2) return;
    VecView v{pts.data(),pts.data()+pts.size(),pts.data()+pts.size()};
    g_showPath(map,&v);
}
// Called in place of the map's checkpoint getter (map screen, game thread).
static void* CpHook(void* self,void* out){
    if(*g_map && *g_map!=(uintptr_t)self) logf("{\"ev\":\"map_moved\"}");   // the early lookup found another object
    *g_map=(uintptr_t)self;
    void* r=g_cpget(self,out);
    __try {
        refresh_entity(true);
        float mx=0,my=0,px=0,py=0; bool havePos=player_pos(px,py), haveMk=read_marker(mx,my);
        if(sync_destination(haveMk,mx,my) && g_route.empty() && havePos){   // plan from where the rider is
            Graph* gr=graph_at(px,py);
            if(gr){ set_level(gr); int n0=gr->nearest(px,py,MARKER_SNAP); if(n0>=0){ bool mv=g_headOk; g_hx=mv?g_mhx:(g_faceOk?g_fx:0); g_hy=mv?g_mhy:(g_faceOk?g_fy:0); plan_from(n0,!mv); } }
        }
        draw_route(self,havePos,px,py);
    } __except(EXCEPTION_EXECUTE_HANDLER){ logf("{\"ev\":\"map_line_error\",\"msg\":\"map line disabled for this session\"}"); g_showPath=nullptr; }
    return r;
}

// ---------------- rider tracking + arrival (20 ms, off the game thread; reads memory only) ----------------
static void track_position(uint64_t now){
    static float lx=0,ly=0,vx=0,vy=0; static uint64_t lt=0;
    float x=0,y=0;
    if(!player_pos(x,y)){ g_posOk=false; g_headOk=false; lt=0; return; }
    if(lt && now>lt){
        float dt=(now-lt)/1000.f, ix=(x-lx)/dt, iy=(y-ly)/dt, v=hypotf(ix,iy);
        if(v<40.f){ vx=vx*0.85f+ix*0.15f; vy=vy*0.85f+iy*0.15f; g_speed=g_speed*0.8f+v*0.2f; }
        float vl=hypotf(vx,vy);
        if(vl>1.0f){ if(!g_headOk) g_moveT=now; g_mhx=vx/vl; g_mhy=vy/vl; g_headOk=true; } else if(vl<0.5f) g_headOk=false;
    }
    lx=x; ly=y; lt=now; g_px=x; g_py=y; g_posOk=true;
    {   // facing: the entity's forward (Y) axis, world matrix at +0x58 (row-major 3x4: m01 @+0x5C, m11 @+0x6C)
        float fx=0,fy=0; uintptr_t e=g_ent;
        if(e && rd(e+0x5C,&fx) && rd(e+0x6C,&fy) && std::isfinite(fx) && std::isfinite(fy)){
            float l=hypotf(fx,fy); if(l>0.3f && l<1.5f){ g_fx=fx/l; g_fy=fy/l; g_faceOk=true; } else g_faceOk=false;
        } else g_faceOk=false;
    }
    // Riding with a destination: auto-follow asked us about roads recently (it does not ask on long
    // stretches without junctions, so allow a generous window) and the rider is moving.
    if(g_endOk && !g_arrived && now-g_hookT<FOLLOW_MS && g_speed>1.5f){
        // Arrived: at the point of road closest to the marker.  Fallback: came within reach of the
        // marker and are now moving away again (took another road past it).
        float lead=g_arriveDist + g_speed*BRAKE_LEAD_S;
        float d=hypotf(x-g_endX,y-g_endY), dm=hypotf(x-g_mx,y-g_my);
        if(dm<g_dmMin) g_dmMin=dm;
        bool passed = g_dmMin <= g_endMk + lead + 8.f && dm > g_dmMin + 4.f;
        if(d <= lead || passed){
            g_arrived=true;
            logf("{\"ev\":\"arrived\",\"marker_dist\":%.1f,\"end_dist\":%.1f,\"speed\":%.1f,\"why\":\"%s\"}",dm,d,(float)g_speed, d<=lead?"road_point":"passed");
            if(g_brakeOn) SetEvent(g_brakeEvt);
        }
    }
}
static void track_position_safe(){ __try { track_position(GetTickCount64()); } __except(EXCEPTION_EXECUTE_HANDLER){ g_posOk=false; } }

// Hold S (scan code, as DirectInput reads it), off the game thread; never while another program has focus.
static DWORD WINAPI BrakeThread(LPVOID){
    for(;;){
        WaitForSingleObject(g_brakeEvt,INFINITE);
        DWORD pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&pid);
        if(pid!=GetCurrentProcessId()){ logf("{\"ev\":\"brake_skipped\",\"why\":\"game not focused\"}"); continue; }
        float v0=g_speed;
        INPUT in={}; in.type=INPUT_KEYBOARD; in.ki.wScan=0x1F; in.ki.dwFlags=KEYEVENTF_SCANCODE;
        UINT sent=SendInput(1,&in,sizeof(in)); Sleep(BRAKE_MS);
        in.ki.dwFlags=KEYEVENTF_SCANCODE|KEYEVENTF_KEYUP; SendInput(1,&in,sizeof(in));
        logf("{\"ev\":\"braked\",\"sent\":%u,\"speed_before\":%.1f,\"speed_after\":%.1f}",sent,v0,(float)g_speed);
    }
}

// ---------------- patching ----------------
static uint8_t* alloc_near(uintptr_t target){
    SYSTEM_INFO si; GetSystemInfo(&si);
    for(uintptr_t d=0x10000; d<0x70000000; d+=si.dwAllocationGranularity){
        for(int s=-1;s<=1;s+=2){
            uintptr_t a=(target+s*(intptr_t)d) & ~(uintptr_t)(si.dwAllocationGranularity-1);
            void* p=VirtualAlloc((void*)a,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
            if(p) return (uint8_t*)p;
        }
    }
    return nullptr;
}
static uint8_t* g_page=nullptr; static size_t g_pos=0;
static void emit(const void* b,size_t n){ memcpy(g_page+g_pos,b,n); g_pos+=n; }
static void emit_rip_store(uint8_t op1,uint8_t op2,uint8_t modrm,volatile uintptr_t* slot){   // mov [rip+slot], reg
    uint8_t* p=g_page+g_pos; p[0]=op1; p[1]=op2; p[2]=modrm;
    *(int32_t*)(p+3)=(int32_t)((uint8_t*)slot-(p+7)); g_pos+=7;
}
static void emit_jmp_abs(const void* dst){
    uint8_t j[6]={0xFF,0x25,0,0,0,0}; emit(j,6); uint64_t a=(uint64_t)dst; emit(&a,8);
}
static void patch_call(uintptr_t site,uint8_t* stub){
    uint8_t* s=(uint8_t*)site;
    DWORD old; VirtualProtect(s,5,PAGE_EXECUTE_READWRITE,&old);
    *(int32_t*)(s+1)=(int32_t)(stub-(s+5));
    VirtualProtect(s,5,old,&old); FlushInstructionCache(GetCurrentProcess(),s,5);
}

// ---------------- address resolution ----------------
struct Sites { uintptr_t fork, snap, cp, filter, cpget, fwglob, vtca, vtent, showpath, vtmap, hname, sctor, sdtor, send; };
#ifdef KC_DEVTOOLS
// Live reload (development builds only): the stub page records the game's original call targets,
// so a newer copy injected into a running game can take the call sites over from the old one.
static const uint64_t PAGE_MAGIC=0x4547415054414B43ull;   // "CKATPAGE"
struct PageInfo { uint64_t magic; uintptr_t filter, cpget, ssUpdate; };
static PageInfo g_prev{};                         // the copy this one takes over from
static const size_t PAGE_INFO=0xF80;
static uintptr_t taken_over(uintptr_t site,bool getter){
    if(!site || *(uint8_t*)site!=0xE8) return 0;
    uintptr_t t=kc::rel32(site+1), pg=t&~(uintptr_t)0xFFF;
    PageInfo pi{}; if(!rdn(pg+PAGE_INFO,&pi,sizeof pi) || pi.magic!=PAGE_MAGIC) return 0;
    g_prev=pi;
    return getter? pi.cpget : pi.filter;
}
#endif
// Resolves every game address; false (with the reasons logged) if this game build is not supported.
static bool resolve(kc::Resolver& R, Sites& s){
    s.fork=R.find("fork chooser call",SIG_FORK,0x9); s.snap=R.find("snap chooser call",SIG_SNAP,0x20); s.cp=R.find("map marker call",SIG_CP,0x8);
    uintptr_t f2=0;
#ifdef KC_DEVTOOLS
    if(uintptr_t o=taken_over(s.fork,false)){ s.filter=o; f2=taken_over(s.snap,false); s.cpget=taken_over(s.cp,true); logf("{\"ev\":\"live_reload\"}"); }
    else
#endif
    { s.filter=R.branch("angle filter",s.fork); f2=R.branch("angle filter (snap)",s.snap); s.cpget=R.branch("marker getter",s.cp); }
    R.verify("marker getter",s.cpget,SIG_CPGET);
    if(R.ok() && s.filter!=f2) R.fail("angle filter","call sites disagree");
    uintptr_t fwref=R.find("game framework",SIG_FRAMEWORK_REF); if(fwref) s.fwglob=R.riprel("game framework",fwref+3);
    s.vtca=R.vtable("CCryAction",".?AVCCryAction@@",0); R.slot("CCryAction::GetClientEntity",s.vtca,66,SIG_GETCLIENTENT);
    s.vtent=R.vtable("CEntity",".?AVCEntity@@",0); R.slot("CEntity::GetWorldPos",s.vtent,46,SIG_GETWORLDPOS);
    s.showpath=R.find("map path line",SIG_SHOWPATH); s.vtmap=R.vtable("C_UIMap",".?AVC_UIMap@guimodule@wh@@",0);
    uintptr_t h=R.find("map path hide",SIG_HIDEPATH);
    if(h){ s.hname=R.riprel("hide event name",h+3); s.sctor=R.branch("string ctor",h+21); s.send=R.branch("send UI event",h+43); s.sdtor=R.branch("string dtor",h+53);
           if(s.hname && strcmp((const char*)s.hname,"HideFastTravelPath")) R.fail("hide event name","unexpected string"); }
    return R.ok();
}
#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out){   // tools/selftest
    kc::Resolver R; R.im.load(game); Sites s{}; bool ok=resolve(R,s);
    out[0]=s.fork; out[1]=s.snap; out[2]=s.cp; out[3]=s.filter; out[4]=s.cpget; out[5]=s.fwglob; out[6]=s.vtca; out[7]=s.vtent; out[8]=s.showpath; out[9]=s.vtmap; return ok?10:-R.fails;
}
// tools/routetest: graph dir, level, start, heading, marker -> route length; returns 1 if it starts backwards
extern "C" __declspec(dllexport) int KC_RouteTest(const char* dir,const char* lvl,float x,float y,float hx,float hy,float mx,float my,float uturn,float* len){
    strcpy_s(kc::g_dir,dir); Graph g; if(!g.load(lvl)) return -1;
    int s=g.nearest(x,y,50.f), t=g.nearest(mx,my,MARKER_SNAP); std::vector<int> p;
    float l=hypotf(hx,hy); hx/=l; hy/=l;
    if(s<0||t<0||!g.route(s,t,p,hx,hy,uturn,FIRST_BAN)) return -2;
    *len=0; for(size_t i=1;i<p.size();i++) *len+=hypotf(g.x[p[i]]-g.x[p[i-1]],g.y[p[i]]-g.y[p[i-1]]);
    return p.size()>1 && g.reverses(hx,hy,p[0],p[1]);
}
#endif

static void run_notice(void* ss){
    const char* line=(const char*)InterlockedExchangePointer((void* volatile*)&g_notice,nullptr);
    if(line) g_ssExec(ss,line,strlen(line),"HorseRouteFollow",nullptr);
}
// Orientation stage (game thread, ~10 times a second from the script-system hook: the road chooser
// is not asked on straight roads, so it cannot drive this).
static void orient_step(uint64_t now){
    if(!g_cur || g_route.empty() || !g_haveMarker || g_arrived || !g_posOk){ g_wrongWay=false; g_showWrong=false; return; }
    update_progress();
    // Orientation stage.  Auto-follow cannot turn a horse around, so when the horse stands (or is just
    // moving off) facing away from the route, the rider is told to turn around and the route is
    // kept.  Turning around simply continues it; riding on the wrong way for WRONG_WAY_M recalculates
    // (quietly).  Only a real facing-away start warns: routes planned while moving always start ahead.
    // "Standing" includes the first seconds of moving off.
    bool moving=g_posOk && g_speed>1.5f;
    bool starting = !moving || (g_speed<6.f && now-g_moveT<3000);
    if(starting && g_faceOk && g_planFree){
        bool w=against_route(g_fx,g_fy);
        if(w && !g_wrongWay){
            g_wrongWay=true; g_wrongT=now;
            logf("{\"ev\":\"wrong_way\",\"at\":[%.1f,%.1f],\"facing\":[%.2f,%.2f]}",(float)g_px,(float)g_py,(float)g_fx,(float)g_fy);
        }
        if(!w && g_wrongWay){ g_wrongWay=false; logf("{\"ev\":\"facing_route\"}"); }
    }
    if(moving && !starting){
        bool along=!against_route(g_mhx,g_mhy);
        if(g_wrongWay){
            if(along){ g_wrongWay=false; logf("{\"ev\":\"turned\",\"secs\":%.1f}",(now-g_wrongT)/1000.f); }
            else if(g_posOk && dist_to_route(g_px,g_py)>=WRONG_WAY_M){ g_wrongWay=false; logf("{\"ev\":\"wrong_way_replan\"}"); }
        }
    }
    // Show the notice only while really riding the wrong way on auto-follow: auto-follow asked about
    // roads recently (never the case on foot, where sprinting can exceed SHOW_SPEED), at riding speed
    // (canter or faster, so neither standing nor walking the horse about), on a road, and moving (not
    // just facing) away from the route.
    g_showWrong = g_wrongWay && g_posOk && g_headOk && now-g_hookT<FOLLOW_MS && g_speed>SHOW_SPEED
                  && against_route(g_mhx,g_mhy) && g_cur->nearest(g_px,g_py,8.f)>=0;
}
static void* SsUpdateHook(void* self,void* a,void* b,void* c){
    void* r=g_ssUpdate(self,a,b,c);
    // The wrong-way notice stays up (re-sent every 2.5 s) while the horse is actually riding the road
    // away from the route on auto-follow (see g_showWrong); never while standing or wandering about.
    uint64_t now=GetTickCount64();
    static uint64_t ot=0;
    if(now-ot>=100){ ot=now; __try { orient_step(now); } __except(EXCEPTION_EXECUTE_HANDLER){ g_wrongWay=g_showWrong=false; } }
    if(g_showWrong && now-g_sayT>2500){ g_sayT=now; notify(WRONG_WAY_LINE); }
    if(g_notice && g_ssExec){ __try { run_notice(self); } __except(EXCEPTION_EXECUTE_HANDLER){ g_ssExec=nullptr; logf("{\"ev\":\"notice_error\"}"); } }
    return r;
}
// Optional: without it the mod works, just without the wrong-way notice.
static void install_notices(const kc::Image& im){
    kc::Resolver R; R.im=im;
    uintptr_t vt=R.vtable("CScriptSystem",".?AVCScriptSystem@@",0), up=0;
#ifdef KC_DEVTOOLS
    if(g_prev.ssUpdate && vt){ up=g_prev.ssUpdate; R.verify("CScriptSystem::Update",up,SIG_SS_UPDATE); }   // live reload: slot holds the old copy\"s hook
    else
#endif
    up=R.slot("CScriptSystem::Update",vt,1,SIG_SS_UPDATE);
    uintptr_t ex=R.slot("CScriptSystem::ExecuteBuffer",vt,6,SIG_SS_EXEC);
    if(!R.ok()){ logf("{\"ev\":\"notices_off\",\"msg\":\"on-screen notices not available in this game version\"}"); return; }
    g_ssExec=(SsExec_t)ex; g_ssUpdate=(SsUpdate_t)up;
    kc::write_ptr(&((void**)vt)[1],(void*)&SsUpdateHook);
#ifdef KC_DEVTOOLS
    ((PageInfo*)(g_page+PAGE_INFO))->ssUpdate=up;
#endif
}

// ---------------- map lookup without opening the map ----------------
// The map screen (C_UIMap) lives inside the game's UI module (C_UIApse, +0x1508) from startup, and it
// holds the custom marker. Found the way the game itself does it (0x2BA782B): game->[+0xE8] ->
// UI-module getter, whose result is cached in a global. Optional: without it the route is planned
// the first time the map is opened.
typedef void* (*GetGame_t)(); typedef void* (*GetModule_t)(void*);
static GetGame_t g_getGame=nullptr; static GetModule_t g_getApse=nullptr;
static void* volatile* g_apseCache=nullptr; static uintptr_t g_vtApse=0;
static void install_map_lookup(const kc::Image& im){
    kc::Resolver R; R.im=im;
    uintptr_t s=R.find("UI module call",SIG_APSE_REF);
    uintptr_t gg=R.branch("game getter",s), ga=s?R.branch("UI module getter",s+12):0;
    R.verify("UI module getter",ga,SIG_GETMODULE);
    uintptr_t cache=ga?R.riprel("UI module cache",ga+16):0;
    uintptr_t vt=R.vtable("C_UIApse",".?AVC_UIApse@guimodule@wh@@",0);
    if(!R.ok()){ logf("{\"ev\":\"map_lookup_off\",\"msg\":\"open the map once after loading to plan the route\"}"); return; }
    g_getGame=(GetGame_t)gg; g_apseCache=(void* volatile*)cache; g_vtApse=vt; g_getApse=(GetModule_t)ga;
}
static void find_map_now(){
    uintptr_t apse=(uintptr_t)*g_apseCache;
    if(!apse){ uintptr_t g=(uintptr_t)g_getGame(); void* mm=g?*(void**)(g+0xE8):nullptr; if(mm) apse=(uintptr_t)g_getApse(mm); }
    if(!apse || !kc::is_a(apse,g_vtApse) || !kc::is_a(apse+0x1508,g_vtUIMap)) return;
    *g_map=apse+0x1508;
    float x,y; bool mk=read_marker(x,y);
    logf("{\"ev\":\"map_found\",\"marker\":%d}",mk);
}
static void find_map(){                          // game thread
    if(!g_getApse || *g_map) return;
    static uint64_t t=0; uint64_t now=GetTickCount64(); if(now-t<2000) return; t=now;
    __try { find_map_now(); } __except(EXCEPTION_EXECUTE_HANDLER){ g_getApse=nullptr; logf("{\"ev\":\"map_lookup_error\"}"); }
}

// ---------------- startup ----------------
static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_autotravel");
    g_brakeOn=kc::ini_int("Options","BrakeOnArrival",1)!=0;
    g_arriveDist=(float)kc::ini_int("Options","ArriveDistance",3);
    g_uturnCost=(float)kc::ini_int("Options","UTurnPenalty",400);
    g_mapLine=kc::ini_int("Options","MapRoute",1)!=0;
    HMODULE h=kc::wait_game(); if(!h){ logf("{\"ev\":\"abort\",\"msg\":\"WHGame.dll not loaded\"}"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("{\"ev\":\"start\",\"version\":\"1.1.0\",\"game_ts\":\"0x%08X\"}",R.im.timestamp);
    Sites s{};
    if(!resolve(R,s)){ logf("{\"ev\":\"disabled\",\"msg\":\"this game version is not supported yet - nothing was changed\"}"); return 0; }
    g_filter=(Filter_t)s.filter; g_cpget=(CpGetter_t)s.cpget;
    g_fwGlobal=(void**)s.fwglob; g_vtCryAction=s.vtca; g_vtEntity=s.vtent;
    g_showPath=(ShowPath_t)s.showpath; g_vtUIMap=s.vtmap;
    g_hideName=(const char*)s.hname; g_strCtor=(StrCtor_t)s.sctor; g_strDtor=(StrDtor_t)s.sdtor; g_sendEvent=(SendEvent_t)s.send;
    for(const char* lvl : {"trosecko","kutnohorsko","klaster"}) if(g_graphs[g_ng].load(lvl)) g_ng++;
    if(!g_ng){ logf("{\"ev\":\"abort\",\"msg\":\"road graphs (.amg) missing next to the DLL\"}"); return 0; }
    g_page=alloc_near(s.fork); if(!g_page){ logf("{\"ev\":\"abort\",\"msg\":\"could not allocate the hook page\"}"); return 0; }
#ifdef KC_DEVTOOLS
    { PageInfo pi{PAGE_MAGIC,s.filter,s.cpget,0}; memcpy(g_page+PAGE_INFO,&pi,sizeof pi); }
#endif
    g_p0=(volatile uintptr_t*)(g_page+0xF00); g_p1=(volatile uintptr_t*)(g_page+0xF08); g_map=(volatile uintptr_t*)(g_page+0xF10);
    uint8_t* stubA=g_page+g_pos;                  // fork chooser: candidate r14 -> r15
    emit_rip_store(0x4C,0x89,0x35,g_p0); emit_rip_store(0x4C,0x89,0x3D,g_p1); emit_jmp_abs((void*)&FilterHook);
    uint8_t* stubB=g_page+g_pos;                  // snap chooser: candidate rbx -> rsi
    emit_rip_store(0x48,0x89,0x1D,g_p0); emit_rip_store(0x48,0x89,0x35,g_p1); emit_jmp_abs((void*)&FilterHook);
    uint8_t* stubM=g_page+g_pos;                  // map checkpoint getter
    emit_jmp_abs((void*)&CpHook);
    g_brakeEvt=CreateEventA(nullptr,FALSE,FALSE,nullptr); CreateThread(nullptr,0,BrakeThread,nullptr,0,nullptr);
    patch_call(s.fork,stubA); patch_call(s.snap,stubB); patch_call(s.cp,stubM);
    install_map_lookup(R.im);
    {   // optional: without it, markers are never ignored (only matters in hardcore)
        kc::Resolver H; H.im=R.im;
        uintptr_t gm=H.find("game mode check",SIG_GAMEMODE);
        uintptr_t gl=gm? H.riprel("game mode object",gm+3) : 0;
        if(H.ok()){ g_gmGlobal=(void**)gl; g_hcJump=gm+0x1C; }
        else logf("{\"ev\":\"hardcore_check_off\"}");
    }
    install_notices(R.im);
    logf("{\"ev\":\"ready\",\"graphs\":%d}",g_ng);
    for(;;){
        Sleep(20);
        track_position_safe();
    }
}

KC_PLUGIN("Horse Route Follow", KC_AUTHOR, 110)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); }
    return TRUE;
}
