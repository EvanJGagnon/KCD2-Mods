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
static uint64_t (*g_clock)() = GetTickCount64;   // dev builds: tools/forgesim drives time
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
// batch state ([Options] Queue=1; see the recipe page section below)
static char g_batchRecipe[96]={0}; static int g_batchTotal=0, g_batchLeft=0; static bool g_batchAuto=false;
static void batch_cancel(const char* why); static void batch_quenched();
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
static const char* recipeId(uintptr_t m){ const char* r=F<const char*>(m,8); return (r && kc::readable((uintptr_t)r,1))? r : "?"; }   // model+8 (0x11A08B8)

// ---------------- tunables (conservative v1) ----------------
static const float ZONE_TARGET=1280.f;     // heat every zone to this before going to the anvil
static const float FIRE_LO=1300.f, FIRE_HI=1350.f; // bellows hysteresis (fire temp); overheat quality loss starts >1400
static float g_strikeMin=800.f;           // reheat when the coldest zone drops below this (learned upward if a full-charge hit loses quality)
static const float STAM_LOW=3.f;           // stamina finds its own equilibrium (~30) at this beat; skip a beat only if it bottoms out
// The hit result depends on rhythm: the game needs a steady tempo (28..106 BPM) and the first ~3 strokes after any
// break score a baseline.  So: rigid beat grid, fixed hold, as few breaks as possible.
static const uint64_t STROKE_PERIOD=1333,   // 45 BPM = blacksmith_045 music track (43..47 band)
                       STROKE_HOLD=600, EVAL_DELAY=650;
// Planned flip.  Turning the piece breaks the game's rhythm (any gap more than wh_pl_BlacksmithTimingTolerance
// = 0.28 s off the beat resets it, 0x11e0064), so the next 3 strokes get no rhythmic bonus.  Without the bonus a
// stroke's effectivity is ~0.7 + 0.7 * work-map value (fitted from logged strokes), so a spot with work >= 0.45
// still gives a full stroke: those 3 strokes aim at the freshest spot instead of costing quality.
static int g_flipAtPct=50;                  // [Options] FlipAt: turn the piece over at this % completion (0 = only when needed)
static const float FRESH_MIN=0.45f;
static const int RHYTHM_STROKES=3;          // strokes after a rhythm break that score without the bonus

// ---------------- hammer-point learner ----------------
static const int GX=9, GY=9;
struct Cell{ int hit, miss; int lastUse; };
static Cell g_cells[2][GX][GY]; static int g_side=0; static bool g_2d=false; static int g_strokeNo=0;
static float cx(int i){ return (i+0.5f)/GX; } static float cy(int j){ return (j+0.5f)/GY; }
static bool inb(const Bounds& b,float x,float y){ return x>=b.x0-0.02f&&x<=b.x1+0.02f&&(!g_2d||(y>=b.y0-0.02f&&y<=b.y1+0.02f)); }
#ifndef KC_SEED
#define KC_SEED 0x1234567
#endif
static uint32_t g_rng=KC_SEED;
static float rnd(){ g_rng=g_rng*1664525u+1013904223u; return (g_rng>>8)/16777216.f; }
static int g_ci=-1,g_cj=-1; static float g_tx=0.5f,g_ty=0.5f;
static int g_fresh=0;                       // strokes left that should land on a fresh spot (set after a flip)
static int g_flips=0, g_afterFlipLog=0;
#ifndef KC_EXPLORE
#define KC_EXPLORE 0.1f
#endif
static const float EXPLORE_KNOWN=KC_EXPLORE;
#ifndef KC_WPREF
#define KC_WPREF 1.f
#endif
static const float W_PREF=KC_WPREF;               // weight of fresh metal when picking a known spot (fresh metal is near the edges: riskier)
static void choose_point(){
    Bounds b=bounds(); int ny=g_2d?GY:1;
    auto& C=g_cells[g_side];
    int bi=-1,bj=-1; float best=-1e9f;
    bool anyGood=false; for(int i=0;i<GX;i++) for(int j=0;j<ny;j++) if(C[i][j].hit>0) anyGood=true;
    int good=0; for(int i=0;i<GX;i++) for(int j=0;j<ny;j++) if(C[i][j].hit>0) good++;
    bool explore = !anyGood || rnd()<(good>=4? EXPLORE_KNOWN : 0.25f);   // misses cost quality: explore less once spots are known
    for(int i=0;i<GX;i++) for(int j=0;j<ny;j++){
        float x=cx(i), y=g_2d?cy(j):0.5f; if(!inb(b,x,y)) continue;
        Cell& c=C[i][j];
        if(g_strokeNo-c.lastUse<=2 && c.lastUse>0) continue;               // no striking the same place
        float s;
        if(c.hit==0 && c.miss>=1) continue;                                 // proven miss
        float w=remainingAt(x,y);
        if(c.hit>0 && exhausted(w)) continue;                               // worked out on this side: a dud, not a hit
        if(g_fresh>0 && c.hit>0){ s=w*10.f; }                               // no rhythm yet (just flipped): freshest known spot
        else if(!explore){ if(c.hit==0) continue;
            if(c.miss>=2 && c.miss*2>c.hit) continue;                       // an edge spot: misses too often
            s=(c.hit+1.f)/(c.hit+c.miss*3.f+2.f)*10.f+std::min(1.f,(g_strokeNo-c.lastUse)*0.1f)+W_PREF*w; }
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
    // The game's wear map is much finer than these cells and every hit wears the exact point struck, so pick the
    // least-worn point inside the cell (a fully worn point, -1, is a sparkless dud even in rhythm; without rhythm any
    // worn point is a weak stroke).  A cell that has missed is near an edge: stay closer to its middle.
    float span=(g_cells[g_side][bi][bj].miss? 0.3f : 0.7f)/GX, spany=(g_cells[g_side][bi][bj].miss? 0.3f : 0.7f)/GY;
    float bw=-9.f, bx=cx(bi), by=g_2d? cy(bj) : F<float>(g_m,M_POS+4);
    for(int u=0;u<5;u++) for(int v=0;v<(g_2d?5:1);v++){
        float x=std::min(std::max(cx(bi)+(u/4.f-0.5f)*span,b.x0),b.x1);
        float y=g_2d? std::min(std::max(cy(bj)+(v/4.f-0.5f)*spany,b.y0),b.y1) : by;
        float w=workAt(x,y)+rnd()*0.05f;                          // ties (an untouched side reads 0 everywhere): any
        if(w>bw){ bw=w; bx=x; by=y; } }
    g_tx=bx; g_ty=by;
}
// ---- seeded paths for known 2D pieces (from a manual forge trace); stepped ~0.1 per stroke, ping-pong ----
struct WP{ float x,y; };
static const WP HORSESHOE[]={ {0.82f,0.34f},{0.72f,0.34f},{0.62f,0.34f},{0.52f,0.33f},{0.42f,0.33f},{0.32f,0.33f},{0.22f,0.34f},{0.12f,0.37f},
    {0.06f,0.45f},{0.02f,0.55f},{0.03f,0.65f},{0.12f,0.74f},
    {0.22f,0.77f},{0.32f,0.78f},{0.42f,0.78f},{0.52f,0.78f},{0.62f,0.78f},{0.72f,0.79f},{0.82f,0.80f} };
static const WP* g_path=nullptr; static int g_pn=0, g_pi=0, g_pdir=1, g_pk=-1;
static int g_pmiss[64]; static float g_poff[64]; static bool g_preAimed=false;
// The seeded path was traced on side 0.  A turned-over piece lies mirrored left-right, so on the flipped side the path
// starts mirrored; if it misses twice in a row without a hit, it is used as traced instead (once per side).
static bool g_pathMirror[2], g_mirrorDecided[2]; static int g_sideMisses=0;
static float path_x(int k){ float x=g_path[k].x; return g_pathMirror[g_side]? 0.84f-x : x; }   // traced x spans 0.02..0.82
static bool path_point(){
    if(!g_path) return false;
    if(g_fresh>0){                                                    // no rhythm yet: the freshest spot on the path
        int bk=-1; float bw=FRESH_MIN;
        for(int k=0;k<g_pn;k++){ if(g_pmiss[k]>=3) continue; float w=remainingAt(path_x(k),g_path[k].y+g_poff[k]); if(w>=bw){ bw=w; bk=k; } }
        if(bk>=0){
            g_pi=bk+g_pdir; if(g_pi>=g_pn){ g_pi=g_pn-2; g_pdir=-1; } else if(g_pi<0){ g_pi=1; g_pdir=1; }
            g_pk=bk; g_ci=-1; g_cj=bk; g_tx=path_x(bk); g_ty=g_path[bk].y+g_poff[bk]; return true;
        }
    }
    for(int tries=0;tries<2*g_pn;tries++){
        int k=g_pi; g_pi+=g_pdir; if(g_pi>=g_pn){ g_pi=g_pn-2; g_pdir=-1; } else if(g_pi<0){ g_pi=1; g_pdir=1; }
        if(g_pmiss[k]>=3) continue;                                   // gave up on this spot
        float x=path_x(k), y=g_path[k].y+g_poff[k];
        if(exhausted(remainingAt(x,y))) continue;                     // worked out on this side
        g_pk=k; g_ci=-1; g_cj=k; g_tx=x; g_ty=y; return true;
    }
    return false;
}
static void path_result(bool hit){
    if(g_pk<0) return;
    if(!g_mirrorDecided[g_side]){                                     // first strokes on a newly flipped side
        if(hit){ g_mirrorDecided[g_side]=true; logf("flipped side %d: path %s hits",g_side,g_pathMirror[g_side]?"(mirrored)":"(as traced)"); }
        else if(++g_sideMisses>=2){
            g_mirrorDecided[g_side]=true; g_pathMirror[g_side]=!g_pathMirror[g_side]; g_sideMisses=0;
            memset(g_pmiss,0,sizeof g_pmiss); memset(g_poff,0,sizeof g_poff);
            logf("flipped side %d: path missed twice -> trying it %s",g_side,g_pathMirror[g_side]?"mirrored":"as traced"); return;
        }
    }
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
    if(g_fresh>0){                                     // no rhythm yet (just flipped): the freshest hot spot, if fresh enough
        float fr=FRESH_MIN, fx=0, fy=0; bool ok=false;
        for(int k=0;k<NX;k++){ float x=lo+(hi-lo)*k/(float)(NX-1); int z=std::min(n-1,std::max(0,(int)(x*n))); if(t[z]<g_strikeMin) continue;
            for(int j=0;j<ny;j++){ float r=remainingAt(x,ys[j]); if(r>=fr){ fr=r; fx=x; fy=ys[j]; ok=true; } } }
        if(ok){ g_tx=fx; g_ty=fy; g_lastX=fx; g_ci=-1; g_cj=-1; return 1; }
    }
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
static bool g_oneMore=false;              // completion looked done but the game did not finish: strike again first
static int g_pass=0, g_passStrokes=0;      // anvil passes this piece, strokes in the current pass

static void setph(Phase p,const char* why){ logf("phase %s -> %s (%s) st=%d comp=%.3f q=%.3f fire=%.0f stam=%.0f",PN[g_ph],PN[p],why,st(),comp(),qual(),fire(),stamina()); g_ph=p; g_phT=g_clock(); g_t=0; }
// Turned over, a piece lies mirrored left-right (an axe head that faced right now faces left; blades and horseshoes
// are symmetric).  So a new side starts with the old side's good spots mirrored about the middle of the piece instead
// of guessing (a guess off the piece is a sparkless dud that costs quality and time).  If the first two strokes there
// miss before any hit, the side is seeded again as it lay before.  Only for a side that has not been learned yet.
#ifndef KC_SEEDMIN
#define KC_SEEDMIN 2
#endif
static const int SEED_MIN_HITS=KC_SEEDMIN;
static bool g_seedDecided[2]={true,true}; static bool g_seedMirror[2]; static int g_seedMisses=0;
static void seed_side(int side,bool mirror){
    auto& N=g_cells[side]; auto& O=g_cells[side^1]; int ny=g_2d?GY:1, n=0;
    Bounds b=bounds();
    memset(N,0,sizeof N);
    for(int i=0;i<GX;i++) for(int j=0;j<ny;j++) if(O[i][j].hit>=SEED_MIN_HITS && O[i][j].miss==0){   // reliable spots only: edge spots miss once turned
        int k=i; if(mirror){ float mx=b.x0+b.x1-cx(i); k=std::min(GX-1,std::max(0,(int)(mx*GX))); }
        if(!N[k][j].hit){ N[k][j].hit=1; n++; } }
    for(int i=0;i<GX;i++) for(int j=0;j<ny;j++) if(O[i][j].hit==0 && O[i][j].miss>0){   // and its known misses (off the piece)
        int k=i; if(mirror){ float mx=b.x0+b.x1-cx(i); k=std::min(GX-1,std::max(0,(int)(mx*GX))); }
        if(!N[k][j].hit) N[k][j].miss=1; }
    g_seedMirror[side]=mirror; g_seedDecided[side]=false; g_seedMisses=0;
    logf("side %d: starting from %d spots of the other side%s",side,n,mirror?" (mirrored)":" (as they were)");
}
static void seed_result(bool hit){                  // first strokes on a newly seeded side
    if(g_seedDecided[g_side] || g_ci<0) return;
    if(hit){ g_seedDecided[g_side]=true; logf("side %d: %s spots hit",g_side,g_seedMirror[g_side]?"mirrored":"unmirrored"); return; }
    if(++g_seedMisses>=2){ logf("side %d: %s spots missed twice -> trying them the other way",g_side,g_seedMirror[g_side]?"mirrored":"unmirrored");
        seed_side(g_side,!g_seedMirror[g_side]); g_seedDecided[g_side]=true; }
}
static void do_flip(const char* why){       // the game's own Flip action (only valid at the idle anvil)
    g_flipped=true; g_sinceFlip=0; g_zeroStreak=0; g_preAimed=false;
    act0(A_FLIP); setph(Flip,why);
}
static bool planned_flip_due(float c){
    if(g_flipAtPct<=0 || g_flipped || g_flipDisabled) return false;
    float at=g_flipAtPct/100.f;
    return c>=at || (g_pass>=2 && g_passStrokes==0 && c>=at*0.5f);   // a new anvil pass has no rhythm to lose
}
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
    g_fresh=0; g_flips=0; g_afterFlipLog=0; g_oneMore=false; g_sideMisses=0; g_pass=0; g_passStrokes=0;
    g_pathMirror[0]=false; g_pathMirror[1]=true; g_mirrorDecided[0]=true; g_mirrorDecided[1]=false;   // side 0 is the traced side; turned over = mirrored
    g_seedDecided[0]=g_seedDecided[1]=true; g_seedMisses=0;
    if(!strcmp(wpid(),"horseshoe")){ g_path=HORSESHOE; g_pn=(int)(sizeof HORSESHOE/sizeof HORSESHOE[0]); logf("using seeded horseshoe path (%d spots)",g_pn); }
    Bounds b=bounds(); g_2d=is2d(b);
    char z[64]; logzones(z,sizeof z);
    logf("recipe=%s",recipeId(g_m));
    g_batchAuto=false;
    logf("START piece=%s zones=[%s] bounds=(%.2f,%.2f)-(%.2f,%.2f) %s st=%d comp=%.3f q=%.3f stam=%.0f",wpid(),z,b.x0,b.y0,b.x1,b.y1,g_2d?"2D":"1D",st(),comp(),qual(),stamina());
    int s=st();
    if(s==ST_FORGE) setph(Heat,"start at forge");
    else if(s==ST_ANVIL||s==ST_FINISHSTROKE){ g_pass=1; setph(Work,"start at anvil"); }
    else { logf("not at forge/anvil (state %d); F9 when Henry is at the forge",s); g_ph=Idle; }
}

static void tick(float dt){
    uint64_t now=g_clock();
    bool pressed=g_key.pressed();
    if(pressed){ if(g_ph==Idle) start(); else { stop("user key"); batch_cancel("stopped with the key"); } return; }
    if(g_ph==Idle){
        int s0=st();
        if(g_batchAuto && F<uintptr_t>(g_m,M_WP) && (s0==ST_FORGE||s0==ST_ANVIL)){
            logf("batch: forging piece %d of %d %s",g_batchTotal-g_batchLeft+1,g_batchTotal,g_batchRecipe); start();
        }
        return;
    }
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

    if(c>=0.999f && g_ph!=Finish && g_sub!=Charging && !g_oneMore){ bellows(false); setph(Finish,"completion reached"); }

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
        if(s==ST_ANVIL){ Bounds b=bounds(); g_2d=is2d(b); g_sub=Ready; g_lastStart=0; g_pass++; g_passStrokes=0;
            g_fresh=RHYTHM_STROKES;                    // a new anvil pass starts without rhythm: aim at fresh metal first
            setph(Work,"at anvil"); }
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
            bool turned=g_t!=0;
            if(!turned){ g_flipDisabled=true; logf("flip did nothing -> disabled"); }
            else {
                g_side^=1; g_flips++; g_fresh=RHYTHM_STROKES; g_afterFlipLog=RHYTHM_STROKES+2; g_sideMisses=0;
                { int ny=g_2d?GY:1, any=0; for(int i=0;i<GX;i++) for(int j=0;j<ny;j++) any+=g_cells[g_side][i][j].hit+g_cells[g_side][i][j].miss;
                  if(!any && g_2d && !g_path) seed_side(g_side,true); }
                memset(g_pmiss,0,sizeof g_pmiss); memset(g_poff,0,sizeof g_poff);       // the other side is new ground
                logf("flipped, side=%d (next %d strokes aim at fresh spots)",g_side,RHYTHM_STROKES);
            }
            g_sub=Ready; g_lastStart=now; setph(Work,"flip done");
            if(turned && g_2d){ if(!path_point()) choose_point(); g_preAimed=true; aim(); }   // hammer travels during the beat
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
            if(g_afterFlipLog>0){ g_afterFlipLog--;      // always logged: shows what a flip cost
                logf("after flip: stroke#%d side=%d aim=(%.2f,%.2f) %s work=%.2f charge=%.2f gain=%.3f dq=%.4f q=%.3f bpm=%.1f",
                    g_strokeNo,g_side,g_tx,g_ty,hit?"HIT":"MISS",g_w0,F<float>(g_m,M_CHARGE),gain,dq,qual(),F<float>(g_m,M_BPM)); }
            if(g_2d){ if(g_path) path_result(hit); else seed_result(hit); }
            g_sub=Ready; g_sinceFlip++; g_oneMore=false;
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
            do_flip(c>=0.5f?"half done":"stalled"); break;
        }
        if(planned_flip_due(c)){                    // turn the piece over once, like a smith would
            if(s==ST_ANVIL){ do_flip(c>=g_flipAtPct/100.f? "planned: half way" : "planned: new anvil pass"); break; }
            if(now-g_lastStart<STROKE_PERIOD+1500) break;   // wait for the idle anvil; the flip resets the rhythm anyway
        }
        if(now-g_lastStart<STROKE_PERIOD) break;
        if(!g_2d){ int sp=sweep_point();
            if(sp==0){ if(s!=ST_ANVIL) break; logf("no hot section -> reheat"); act0(A_TOFORGE); setph(GoForge,"reheat"); break; }
            if(sp==2){ if(s!=ST_ANVIL) break;
                if(!g_flipDisabled && g_sinceFlip>=2){ logf("side worked out -> flip"); do_flip("side done"); break; }
                if(!g_forced){ g_forced=true; logf("both sides worked out at every hot spot -> taking best remaining spots"); }
                if(sweep_point()!=1) break; } }
        g_strokeNo++; g_passStrokes++; if(g_2d && !g_preAimed){ if(!path_point()) choose_point(); } g_preAimed=false;
        if(g_fresh>0) g_fresh--;
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
        // completion reads 0.999x but the game only finishes at 100%: the piece is still on the idle anvil -> one more stroke
        if((s==ST_ANVIL||s==ST_FINISHSTROKE) && inPh>2500 && !g_t){ logf("not finished at comp=%.4f -> one more stroke",c); g_oneMore=true; g_sub=Ready; g_lastStart=0; setph(Work,"not finished yet"); }
        if(h==2 && g_t!=2){ g_t=2; logf("quench reached: q=%.3f",qual()); stop("done (confirm the result yourself)"); batch_quenched(); }
        if(inPh>60000) stop("finish timeout");
        break;
    }
    default: break;
    }
}

// [Options] WatchForge=1: read-only research log for a future auto-loop option.  Logs every change of the forge
// state (1 = recipe menu, 2 = recipe chosen, 4.. = piece in progress, 21/22 = leaving), the model and the recipe id
// (CryString at model+8), so a test shows what the game does after a quench.  Reads only; changes nothing.
static bool g_watch=false; static uintptr_t g_wBs=0, g_wM=0; static int g_wSt=-1, g_wHard=-1; static uint64_t g_wT0=0;
static void watch(uintptr_t bs,uintptr_t m){
    if(!m) return;
    int s=F<uint8_t>(m,M_STATE), h=F<uint8_t>(m,M_HARD);
    if(bs==g_wBs && m==g_wM && s==g_wSt && h==g_wHard) return;
    uint64_t now=g_clock(); if(!g_wT0) g_wT0=now;
    logf("watch +%.1fs: state %d -> %d  hard %d -> %d  bs %s  model %s  recipe=%s  workpiece=%s  phase=%s",(now-g_wT0)/1000.0,
        g_wSt,s,g_wHard,h, bs==g_wBs?"same":"NEW", m==g_wM?"same":"NEW", recipeId(m), F<uintptr_t>(m,M_WP)?"yes":"none", PN[g_ph]);
    g_wBs=bs; g_wM=m; g_wSt=s; g_wHard=h;
}

// ---------------- [Options] Queue=1: forge a batch, chosen on the game's own recipe page ----------------
// On the forge's recipe menu, + and - set how many of the shown recipe to make.  The game's own recipe page then
// shows it: "[Hunting Sword] Common x2" with every required amount doubled.  The page data is the game's
// C_UIFlashContentRecipeBlacksmith (built by 0x119F710 for the crafting page 0x119F030); we only change that display
// copy after the game made it.  The owned counts on it are the game's own, so the most you can pick is what the
// game says you can afford.  Recipe data, material checks and material use are not touched: each piece is still
// picked and confirmed in the game's menu, which checks and takes the materials as usual.  Pieces of a batch are
// forged without F9; after each quench the page shows how many are left for that recipe.
static const char* SIG_PAGE = "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 70 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 F0 48 8B 41 08 4C 8B F9 41 8A D8 4C 8B E2 48 8B 88 90 00 00 00 4C 8B A9 68 06 00 00 E8 ?? ?? ?? ?? 45 32 F6 41 8A F6 84 DB 0F 85 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 49 8B D4 FF 50 10 4D 8B C5 48 8D 4D C0 48 8B D0 48 8B F0 E8 ?? ?? ?? ?? 48 8D 55 C0 48 8D 4D E0 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B D8 48 8D 4D B0 E8 ?? ?? ?? ?? 49 8D BF F0 01 00 00 4C 8B C3 48 8B CF 48 8D 55 B0 E8 ?? ?? ?? ?? 48 8D 4D B0 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D B0 E8 ?? ?? ?? ??";   // 0x119F030 crafting page: show recipe (this, CryString id [callee frees], type 0 = blacksmith)
static const char* SIG_PAGE_MAKE = "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 83 3D ?? ?? ?? ?? 00 49 8B F0 48 8B EA 48 8B F9 75 ?? E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 54 24 30 45 33 C0 41 8D 48 70 FF D0";   // 0x119F710 make_shared<C_UIFlashContentRecipeBlacksmith>(recipe, soul)
static const char* SIG_PAGE_CTOR = "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 90 48 81 EC 70 01 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 60 48 8B 02 4C 8B E1 48 8B CA 4D 8B F0 4C 8B FA FF 50 08 48 8B D0 48 8D 4C 24 38 E8 ?? ?? ?? ?? 48 8B D8 49 8B CF 49 8B 07 FF 10 45 33 C0 48 8D 4C 24 20 48 8B D0 E8 ?? ?? ?? ?? 4C 8B C3 48 8B D0 49 8B CC E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 49 8D 4C 24 18 49 89 04 24 E8 ?? ?? ?? ?? 49 8D 4C 24 20 E8 ?? ?? ?? ?? 49 C7 44 24 28 00 00 00 00 49 C7 44 24 30 00 00 00 00 49 C7 44 24 38 00 00 00 00 49 C7 44 24 48 00 00 00 00 49 C7 44 24 50 00 00 00 00 49 C7 44 24 58 00 00 00 00 E8 ?? ?? ?? ?? 49 8B 07 49 8B CF FF 50 10 48 8B D0 49 8D 4C 24 18 E8 ?? ?? ?? ??";   // 0x119F7E8 page content ctor: name CryString +0x10, ingredient rows vector +0x48
static const char* SIG_PAGE_ROWS = "E8 ?? ?? ?? ?? 48 8B 4E 08 48 8D 54 24 58 E8 ?? ?? ?? ?? 48 8B 13 48 8B CB 8B F8 FF 52 08 48 8B 76 10 48 8D 54 24 20 48 8D 4C 24 40 8B D8 E8 ?? ?? ?? ?? 48 8D 54 24 30 48 8D 4C 24 48 E8 ?? ?? ?? ?? 48 8B 56 50 4C 8D 44 24 40 89 7C 24 50 89 5C 24 54 48 3B 56 58 74 ?? E8 ?? ?? ?? ?? 48 83 46 50 18";   // 0x1F5BEC5 ingredient row {name, icon, owned +0x10, required +0x14}, 0x18 bytes
static const uintptr_t PG_CALL=0x74, PG_STR=0x94, PG_STRFREE=0xB3, MK_CTOR=0x81, CT_ASSIGN=0xD8;   // call sites inside those functions
static const uintptr_t PC_NAME=0x10, PC_ROWS=0x48, ROW_SIZE=0x18, ROW_OWNED=0x10, ROW_NEED=0x14;
typedef void** (*MakeContent_t)(void**, uintptr_t, uintptr_t);
typedef void (*ShowPage_t)(uintptr_t, void*, uint8_t);
typedef void (*StrFromC_t)(void*, const char*);
typedef void (*StrFree_t)(void*);
typedef void (*StrAssign_t)(void*, const void*);
static MakeContent_t pMakeContent=nullptr; static ShowPage_t pShowPage=nullptr;
static StrFromC_t pStrFromC=nullptr; static StrFree_t pStrFree=nullptr; static StrAssign_t pStrAssign=nullptr;
static volatile uintptr_t* g_pageSlot=nullptr;    // the hook stub stores the page object (r15 in 0x119F030) here
static bool g_queue=false;
static uint64_t g_menuSeen=0;                     // last tick the forge showed its recipe menu (state 1)
static uintptr_t g_pageObj=0, g_pageVt=0;         // crafting page that showed the current recipe
static char g_pageRecipe[96]={0}; static int g_pageQty=1, g_pageCap=0;
static char g_pageLog[400]={0};
static kc::Hotkey g_plus, g_minus, g_padPlus, g_padMinus;
static int g_lastSt=-1;

static const char* cstr(uintptr_t cryString){ const char* s=cryString? *(const char**)cryString : nullptr; return (s && kc::readable((uintptr_t)s,1))? s : "?"; }
static bool page_open(){ return g_menuSeen && g_clock()-g_menuSeen<1500; }

// Runs right after the game built the recipe page data, before the game sends it to the page.
static void page_content(uintptr_t obj, uintptr_t content, uintptr_t recipe){
    if(!content || !recipe) return;
    const char* id=cstr(((uintptr_t(*)(uintptr_t))(*(void***)recipe)[0])(recipe));   // recipe vtbl[0] = id CryString
    uintptr_t b=F<uintptr_t>(content,PC_ROWS), e=F<uintptr_t>(content,PC_ROWS+8);
    int n=(b && e>b && (e-b)%ROW_SIZE==0)? (int)((e-b)/ROW_SIZE) : 0;
    if(n<1 || n>12 || !kc::readable(b,e-b)){ logf("recipe page: %s has an unexpected material list (%d rows) - left as the game made it",id,n); return; }
    if(strcmp(id,g_pageRecipe)){
        strncpy_s(g_pageRecipe,id,_TRUNCATE);
        g_pageQty=(g_batchLeft>0 && !strcmp(id,g_batchRecipe))? g_batchLeft : 1;
    }
    int cap=1<<30; char line[400]; size_t k=0; line[0]=0;
    for(int i=0;i<n;i++){
        uintptr_t r=b+i*ROW_SIZE; int have=F<int>(r,ROW_OWNED), need=F<int>(r,ROW_NEED);
        if(k<sizeof line-80) k+=sprintf_s(line+k,sizeof line-k,"%s%s %d/%d",i?", ":"",cstr(r),have,need);
        if(need<=0 || have<0 || need>100000){ cap=-1; break; }
        cap=std::min(cap,have/need);
    }
    g_pageObj=obj; g_pageVt=obj? *(uintptr_t*)obj : 0; g_pageCap=cap;
    int qty=(cap>=1)? std::clamp(g_pageQty,1,cap) : 1;
    if(qty!=g_pageQty) logf("recipe page: %s x%d lowered to x%d (materials for %d)",id,g_pageQty,qty,cap);
    g_pageQty=qty;
    char sig[400]; sprintf_s(sig,"%s|%s|%d",id,line,qty);
    if(strcmp(sig,g_pageLog)){ strcpy_s(g_pageLog,sig); logf("recipe page: %s \"%s\" owned/required: %s -> materials for %d, showing x%d",id,cstr(content+PC_NAME),line,cap,qty); }
    if(qty<=1 || !page_open()) return;   // changes only ever happen on the forge's own recipe menu
    for(int i=0;i<n;i++){ uintptr_t r=b+i*ROW_SIZE; F<int>(r,ROW_NEED)*=qty; }
    char title[200]; sprintf_s(title,"%s x%d",cstr(content+PC_NAME),qty);
    void* s=nullptr; pStrFromC(&s,title); pStrAssign((void*)(content+PC_NAME),&s); pStrFree(&s);
}
static void** MakeContentHook(void** out, uintptr_t recipe, uintptr_t soul){
    void** r=pMakeContent(out,recipe,soul);
    __try{ page_content(g_pageSlot? *g_pageSlot : 0,(uintptr_t)out[0],recipe); }
    __except(EXCEPTION_EXECUTE_HANDLER){ logf("recipe page: fault while adjusting the page - left as the game made it"); }
    return r;
}
// Asks the game to show the recipe page again (the same call the game makes when you pick the recipe in the list).
static bool page_refresh(){
    if(!page_open() || !g_pageObj || !g_pageRecipe[0] || !kc::readable(g_pageObj,8) || *(uintptr_t*)g_pageObj!=g_pageVt){
        logf("recipe page: cannot redraw (page not open or changed) - pick the recipe in the list again"); return false;
    }
    void* s=nullptr; pStrFromC(&s,g_pageRecipe);
    pShowPage(g_pageObj,&s,0);   // the game frees s
    return true;
}
static void page_keys(int s){
    bool plus=g_plus.pressed(), minus=g_minus.pressed();
    plus=g_padPlus.pressed()||plus; minus=g_padMinus.pressed()||minus;
    if(!(plus^minus) || s!=1 || F<uintptr_t>(g_m,M_WP) || !page_open() || !g_pageRecipe[0]) return;
    int want=g_pageQty+(plus?1:-1);
    if(want<1){ return; }
    if(g_pageCap<1 || want>g_pageCap){ logf("recipe page: + ignored, materials for %d %s",g_pageCap<0?0:g_pageCap,g_pageRecipe); return; }
    g_pageQty=want;
    logf("recipe page: %c -> x%d %s",plus?'+':'-',want,g_pageRecipe);
    page_refresh();
}
static void batch_cancel(const char* why){
    if(g_batchLeft>0) logf("batch: cancelled (%s), %d of %d %s not made",why,g_batchLeft,g_batchTotal,g_batchRecipe);
    g_batchLeft=0; g_batchTotal=0; g_batchRecipe[0]=0; g_batchAuto=false;
}
// The game left its recipe menu with a piece chosen (and paid for) through its own menu.
static void batch_chosen(){
    const char* id=recipeId(g_m);
    int qty=(!strcmp(id,g_pageRecipe))? g_pageQty : 1;
    if(g_batchLeft>0 && !strcmp(id,g_batchRecipe)){
        if(qty<g_batchLeft){ logf("batch: lowered from %d left to %d on the recipe page",g_batchLeft,qty); g_batchLeft=qty; }
        logf("batch: piece %d of %d %s chosen in the game's menu",g_batchTotal-g_batchLeft+1,g_batchTotal,id);
        g_batchAuto=true; return;
    }
    if(g_batchLeft>0) batch_cancel("a different recipe was chosen");
    if(qty<=1) return;
    strncpy_s(g_batchRecipe,id,_TRUNCATE); g_batchTotal=g_batchLeft=qty; g_batchAuto=true;
    logf("batch: x%d %s started (game showed materials for %d); piece 1 of %d",qty,id,g_pageCap,qty);
}
static void batch_quenched(){
    if(g_batchLeft<=0 || strcmp(recipeId(g_m),g_batchRecipe)) return;
    g_batchLeft--;
    if(g_batchLeft>0) logf("batch: %d of %d %s done; %d left - pick the recipe in the game's menu and confirm it (the page shows x%d)",
                           g_batchTotal-g_batchLeft,g_batchTotal,g_batchRecipe,g_batchLeft,g_batchLeft);
    else { logf("batch: all %d %s done",g_batchTotal,g_batchRecipe); batch_cancel("finished"); }
}
static void batch_tick(int s){
    if(s==1) g_menuSeen=g_clock();
    if(g_lastSt==1 && s!=1){   // the recipe menu closed: with a piece chosen (2..20) or without (21 = leaving)
        if(s>=2 && s<=20) batch_chosen();
        g_pageObj=0; g_pageRecipe[0]=0; g_pageQty=1; g_pageLog[0]=0; g_menuSeen=0;
    }
    g_lastSt=s;
    page_keys(s);
}
// Hook page for the one call this patches (0x119F030 -> 0x119F710): store r15 (the page), then jump to the hook.
static uint8_t* alloc_near(uintptr_t target){
    SYSTEM_INFO si; GetSystemInfo(&si);
    for(uintptr_t d=0x10000; d<0x70000000; d+=si.dwAllocationGranularity)
        for(int sg=-1;sg<=1;sg+=2){
            uintptr_t a=(target+sg*(intptr_t)d) & ~(uintptr_t)(si.dwAllocationGranularity-1);
            if(void* p=VirtualAlloc((void*)a,0x1000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE)) return (uint8_t*)p;
        }
    return nullptr;
}
static bool hook_page(uintptr_t site, uintptr_t factory){
    if(*(uint8_t*)site!=0xE8 || kc::rel32(site+1)!=factory){ logf("batch: recipe page call is not what this build expects - batches off"); return false; }
    uint8_t* pg=alloc_near(site); if(!pg){ logf("batch: no memory for the recipe page hook - batches off"); return false; }
    g_pageSlot=(volatile uintptr_t*)(pg+0x800);
    uint8_t* p=pg;
    p[0]=0x4C; p[1]=0x89; p[2]=0x3D; *(int32_t*)(p+3)=(int32_t)((uint8_t*)g_pageSlot-(p+7)); p+=7;   // mov [rip+slot], r15
    p[0]=0xFF; p[1]=0x25; *(int32_t*)(p+2)=0; p+=6; *(uint64_t*)p=(uint64_t)&MakeContentHook;          // jmp [rip+0] -> hook
    FlushInstructionCache(GetCurrentProcess(),pg,0x40);
    pMakeContent=(MakeContent_t)factory;
    DWORD old; VirtualProtect((void*)site,5,PAGE_EXECUTE_READWRITE,&old);
    *(int32_t*)(site+1)=(int32_t)((uintptr_t)pg-(site+5));
    VirtualProtect((void*)site,5,old,&old); FlushInstructionCache(GetCurrentProcess(),(void*)site,5);
    return true;
}
struct QFound { uintptr_t page, make, ctor, rows, strFromC, strFree, strAssign; };
static bool resolve_queue(kc::Resolver& R, QFound& q){
    int before=R.fails;
    q.page=R.find("recipe page",SIG_PAGE);
    q.make=R.branch("recipe page data",q.page? q.page+PG_CALL : 0);
    if(q.make) R.verify("recipe page data",q.make,SIG_PAGE_MAKE);
    q.ctor=R.branch("recipe page data ctor",q.make? q.make+MK_CTOR : 0);
    if(q.ctor) R.verify("recipe page data ctor",q.ctor,SIG_PAGE_CTOR);
    q.rows=R.find("recipe page material rows",SIG_PAGE_ROWS);
    q.strFromC=R.branch("string from text",q.page? q.page+PG_STR : 0);
    q.strFree=R.branch("string free",q.page? q.page+PG_STRFREE : 0);
    q.strAssign=R.branch("string assign",q.ctor? q.ctor+CT_ASSIGN : 0);
    return R.fails==before;
}

static void UpdateHook(void* self,float dt){
    pOrigUpdate(self,dt);
    if(g_dead) return;
    uintptr_t bs=(uintptr_t)self;
    __try{
        uintptr_t m=F<uintptr_t>(bs,BS_MODEL);
        if(g_watch) watch(bs,m);
        if(bs!=g_bs||m!=g_m){ if(g_ph!=Idle) logf("session changed -> idle"); g_ph=Idle; g_bs=bs; g_m=m; g_lastSt=-1; }
        if(m && g_queue) batch_tick(F<uint8_t>(m,M_STATE));
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
    kc::Resolver R; R.im.load(game); Found f{}; QFound q{}; bool ok=resolve(R,f); ok=resolve_queue(R,q) && ok;
    uintptr_t* p=(uintptr_t*)&f; size_t n=0; for(size_t i=0;i<sizeof f/8;i++) out[n++]=p[i];
    p=(uintptr_t*)&q; for(size_t i=0;i<sizeof q/8;i++) out[n++]=p[i];
    return ok? (int)n : -R.fails;
}
// Offline forge simulator hooks (tools/forgesim.cpp): a fake C_Blacksmithing/model/workpiece in plain memory.
extern "C" __declspec(dllexport) void KC_SimInit(uintptr_t bs, void* workmap, uint64_t (*clock)(), int flipAt, const char* logPath){
    g_bs=bs; g_m=F<uintptr_t>(bs,BS_MODEL); pWorkMap=(uintptr_t)workmap; g_clock=clock; g_flipAtPct=flipAt;
    kc::g_debug=true; if(kc::g_log) fclose(kc::g_log); fopen_s(&kc::g_log,logPath,"w"); g_ph=Idle; g_dead=false;
}
extern "C" __declspec(dllexport) void KC_SimStart(){ start(); }
extern "C" __declspec(dllexport) int KC_SimTick(){ tick(0.02f); return (int)g_ph; }
#endif

static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_autoforge");
    g_key.vk=kc::ini_key("Forge",VK_F9);
    g_flipAtPct=std::min(95,std::max(0,kc::ini_int("Options","FlipAt",50)));
    g_watch=kc::ini_int("Options","WatchForge",0)!=0;
    g_queue=kc::ini_int("Options","Queue",0)!=0;
    g_plus.vk=kc::ini_key("IncreaseQueue",VK_OEM_PLUS); g_minus.vk=kc::ini_key("DecreaseQueue",VK_OEM_MINUS);
    g_padPlus.vk=VK_ADD; g_padMinus.vk=VK_SUBTRACT;
    HMODULE h=kc::wait_game(); if(!h){ logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h); B=R.im.base;
    logf("start v1.2.0-test (game ts 0x%08X) flip at %d%%%s%s",R.im.timestamp,g_flipAtPct,g_watch?", forge watch on (read-only)":"",g_queue?", batches on the recipe page":"");
    Found f{};
    if(!resolve(R,f)){ logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    pWorkMap=f.workmap;
    if(g_queue){   // optional: a failure here only turns batches off; forging works as before
        QFound q{}; int fails=R.fails;
        if(resolve_queue(R,q)){
            pShowPage=(ShowPage_t)q.page; pStrFromC=(StrFromC_t)q.strFromC; pStrFree=(StrFree_t)q.strFree; pStrAssign=(StrAssign_t)q.strAssign;
            g_queue=hook_page(q.page+PG_CALL,q.make);
        } else g_queue=false;
        R.fails=fails;
        char kp[16],km[16];
        logf(g_queue? "batch: ready (%s / %s on the forge's recipe page)" : "batch: off (this game version is not supported for batches)",
             kc::key_name(g_plus.vk,kp,sizeof kp),kc::key_name(g_minus.vk,km,sizeof km));
    }
    void** slot=&((void**)f.vt)[19];
    pOrigUpdate=(Update_t)f.update;
    kc::write_ptr(slot,(void*)&UpdateHook);
    char kn[16]; logf("ready (%s at the forge)",kc::key_name(g_key.vk,kn,sizeof kn));
    return 0;
}
KC_PLUGIN("Theatrical AutoForge", KC_AUTHOR, 112)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); } return TRUE; }
