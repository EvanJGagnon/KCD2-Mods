// autoforge.cpp -- Theatrical AutoForge for Kingdom Come: Deliverance II (kcd2_autoforge.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Drives the blacksmithing minigame only through the game's own I_BlacksmithingActions
// interface (C_Blacksmithing+0x68): SetForgeZone, bellows, ToAnvil/ToForge, MoveHammer,
// StartStroke/FinishStroke, Flip. No synthetic input.  Ticks inside C_Blacksmithing::Update.
// Use: pick a recipe, wait until Henry is at the forge (or anvil), press F9 (kcd2_autoforge.ini).  F9 again stops.
// Hardening/quench after completion is automatic in the game; we only watch it.
// Game code is found by RTTI + signature at startup (kcd2_common.h); on a mismatch nothing is hooked.
// Log: kcd2_autoforge.log next to the DLL.
#include "kcd2_common.h"
#include <cmath>
#include <algorithm>

static uintptr_t B = 0;
typedef void (*Update_t)(void*, float);
static Update_t pOrigUpdate = nullptr;
using kc::logf;

// Signatures (build 15693 addresses in comments).  Struct offsets in them stay literal.
static const char* SIG_UPDATE   = "48 8B C4 48 89 58 10 55 56 57 41 56 41 57 48 8D 68 A1 48 81 EC B0 00 00 00";  // C_Blacksmithing vtbl[19] 0x857D90
static const char* SIG_A_TOANVIL= "48 83 EC 28 48 8B 01 FF 50 60 B2 06 38 90 88 00 00 00 74 ?? 48 8D 88 90 00 00 00"; // actions[0]
static const char* SIG_A_TOFORGE= "48 83 EC 28 48 8B 01 FF 50 60 B2 05 38 90 88 00 00 00 74 ?? 48 8D 88 90 00 00 00"; // actions[1]
static const char* SIG_A_TOHARD = "48 83 EC 28 48 8B 01 FF 50 60 B2 07 38 90 88 00 00 00 74 ?? 48 8D 88 90 00 00 00"; // actions[2]
static const char* SIG_A_BELLOWS= "40 53 48 83 EC 20 48 8B 01 8A DA FF 50 60 38 98 00 01 00 00 75 ??";             // actions[4] (model +0x100)
static const char* SIG_A_MOVE   = "48 8B 49 48 41 B0 01 E9 ?? ?? ?? ??";                                          // actions[5]
static const char* SIG_A_FLIP   = "40 53 48 83 EC 20 48 8B 41 48 48 8B 08 48 83 C1 68 48 8B 01 FF 50 60 48 8B D8"; // actions[6]
static const char* SIG_A_STROKE = "48 8B 49 48 E9 ?? ?? ?? ??";                                                   // actions[7], [8]
static const char* SIG_A_ZONE   = "48 8B 49 40 E9 ?? ?? ?? ??";                                                   // actions[9]
static const char* SIG_WORKMAP  = "40 53 48 83 EC 20 4C 8B C2 48 8B D9 48 8D 54 24 30 E8 ?? ?? ?? ?? 48 8B CB";   // 0xB6619C (work map value; called with workpiece+0x40)
static uintptr_t pWorkMap = 0;
template<class T> static T& F(uintptr_t p,uintptr_t o){ return *(T*)(p+o); }

// ---------------- layout (build 15693) ----------------
// C_Blacksmithing
static const uintptr_t BS_ACTOR=0x18, BS_ACTIONS=0x68, BS_MODEL=0x98;
// model-state property values (property+8)
static const uintptr_t M_COMP=0x38, M_QUAL=0x60, M_STATE=0x88, M_HARD=0xB0, M_BELLOWS=0x100, M_FIRE=0x128,
    M_ZONE=0x150, M_READY=0x178, M_POS=0x1A0, M_BPM=0x220, M_CHARGE=0x270, M_WP=0x2F8;
// workpiece: def +0x18 (C_BlacksmithWorkpiece), zone ptr vector +0x28/+0x30, zone temp at zone+8
enum { ST_TOFORGE=5, ST_TOANVIL=6, ST_HARDEN=7, ST_FORGE=8, ST_ANVIL=11, ST_FLIP=12, ST_FLIPBACK=13,
       ST_STROKE=14, ST_HIT=15, ST_FINISHSTROKE=16, ST_RETHAMMER=19, ST_FINIDLE=20 };
// I_BlacksmithingActions slots
enum { A_TOANVIL=0, A_TOFORGE=1, A_TOHARDEN=2, A_BELLOWS=4, A_MOVEHAMMER=5, A_FLIP=6, A_STARTSTROKE=7, A_FINISHSTROKE=8, A_SETZONE=9 };

static uintptr_t g_bs=0, g_m=0;
static void** actvt(){ return *(void***)(g_bs+BS_ACTIONS); }
static void* actthis(){ return (void*)(g_bs+BS_ACTIONS); }
static void act0(int s){ ((void(*)(void*))actvt()[s])(actthis()); }
static void actBellows(bool on){ ((void(*)(void*,bool))actvt()[A_BELLOWS])(actthis(),on); }
static void actSetZone(int z){ int v=z; ((void(*)(void*,int*))actvt()[A_SETZONE])(actthis(),&v); }
static void actMoveHammer(float dx,float dy){ uint64_t p; float v[2]={dx,dy}; memcpy(&p,v,8); ((void(*)(void*,uint64_t))actvt()[A_MOVEHAMMER])(actthis(),p); }

// per-side work map at a hammer position (0x2E58CD4 -> 0xB6619C: map[cell][side], side = model+0x1C8 flag)
typedef float (*WorkAt_t)(void*, const float*);
static float workAt(float x,float y){ uintptr_t w=F<uintptr_t>(g_m,M_WP); if(!w) return 0; float p[2]={x,y}; return ((WorkAt_t)pWorkMap)((void*)(w+0x40),p); }   // = 0x2E58CD4 thunk
// learned map semantics: g_wsign<0 => value falls as work is done (value = remaining)
static float g_wsign=0, g_wsum=0; static int g_wn=0; static float g_rMiss=-1e9f, g_rHitMin=1e9f; static bool g_missKnown=false;
static float remainingAt(float x,float y){ return workAt(x,y); }
// observed on sword/axe/horseshoe/longsword: fresh 1.0, each full hit -0.6, -1.0 = worked out (strike there = 0 result, -0.017 q)
static bool exhausted(float r){ return r<=-0.95f; }
static int   st(){ return F<uint8_t>(g_m,M_STATE); }
static float comp(){ return F<float>(g_m,M_COMP); }
static float qual(){ return F<float>(g_m,M_QUAL); }
static float fire(){ return F<float>(g_m,M_FIRE); }
static float stamina(){ uintptr_t a=F<uintptr_t>(g_bs,BS_ACTOR); if(!a) return -1; uintptr_t s=F<uintptr_t>(a,0x668); return s? F<float>(s,0x70C):-1; }
static uintptr_t wp(){ return F<uintptr_t>(g_m,M_WP); }
static int zones(float* t,int cap){
    uintptr_t w=wp(); if(!w) return 0;
    uintptr_t b=F<uintptr_t>(w,0x28), e=F<uintptr_t>(w,0x30); int n=(int)((e-b)/8); if(n<1||n>cap) return 0;
    for(int i=0;i<n;i++){ uintptr_t z=F<uintptr_t>(b,8*i); t[i]=z? F<float>(z,8):0; }
    return n;
}
struct Bounds{ float x0,y0,x1,y1; };
static Bounds bounds(){  // same lerp the game uses in 0x8587B4
    Bounds r{0,0,1,1}; uintptr_t w=wp(); if(!w) return r; uintptr_t d=F<uintptr_t>(w,0x18); if(!d) return r;
    float c=comp(); auto L=[&](uintptr_t a,uintptr_t b,int k){ float s=F<float>(d,a+4*k), e=F<float>(d,b+4*k); return s+(e-s)*c; };
    r.x0=L(0x5C,0x64,0); r.y0=L(0x5C,0x64,1); r.x1=L(0x6C,0x74,0); r.y1=L(0x6C,0x74,1);
    if(r.x1<r.x0) std::swap(r.x0,r.x1); if(r.y1<r.y0) std::swap(r.y0,r.y1);
    return r;
}
static const char* wptag(){ uintptr_t w=wp(); if(!w) return ""; uintptr_t d=F<uintptr_t>(w,0x18); if(!d) return ""; const char* t=F<const char*>(d,0x18); return t? t:""; }
static bool is2d(const Bounds& b){ return (b.y1-b.y0)>0.05f && strcmp(wptag(),"sword")!=0; }  // swords (short+long) sweep along the blade
static const char* wpid(){ uintptr_t w=wp(); if(!w) return "?"; uintptr_t d=F<uintptr_t>(w,0x18); if(!d) return "?"; const char* s=F<const char*>(d,0x08); return s? s:"?"; }

// ---------------- tunables (conservative v1) ----------------
static const float ZONE_TARGET=1280.f;     // heat every zone to this before going to the anvil
static const float FIRE_LO=1300.f, FIRE_HI=1350.f; // bellows hysteresis (fire temp); overheat quality loss starts >1400
static float g_strikeMin=800.f;           // reheat when the coldest zone drops below this (learned upward if a full-charge hit loses quality)
static const float STAM_LOW=3.f;           // stamina finds its own equilibrium (~30) at this beat; skip a beat only if it bottoms out
// The hit result depends on rhythm: the game needs a steady tempo (28..106 BPM) and the first ~3 strokes after any
// break score a baseline.  So: rigid beat grid, fixed hold, as few breaks as possible.
static const uint64_t STROKE_PERIOD=1333,   // 45 BPM = blacksmith_045 music track (43..47 band)
                       STROKE_HOLD=600, EVAL_DELAY=650;

// ---------------- hammer-point learner ----------------
static const int GX=9, GY=9;
struct Cell{ int hit, miss; int lastUse; };
static Cell g_cells[2][GX][GY]; static int g_side=0; static bool g_2d=false; static int g_strokeNo=0;
static float cx(int i){ return (i+0.5f)/GX; } static float cy(int j){ return (j+0.5f)/GY; }
static bool inb(const Bounds& b,float x,float y){ return x>=b.x0-0.02f&&x<=b.x1+0.02f&&(!g_2d||(y>=b.y0-0.02f&&y<=b.y1+0.02f)); }
static uint32_t g_rng=0x1234567;
static float rnd(){ g_rng=g_rng*1664525u+1013904223u; return (g_rng>>8)/16777216.f; }
static int g_ci=-1,g_cj=-1; static float g_tx=0.5f,g_ty=0.5f;
static void choose_point(){
    Bounds b=bounds(); int ny=g_2d?GY:1;
    auto& C=g_cells[g_side];
    int bi=-1,bj=-1; float best=-1e9f;
    bool anyGood=false; for(int i=0;i<GX;i++) for(int j=0;j<ny;j++) if(C[i][j].hit>0) anyGood=true;
    bool explore = !anyGood || rnd()<0.25f;
    for(int i=0;i<GX;i++) for(int j=0;j<ny;j++){
        float x=cx(i), y=g_2d?cy(j):0.5f; if(!inb(b,x,y)) continue;
        Cell& c=C[i][j];
        if(g_strokeNo-c.lastUse<=2 && c.lastUse>0) continue;               // no striking the same place
        float s;
        if(c.hit==0 && c.miss>=1) continue;                                 // proven miss
        if(!explore){ if(c.hit==0) continue; s=(c.hit+1.f)/(c.hit+c.miss+2.f)*10.f+(g_strokeNo-c.lastUse)*0.1f; }
        else if(c.hit==0 && c.miss==0){
            // unknown cell: prefer ones next to known hits, else near the piece centre
            float nb=0; for(int di=-1;di<=1;di++) for(int dj=-1;dj<=1;dj++){ int a=i+di,bb=j+dj; if(a>=0&&a<GX&&bb>=0&&bb<ny&&C[a][bb].hit>0) nb+=1; }
            float dx=x-0.5f, dy=g_2d?y-0.5f:0; s=nb*3.f-sqrtf(dx*dx+dy*dy)*4.f;
        } else continue;
        s+=rnd()*0.3f;
        if(s>best){ best=s; bi=i; bj=j; }
    }
    if(bi<0){ // nothing eligible: least-recently-used good cell, else centre of bounds
        int lu=1<<30; for(int i=0;i<GX;i++) for(int j=0;j<ny;j++){ float x=cx(i),y=g_2d?cy(j):0.5f; if(!inb(b,x,y)) continue; if(C[i][j].hit>0&&C[i][j].lastUse<lu){ lu=C[i][j].lastUse; bi=i; bj=j; } }
    }
    if(bi<0){ g_ci=g_cj=-1; g_tx=(b.x0+b.x1)*0.5f; g_ty=(b.y0+b.y1)*0.5f; return; }
    g_ci=bi; g_cj=bj;
    float jx=(rnd()-0.5f)*0.6f/GX, jy=(rnd()-0.5f)*0.6f/GY;
    g_tx=std::min(std::max(cx(bi)+jx,b.x0),b.x1);
    g_ty=g_2d? std::min(std::max(cy(bj)+jy,b.y0),b.y1) : F<float>(g_m,M_POS+4);
}
// ---- seeded paths for known 2D pieces (from a manual forge trace); stepped ~0.1 per stroke, ping-pong ----
struct WP{ float x,y; };
static const WP HORSESHOE[]={ {0.82f,0.34f},{0.72f,0.34f},{0.62f,0.34f},{0.52f,0.33f},{0.42f,0.33f},{0.32f,0.33f},{0.22f,0.34f},{0.12f,0.37f},
    {0.06f,0.45f},{0.02f,0.55f},{0.03f,0.65f},{0.12f,0.74f},
    {0.22f,0.77f},{0.32f,0.78f},{0.42f,0.78f},{0.52f,0.78f},{0.62f,0.78f},{0.72f,0.79f},{0.82f,0.80f} };
static const WP* g_path=nullptr; static int g_pn=0, g_pi=0, g_pdir=1, g_pk=-1;
static int g_pmiss[64]; static float g_poff[64]; static bool g_preAimed=false;
static bool path_point(){
    if(!g_path) return false;
    for(int tries=0;tries<2*g_pn;tries++){
        int k=g_pi; g_pi+=g_pdir; if(g_pi>=g_pn){ g_pi=g_pn-2; g_pdir=-1; } else if(g_pi<0){ g_pi=1; g_pdir=1; }
        if(g_pmiss[k]>=3) continue;                                   // gave up on this spot
        float x=g_path[k].x, y=g_path[k].y+g_poff[k];
        if(exhausted(remainingAt(x,y))) continue;                     // worked out on this side
        g_pk=k; g_ci=-1; g_cj=k; g_tx=x; g_ty=y; return true;
    }
    return false;
}
static void path_result(bool hit){
    if(g_pk<0) return;
    if(hit){ g_pmiss[g_pk]=0; return; }
    int m=++g_pmiss[g_pk]; g_poff[g_pk]= (m==1)? 0.04f : (m==2? -0.04f : 0.f);   // nudge, then give up
}
static float g_w0=0; static int g_sweep=0; static uint64_t g_hold=0; static float g_holdCh=0;
// 1D pieces (sword): 3 points per zone across the whole blade, visited in order
// zone at x is floor(x*zones) (native 0x16090AC); positions limited to the clamp bounds
// Blade sweep: fine candidate grid along the blade (and across it when the piece has a y range),
// walk forward ~0.08 per stroke, only spots whose zone is hot and whose work map still has work.
// returns 0 = no hot section (reheat), 1 = ok, 2 = hot but this side worked out
static float g_lastX=-1; static bool g_forced=false;
static int sweep_point(){
    Bounds b=bounds(); float t[8]; int n=zones(t,8); if(n<1){ n=1; t[0]=9999.f; }
    float lo=std::max(b.x0,0.f)+0.02f, hi=std::min(std::min(b.x1,0.999f)-0.02f,0.93f); if(hi<lo) hi=lo;
    float ys[3]; int ny=1;
    if((b.y1-b.y0)>0.05f){ float c=(b.y0+b.y1)*0.5f; ys[0]=c; ys[1]=c-0.12f; ys[2]=c+0.12f; ny=3; } else ys[0]=F<float>(g_m,M_POS+4);
    const int NX=48; bool anyHot=false; float bestR=-1e9f,bx=0,by=0; bool haveBest=false;
    float start=g_lastX<0? lo : g_lastX+0.08f;
    for(int pass=0;pass<2;pass++)                      // pass 0: >=0.08 ahead (wrapping), pass 1: anything
    for(int k=0;k<NX;k++){
        float x=start+(hi-lo)*k/(float)NX; if(x>hi) x=lo+(x-hi); if(x>hi) x=hi;
        if(pass==1) x=lo+(hi-lo)*k/(float)(NX-1);
        int z=std::min(n-1,std::max(0,(int)(x*n))); if(t[z]<g_strikeMin) continue;
        anyHot=true;
        for(int j=0;j<ny;j++){
            float r=remainingAt(x,ys[j]);
            if(r>bestR){ bestR=r; bx=x; by=ys[j]; haveBest=true; }
            if(exhausted(r)) continue;
            if(pass==0 && g_lastX>=0 && fabsf(x-g_lastX)<0.06f) continue;
            g_tx=x; g_ty=ys[j]; g_lastX=x; g_ci=-1; g_cj=k; return 1;
        }
    }
    if(!anyHot) return 0;
    if(g_forced && haveBest){ g_tx=bx; g_ty=by; g_lastX=bx; g_ci=-1; g_cj=-1; return 1; }  // both sides done: best remaining spot
    return 2;
}
static float maxzone(){ float t[8]; int n=zones(t,8); float m=-1; for(int i=0;i<n;i++) m=std::max(m,t[i]); return m; }
static void aim(){ float px=F<float>(g_m,M_POS), py=F<float>(g_m,M_POS+4); float dx=g_tx-px, dy=g_2d? g_ty-py:0.f; if(fabsf(dx)>1e-4f||fabsf(dy)>1e-4f) actMoveHammer(dx,dy); }

// ---------------- state machine ----------------
enum Phase{ Idle, Heat, GoAnvil, Work, GoForge, Flip, Finish };
static const char* PN[]={"Idle","Heat","GoAnvil","Work","GoForge","Flip","Finish"};
static Phase g_ph=Idle; static uint64_t g_phT=0, g_t=0, g_logT=0;
enum Sub{ Ready, Charging, Released };
static Sub g_sub=Ready; static uint64_t g_lastStart=0, g_rel=0;
static float g_c0=0,g_q0=0,g_s0=0; static bool g_rest=false; static float g_stamMax=0;
static bool g_flipped=false, g_flipDisabled=false; static int g_zeroStreak=0, g_sinceFlip=0;
static float g_fireCap=9999.f, g_capRef=0; static uint64_t g_capT=0; static bool g_bel=false;
static kc::Hotkey g_key; static int g_hits=0,g_miss=0;
static int g_insane=0; static bool g_dead=false;

static void setph(Phase p,const char* why){ logf("phase %s -> %s (%s) st=%d comp=%.3f q=%.3f fire=%.0f stam=%.0f",PN[g_ph],PN[p],why,st(),comp(),qual(),fire(),stamina()); g_ph=p; g_phT=GetTickCount64(); g_t=0; }
static void bellows(bool on){ if(on!=g_bel||F<uint8_t>(g_m,M_BELLOWS)!=(uint8_t)on){ actBellows(on); g_bel=on; } }
static void stop(const char* why){
    if(g_ph==Idle) return;
    if(g_bel) bellows(false);
    if(g_sub==Charging) act0(A_FINISHSTROKE);
    g_sub=Ready; logf("stop: %s  strokes=%d hits=%d miss=%d comp=%.3f q=%.3f",why,g_strokeNo,g_hits,g_miss,comp(),qual()); g_ph=Idle;
}
static float minzone(int* idx=nullptr){ float t[8]; int n=zones(t,8); float m=1e9f; int k=0; for(int i=0;i<n;i++) if(t[i]<m){ m=t[i]; k=i; } if(idx) *idx=k; return n? m:-1; }
static void logzones(char* out,size_t cap){ float t[8]; int n=zones(t,8); size_t k=0; out[0]=0; for(int i=0;i<n;i++) k+=sprintf_s(out+k,cap-k,"%s%.0f",i?"/":"",t[i]); }

static void start(){
    memset(g_cells,0,sizeof g_cells); g_side=0; g_strokeNo=0; g_hits=g_miss=0; g_flipped=false; g_flipDisabled=false; g_zeroStreak=0; g_sinceFlip=0;
    g_fireCap=9999.f; g_capT=0; g_bel=false; g_sub=Ready; g_rest=false; g_stamMax=std::max(stamina(),60.f); g_strikeMin=800.f; g_sweep=0; g_lastX=-1; g_forced=false; g_wsign=0; g_wsum=0; g_wn=0; g_rMiss=-1e9f; g_rHitMin=1e9f; g_missKnown=false;
    g_path=nullptr; g_pn=0; g_pi=0; g_pdir=1; g_pk=-1; g_preAimed=false; memset(g_pmiss,0,sizeof g_pmiss); memset(g_poff,0,sizeof g_poff);
    if(!strcmp(wpid(),"horseshoe")){ g_path=HORSESHOE; g_pn=(int)(sizeof HORSESHOE/sizeof HORSESHOE[0]); logf("using seeded horseshoe path (%d spots)",g_pn); }
    Bounds b=bounds(); g_2d=is2d(b);
    char z[64]; logzones(z,sizeof z);
    logf("START piece=%s zones=[%s] bounds=(%.2f,%.2f)-(%.2f,%.2f) %s st=%d comp=%.3f q=%.3f stam=%.0f",wpid(),z,b.x0,b.y0,b.x1,b.y1,g_2d?"2D":"1D",st(),comp(),qual(),stamina());
    int s=st();
    if(s==ST_FORGE) setph(Heat,"start at forge");
    else if(s==ST_ANVIL||s==ST_FINISHSTROKE) setph(Work,"start at anvil");
    else { logf("not at forge/anvil (state %d); F9 when Henry is at the forge",s); g_ph=Idle; }
}

static void tick(float dt){
    uint64_t now=GetTickCount64();
    bool pressed=g_key.pressed();
    if(pressed){ if(g_ph==Idle) start(); else stop("user key"); return; }
    if(g_ph==Idle) return;
    int s=st(); float c=comp();
    {   // sanity: values the mod relies on must look like the build it was made for
        float f=fire(), q=qual();
        if(s>40 || !(c>=-0.01f && c<=1.5f) || !(q>=-0.5f && q<=1.5f) || !(f>-100.f && f<5000.f)){
            stop("unexpected forge state");
            if(++g_insane>=3){ g_dead=true; logf("disabled: forge data does not look right (game updated?)"); }
            return;
        }
    }
    float stam=stamina(); if(stam>g_stamMax) g_stamMax=stam;
    uint64_t inPh= now>g_phT? now-g_phT : 0;

    if(c>=0.999f && g_ph!=Finish && g_sub!=Charging){ bellows(false); setph(Finish,"completion reached"); }

    switch(g_ph){
    case Heat: {
        if(s!=ST_FORGE){ if(inPh>20000){ stop("heat: not at forge"); } break; }
        float t[8]; int n=zones(t,8); if(!n){ stop("no workpiece zones"); break; }
        float f=fire();
        // bellows hysteresis + detect fire cap
        if(f<std::min(FIRE_LO,g_fireCap-40.f)) bellows(true); else if(f>std::min(FIRE_HI,g_fireCap)) bellows(false);
        if(g_bel){ if(!g_capT){ g_capT=now; g_capRef=f; } else if(now-g_capT>5000){ if(f-g_capRef<8.f && f<FIRE_LO){ g_fireCap=f; logf("fire plateau %.0f -> cap",f); } g_capT=now; g_capRef=f; } } else g_capT=0;
        float target=std::min(ZONE_TARGET,g_fireCap-60.f);
        int k; float mn=minzone(&k);
        if(mn>=target || (inPh>120000 && mn>=g_strikeMin+100.f)){
            bellows(false); act0(A_TOANVIL); char z[64]; logzones(z,sizeof z); logf("heated zones=[%s] fire=%.0f -> anvil",z,f); setph(GoAnvil,"heated"); break;
        }
        if(inPh>180000){ stop("heat timeout"); break; }
        // dwell: stay in the current zone >=3s and until it is 60 deg hotter than the coldest
        int cz=F<int>(g_m,M_ZONE);
        if(cz!=k && (cz<0||cz>=n|| (now-g_t>3000 && (t[cz]>=target || t[cz]>=mn+60.f)))){ actSetZone(k); g_t=now; }
        if(now-g_logT>2000){ g_logT=now; char z[64]; logzones(z,sizeof z); KC_DLOG("heat fire=%.0f bel=%d zone=%d zones=[%s] target=%.0f",f,(int)g_bel,F<int>(g_m,M_ZONE),z,target); }
        break;
    }
    case GoAnvil:
        if(s==ST_ANVIL){ Bounds b=bounds(); g_2d=is2d(b); g_sub=Ready; g_lastStart=0; setph(Work,"at anvil"); }
        else if(s==ST_FORGE && inPh>4000 && !g_t){ act0(A_TOANVIL); g_t=now; logf("retry ToAnvil"); }
        else if(inPh>20000) stop("anvil transition timeout");
        break;
    case GoForge:
        if(s==ST_FORGE){ g_capT=0; setph(Heat,"at forge"); }
        else if(s==ST_ANVIL && inPh>4000 && !g_t){ act0(A_TOFORGE); g_t=now; logf("retry ToForge"); }
        else if(inPh>20000) stop("forge transition timeout");
        break;
    case Flip:
        if(s==ST_FLIP||s==ST_FLIPBACK) g_t=1;
        if(s==ST_ANVIL && (g_t||inPh>1500)){
            if(!g_t && inPh>1500){ g_flipDisabled=true; logf("flip did nothing -> disabled"); } else { g_side^=1; logf("flipped, side=%d",g_side); }
            g_sub=Ready; g_lastStart=now; setph(Work,"flip done");
        } else if(inPh>8000) stop("flip timeout");
        break;
    case Work: {
        if(g_sub==Charging){
            aim();
            if(now-g_lastStart>=STROKE_HOLD){ g_hold=now-g_lastStart; g_holdCh=F<float>(g_m,0x248); act0(A_FINISHSTROKE); g_rel=now; g_sub=Released; }
            break;
        }
        if(g_sub==Released){
            if(now-g_rel<EVAL_DELAY) break;
            float gain=c-g_c0, dq=qual()-g_q0, cost=g_s0-stam; char z[64]; logzones(z,sizeof z);
            bool hit=gain>0.0005f; float w1=workAt(g_tx,g_ty);
            if(hit && gain>0.015f && fabsf(w1-g_w0)>1e-5f && g_wn<20){ g_wsum+=(w1-g_w0); g_wn++; g_wsign=g_wsum>0?1.f:-1.f; }
            { float r0= g_wsign>0? -g_w0 : g_w0; if(!hit){ g_rMiss=g_missKnown? std::max(g_rMiss,r0):r0; g_missKnown=true; } else if(gain>0.015f) g_rHitMin=std::min(g_rHitMin,r0); }
            if(g_ci>=0){ Cell& cl=g_cells[g_side][g_ci][g_2d?g_cj:0]; if(hit) cl.hit++; else cl.miss++; }
            if(hit){ g_hits++; g_zeroStreak=0; } else { g_miss++; g_zeroStreak++; }
            KC_DLOG("stroke#%d side=%d aim=(%.2f,%.2f) cell=%d,%d %s gain=%.3f comp=%.3f dq=%.4f q=%.3f stam=%.0f(-%.0f) bpm=%.1f charge=%.2f work=%.4f->%.4f zones=[%s]",
                g_strokeNo,g_side,g_tx,g_ty,g_ci,g_cj,hit?"HIT":"MISS",gain,c,dq,qual(),stam,cost,F<float>(g_m,M_BPM),F<float>(g_m,M_CHARGE),g_w0,w1,z);
            if(g_2d) path_result(hit);
            g_sub=Ready; g_sinceFlip++;
            if(g_2d){ if(!path_point()) choose_point(); g_preAimed=true; aim(); }   // move early so the hammer has arrived by the swing
            break;
        }
        // Ready
        if(s!=ST_ANVIL && s!=ST_FINISHSTROKE) { if(inPh>15000 && now-g_lastStart>15000) stop("work: unexpected state"); break; }
        float mz=maxzone();
        if(mz>=0 && mz<g_strikeMin && s==ST_ANVIL){ char z[64]; logzones(z,sizeof z); logf("cold zones=[%s] < %.0f -> reheat",z,g_strikeMin); act0(A_TOFORGE); setph(GoForge,"reheat"); break; }
        if(stam>=0){
            if(stam<STAM_LOW && now-g_lastStart>=STROKE_PERIOD){ g_lastStart+=STROKE_PERIOD; logf("skip beat: stam %.0f",stam); break; }
        }
        if(s==ST_ANVIL && !g_flipDisabled && (g_zeroStreak>=6 && g_sinceFlip>=8)){
            g_flipped=true; g_zeroStreak=0; g_sinceFlip=0; act0(A_FLIP); g_t=0; setph(Flip, c>=0.5f?"half done":"stalled"); break;
        }
        if(now-g_lastStart<STROKE_PERIOD) break;
        if(!g_2d){ int sp=sweep_point();
            if(sp==0){ if(s!=ST_ANVIL) break; logf("no hot section -> reheat"); act0(A_TOFORGE); setph(GoForge,"reheat"); break; }
            if(sp==2){ if(s!=ST_ANVIL) break;
                if(!g_flipDisabled && g_sinceFlip>=2){ logf("side worked out -> flip"); g_sinceFlip=0; g_zeroStreak=0; act0(A_FLIP); g_t=0; setph(Flip,"side done"); break; }
                if(!g_forced){ g_forced=true; logf("both sides worked out at every hot spot -> taking best remaining spots"); }
                if(sweep_point()!=1) break; } }
        g_strokeNo++; if(g_2d && !g_preAimed){ if(!path_point()) choose_point(); } g_preAimed=false;
        if(g_ci>=0) g_cells[g_side][g_ci][g_2d?g_cj:0].lastUse=g_strokeNo;
        aim();
        g_c0=c; g_q0=qual(); g_s0=stam; g_w0=workAt(g_tx,g_ty);
        act0(A_STARTSTROKE); g_lastStart = (g_lastStart && now-g_lastStart<STROKE_PERIOD+150)? g_lastStart+STROKE_PERIOD : now; g_sub=Charging;
        break;
    }
    case Finish: {
        inPh= now>g_phT? now-g_phT : 0;   // the phase may have switched this tick (g_phT can be a tick ahead of now)
        int h=F<uint8_t>(g_m,M_HARD);
        if(s==ST_FINIDLE && inPh>3000 && !g_t){ act0(A_TOHARDEN); g_t=now; logf("nudged ToHardening"); }
        if(h==2 && g_t!=2){ g_t=2; logf("quench reached: q=%.3f",qual()); stop("done (confirm the result yourself)"); }
        if(inPh>60000) stop("finish timeout");
        break;
    }
    default: break;
    }
}

static void UpdateHook(void* self,float dt){
    pOrigUpdate(self,dt);
    if(g_dead) return;
    uintptr_t bs=(uintptr_t)self;
    __try{
        uintptr_t m=F<uintptr_t>(bs,BS_MODEL);
        if(bs!=g_bs||m!=g_m){ if(g_ph!=Idle) logf("session changed -> idle"); g_ph=Idle; g_bs=bs; g_m=m; }
        if(m) tick(dt);
    }__except(EXCEPTION_EXECUTE_HANDLER){ logf("exception in tick (phase %s)",PN[g_ph]); g_ph=Idle; }
}

struct Found { uintptr_t vt, avt, update, a[10], workmap; };
static bool resolve(kc::Resolver& R, Found& f){
    f.vt=R.vtable("C_Blacksmithing",".?AVC_Blacksmithing@playermodule@wh@@",0);
    f.avt=R.vtable("C_Blacksmithing actions",".?AVC_Blacksmithing@playermodule@wh@@",(uint32_t)BS_ACTIONS);   // also proves the +0x68 layout
    f.update=R.slot("C_Blacksmithing::Update",f.vt,19,SIG_UPDATE);
    const char* sig[10]={SIG_A_TOANVIL,SIG_A_TOFORGE,SIG_A_TOHARD,nullptr,SIG_A_BELLOWS,SIG_A_MOVE,SIG_A_FLIP,SIG_A_STROKE,SIG_A_STROKE,SIG_A_ZONE};
    for(int i=0;i<10;i++) if(sig[i]){ char n[48]; sprintf_s(n,"forge action %d",i); f.a[i]=R.slot(n,f.avt,i,sig[i]); }
    f.workmap=R.find("work map",SIG_WORKMAP);
    return R.ok();
}
#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out){   // offline check (tools/selftest)
    kc::Resolver R; R.im.load(game); Found f{}; bool ok=resolve(R,f);
    uintptr_t* p=(uintptr_t*)&f; for(size_t i=0;i<sizeof f/8;i++) out[i]=p[i];
    return ok? (int)(sizeof f/8) : -R.fails;
}
#endif

static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_autoforge");
    g_key.vk=kc::ini_key("Forge",VK_F9);
    HMODULE h=kc::wait_game(); if(!h){ logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h); B=R.im.base;
    logf("start v1.0.0 (game ts 0x%08X)",R.im.timestamp);
    Found f{};
    if(!resolve(R,f)){ logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    pWorkMap=f.workmap;
    void** slot=&((void**)f.vt)[19];
    pOrigUpdate=(Update_t)f.update;
    kc::write_ptr(slot,(void*)&UpdateHook);
    char kn[16]; logf("ready (%s at the forge)",kc::key_name(g_key.vk,kn,sizeof kn));
    return 0;
}
KC_PLUGIN("Theatrical AutoForge", KC_AUTHOR, 100)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); } return TRUE; }
