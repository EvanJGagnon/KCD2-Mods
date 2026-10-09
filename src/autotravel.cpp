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
//     CpHook), which also draws the route with the game's own fast-travel path line.  The game
//     takes that line for a hovered fast-travel point and hides it afterwards (that call goes
//     through HideHook), and the route is drawn again.
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
// Mounted state: the game's own check behind Lua human:IsMounted() (C_ScriptBindHuman table entry ->
// native bool(actor): [actor+0x990]->vtbl[6]() == 2), on the client actor (CCryAction vtbl[64]).
static const char* SIG_ISMOUNTED      = "48 83 EC 28 48 8B 89 90 09 00 00 48 8B 01 FF 50 30 3C 02 0F 94 C0 48 83 C4 28 C3";   // 0x46D45C
static const char* SIG_GETCLIENTACTOR = "48 83 EC 28 48 8B 89 88 00 00 00 33 C0 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 83 C4 28 C3";  // CCryAction vtbl[64]
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
static const float    EXACT_TOL    = 1.5f;    // candidate point is this graph node (graph nodes are the game's own road points)
static const float    MARKER_SNAP   = 5000.0f; // marker -> nearest road node (off-road markers route to the closest road point)
static const float    CORRIDOR      = 6.0f;    // metres around the route that count as on it (parallel streets/alleys)
static const uint64_t STUCK_MS      = 400;     // standing: every road ahead refused this long -> let the game choose
static const uint64_t STUCK_MOVE_MS = 150;     // moving:   every road ahead refused this long -> let the game choose
static const uint64_t STUCK_NONE_MS = 900;     // moving:   no road accepted at all this long -> let the game choose
static const float    SHOW_SPEED    = 6.5f;    // wrong-way notice only above this speed (m/s): riding, not walking about
static const uint64_t FOLLOW_MS     = 20000;   // auto-follow asked about roads this recently = riding on auto-follow
static const float    START_TURN_COST = 1500.0f; // moving: a route starts by turning round only if every way ahead is this much longer
static const float    WRONG_WAY_M   = 60.0f;   // riding on the wrong way this far from the route -> recalculate
static const float    DEVIATE       = 15.0f;   // rider this far from the route ...
static const uint64_t DEVIATE_MS    = 1000;    // ... for this long = left the route: recalculate
static const uint64_t MARKER_GONE_MS= 1500;    // a marker missing this long is removed (the game briefly empties the list when markers change)
static const uint64_t STILL_MS      = 1500;    // standing this long = stopped (e.g. at a locked gate)
static const uint64_t STILL_RECENT  = 60000;   // a stop on the route this recent, then leaving it near the stop = the way was blocked
static const uint64_t NOT_OFFERED_RECENT = 10000; // the game did not offer the route's road this recently, then the rider left = that turn is not auto-followable
static const float    AVOID_LEN     = 200.0f;  // blocked: this many metres of the old route ahead of the stop are avoided
static const float    AVOID_TURN_LEN= 40.0f;   // not offered: this many metres after the junction are avoided
static const float    AVOID_COST    = 600.0f;  // bounded: an avoided stretch never forces a detour more than this many metres longer
static const uint64_t AVOID_TTL     = 300000;  // avoided stretches are forgotten after 5 minutes, ...
static const float    AVOID_FAR     = 300.0f;  // ... when the rider is this far from them, or rides them after all
static const size_t   AVOID_MAX     = 4;
static const float    LEVEL_TOL     = 0.25f;   // a road point of another level matches its graph this exactly
static const float    MATCH_TOL     = 4.0f;    // the rider is on the road segment within this distance (auto-follow tracks the road within ~2 m)
static const float    PROG_BACK     = 30.0f;   // progress window: this far behind ...
static const float    PROG_AHEAD    = 60.0f;   // ... and this far ahead of the last progress, ...
static const float    PROG_MAX_SPEED= 15.0f;   // ... plus this many m/s since the last update (a horse is slower)
static const float    BRANCH_MIN    = 4.0f;    // within this of the route: on it (tracking error)
static const float    BRANCH_MARGIN = 2.0f;    // on another road: closer to it than to the route by this much
static const float    BRANCH_MOVE   = 10.0f;   // ... and ridden this far along it -> left the route
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
// Mounted state (the game's own IsMounted on the client actor): 1 mounted, 0 not, -1 unknown.  Sampled on
// the game thread; the brake never acts on anything but a fresh 1.
typedef bool  (*IsMounted_t)(void*); typedef void* (*GetActor_t)(void*);
static IsMounted_t g_isMounted=nullptr;
static volatile int g_mount=-1; static volatile uint64_t g_mountT=0; static volatile uintptr_t g_actor=0;
static const uint64_t MOUNT_FRESH_MS = 300;      // a game-thread sample this recent is trusted ...
static const uint64_t MOUNT_STALE_MS = 2000;     // ... older than this (no game-thread sample): unknown
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
    std::vector<float> extra;                                 // per directed edge: avoid cost (a stretch the rider was blocked on)
    std::vector<uint32_t> src;                                // per directed edge (CSR index): its start node
    std::unordered_map<int64_t, std::vector<uint32_t>> egrid; // road segments (CSR index u->v, u<v) by 8 m cell: which road is the rider on
    std::vector<int> comp; int mainComp=-1;                   // connected road networks; the main one (by far the largest) is
                                                              // the only one routes start on and waypoints snap to
    std::unordered_map<int64_t, std::vector<int>> grid;       // 8 m buckets
    static int64_t key(int gx,int gy){ return ((int64_t)gx<<32) ^ (uint32_t)gy; }
    // Loads <level>.amg and checks it: magic, exact size for its header, every coordinate finite and
    // on the map, every edge between two different existing nodes, its length matching its points and
    // its cost a road-class multiple of the length (x1 .. x2.5).  Anything else: not loaded (logged).
    bool load(const char* lvl){
        char fn[64], p[MAX_PATH]; sprintf_s(fn,"%s.amg",lvl); kc::path(p,sizeof p,fn);
        FILE* f=nullptr; fopen_s(&f,p,"rb"); if(!f) return false;
        auto bad=[&](const char* why){ fclose(f); x.clear(); y.clear(); logf("{\"ev\":\"bad_graph\",\"level\":\"%s\",\"why\":\"%s\"}",lvl,why); return false; };
        char mg[4]={0}; uint32_t nn=0,ne=0;
        if(fread(mg,1,4,f)!=4 || memcmp(mg,"AMG1",4)) return bad("not an AMG1 file");
        if(fread(&nn,4,1,f)!=1 || fread(&ne,4,1,f)!=1) return bad("header cut short");
        if(nn==0||nn>2000000||ne==0||ne>4000000) return bad("node or edge count out of range");
        _fseeki64(f,0,SEEK_END); long long sz=_ftelli64(f); _fseeki64(f,12,SEEK_SET);
        if(sz!=12+(long long)nn*8+(long long)ne*16) return bad("file size does not match its header");
        std::vector<float> xy((size_t)nn*2);
        if(fread(xy.data(),8,nn,f)!=nn) return bad("nodes cut short");
        x.resize(nn); y.resize(nn);
        for(uint32_t i=0;i<nn;i++){ x[i]=xy[2*i]; y[i]=xy[2*i+1]; if(!sane(x[i],y[i])) return bad("node coordinate not finite / off the map"); }
        struct E { uint32_t a,b; float l,c; }; std::vector<E> ev(ne);
        if(fread(ev.data(),16,ne,f)!=ne) return bad("edges cut short");
        fclose(f);
        std::vector<uint32_t> ea(ne), eb(ne); std::vector<float> ec(ne), el(ne);
        for(uint32_t i=0;i<ne;i++){
            const E& e=ev[i];
            if(e.a>=nn || e.b>=nn || e.a==e.b){ x.clear(); y.clear(); logf("{\"ev\":\"bad_graph\",\"level\":\"%s\",\"why\":\"edge with a missing node or to itself\"}",lvl); return false; }
            float g=hypotf(x[e.a]-x[e.b],y[e.a]-y[e.b]);
            if(!std::isfinite(e.l) || !std::isfinite(e.c) || e.l<=0.f || e.l>1000.f || fabsf(e.l-g)>0.05f+0.001f*g || e.c<e.l*0.99f || e.c>e.l*2.6f){
                x.clear(); y.clear(); logf("{\"ev\":\"bad_graph\",\"level\":\"%s\",\"why\":\"edge length or cost inconsistent\"}",lvl); return false; }
            ea[i]=e.a; eb[i]=e.b; el[i]=e.l; ec[i]=e.c;
        }
        off.assign(nn+1,0);
        for(uint32_t i=0;i<ne;i++){ off[ea[i]+1]++; off[eb[i]+1]++; }
        for(uint32_t i=0;i<nn;i++) off[i+1]+=off[i];
        to.resize(off[nn]); cost.resize(off[nn]); std::vector<uint32_t> fill(off.begin(),off.end()-1);
        for(uint32_t i=0;i<ne;i++){
            to[fill[ea[i]]]=eb[i]; cost[fill[ea[i]]++]=ec[i];
            to[fill[eb[i]]]=ea[i]; cost[fill[eb[i]]++]=ec[i];
        }
        for(uint32_t i=0;i<nn;i++) grid[key((int)floorf(x[i]/8),(int)floorf(y[i]/8))].push_back(i);
        extra.assign(to.size(),0.f);
        src.resize(to.size());
        for(uint32_t u=0;u<nn;u++) for(uint32_t e=off[u];e<off[u+1];e++){
            src[e]=u; uint32_t v=to[e]; if(v<=u) continue;
            int x0=(int)floorf(std::min(x[u],x[v])/8), x1=(int)floorf(std::max(x[u],x[v])/8), y0=(int)floorf(std::min(y[u],y[v])/8), y1=(int)floorf(std::max(y[u],y[v])/8);
            for(int gx=x0;gx<=x1;gx++) for(int gy=y0;gy<=y1;gy++) egrid[key(gx,gy)].push_back(e);
        }
        comp.assign(nn,-1); std::vector<int> sizes, st;
        for(uint32_t s0=0;s0<nn;s0++){
            if(comp[s0]>=0) continue;
            int c=(int)sizes.size(), k=0; st.assign(1,(int)s0); comp[s0]=c;
            while(!st.empty()){ int u=st.back(); st.pop_back(); k++; for(uint32_t e=off[u];e<off[u+1];e++) if(comp[to[e]]<0){ comp[to[e]]=c; st.push_back((int)to[e]); } }
            sizes.push_back(k);
        }
        mainComp=(int)(std::max_element(sizes.begin(),sizes.end())-sizes.begin());
        name=lvl; return true;
    }
    bool on_main(int n) const { return n>=0 && comp[n]==mainComp; }
    int nearest_main(float px,float py,float maxd) const {   // nearest node on the main road network
        int best=-1; float bd=maxd*maxd;
        if(maxd>200){ for(size_t i=0;i<x.size();i++){ if(comp[i]!=mainComp) continue; float d=(x[i]-px)*(x[i]-px)+(y[i]-py)*(y[i]-py); if(d<bd){bd=d;best=(int)i;} } return best; }
        int r=(int)ceilf(maxd/8), gx=(int)floorf(px/8), gy=(int)floorf(py/8);
        for(int dx=-r;dx<=r;dx++) for(int dy=-r;dy<=r;dy++){
            auto it=grid.find(key(gx+dx,gy+dy)); if(it==grid.end()) continue;
            for(int i:it->second){ if(comp[i]!=mainComp) continue; float d=(x[i]-px)*(x[i]-px)+(y[i]-py)*(y[i]-py); if(d<bd){bd=d;best=i;} }
        }
        return best;
    }
    int edge(int u,int v) const { for(uint32_t e=off[u];e<off[u+1];e++) if((int)to[e]==v) return (int)e; return -1; }
    void avoid(int u,int v,float c){ int e=edge(u,v); if(e>=0) extra[e]=std::max(extra[e],c); e=edge(v,u); if(e>=0) extra[e]=std::max(extra[e],c); }
    bool avoided(int u) const { for(uint32_t e=off[u];e<off[u+1];e++) if(extra[e]>0) return true; return false; }
    void clear_avoid(){ std::fill(extra.begin(),extra.end(),0.f); }
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
    int nearest_free(float px,float py,float maxd) const {   // nearest node not on an avoided stretch (maxd <= 200)
        int best=-1; float bd=maxd*maxd; int r=(int)ceilf(maxd/8);
        int gx=(int)floorf(px/8), gy=(int)floorf(py/8);
        for(int dx=-r;dx<=r;dx++) for(int dy=-r;dy<=r;dy++){
            auto it=grid.find(key(gx+dx,gy+dy)); if(it==grid.end()) continue;
            for(int i:it->second){ if(avoided(i) || comp[i]!=mainComp) continue; float d=(x[i]-px)*(x[i]-px)+(y[i]-py)*(y[i]-py); if(d<bd){bd=d;best=i;} }
        }
        return best;
    }
    // Does u->v point against direction (ax,ay)?  (more than ~100 degrees; a geometric test for the
    // rider's heading -- NOT how route turns are judged, see route())
    bool reverses(float ax,float ay,int u,int v) const {
        float dx=x[v]-x[u], dy=y[v]-y[u], l=hypotf(dx,dy);
        return l>0.01f && (ax!=0||ay!=0) && (dx*ax+dy*ay)/l < -0.2f;
    }
    // The road behind a horse at node s travelling (hx,hy): the segment from s pointing most nearly
    // back (more than 120 degrees from the heading), or -1.
    int behind_edge(int s,float hx,float hy) const {
        if(hx==0 && hy==0) return -1;
        int best=-1; float bd=-0.5f;
        for(uint32_t e=off[s];e<off[s+1];e++){ float dx,dy; dir(e,s,dx,dy); float d=dx*hx+dy*hy; if(d<bd){ bd=d; best=(int)e; } }
        return best;
    }
    // Nearest road segment within maxd (<= ~40 m) of (px,py): its end nodes and distance.
    bool nearest_edge(float px,float py,float maxd,int& eu,int& ev,float& ed) const {
        float bd=maxd*maxd; eu=ev=-1; int r=(int)ceilf(maxd/8), gx=(int)floorf(px/8), gy=(int)floorf(py/8);
        for(int dx=-r;dx<=r;dx++) for(int dy=-r;dy<=r;dy++){
            auto it=egrid.find(key(gx+dx,gy+dy)); if(it==egrid.end()) continue;
            for(uint32_t e:it->second){
                int u=(int)src[e], v=(int)to[e]; float ax=x[u], ay=y[u], ex=x[v]-ax, ey=y[v]-ay, l2=ex*ex+ey*ey;
                float t= l2>1e-6f ? ((px-ax)*ex+(py-ay)*ey)/l2 : 0.f; t=std::min(1.f,std::max(0.f,t));
                float qx=ax+t*ex-px, qy=ay+t*ey-py, d=qx*qx+qy*qy;
                if(d<bd){ bd=d; eu=u; ev=v; }
            }
        }
        ed=sqrtf(bd); return eu>=0;
    }
    // Closest point to (px,py) on a road segment of the main network: the point (qx,qy) on segment a-b.
    bool closest_on_road(float px,float py,float& qx,float& qy,int& a,int& b) const {
        float bd=1e30f; a=b=-1;
        for(int u=0;u<(int)x.size();u++) for(uint32_t e=off[u];e<off[u+1];e++){
            int v=to[e]; if(v<u || comp[u]!=mainComp) continue;     // waypoints only on the main road network
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
    // Cheapest route s->t over directed road segments (cost = length x road class, + avoid costs).
    // Turning around costs `uturn`: going back along the segment just ridden (backtracking, e.g. at a
    // dead end) and -- as the first step -- taking the road behind the horse (`first`: 0 when it stands,
    // the U-turn cost when it moves).  A sharp turn onto ANOTHER road at a junction is an ordinary turn
    // and costs nothing extra.  costOut: the route's total cost.
    bool route(int s,int t,std::vector<int>& path,float hx,float hy,float uturn,float first,float* costOut=nullptr) const {
        path.clear();
        if(costOut) *costOut=0;
        if(s==t){ path.push_back(s); return true; }
        size_t E=to.size(); int back=behind_edge(s,hx,hy);
        std::vector<float> dist(E,1e30f); std::vector<int32_t> prev(E,-1), from(E,-1);
        typedef std::pair<float,uint32_t> P; std::priority_queue<P,std::vector<P>,std::greater<P>> q;
        for(uint32_t e=off[s];e<off[s+1];e++){
            float d=cost[e]+extra[e]+((int)e==back?first:0.f);
            from[e]=s; if(d<dist[e]){ dist[e]=d; q.push({d,e}); }
        }
        int64_t hit=-1;
        while(!q.empty()){
            auto [d,e]=q.top(); q.pop();
            if(d>dist[e]) continue;
            int u=to[e];
            if(u==t){ hit=e; break; }
            for(uint32_t f=off[u];f<off[u+1];f++){
                float nd=d+cost[f]+extra[f]+((int32_t)to[f]==from[e]?uturn:0.f);      // straight back along the same segment
                if(nd<dist[f]){ dist[f]=nd; prev[f]=(int32_t)e; from[f]=u; q.push({nd,f}); }
            }
        }
        if(hit<0) return false;
        if(costOut) *costOut=dist[hit];
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
static uint64_t g_progT=0;                      // last progress update (see update_progress)
static float g_mx=0,g_my=0; static bool g_haveMarker=false;
static uint64_t g_lastPlan=0, g_fwdRejectT=0, g_yieldUntil=0;
// The route is planned ONCE per destination.  The only reasons to plan again: no route yet ("first"),
// the marker was placed/moved ("marker"), the level changed ("level"), the previous attempt found no
// route ("retry"), or the rider left the route ("left_route").  Every plan logs its reason.
static const char* g_needWhy="first";
static volatile uint64_t g_stillT=0; static volatile float g_stillX=0,g_stillY=0;   // last real stop of the rider (20 ms thread)
// Avoided stretches (a way the rider could not take): a bounded extra cost on their road segments for
// re-plans to the current destination.  None is permanent: each is dropped when the rider rides it after
// all, gets far from it, or after AVOID_TTL; all are dropped when the destination or the level changes.
struct AvoidStretch { std::vector<int> nodes; std::vector<float> along; uint64_t t; const char* why; };
static std::vector<AvoidStretch> g_avoids;
// The whole stretch costs AVOID_COST extra, spread over its segments by length (riding all of it = +AVOID_COST).
static void apply_avoids(){
    if(!g_cur) return; g_cur->clear_avoid();
    for(auto& a:g_avoids){ float tot=a.along.back(); for(size_t i=0;i+1<a.nodes.size();i++) g_cur->avoid(a.nodes[i],a.nodes[i+1], tot>0.01f? AVOID_COST*(a.along[i+1]-a.along[i])/tot : AVOID_COST); }
}
static void clear_avoid_all(){ g_avoids.clear(); for(int i=0;i<g_ng;i++) g_graphs[i].clear_avoid(); }
static int g_planGen=0;                          // bumped by every plan (a recorded route index belongs to one plan)
static int g_noK=-1, g_noGen=-1; static uint64_t g_noT=0;   // where the game last did not offer the route's road

// The map's custom-marker list (+0x588..+0x590: pointers to records, position at +0x50/+0x54) normally
// holds one entry, but it can also hold entries the map does not show (e.g. a leftover marker; a player
// was routed there and it never moved).  The map's own marker refresh says which entry it shows (CpHook
// passes the getter's result: none when the map hides the flag), and that entry is used.  Between
// refreshes, the entry placed or moved last: any entry that is new or moved since the previous read (at
// the first read that is the last one, the newest).  Once the entry in use is gone, no other entry takes
// its place until one is placed or moved, so a marker the map does not show is never picked up.  An
// empty or unreadable list leaves all this as it is (it can be a brief gap).
static SRWLOCK g_mkLock=SRWLOCK_INIT;            // read from the game thread and the map's draw call
struct Mk { uintptr_t rec; float x,y; };
struct MkSeen { float x,y; uint64_t t; };
static struct { MkSeen hist[64]; size_t nh; bool havePick; float pickX,pickY; float prevX[32],prevY[32]; size_t np; size_t lastN,lastNc; int lists,dumps; } g_mkl{};   // what the list read remembers
static bool read_marker(float& mx,float& my,const Mk* shown=nullptr){
    uintptr_t m=g_map?*g_map:0; if(!m) return false;
    uintptr_t b=0,e=0; if(!rd(m+0x588,&b)||!rd(m+0x590,&e)||!b||e<=b) return false;
    auto& hist=g_mkl.hist; auto& nh=g_mkl.nh;                 // positions seen in the last minute
    auto& havePick=g_mkl.havePick; auto& pickX=g_mkl.pickX; auto& pickY=g_mkl.pickY;   // the position in use
    auto& prevX=g_mkl.prevX; auto& prevY=g_mkl.prevY; auto& np=g_mkl.np;               // positions in the previous read
    auto& lastN=g_mkl.lastN; auto& lastNc=g_mkl.lastNc; auto& lists=g_mkl.lists; auto& dumps=g_mkl.dumps;   // log limits (the map can re-read the list several times a second)
    uint64_t now=GetTickCount64();
    Mk cur[32]; size_t nc=0; bool changed=false; float chX=0,chY=0; bool app[32]={false};
    Mk all[32]; size_t na=0, zeros=0;
    size_t n=(e-b)/8; if(n>32) n=32;
    AcquireSRWLockExclusive(&g_mkLock);
    for(size_t i=0;i<n;i++){
        uintptr_t rec=0; float x=0,y=0;
        if(!rd(b+8*i,&rec)||!rec||!rd(rec+0x50,&x)||!rd(rec+0x54,&y)||!sane(x,y)) continue;
        all[na++]={rec,x,y};
        // A position of exactly (0,0) is an unset record, never a place the rider asked for: routing there
        // sent riders to the map's corner.  Skip it (the entry is still shown in the DebugLog dump).
        if(fabsf(x)<0.01f && fabsf(y)<0.01f){ zeros++; continue; }
        bool known=false;
        for(size_t k=0;k<nh;k++) if(fabsf(hist[k].x-x)<0.5f && fabsf(hist[k].y-y)<0.5f){ hist[k].t=now; known=true; break; }
        if(!known){
            changed=true; chX=x; chY=y;
            size_t slot=nh<64? nh++ : 0;
            if(slot==0 && nh==64){ for(size_t k=1;k<64;k++) if(hist[k].t<hist[slot].t) slot=k; }
            hist[slot]={x,y,now};
        }
        bool was=false; for(size_t k=0;k<np;k++) if(fabsf(prevX[k]-x)<0.5f && fabsf(prevY[k]-y)<0.5f){ was=true; break; }
        app[nc]=!was;                                          // present now, not in the previous read
        cur[nc++]={rec,x,y};
    }
    { size_t w=0; for(size_t k=0;k<nh;k++) if(now-hist[k].t<=60000) hist[w++]=hist[k]; nh=w; }   // forget positions gone for a minute
    bool listChanged = (changed || n!=lastN); lastN=n;
    if(kc::g_debug && (na>1 || zeros) && listChanged && dumps<10){      // what the list holds: where is the real marker stored?
        dumps++;
        static uintptr_t whBase=(uintptr_t)GetModuleHandleA("WHGame.dll");
        logf("{\"ev\":\"marker_dump\",\"entries\":%zu,\"valid\":%zu,\"zero_pos\":%zu,\"vec\":\"0x%llX\",\"map\":\"0x%llX\"}",n,na,zeros,(unsigned long long)b,(unsigned long long)m);
        for(size_t i=0;i<na;i++){
            unsigned char raw[0x80]={0}; rdn(all[i].rec,raw,sizeof raw); uintptr_t vt=0; rd(all[i].rec,&vt);
            char hex[0x80*2+1]; for(int k=0;k<0x80;k++) sprintf_s(hex+2*k,3,"%02x",raw[k]);
            logf("{\"ev\":\"marker_rec\",\"i\":%zu,\"rec\":\"0x%llX\",\"x\":%.2f,\"y\":%.2f,\"vt_rva\":\"0x%llX\",\"raw\":\"%s\"}",
                 i,(unsigned long long)all[i].rec,all[i].x,all[i].y,(unsigned long long)(vt>whBase?vt-whBase:0),hex);
        }
    }
    // Which entry: a position not seen in the last minute (placed/moved) wins; else the one in use, while it
    // is there; if it is gone, the entry that appeared in this read (the game moves a marker by removing it
    // and adding it again -- also back to a place used a moment ago).  If it is gone and nothing appeared,
    // there is NO marker in this read: a stale entry that was there all along never takes over (the
    // destination is kept through short gaps by sync_destination).
    size_t i=nc;
    if(shown && !shown->rec){ havePick=true; pickX=pickY=-1e9f; }                // the map hides the flag: no marker (not one of the other entries)
    else if(nc>0){
        if(shown){ if(shown->rec){ pickX=shown->x; pickY=shown->y; havePick=true; } }   // the map's own word wins
        else if(changed){ pickX=chX; pickY=chY; havePick=true; }
        if(havePick) for(size_t k=0;k<nc;k++) if(fabsf(cur[k].x-pickX)<0.5f && fabsf(cur[k].y-pickY)<0.5f) i=k;
        if(i==nc){
            if(!havePick) i=nc-1;                              // first read: the newest entry
            else for(size_t k=0;k<nc;k++) if(app[k]) i=k;      // the marker in use moved here
            if(i<nc){ pickX=cur[i].x; pickY=cur[i].y; havePick=true; }
            else if(lists<40){ lists++; logf("{\"ev\":\"marker_list\",\"count\":%zu,\"note\":\"marker in use gone - the other entries are not used\"}",nc); }
        }
    }
    bool ok=i<nc || (shown && shown->rec);
    for(size_t k=0;k<nc;k++){ prevX[k]=cur[k].x; prevY[k]=cur[k].y; } np=nc;
    if(ok){
        if(i<nc && (nc>1||zeros) && (changed || nc!=lastNc) && lists<40){ lists++; logf("{\"ev\":\"marker_list\",\"count\":%zu,\"zero_skipped\":%zu,\"using\":%zu,\"at\":[%.1f,%.1f]}",nc,zeros,i,cur[i].x,cur[i].y); }
        if(i<nc){ mx=cur[i].x; my=cur[i].y; } else { mx=shown->x; my=shown->y; }
    } else if(nc==0 && zeros && listChanged && lists<40){ lists++; logf("{\"ev\":\"marker_list\",\"count\":0,\"zero_skipped\":%zu,\"note\":\"only unset (0,0) entries - no marker\"}",zeros); }
    lastNc=nc;
    ReleaseSRWLockExclusive(&g_mkLock);
    return ok;
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
    g_needWhy="marker"; clear_avoid_all();          // a new destination: forget stretches avoided on the way to the old one
    set_dest();
}
static void set_level(Graph* g){
    if(g==g_cur) return;
    g_cur=g; clear_route(); g_lastPlan=0; g_dmMin=1e9f; set_dest();
    g_needWhy="level"; clear_avoid_all();
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

// A marker that goes missing (the map hides it, or it is gone from the list) keeps its destination for
// MARKER_GONE_MS, so a brief gap changes nothing; still missing after that, it has been removed.
static volatile uint64_t g_goneT=0;               // when the marker went missing (0: it is there)
static bool marker_gone(uint64_t now){ uint64_t t=g_goneT; return t && now-t>=MARKER_GONE_MS; }   // any thread
static void clear_destination(){                  // the marker is gone: its route, arrival and map line with it
    g_haveMarker=false; g_goneT=0;
    clear_route(); g_prog=0; g_endOk=false; g_endA=g_endB=-1;
    g_arrived=false; g_dmMin=1e9f; g_wrongWay=false; g_showWrong=false; clear_avoid_all();
    logf("{\"ev\":\"marker_removed\"}");
}
// Applies the current marker state; true if there is a destination to route to.
static bool sync_destination(bool mk,float mx,float my,uint64_t now=GetTickCount64()){
    if(mk && markers_hidden()){                   // hardcore without the add-on: a stored marker is invisible
        static bool said=false; if(!said){ said=true; logf("{\"ev\":\"hardcore_marker_ignored\"}"); }
        mk=false;
    }
    if(mk){
        if(g_goneT){ g_goneT=0; KC_DLOG("{\"ev\":\"marker_back\"}"); }
        if(!g_haveMarker || hypotf(mx-g_mx,my-g_my)>5) set_marker(mx,my);
        return true;
    }
    if(!g_haveMarker || !g_map || !*g_map) return false;
    // The game empties and refills the list when markers change: a marker missing from one read is not a
    // removed marker.  Only one gone for MARKER_GONE_MS drops the destination (and the route with it).
    if(!g_goneT){ g_goneT=now; KC_DLOG("{\"ev\":\"marker_missing\"}"); }
    if(!marker_gone(now)) return true;            // a brief gap: keep everything
    clear_destination();
    return false;
}
static bool marker_destination(uint64_t now){     // game thread: the marker as the list holds it
    float mx=0,my=0; bool mk=read_marker(mx,my);
    return sync_destination(mk,mx,my,now);
}
static void marker_step(uint64_t now){ if(g_goneT) marker_destination(now); }   // game thread: settle a missing marker even if nothing asks
static bool g_planFree=false;                     // plan_from: may the route start behind the horse?
static bool g_viaA=false;                         // last plan arrived at the destination from its segment's end A (logged; tests)
// Standing: the cheapest route, whichever way it starts; a later turn-back costs UTurnPenalty.  Moving:
// the horse carries on ahead unless turning round saves START_TURN_COST -- wherever the turn would be
// (at once, or a few road points further on: both are the same turn-around for a moving horse).  A route
// that turns round is never left to auto-follow: the rider is told (wrong-way notice) and the route waits.
static bool route_to(int n0,int t,std::vector<int>& p,float* c){
    if(g_planFree) return g_cur->route(n0,t,p,g_hx,g_hy,g_uturnCost,0.f,c);
    float turn=std::max(g_uturnCost,START_TURN_COST);
    return g_cur->route(n0,t,p,g_hx,g_hy,turn,turn,c);
}

// DebugLog only: what the planner was given and the road points it chose, for "the route is wrong" reports.
// Nodes are indices into <level>.amg with their world x,y and the distance from the route start.
static void log_route_detail(int n0,bool useA,float ta,float tb){
    const Graph& g=*g_cur;
    int revs=0; for(size_t i=0;i<g_rev.size();i++) revs+=g_rev[i]?1:0;
    // Keep the log small: an identical route is not listed twice in a row, and at most MAX_DUMPS lists per session.
    static uint32_t lastHash=0; static int dumps=0; const int MAX_DUMPS=12;
    uint32_t h=2166136261u; for(int id:g_route){ h=(h^(uint32_t)id)*16777619u; }
    bool same=(h==lastHash && lastHash!=0); lastHash=h;
    bool list=!same && dumps<MAX_DUMPS; if(list) dumps++;
    logf("{\"ev\":\"plan_detail\",\"same_route\":%d,\"nodes_listed\":%d,\"start_node\":%d,\"start\":[%.1f,%.1f],\"rider\":[%.1f,%.1f],\"heading\":[%.2f,%.2f],\"free\":%d,\"uturn\":%.0f,"
         "\"marker\":[%.1f,%.1f],\"dest\":[%.1f,%.1f],\"marker_to_road\":%.1f,\"seg\":[%d,%d],\"seg_a\":[%.1f,%.1f],\"seg_b\":[%.1f,%.1f],"
         "\"via\":\"%c\",\"cost_a\":%.0f,\"cost_b\":%.0f,\"nodes\":%zu,\"reversals\":%d}",
         (int)same,(int)list,n0,g.x[n0],g.y[n0],(float)g_px,(float)g_py,g_hx,g_hy,(int)g_planFree,(float)g_uturnCost,
         g_mx,g_my,(float)g_endX,(float)g_endY,(float)g_endMk,g_endA,g_endB,g.x[g_endA],g.y[g_endA],g.x[g_endB],g.y[g_endB],
         useA?'A':'B',ta<1e29f?ta:-1.f,tb<1e29f?tb:-1.f,g_route.size(),revs);
    static bool saidLimit=false;
    if(!list){ if(!same && !saidLimit){ saidLimit=true; logf("{\"ev\":\"route_nodes_suppressed\",\"why\":\"limit of %d lists reached\"}",MAX_DUMPS); } return; }
    const size_t PER=30; size_t n=g_route.size(), parts=(n+PER-1)/PER;
    for(size_t part=0;part<parts;part++){
        char buf[1536]; int len=sprintf_s(buf,"{\"ev\":\"route_nodes\",\"part\":%zu,\"of\":%zu,\"n\":[",part+1,parts);
        for(size_t i=part*PER;i<n && i<(part+1)*PER && len<(int)sizeof buf-80;i++){
            int id=g_route[i]; len+=sprintf_s(buf+len,sizeof buf-len,"%s[%d,%.1f,%.1f,%.0f%s]",i>part*PER?",":"",id,g.x[id],g.y[id],g_cum[i],g_rev[i]?",\"rev\"":"");
        }
        sprintf_s(buf+len,sizeof buf-len,"]}"); logf("%s",buf);
    }
}

// Plans from graph node n0.  free=true (horse standing): the truly shortest route, even if it starts
// behind the horse -- the orientation stage then turns the horse around.  free=false (moving): the
// shortest route that starts the way the horse is going.
static void plan_from(int n0,bool free=false,const char* why=nullptr){
    const char* reason = why? why : g_needWhy;
    g_planFree=free; g_wrongWay=false; g_showWrong=false;
    clear_route(); g_prog=0; g_lastPlan=GetTickCount64(); g_progT=g_lastPlan; g_planGen++;
    g_needWhy="retry";                                // until this plan succeeds
    if(!g_endOk) set_dest();
    if(!g_endOk){ logf("{\"ev\":\"plan_fail\",\"why\":\"no_destination\",\"mx\":%.1f,\"my\":%.1f}",g_mx,g_my); return; }
    // Route onto the destination's segment and across it, so the rider passes the exact closest point.
    std::vector<int> pa,pb; float ca=0,cb=0; bool oa=route_to(n0,g_endA,pa,&ca), ob=route_to(n0,g_endB,pb,&cb);
    if(!oa && !ob){ logf("{\"ev\":\"plan_fail\",\"why\":\"no_route\"}"); return; }
    g_needWhy="unexpected";                           // a later plan with no reason set would show up as this in the log
    // Which end of the destination's segment to arrive from: the cheaper TOTAL (route cost to that end +
    // the segment's own cost up to the destination point), not the shorter distance.
    float segLen=hypotf(g_cur->x[g_endB]-g_cur->x[g_endA],g_cur->y[g_endB]-g_cur->y[g_endA]);
    int eAB=g_cur->edge(g_endA,g_endB); float segCost= eAB>=0? g_cur->cost[eAB] : segLen;
    float fa= segLen>0.01f? hypotf(g_endX-g_cur->x[g_endA],g_endY-g_cur->y[g_endA])/segLen : 0.f;
    float ta= oa? ca+segCost*fa : 1e30f, tb= ob? cb+segCost*(1.f-fa) : 1e30f;
    bool useA = ta<=tb; g_viaA=useA;
    g_route = useA? pa : pb;
    int other = useA? g_endB : g_endA;
    if(g_route.size()<2 || g_route[g_route.size()-2]!=other) g_route.push_back(other);
    size_t n=g_route.size();
    g_cum.assign(n,0.f); g_rev.assign(n,0);
    for(size_t i=1;i<n;i++){ int a=g_route[i-1],b=g_route[i]; g_cum[i]=g_cum[i-1]+hypotf(g_cur->x[a]-g_cur->x[b],g_cur->y[a]-g_cur->y[b]); }
    for(size_t i=1;i+1<n;i++) g_rev[i]= g_route[i+1]==g_route[i-1];   // the route goes straight back here (a U-turn auto-follow cannot make)
    logf("{\"ev\":\"plan\",\"why\":\"%s\",\"level\":\"%s\",\"from\":[%.1f,%.1f],\"to\":[%.1f,%.1f],\"len\":%.0f}",
         reason,g_cur->name,g_cur->x[n0],g_cur->y[n0],g_mx,g_my,g_cum.back());
    if(!free && n>1){                                 // moving, and the route starts back the way the horse came
        int b=g_cur->behind_edge(n0,g_hx,g_hy);
        if(b>=0 && (int)g_cur->to[b]==g_route[1]){
            g_wrongWay=true; g_wrongT=GetTickCount64();
            logf("{\"ev\":\"wrong_way\",\"why\":\"route_starts_behind\",\"at\":[%.1f,%.1f]}",(float)g_px,(float)g_py);
        }
    }
    if(kc::g_debug) log_route_detail(n0,useA,ta,tb);
}
// ---- where on the route is the rider? ----
// Everything is judged in a window around the rider's progress, by DISTANCE along the route: from
// PROG_BACK metres behind to PROG_AHEAD metres ahead, plus as far as a horse can have ridden since the
// progress was last updated.  A part of the route outside the window (a loop or switchback passing
// close by, the same road ridden again later) never counts as "here".
// The window is made of route SEGMENTS: every segment that overlaps [c0-back, c0+fwd] (c0 = progress)
// counts, also one that only partly lies inside -- so a long segment (road points can be ~58 m apart)
// holding the rider, e.g. the last segment when the progress is its end node, is never left out.
// Returned as node indices [lo,hi): segments lo .. hi-2 and their end nodes.
static void route_window(float back,float fwd,int& lo,int& hi){
    int n=(int)g_route.size(); if(n==0){ lo=hi=0; return; }
    int p=std::min(std::max(g_prog,0),n-1); float c0=g_cum[p];
    lo=p; while(lo>0 && g_cum[lo]>c0-back) lo--;              // segment lo-1..lo ends inside the window
    hi=p+1; while(hi<n && g_cum[hi-1]<c0+fwd) hi++;           // segment hi-1..hi starts inside the window
}
static float travel_allowance(){ uint64_t now=GetTickCount64(); return g_progT? std::min(3000.f,(now-g_progT)/1000.f*PROG_MAX_SPEED) : 0.f; }
// Route index nearest (x,y) within the corridor, in the window.
static int route_index_near(float x,float y){
    int lo,hi; route_window(PROG_BACK,PROG_AHEAD+travel_allowance(),lo,hi); int best=-1; float bd=CORRIDOR*CORRIDOR;
    for(int k=lo;k<hi;k++){ int n=g_route[k]; float dx=g_cur->x[n]-x, dy=g_cur->y[n]-y, d=dx*dx+dy*dy; if(d<bd){ bd=d; best=k; } }
    return best;
}
// Route index of graph node n (first match at or after `from`, within the window), or -1.
static int route_index_of(int n,int from){
    int lo,hi; route_window(PROG_BACK,PROG_AHEAD+travel_allowance(),lo,hi); lo=std::max(lo,from);
    for(int k=lo;k<hi;k++) if(g_route[k]==n) return k;
    return -1;
}
// Distance from (x,y) to the route segments in the window; kOut/tOut: closest segment and position on it.
static float dist_to_route_win(float x,float y,float back,float fwd,int* kOut=nullptr,float* tOut=nullptr){
    int lo,hi; route_window(back,fwd,lo,hi); float bd=1e30f; int bk=-1; float bt=0;
    for(int k=lo;k+1<hi;k++){
        int u=g_route[k], v=g_route[k+1]; float ax=g_cur->x[u], ay=g_cur->y[u], ex=g_cur->x[v]-ax, ey=g_cur->y[v]-ay, l2=ex*ex+ey*ey;
        float t= l2>1e-6f ? ((x-ax)*ex+(y-ay)*ey)/l2 : 0.f; t=std::min(1.f,std::max(0.f,t));
        float dx=ax+t*ex-x, dy=ay+t*ey-y, d=dx*dx+dy*dy; if(d<bd){ bd=d; bk=k; bt=t; }
    }
    if(kOut) *kOut=bk;
    if(tOut) *tOut=bt;
    return bk<0? 1e30f : sqrtf(bd);
}
static float dist_to_route(float x,float y){ return dist_to_route_win(x,y,PROG_BACK,PROG_AHEAD+travel_allowance()); }
// Is road segment u-v part of the route in the window?  kOut: its route index (nearest the progress).
static bool route_edge_win(int u,int v,int* kOut){
    int lo,hi; route_window(PROG_BACK,PROG_AHEAD+travel_allowance(),lo,hi);
    int c0=std::min(std::max(g_prog,0),(int)g_route.size()-1), best=-1; float bs=1e30f;
    for(int k=lo;k+1<hi;k++){
        int a=g_route[k], b=g_route[k+1];
        if((a==u&&b==v)||(a==v&&b==u)){
            // a road ridden both ways (a turn-back): the leg pointing the way the rider moves
            float sc=fabsf(g_cum[k]-g_cum[c0]);
            if(g_headOk){ float dx=g_cur->x[b]-g_cur->x[a], dy=g_cur->y[b]-g_cur->y[a]; if(dx*g_mhx+dy*g_mhy<0) sc+=1e4f; }
            if(sc<bs){ bs=sc; best=k; }
        }
    }
    if(kOut) *kOut=best;
    return best>=0;
}
// Progress follows the rider ALONG the route: to the route segment of the road the rider is on (map
// matching), else to the nearest route point within the corridor -- only ever inside the window, so it
// cannot jump to a distant part of the route that happens to lie close by.
static void update_progress(){
    if(!g_posOk || g_route.size()<2) return;
    int eu,ev,k=-1; float de;
    if(g_cur->nearest_edge(g_px,g_py,MATCH_TOL,eu,ev,de) && route_edge_win(eu,ev,&k)){
        float d0=hypotf(g_cur->x[g_route[k]]-g_px,g_cur->y[g_route[k]]-g_py), d1=hypotf(g_cur->x[g_route[k+1]]-g_px,g_cur->y[g_route[k+1]]-g_py);
        g_prog= d1<d0? k+1 : k; g_progT=GetTickCount64(); return;
    }
    float t; float d=dist_to_route_win(g_px,g_py,PROG_BACK,PROG_AHEAD+travel_allowance(),&k,&t);
    if(k>=0 && d<=CORRIDOR){ g_prog= t>0.5f? k+1 : k; g_progT=GetTickCount64(); }
}
// Is the rider on the route?  RF_ON: on a route road in the window, or within the horse's tracking error
// of it.  RF_BRANCH: on a different road that is clearly closer than the route (a parallel street, the
// other arm of a fork, a later part of a loop).  RF_FAR: off any road and far from the route.
enum { RF_ON=0, RF_BRANCH=1, RF_FAR=2 };
static int route_fix(float x,float y,float* drOut){
    float dr=dist_to_route(x,y);
    if(drOut) *drOut=dr;
    int eu,ev; float de; bool onEdge=g_cur->nearest_edge(x,y,MATCH_TOL,eu,ev,de);
    if(onEdge && route_edge_win(eu,ev,nullptr)) return RF_ON;
    if(dr<=BRANCH_MIN) return RF_ON;
    if(onEdge && de+BRANCH_MARGIN<dr) return RF_BRANCH;
    if(dr>DEVIATE) return RF_FAR;
    return RF_ON;                                 // near the route, no other road closer: beside the road, still on it
}
// Does direction (dx,dy) point away from the route ahead of the rider (the point ~20 m further along)?
static bool against_route(float dx,float dy){
    if(g_route.size()<2 || !g_posOk) return false;
    int k=g_prog; while(k+1<(int)g_route.size() && g_cum[k]-g_cum[g_prog]<20.f) k++;
    float tx=g_cur->x[g_route[k]]-g_px, ty=g_cur->y[g_route[k]]-g_py, l=hypotf(tx,ty);
    return l>3.f && (tx*dx+ty*dy)/l < -0.3f;
}
// Left the route = for DEVIATE_MS either on a different road (clearly closer to it than to the route,
// and ridden BRANCH_MOVE metres along it -- not just cutting a junction) or off the roads and more than
// DEVIATE metres from the route.  Returns RF_BRANCH / RF_FAR when that happened (once), else RF_ON.
static int offroute_step(uint64_t now,float x,float y,float* drOut){
    static uint64_t offT=0; static float offX=0,offY=0; static int offGen=-1;
    if(offGen!=g_planGen){ offGen=g_planGen; offT=0; }
    float dr; int fix=route_fix(x,y,&dr);
    if(drOut) *drOut=dr;
    if(fix==RF_ON){ offT=0; return RF_ON; }
    if(!offT){ offT=now; offX=x; offY=y; return RF_ON; }
    if(now-offT>DEVIATE_MS && (fix==RF_FAR || hypotf(x-offX,y-offY)>=BRANCH_MOVE)){ offT=0; return fix; }
    return RF_ON;
}
// Distance to the WHOLE route (any part of it): used where "anywhere on the route" is meant (no level
// switch while on the route; where on the route a stop happened).  kOut: the closest route index.
static float dist_to_route_full(float x,float y,int* kOut){
    float bd=1e30f; int bk=-1;
    for(int k=0;k+1<(int)g_route.size();k++){
        int u=g_route[k], v=g_route[k+1]; float ax=g_cur->x[u], ay=g_cur->y[u], ex=g_cur->x[v]-ax, ey=g_cur->y[v]-ay, l2=ex*ex+ey*ey;
        float t= l2>1e-6f ? ((x-ax)*ex+(y-ay)*ey)/l2 : 0.f; t=std::min(1.f,std::max(0.f,t));
        float dx=ax+t*ex-x, dy=ay+t*ey-y, d=dx*dx+dy*dy; if(d<bd){ bd=d; bk=k; }
    }
    if(kOut) *kOut=bk;
    return bk<0? 1e30f : sqrtf(bd);
}
// Avoid `len` metres of the current route from index k0 (the way the rider could not take).
static void avoid_from(int k0,float len,const char* why){
    AvoidStretch a; a.t=GetTickCount64(); a.why=why;
    for(int k=k0;k<(int)g_route.size() && (k==k0 || g_cum[k-1]-g_cum[k0]<len);k++){ a.nodes.push_back(g_route[k]); a.along.push_back(g_cum[k]-g_cum[k0]); }
    if(a.nodes.size()<2) return;
    if(g_avoids.size()>=AVOID_MAX) g_avoids.erase(g_avoids.begin());
    int f=a.nodes.front(), l=a.nodes.back(); float m=a.along.back();
    g_avoids.push_back(std::move(a)); apply_avoids();
    logf("{\"ev\":\"avoid\",\"why\":\"%s\",\"from\":[%.1f,%.1f],\"to\":[%.1f,%.1f],\"m\":%.0f,\"extra_cost\":%.0f,\"active\":%zu}",
         why,g_cur->x[f],g_cur->y[f],g_cur->x[l],g_cur->y[l],m,AVOID_COST,g_avoids.size());
}
// The rider just left the route (genuinely: off every part of it).  Was the way ahead not passable?
//  - stopped_then_left: they stood still ON the route (not at its start or at the destination) shortly
//    before, and left it near that stop without having ridden on past it (a locked gate, a blocked road);
//  - not_offered_then_left: the game kept not offering the route's road at a junction just before
//    (that turn is not auto-followable), and they left the route there.
// Otherwise (a plain wrong turn at riding speed) nothing is avoided.
static void avoid_on_leave(uint64_t now){
    if(g_route.size()<2) return;
    float total=g_cum.back();
    if(g_stillT && now-g_stillT<=STILL_RECENT){
        int ks=-1; float ds=dist_to_route_full(g_stillX,g_stillY,&ks);
        if(ks>=0 && ds<=CORRIDOR && g_cum[ks]>=30.f && total-g_cum[ks]>=30.f && g_cum[g_prog]<=g_cum[ks]+20.f && g_cum[g_prog]>=g_cum[ks]-60.f){
            g_stillT=0; avoid_from(ks,AVOID_LEN,"stopped_then_left"); return;
        }
    }
    if(g_noK>=0 && g_noGen==g_planGen && now-g_noT<=NOT_OFFERED_RECENT && g_noK+1<(int)g_route.size()
       && g_cum[g_prog]<=g_cum[g_noK]+40.f && g_cum[g_prog]>=g_cum[g_noK]-60.f){
        int k=g_noK; g_noK=-1; avoid_from(k,AVOID_TURN_LEN,"not_offered_then_left");
    }
}
// Drop avoided stretches that no longer apply (game thread, every road query).
static void expire_avoids(uint64_t now){
    if(g_avoids.empty() || !g_cur || !g_posOk) return;
    bool changed=false;
    for(size_t i=0;i<g_avoids.size();){
        const AvoidStretch& a=g_avoids[i]; const char* why=nullptr; float dmin=1e30f;
        for(size_t k=0;k<a.nodes.size();k++){
            float dx=g_cur->x[a.nodes[k]]-g_px, dy=g_cur->y[a.nodes[k]]-g_py, d=sqrtf(dx*dx+dy*dy); dmin=std::min(dmin,d);
            if(d<=CORRIDOR && a.along[k]>=25.f) why="ridden";          // they ride it after all: it is passable
        }
        if(!why && dmin>AVOID_FAR) why="far";
        if(!why && now-a.t>AVOID_TTL) why="expired";
        if(why){ logf("{\"ev\":\"avoid_cleared\",\"why\":\"%s\",\"was\":\"%s\"}",why,a.why); g_avoids.erase(g_avoids.begin()+i); changed=true; }
        else i++;
    }
    if(changed) apply_avoids();
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
// Game thread: sample the mounted state (the game's own IsMounted on the client actor).
static void sample_mount(){
    int m=-1;
    if(g_isMounted && g_fwGlobal){
        __try {
            void* fw=*g_fwGlobal;
            if(fw && kc::is_a((uintptr_t)fw,g_vtCryAction)){
                void* a=((GetActor_t)(*(void***)fw)[64])(fw);          // CCryAction::GetClientActor
                g_actor=(uintptr_t)a; m= a? (g_isMounted(a)?1:0) : 0;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER){ m=-1; g_actor=0; }
    }
    g_mount=m; g_mountT=GetTickCount64();
}
// Any thread: the mounted state.  A fresh game-thread sample; else (no recent sample, e.g. the
// script-system hook is not installed) the same check on the cached actor, guarded; else unknown.
static int mount_now(){
    uint64_t now=GetTickCount64(), t=g_mountT;
    if(t && now-t<=MOUNT_FRESH_MS) return g_mount;
    if(!t || now-t>MOUNT_STALE_MS || !g_isMounted) return -1;
    uintptr_t a=g_actor; if(!a) return g_mount==0? 0 : -1;
    __try { return g_isMounted((void*)a)? 1 : 0; } __except(EXCEPTION_EXECUTE_HANDLER){ return -1; }
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
static void orient_step(uint64_t now);
// Replacement for the angle filter at both chooser call sites (game thread).
static bool FilterHook(float* dir, float* cur){
    bool vanilla=g_filter(dir,cur);
    uint64_t now=GetTickCount64(); g_hookT=now;
    if(kc::g_debug){                              // diagnostics: is the chooser asking while the horse starts off?
        static uint64_t dt=0;
        if(now-dt>500){ dt=now; KC_DLOG("{\"ev\":\"ask\",\"speed\":%.1f,\"since_move\":%lld,\"face\":[%.2f,%.2f],\"route\":%d,\"prog\":%d,\"turn\":%d}",
            (float)g_speed,(long long)(g_headOk? (long long)(now-g_moveT) : -1),(float)g_fx,(float)g_fy,(int)g_route.size(),g_prog,(int)g_wrongWay); }
    }
    refresh_entity(false); sample_mount();
    if(!g_ssUpdate){ static uint64_t ot=0; if(now-ot>=100){ ot=now; orient_step(now); } }   // no script-system hook: run the orientation stage from here
    find_map();
    float a[3],b[3];
    if(!rdn(*g_p0,a,12) || !rdn(*g_p1,b,12)) return vanilla;
    // heading: the rider's movement; standing still, the way the horse faces
    if(g_posOk && g_headOk){ g_hx=g_mhx; g_hy=g_mhy; } else if(g_faceOk){ g_hx=g_fx; g_hy=g_fy; } else { g_hx=0; g_hy=0; }
    // Which level are we on?  The game's road points are nodes of exactly one of our graphs.  Level
    // coordinates overlap, so a guess made from the map (CpHook) can be wrong; the first few road
    // points that match another level's graph but not the current one switch to it.
    // A level change (the region changes) re-plans, so it needs proof: three different road points that
    // are exactly points of another level's graph, none of this level's, while the rider is not on the
    // current route.
    static Graph* other=nullptr; static int otherHits=0; static float hx_=1e9f, hy_=1e9f;
    if(!g_cur || g_cur->nearest(a[0],a[1],1.0f)<0){
        Graph* hit=nullptr;
        for(int i=0;i<g_ng;i++) if(&g_graphs[i]!=g_cur && g_graphs[i].nearest(a[0],a[1],LEVEL_TOL)>=0){ hit=&g_graphs[i]; break; }
        if(hit && !(hit==other && fabsf(a[0]-hx_)<0.5f && fabsf(a[1]-hy_)<0.5f)){ otherHits = hit==other? otherHits+1 : 1; other=hit; hx_=a[0]; hy_=a[1]; }
        bool onRoute = g_cur && !g_route.empty() && g_posOk && dist_to_route_full(g_px,g_py,nullptr)<=DEVIATE;
        if(hit && (!g_cur || (otherHits>=3 && !onRoute))){
            if(g_cur) logf("{\"ev\":\"level_switch\",\"from\":\"%s\",\"to\":\"%s\"}",g_cur->name,hit->name);
            set_level(hit); otherHits=0;
        }
        if(!g_cur) return vanilla;
    } else otherHits=0;
    if(!marker_destination(now)) return vanilla;  // no destination: vanilla auto-follow
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
    int nr = g_posOk ? g_cur->nearest_main(g_px,g_py,50.f) : -1;
    expire_avoids(now);
    if(g_route.empty()){
        int ns = nr>=0? nr : (g_cur->on_main(n0)? n0 : -1);
        if(ns>=0 && now-g_lastPlan>1500) plan_from(ns, !moving || (g_speed<6.f && now-g_moveT<3000));
        if(g_route.empty()) return vanilla;
    } else if(g_posOk && !(g_wrongWay && dist_to_route(g_px,g_py)<WRONG_WAY_M)){   // (not while waiting for a turn-around)
        update_progress();
        float dr; int left=offroute_step(now,g_px,g_py,&dr);
        if(left && nr>=0){
            logf("{\"ev\":\"left_route\",\"how\":\"%s\",\"at\":[%.1f,%.1f],\"dist\":%.1f}",left==RF_FAR?"far":"other_road",(float)g_px,(float)g_py,dr);
            avoid_on_leave(now);
            int ns=nr;
            if(g_cur->avoided(nr)){ int f=g_cur->nearest_free(g_px,g_py,50.f); if(f>=0) ns=f; }   // start beside an avoided stretch, not on it
            plan_from(ns,!moving,"left_route"); if(g_route.empty()) return vanilla;
        }
    }
    update_progress();
    int ia=route_index_near(a[0],a[1]);
    if(ia<0) return vanilla;                      // a road not on the route (e.g. behind a junction): not ours to judge
    if(!g_posOk && g_cum.back()-g_cum[ia] <= g_arriveDist){     // fallback without a rider position
        if(!g_arrived && mount_now()==1){ g_arrived=true; logf("{\"ev\":\"arrived\",\"left\":%.1f}",g_cum.back()-g_cum[ia]); }   // speed unknown: no brake
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
        if(moving){ g_noK=ia; g_noGen=g_planGen; g_noT=now; }   // the route's road was not offered here (see avoid_on_leave)
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
// Is there a route line to show?  Not without a marker on the map (also while a missing marker is still
// kept, see MARKER_GONE_MS), after arriving, or before a route is planned.
// The route line for the map: only road points, from the route point nearest the rider (no straight line
// from Henry across country) to the destination itself (the point of road closest to the marker), not past it.
static bool route_shown(){ return g_haveMarker && !g_goneT && !g_arrived && !g_route.empty() && g_cur; }
static void build_line(std::vector<Vec3f>& pts){
    pts.clear();
    if(route_shown() && g_route.size()>=2){
        size_t last=g_route.size()-1;                                       // far end of the destination's road segment
        std::vector<Vec3f> full;
        for(size_t k=(size_t)std::max(0,g_prog);k<last;k++) full.push_back({g_cur->x[g_route[k]],g_cur->y[g_route[k]],0});
        if(g_endOk) full.push_back({(float)g_endX,(float)g_endY,0});
        else full.push_back({g_cur->x[g_route[last]],g_cur->y[g_route[last]],0});
        // Fewer points, but never cutting a bend: every dropped road point stays within 0.5 m of the
        // drawn line (Douglas-Peucker), and drawn points are at most ~8 m apart.
        std::vector<char> keep(full.size(),0);
        if(!full.empty()){ keep.front()=keep.back()=1; }
        std::vector<std::pair<size_t,size_t>> st; if(full.size()>2) st.push_back({0,full.size()-1});
        while(!st.empty()){
            auto [i0,i1]=st.back(); st.pop_back();
            float ax=full[i0].x, ay=full[i0].y, ex=full[i1].x-ax, ey=full[i1].y-ay, l=hypotf(ex,ey);
            size_t worst=0; float wd=0.5f;
            for(size_t k=i0+1;k<i1;k++){
                float d= l>1e-3f ? fabsf((full[k].x-ax)*ey-(full[k].y-ay)*ex)/l : hypotf(full[k].x-ax,full[k].y-ay);
                if(d>wd){ wd=d; worst=k; }
            }
            if(worst){ keep[worst]=1; st.push_back({i0,worst}); st.push_back({worst,i1}); }
        }
        float lx=1e9f,ly=1e9f;
        for(size_t k=0;k<full.size();k++){
            if(!keep[k] && hypotf(full[k].x-lx,full[k].y-ly)<8.f) continue;
            pts.push_back(full[k]); lx=full[k].x; ly=full[k].y;
        }
    }
}
static void draw_route(void* map,bool havePos,float px,float py){
    if(!g_showPath || !g_mapLine || !kc::is_a((uintptr_t)map,g_vtUIMap)) return;
    (void)havePos; (void)px; (void)py;
    std::vector<Vec3f> pts; build_line(pts);
    hide_route(map);                                                        // replace any line already shown
    if(pts.size()<2) return;
    VecView v{pts.data(),pts.data()+pts.size(),pts.data()+pts.size()};
    g_showPath(map,&v);
}
// The map's marker refresh (map screen, game thread).  `shown`: the marker the map shows (rec 0: none,
// the map hides the flag), or null if that could not be read.
static void map_refresh(void* map,const Mk* shown,uint64_t now){
    refresh_entity(true); sample_mount();
    float mx=0,my=0,px=0,py=0; bool havePos=player_pos(px,py), haveMk=read_marker(mx,my,shown);
    if(sync_destination(haveMk,mx,my,now) && haveMk && g_route.empty() && havePos && now-g_lastPlan>1500){   // plan from where the rider is
        Graph* gr=graph_at(px,py);
        if(gr){ set_level(gr); int n0=gr->nearest_main(px,py,MARKER_SNAP); if(n0>=0){ bool mv=g_headOk; g_hx=mv?g_mhx:(g_faceOk?g_fx:0); g_hy=mv?g_mhy:(g_faceOk?g_fy:0); plan_from(n0,!mv); } }
    }
    draw_route(map,havePos,px,py);
}
// Called in place of the map's checkpoint getter (map screen, game thread).  The getter's result is the
// marker the map shows, read as the game does right after (position at +0x50); null: the map has none.
static void* CpHook(void* self,void* out){
    if(*g_map && *g_map!=(uintptr_t)self) logf("{\"ev\":\"map_moved\"}");   // the early lookup found another object
    *g_map=(uintptr_t)self;
    void* r=g_cpget(self,out);
    __try {
        Mk s{0,0,0}; bool known=rd((uintptr_t)out,&s.rec) && (!s.rec || (rd(s.rec+0x50,&s.x) && rd(s.rec+0x54,&s.y) && sane(s.x,s.y)));
        map_refresh(self,known? &s : nullptr,GetTickCount64());
    } __except(EXCEPTION_EXECUTE_HANDLER){ logf("{\"ev\":\"map_line_error\",\"msg\":\"map line disabled for this session\"}"); g_showPath=nullptr; }
    return r;
}
// Called in place of the game's own HideFastTravelPath event (game thread), sent when it is done with the
// path of a hovered fast-travel point, which it shows on the same line: put the route back.
static void HideHook(void* elem,void* name,const bool* animate){
    g_sendEvent(elem,name,animate);
    static bool busy=false;                       // in case drawing the route ends up here itself
    if(busy || !g_mapLine || !route_shown()) return;   // nothing of ours to show: the game's hide stands
    busy=true;
    __try { refresh_entity(true); float px=0,py=0; bool havePos=player_pos(px,py); draw_route((char*)elem-0x208,havePos,px,py); }
    __except(EXCEPTION_EXECUTE_HANDLER){ logf("{\"ev\":\"map_line_error\",\"msg\":\"map line disabled for this session\"}"); g_showPath=nullptr; }
    busy=false;
}

// ---------------- rider tracking + arrival (20 ms, off the game thread; reads memory only) ----------------
// Arrival: decided ONLY while the game says Henry is mounted (mounted==1).  On foot (or with the mounted
// state unknown) nothing about the ride is decided -- not arrival, not the closest approach to the
// marker, not the brake.  Riding with a destination: auto-follow asked us about roads recently (it does
// not ask on long stretches without junctions, so allow a generous window) and the horse is moving.
static bool arrival_step(uint64_t now,float x,float y,int mounted){
    if(!(g_endOk && !g_arrived && !marker_gone(now) && mounted==1 && now-g_hookT<FOLLOW_MS && g_speed>1.5f)) return false;
    // Arrived: at the point of road closest to the marker.  Fallback: came within reach of the marker
    // and are now moving away again (took another road past it).
    float lead=g_arriveDist + g_speed*BRAKE_LEAD_S;
    float d=hypotf(x-g_endX,y-g_endY), dm=hypotf(x-g_mx,y-g_my);
    if(dm<g_dmMin) g_dmMin=dm;
    bool passed = g_dmMin <= g_endMk + lead + 8.f && dm > g_dmMin + 4.f;
    if(!(d <= lead || passed)) return false;
    g_arrived=true;
    logf("{\"ev\":\"arrived\",\"marker_dist\":%.1f,\"end_dist\":%.1f,\"speed\":%.1f,\"why\":\"%s\"}",dm,d,(float)g_speed, d<=lead?"road_point":"passed");
    if(g_brakeOn && g_brakeEvt) SetEvent(g_brakeEvt);   // the brake thread checks the mounted state again
    return true;
}
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
    {   // stops: standing for STILL_MS (see avoid_blocked_stretch)
        static uint64_t slowSince=0;
        if(g_speed<1.0f){ if(!slowSince) slowSince=now; else if(now-slowSince>=STILL_MS){ g_stillX=x; g_stillY=y; g_stillT=now; } }
        else slowSince=0;
    }
    {   // facing: the entity's forward (Y) axis, world matrix at +0x58 (row-major 3x4: m01 @+0x5C, m11 @+0x6C)
        float fx=0,fy=0; uintptr_t e=g_ent;
        if(e && rd(e+0x5C,&fx) && rd(e+0x6C,&fy) && std::isfinite(fx) && std::isfinite(fy)){
            float l=hypotf(fx,fy); if(l>0.3f && l<1.5f){ g_fx=fx/l; g_fy=fy/l; g_faceOk=true; } else g_faceOk=false;
        } else g_faceOk=false;
    }
    arrival_step(now,x,y,mount_now());
}
static void track_position_safe(){ __try { track_position(GetTickCount64()); } __except(EXCEPTION_EXECUTE_HANDLER){ g_posOk=false; } }

// Hold S (scan code, as DirectInput reads it) for up to `ms`, off the game thread -- only while the game
// says Henry is mounted and the game has focus.  Released at once if he starts to dismount (mounted
// state no longer 1, or unknown) or another window takes focus.  0 done, 1 cancelled (dismount/unknown),
// 2 cancelled (focus), 3 skipped (no focus), 4 skipped (not mounted / unknown).
static int brake_hold(int (*mounted)(),bool (*focused)(),void (*key)(bool),DWORD ms){
    if(!focused()) return 3;
    if(mounted()!=1) return 4;
    key(true); int r=0;
    for(DWORD t=0;t<ms;t+=20){ Sleep(20); if(mounted()!=1){ r=1; break; } if(!focused()){ r=2; break; } }
    key(false); return r;
}
static bool game_focused(){ DWORD pid=0; GetWindowThreadProcessId(GetForegroundWindow(),&pid); return pid==GetCurrentProcessId(); }
static void key_s(bool down){ INPUT in={}; in.type=INPUT_KEYBOARD; in.ki.wScan=0x1F; in.ki.dwFlags=KEYEVENTF_SCANCODE|(down?0:KEYEVENTF_KEYUP); SendInput(1,&in,sizeof(in)); }
static DWORD WINAPI BrakeThread(LPVOID){
    static const char* what[]={"braked","brake_cancelled_dismount","brake_cancelled_focus","brake_skipped_focus","brake_skipped_not_mounted"};
    for(;;){
        WaitForSingleObject(g_brakeEvt,INFINITE);
        float v0=g_speed; int r=brake_hold(mount_now,game_focused,key_s,BRAKE_MS);
        logf("{\"ev\":\"%s\",\"speed_before\":%.1f,\"speed_after\":%.1f}",what[r],v0,(float)g_speed);
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
struct Sites { uintptr_t fork, snap, cp, filter, cpget, fwglob, vtca, vtent, showpath, vtmap, hname, sctor, sdtor, send, hide; };
#ifdef KC_DEVTOOLS
// Live reload (development builds only): the stub page records the game's original call targets,
// so a newer copy injected into a running game can take the call sites over from the old one.
static const uint64_t PAGE_MAGIC=0x4547415054414B43ull;   // "CKATPAGE"
struct PageInfo { uint64_t magic; uintptr_t filter, cpget, ssUpdate, send; };
static PageInfo g_prev{};                         // the copy this one takes over from
static const size_t PAGE_INFO=0xF80;
static uintptr_t taken_over(uintptr_t site,uintptr_t PageInfo::*target){
    if(!site || *(uint8_t*)site!=0xE8) return 0;
    uintptr_t t=kc::rel32(site+1), pg=t&~(uintptr_t)0xFFF;
    PageInfo pi{}; if(!rdn(pg+PAGE_INFO,&pi,sizeof pi) || pi.magic!=PAGE_MAGIC) return 0;
    g_prev=pi;
    return pi.*target;
}
#endif
// Resolves every game address; false (with the reasons logged) if this game build is not supported.
static bool resolve(kc::Resolver& R, Sites& s){
    s.fork=R.find("fork chooser call",SIG_FORK,0x9); s.snap=R.find("snap chooser call",SIG_SNAP,0x20); s.cp=R.find("map marker call",SIG_CP,0x8);
    uintptr_t f2=0;
#ifdef KC_DEVTOOLS
    if(uintptr_t o=taken_over(s.fork,&PageInfo::filter)){ s.filter=o; f2=taken_over(s.snap,&PageInfo::filter); s.cpget=taken_over(s.cp,&PageInfo::cpget); logf("{\"ev\":\"live_reload\"}"); }
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
    if(h){ s.hname=R.riprel("hide event name",h+3); s.sctor=R.branch("string ctor",h+21); s.sdtor=R.branch("string dtor",h+53); s.hide=h+43;
#ifdef KC_DEVTOOLS
           s.send=taken_over(s.hide,&PageInfo::send);   // live reload: the call goes to the old copy's HideHook
           if(!s.send)
#endif
           s.send=R.branch("send UI event",s.hide);
           if(s.hname && strcmp((const char*)s.hname,"HideFastTravelPath")) R.fail("hide event name","unexpected string"); }
    return R.ok();
}
#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out){   // tools/selftest
    kc::Resolver R; R.im.load(game); Sites s{}; bool ok=resolve(R,s);
    out[0]=s.fork; out[1]=s.snap; out[2]=s.cp; out[3]=s.filter; out[4]=s.cpget; out[5]=s.fwglob; out[6]=s.vtca; out[7]=s.vtent; out[8]=s.showpath; out[9]=s.vtmap;
    // optional parts must resolve too on the supported build: mounted check [10] IsMounted, [11] GetClientActor
    kc::Resolver M; M.im=R.im; out[10]=M.find("IsMounted",SIG_ISMOUNTED);
    uintptr_t vt=M.vtable("CCryAction",".?AVCCryAction@@",0); out[11]=M.slot("CCryAction::GetClientActor",vt,64,SIG_GETCLIENTACTOR);
    return (ok && M.ok())? 12 : -(R.fails+M.fails);
}
// tools/routetest: graph dir, level, start, heading, marker -> route length; returns 1 if it starts backwards
extern "C" __declspec(dllexport) int KC_RouteTest(const char* dir,const char* lvl,float x,float y,float hx,float hy,float mx,float my,float uturn,float* len){
    strcpy_s(kc::g_dir,dir); Graph g; if(!g.load(lvl)) return -1;
    int s=g.nearest(x,y,50.f), t=g.nearest(mx,my,MARKER_SNAP); std::vector<int> p;
    float l=hypotf(hx,hy); hx/=l; hy/=l;
    if(s<0||t<0||!g.route(s,t,p,hx,hy,uturn,uturn)) return -2;
    *len=0; for(size_t i=1;i<p.size();i++) *len+=hypotf(g.x[p[i]]-g.x[p[i-1]],g.y[p[i]]-g.y[p[i-1]]);
    int b=g.behind_edge(s,hx,hy); return p.size()>1 && b>=0 && (int)g.to[b]==p[1];
}
// dev only: run plan_from with DebugLog on and write what it logs to <dir>\planlog.txt (returns the route's node count)
extern "C" __declspec(dllexport) int KC_PlanLogTest(const char* dir,const char* lvl,float x,float y,float hx,float hy,float mx,float my){
    strcpy_s(kc::g_dir,dir); static Graph g; if(!g.load(lvl)) return -1;
    char p[MAX_PATH]; sprintf_s(p,"%s\\planlog.txt",dir); if(fopen_s(&kc::g_log,p,"a")) return -3;
    kc::g_debug=true; g_cur=&g; g_px=x; g_py=y; g_posOk=true;
    float l=hypotf(hx,hy); if(l>0){ g_hx=hx/l; g_hy=hy/l; } else g_hx=g_hy=0;
    set_marker(mx,my); int n0=g.nearest(x,y,50.f); if(n0<0) return -2;
    plan_from(n0,l==0); fclose(kc::g_log); kc::g_log=nullptr; return (int)g_route.size();
}
// dev only: plan s->marker, stop at (stopx,stopy) on the route, ride away to (lx,ly), recalculate as left_route
// does.  Logs to <dir>\avoidlog.txt (DebugLog on); returns how many avoided edges the new route still uses.
extern "C" __declspec(dllexport) int KC_AvoidTest(const char* dir,const char* lvl,float sx,float sy,float mx,float my,
                                                  float stopx,float stopy,float lx,float ly,float* lenBefore,float* lenAfter){
    strcpy_s(kc::g_dir,dir); static Graph g; if(!g.load(lvl)) return -1;
    char p[MAX_PATH]; sprintf_s(p,"%s\\avoidlog.txt",dir); if(fopen_s(&kc::g_log,p,"a")) return -3;
    kc::g_debug=true; g_cur=&g; g.clear_avoid(); g_posOk=true; g_px=sx; g_py=sy; g_hx=g_hy=0;
    set_marker(mx,my); int n0=g.nearest(sx,sy,50.f); if(n0<0) return -2;
    plan_from(n0,true); *lenBefore=g_route.empty()?-1.f:g_cum.back();
    int ks=0; float bd=1e30f;
    for(size_t k=0;k<g_route.size();k++){ float dx=g.x[g_route[k]]-stopx, dy=g.y[g_route[k]]-stopy, d=dx*dx+dy*dy; if(d<bd){ bd=d; ks=(int)k; } }
    g_prog=ks; g_stillX=stopx; g_stillY=stopy; g_stillT=GetTickCount64();
    g_px=lx; g_py=ly; float hx=lx-stopx, hy=ly-stopy, hl=hypotf(hx,hy); g_hx=hl>0?hx/hl:0; g_hy=hl>0?hy/hl:0;
    g_avoids.clear(); avoid_on_leave(GetTickCount64());
    int nr=g.nearest_main(lx,ly,50.f), ns=nr; if(nr>=0 && g.avoided(nr)){ int f=g.nearest_free(lx,ly,50.f); if(f>=0) ns=f; }
    plan_from(ns,false,"left_route"); *lenAfter=g_route.empty()?-1.f:g_cum.back();
    int used=0; for(size_t i=0;i+1<g_route.size();i++){ int e=g.edge(g_route[i],g_route[i+1]); if(e>=0 && g.extra[e]>0) used++; }
    fclose(kc::g_log); kc::g_log=nullptr; return used;
}
#endif

static void run_notice(void* ss){
    const char* line=(const char*)InterlockedExchangePointer((void* volatile*)&g_notice,nullptr);
    if(line) g_ssExec(ss,line,strlen(line),"HorseRouteFollow",nullptr);
}
// The route turns straight back just ahead (a dead end, a turn-back the planner could not avoid):
// auto-follow cannot do that, so the rider is told (wrong-way notice) and the route waits for them.
static bool check_turn_back(uint64_t now){
    if(g_wrongWay || g_route.size()<3 || !g_posOk) return false;
    for(int k=std::max(g_prog,1);k<(int)g_route.size()-1 && k<=g_prog+3;k++){
        if(!g_rev[k]) continue;
        float d=hypotf(g_cur->x[g_route[k]]-g_px,g_cur->y[g_route[k]]-g_py);
        if(d<=CORRIDOR+2.f){
            g_wrongWay=true; g_wrongT=now; g_prog=k+1 < (int)g_route.size()? k : g_prog;   // turning round here continues the route
            logf("{\"ev\":\"wrong_way\",\"why\":\"route_turns_back\",\"at\":[%.1f,%.1f]}",(float)g_px,(float)g_py);
            return true;
        }
    }
    return false;
}
// Orientation stage (game thread, ~10 times a second from the script-system hook: the road chooser
// is not asked on straight roads, so it cannot drive this).
static void orient_step(uint64_t now){
    if(!g_cur || g_route.empty() || !g_haveMarker || g_arrived || !g_posOk){ g_wrongWay=false; g_showWrong=false; return; }
    update_progress();
    // Orientation stage.  Auto-follow cannot turn a horse around, so when the horse stands (or is just
    // moving off) facing away from the route, the rider is told to turn around and the route is
    // kept.  Turning around simply continues it; riding on the wrong way for WRONG_WAY_M recalculates
    // (quietly).  Routes planned while moving start ahead unless every way ahead is far longer; when one
    // does start behind (or turns back on the way), plan_from / check_turn_back raise the same warning.
    // "Standing" includes the first seconds of moving off.
    check_turn_back(now);
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
    static uint64_t mt=0; if(now-mt>=50){ mt=now; sample_mount(); }
    static uint64_t ot=0;
    if(now-ot>=100){ ot=now; __try { marker_step(now); orient_step(now); } __except(EXCEPTION_EXECUTE_HANDLER){ g_wrongWay=g_showWrong=false; } }
    if(g_showWrong && now-g_sayT>2500){ g_sayT=now; notify(WRONG_WAY_LINE); }
    if(g_notice && g_ssExec){ __try { run_notice(self); } __except(EXCEPTION_EXECUTE_HANDLER){ g_ssExec=nullptr; logf("{\"ev\":\"notice_error\"}"); } }
    return r;
}
// Optional: the mounted check for the arrival brake.  Without it the mod works, but never brakes.
static void install_mount(const kc::Image& im){
    kc::Resolver R; R.im=im;
    uintptr_t f=R.find("IsMounted",SIG_ISMOUNTED);
    uintptr_t vt=R.vtable("CCryAction",".?AVCCryAction@@",0); R.slot("CCryAction::GetClientActor",vt,64,SIG_GETCLIENTACTOR);
    if(!R.ok()){ logf("{\"ev\":\"brake_off\",\"msg\":\"mounted state not available in this game version - the arrival brake is off\"}"); return; }
    g_isMounted=(IsMounted_t)f;
}
// Optional: without it the mod works, just without the wrong-way notice.
static void install_notices(const kc::Image& im){
    kc::Resolver R; R.im=im;
    uintptr_t vt=R.vtable("CScriptSystem",".?AVCScriptSystem@@",0), up=0;
#ifdef KC_DEVTOOLS
    if(g_prev.ssUpdate && vt){ up=g_prev.ssUpdate; R.verify("CScriptSystem::Update",up,SIG_SS_UPDATE); }   // live reload: slot holds the old copy\"s hook
    else
#endif
    {   // another mod may have hooked the slot first: chain to it
        uintptr_t cur= vt? ((uintptr_t*)vt)[1] : 0;
        if(cur && !R.im.in_image(cur)){ if(R.find("CScriptSystem::Update",SIG_SS_UPDATE)) up=cur; }
        else up=R.slot("CScriptSystem::Update",vt,1,SIG_SS_UPDATE);
    }
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

#ifdef KC_DEVTOOLS
#include "autotravel_tests.inc"                   // KC_MarkerTest (tools/markertest)
#endif

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
    logf("{\"ev\":\"start\",\"version\":\"1.1.3\",\"game_ts\":\"0x%08X\"}",R.im.timestamp);
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
    { PageInfo pi{PAGE_MAGIC,s.filter,s.cpget,0,s.send}; memcpy(g_page+PAGE_INFO,&pi,sizeof pi); }
#endif
    g_p0=(volatile uintptr_t*)(g_page+0xF00); g_p1=(volatile uintptr_t*)(g_page+0xF08); g_map=(volatile uintptr_t*)(g_page+0xF10);
    uint8_t* stubA=g_page+g_pos;                  // fork chooser: candidate r14 -> r15
    emit_rip_store(0x4C,0x89,0x35,g_p0); emit_rip_store(0x4C,0x89,0x3D,g_p1); emit_jmp_abs((void*)&FilterHook);
    uint8_t* stubB=g_page+g_pos;                  // snap chooser: candidate rbx -> rsi
    emit_rip_store(0x48,0x89,0x1D,g_p0); emit_rip_store(0x48,0x89,0x35,g_p1); emit_jmp_abs((void*)&FilterHook);
    uint8_t* stubM=g_page+g_pos;                  // map checkpoint getter
    emit_jmp_abs((void*)&CpHook);
    uint8_t* stubH=g_page+g_pos;                  // the game hiding the map line
    emit_jmp_abs((void*)&HideHook);
    g_brakeEvt=CreateEventA(nullptr,FALSE,FALSE,nullptr); CreateThread(nullptr,0,BrakeThread,nullptr,0,nullptr);
    patch_call(s.fork,stubA); patch_call(s.snap,stubB); patch_call(s.cp,stubM); patch_call(s.hide,stubH);
    install_map_lookup(R.im);
    {   // optional: without it, markers are never ignored (only matters in hardcore)
        kc::Resolver H; H.im=R.im;
        uintptr_t gm=H.find("game mode check",SIG_GAMEMODE);
        uintptr_t gl=gm? H.riprel("game mode object",gm+3) : 0;
        if(H.ok()){ g_gmGlobal=(void**)gl; g_hcJump=gm+0x1C; }
        else logf("{\"ev\":\"hardcore_check_off\"}");
    }
    install_notices(R.im);
    install_mount(R.im);
    logf("{\"ev\":\"ready\",\"graphs\":%d}",g_ng);
    for(;;){
        Sleep(20);
        track_position_safe();
    }
}

KC_PLUGIN("Horse Route Follow", KC_AUTHOR, 112)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){
    if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); }
    return TRUE;
}
