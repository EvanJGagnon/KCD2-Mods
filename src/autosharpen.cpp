// autosharpen.cpp -- Theatrical AutoSharpening for Kingdom Come: Deliverance II (kcd2_autosharpen.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Plays the grindstone minigame through the same C_Sharpening functions the player's keys call:
// the pedal (xi_pedaling), blade position (sword_position_left/right) and blade angle
// (sword_rotation_up/down). The game itself grinds the blade, scores it and changes the weapon's
// condition; the mod never writes any of that. Ticks after C_Sharpening's own per-frame update.
// When the edge is sharp Henry gets up the way he does when the player presses the exit key: the game's
// own exit request (C_Sharpening vtbl[46]) queues the "out" animation - blade off the wheel, put away,
// stand up. It is sent from the script system's update, outside the minigame's own update.
// Use: sit at a grindstone, pick a weapon, press F7 (kcd2_autosharpen.ini). F7 again stops.
// Game code is found by RTTI + signature at startup (kcd2_common.h); on a mismatch nothing is hooked.
// Log: kcd2_autosharpen.log next to the DLL.
//
// How the minigame works (build 15693, see docs/notes.txt):
//   The blade has 1-5 segments (sharpness 0..1 each, array at +0xC8); the blade position picks one.
//   Every frame the game adds  time * wheel speed * pressure * efficiency * 0.5  to that segment.
//   Efficiency comes from the blade angle: ramps up to 1 over [0, A], stays 1 on [A, B], falls to 0 on
//   [B, C] and is negative (damage) above C, where A, B, C widen with skill from 0.55/0.55/0.7 to
//   0.2/0.84/0.99 - so an angle of 0.55 is ideal at every skill. The wheel knocks the angle down by up to
//   0.029 now and then (SharpeningWeaponBouncingMaxOffset), which the player has to correct.
//   The wheel slows by ~0.29/s; each pedal push adds 0.3 and costs stamina. The game calls a
//   weapon sharp at every segment >= 0.99; the mod goes on to 100%.
#include "kcd2_common.h"
#include <cmath>
#include <algorithm>

static uintptr_t B = 0;
static uint64_t (*g_clock)() = GetTickCount64;   // dev builds: tools/sharpsim drives time
typedef void (*Update_t)(void*);
static Update_t pOrigUpdate = nullptr;
using kc::logf;
template<class T> static T& F(uintptr_t p,uintptr_t o){ return *(T*)(p+o); }

// Signatures (build 15693 addresses in comments). Struct offsets in them stay literal.
static const char* SIG_UPDATE = "40 53 48 83 EC 20 83 B9 8C 01 00 00 07";                         // C_Sharpening vtbl[14] 0x2EAF400
static const char* SIG_PEDAL  = "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 8B D9 40 38 79 2C";       // 0x2EA9088 (xi_pedaling handler)
static const char* SIG_POSDIR = "40 53 48 83 EC 20 F3 0F 11 89 94 00 00 00";                      // 0x2EB05F4 (sword_position_left/right)
static const char* SIG_ROTDIR = "40 53 48 83 EC 20 F3 0F 11 89 98 00 00 00";                      // 0x2EB0708 (sword_rotation_up/down)
// Getting up: the exit request the exit key sends (if not already leaving: 0x2EACB40(this, false) builds a
// C_HumanSharpeningOutAction and queues it; its Enter starts the graceful exit 0x2EAE9FC(this, false)).
// Sharpening.Stop() from Lua is the forced variant (vtbl[5]: no animation) and is not used.
static const char* SIG_EXITREQ   = "48 83 EC 28 80 B9 35 02 00 00 00 75 ?? 33 D2 E8 ?? ?? ?? ?? 48 83 C4 28";   // C_Sharpening vtbl[46] 0x2EAE494
// ...sent from CScriptSystem::Update (vtbl[1], game thread, every frame), chained like Horse Route Follow.
static const char* SIG_SS_UPDATE = "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 48 8B F1 33 D2 0F 29 74 24 20";   // 0xA26B94

// ---------------- C_Sharpening layout (build 15693) ----------------
static const uintptr_t S_ACTOR=0x18, S_ACTIVE=0x2C, S_EFF=0x84, S_POS=0x8C, S_ROT=0x90, S_SPEED=0xAC,
    S_SEGS=0xC8, S_NSEG=0xD8, S_SEGW=0xDC, S_STATE=0x18C;
static const int ST_SHARPENING=4;
typedef bool (*Pedal_t)(void*);              // returns true only when the wheel hit full speed
typedef void (*Dir_t)(void*, float);         // -1..1 like a key/stick; |v|<0.3 = stop, >=0.7 = full speed
static Pedal_t pPedal=nullptr; static Dir_t pPosDir=nullptr, pRotDir=nullptr;
typedef void* (*SsUpdate_t)(void*,void*,void*,void*);
typedef void  (*ExitReq_t)(void*);
static SsUpdate_t g_ssUpdate=nullptr; static ExitReq_t g_exitReq=nullptr;

// ---------------- tunables ----------------
static const float DONE=0.9995f;             // every segment at 100% (the game itself calls it done at 0.99)
static const float FINE=0.99f;               // above this: hold the best angle precisely instead of searching
static const float ANGLE_IDEAL=0.55f;        // inside the ideal band at every skill level
static const float ANGLE_MIN=0.15f, ANGLE_MAX=0.68f;   // the damaging angles start at 0.7 (lowest skill)
static const float END=0.46f;                // blade travel: end to end (the game clamps at +-0.5)
static const float PASS_DULL=0.32f, PASS_SHARP=0.6f;   // pass speed over dull / already sharp parts (stick units)
static const float PEDAL_BELOW=0.8f;         // push the pedal when the wheel is slower than this
static const uint64_t PEDAL_GAP=650;         // ...but not more often than a person would
static const float STAM_REST=12.f, STAM_GO=40.f;   // stop pedalling below, start again above
static const uint64_t EXIT_DELAY=900;        // pause after the last pass before getting up

static uintptr_t g_s=0;
static kc::Hotkey g_key; static bool g_on=false, g_dead=false, g_autoExit=true; static int g_insane=0;
static int g_dir=1, g_passes=0, g_flat=0, g_pedals=0, g_pedalFails=0; static bool g_toEnd=true, g_resting=false;
static float g_angle=ANGLE_IDEAL, g_lastPos=0, g_lastRot=0, g_passSum=0;
static uint64_t g_lastPedal=0, g_logT=0, g_t0=0; static volatile uint64_t g_exitAt=0;

static int   state(){ return F<int>(g_s,S_STATE); }
static bool  active(){ return F<uint8_t>(g_s,S_ACTIVE)!=0; }
static int   nseg(){ int n=F<int>(g_s,S_NSEG); return (n>=1&&n<=5)? n:0; }
static float segv(int i){ uintptr_t p=F<uintptr_t>(g_s,S_SEGS); return p? F<float>(p,4*i) : -1.f; }
static float segw(){ return F<float>(g_s,S_SEGW); }
static float pos(){ return F<float>(g_s,S_POS); }
static float rot(){ return F<float>(g_s,S_ROT); }
static float speed(){ return F<float>(g_s,S_SPEED); }
static float eff(){ return F<float>(g_s,S_EFF); }
static float stamina(){ uintptr_t a=F<uintptr_t>(g_s,S_ACTOR); if(!a) return -1; uintptr_t s=F<uintptr_t>(a,0x668); return s? F<float>(s,0x70C):-1; }
static int   seg_at(float p){ int n=nseg(); float w=segw(); return std::min(n-1,std::max(0,(int)((p+0.5f)/w))); }
static float seg_sum(){ float s=0; for(int i=0;i<nseg();i++) s+=segv(i); return s; }
static float seg_min(){ float m=9; for(int i=0;i<nseg();i++) m=std::min(m,segv(i)); return m; }
static void segs_str(char* out,size_t cap){ int n=nseg(); size_t k=0; out[0]=0; for(int i=0;i<n;i++) k+=sprintf_s(out+k,cap-k,"%s%.3f",i?"/":"",segv(i)); }

// Like holding a key: only send a new value when it changes (press / release).
static void send(Dir_t fn,float& last,float v){ if(fabsf(v-last)>0.05f){ fn((void*)g_s,v); last=v; } }
static float steer(float err,float tol,float gain){       // proportional, but outside the game's dead zone
    if(fabsf(err)<tol) return 0.f;
    float m=std::min(1.f,std::max(0.32f,fabsf(err)*gain)); return err>0? m:-m;
}
static void release(){ send(pPosDir,g_lastPos,0.f); send(pRotDir,g_lastRot,0.f); }

static void stop(const char* why){
    if(!g_on) return;
    release(); g_on=false;
    char z[64]; segs_str(z,sizeof z);
    logf("stop: %s  segments=[%s] passes=%d pedals=%d time=%.1fs",why,z,g_passes,g_pedals,(g_clock()-g_t0)/1000.0);
}
static void finish(uint64_t now,const char* how){
    release(); g_on=false;
    char z[64]; segs_str(z,sizeof z);
    logf("done: segments=[%s] (%s) passes=%d pedals=%d time=%.1fs",z,how,g_passes,g_pedals,(now-g_t0)/1000.0);
    if(g_autoExit && g_exitReq) g_exitAt=now+EXIT_DELAY;
    else if(g_autoExit) logf("cannot get up automatically in this game version - press the exit key");
}

// ---------------- blade angle: the game's own per-frame efficiency (+0x84) is the feedback ----------------
// Negative = the edge is being damaged (angle too steep): back off at once and never go that steep again.
// Below 1 = outside the ideal band: follow the slope of efficiency over angle (the wheel's knocks spread the
// samples).  0 while grinding a dull part = it is as sharp as this angle allows: go back to the best angle
// seen (or 0.55) and look around it.  Only frames over a part that still needs work, with the blade
// already at its angle, count.
static float g_cEst=1.f, g_bLo=2.f, g_bHi=-1.f, g_sx[128], g_sy[128]; static int g_sn=0, g_explore=0; static uint64_t g_angT=0, g_dmgLogT=0;
static float best_angle(){ return g_bHi>=g_bLo? 0.5f*(g_bLo+g_bHi) : ANGLE_IDEAL; }
static void angle_feedback(uint64_t now,float r,float sp,float v){
    if(sp<0.3f || r<0.05f) return;                    // not grinding, or the blade is still being raised
    float e=eff();
    if(e<0.f){
        g_cEst=std::min(g_cEst,r); g_angle=std::min(g_angle,r-0.04f); g_sn=0; g_angT=now;
        if(now-g_dmgLogT>1500){ g_dmgLogT=now; logf("angle %.2f is damaging the edge (efficiency %.2f) -> %.2f",r,e,g_angle); }
        return;
    }
    if(v>=DONE || fabsf(r-g_angle)>0.04f) return;     // over a finished part, or still turning the blade
    if(e>=0.99f){ g_bLo=std::min(g_bLo,r); g_bHi=std::max(g_bHi,r); g_explore=0; }
    if(g_sn<128){ g_sx[g_sn]=r; g_sy[g_sn]=e; g_sn++; }
    if(now-g_angT<500) return;
    g_angT=now; int n=g_sn; g_sn=0; if(n<8) return;
    int k=0; double mx=0,my=0; for(int i=0;i<n;i++) if(g_sy[i]>0){ k++; mx+=g_sx[i]; my+=g_sy[i]; }
    if(!k){                                           // capped at this angle
        float to=best_angle();
        if(fabsf(g_angle-to)>0.015f || v>=FINE){ g_angle=to; return; }   // the last 1%: just hold the best angle closely
        static const float look[6]={+0.04f,-0.04f,+0.08f,-0.08f,+0.12f,-0.12f};
        if(g_explore<6){ g_angle=to+look[g_explore++]; KC_DLOG("capped at %.3f -> trying angle %.2f",v,g_angle); }
        return;
    }
    mx/=k; my/=k;
    if(my>=0.985) return;                             // on the ideal band
    double sxy=0,sxx=0; for(int i=0;i<n;i++) if(g_sy[i]>0){ sxy+=(g_sx[i]-mx)*(g_sy[i]-my); sxx+=(g_sx[i]-mx)*(g_sx[i]-mx); }
    float step= sxx>1e-6? (sxy>0? 0.02f:-0.02f) : (best_angle()>g_angle? 0.02f:-0.02f);
    g_angle+=step;
    KC_DLOG("efficiency %.2f around %.2f -> angle %.2f",my,mx,g_angle);
}

static void start(){
    int n=nseg(); float w=segw();
    if(state()!=ST_SHARPENING || !active() || !n || !(w>0.f&&w<=1.01f)){ logf("not sharpening yet (state %d); press the key once Henry is at the grindstone with a weapon",state()); return; }
    g_on=true; g_t0=g_clock(); g_pedals=g_pedalFails=0; g_lastPedal=0; g_resting=false; g_angle=ANGLE_IDEAL; g_exitAt=0;
    g_cEst=1.f; g_bLo=2.f; g_bHi=-1.f; g_sn=0; g_angT=0; g_explore=0;
    g_lastPos=g_lastRot=0; g_passes=0; g_flat=0; g_toEnd=true; g_dir= pos()<=0? -1 : 1; g_passSum=seg_sum();
    char z[64]; segs_str(z,sizeof z);
    logf("START segments=%d [%s] pos=%.2f angle=%.2f wheel=%.2f stamina=%.0f -> starting at the %s end",n,z,pos(),rot(),speed(),stamina(),g_dir<0?"left":"right");
    if(seg_min()>=DONE){
        g_on=false;
        if(g_autoExit && g_exitReq){ g_exitAt=g_clock()+300; logf("already sharp - getting up"); } else logf("already sharp - nothing to do");
    }
}

static void tick(){
    uint64_t now=g_clock();
    if(g_key.pressed()){ if(g_on) stop("user key"); else if(!g_exitAt) start(); return; }
    if(!g_on) return;
    if(state()!=ST_SHARPENING || !active()){ stop("minigame ended"); return; }
    int n=nseg(); float w=segw(), p=pos(), r=rot(), sp=speed();
    if(!n || !(w>0.f&&w<=1.01f) || !(p>-0.6f&&p<0.6f) || !(r>-0.1f&&r<1.1f) || !(sp>-0.1f&&sp<1.6f)){
        stop("unexpected grindstone state");
        if(++g_insane>=3){ g_dead=true; logf("disabled: grindstone data does not look right (game updated?)"); }
        return;
    }
    if(seg_min()>=DONE){ finish(now,"all sharp"); return; }

    // ---- wheel: pedal in a steady rhythm, rest when out of breath ----
    float st=stamina();
    if(st>=0){ if(!g_resting && st<STAM_REST){ g_resting=true; logf("out of breath (stamina %.0f) - resting",st); }
               else if(g_resting && st>=STAM_GO){ g_resting=false; logf("rested (stamina %.0f)",st); } }
    if(!g_resting && sp<PEDAL_BELOW && now-g_lastPedal>=PEDAL_GAP){
        pPedal((void*)g_s); g_lastPedal=now;
        if(speed()>sp+0.01f) g_pedals++; else if(++g_pedalFails%10==1) logf("pedal did nothing (stamina %.0f)",st);
    }

    // ---- blade position: to one end first, then steady passes from end to end ----
    float target=g_dir*END;
    if(g_toEnd){
        if(fabsf(p-target)<0.015f){ g_toEnd=false; g_dir=-g_dir; KC_DLOG("at the %s end - first pass",g_dir>0?"left":"right"); }
        else send(pPosDir,g_lastPos,steer(target-p,0.01f,6.f));
    }
    if(!g_toEnd){
        target=g_dir*END;
        if((g_dir>0 && p>=target-0.005f) || (g_dir<0 && p<=target+0.005f)){        // reached the far end
            g_passes++; float s=seg_sum();
            char z[64]; segs_str(z,sizeof z); KC_DLOG("pass %d done: segments=[%s]",g_passes,z);
            if(s-g_passSum<0.001f && !g_resting && sp>0.4f){ if(++g_flat>=2){ finish(now,"as sharp as this stone gets"); return; } }
            else g_flat=0;
            g_passSum=s; g_dir=-g_dir; target=g_dir*END;
        }
        float v= segv(seg_at(p))<DONE? PASS_DULL : PASS_SHARP;    // linger where it is still dull
        send(pPosDir,g_lastPos, g_resting? 0.f : g_dir*v);
    }

    // ---- blade angle: hold it in the ideal band, correct the wheel's knocks ----
    angle_feedback(now,r,sp,segv(seg_at(p)));
    g_angle=std::min(std::min(ANGLE_MAX,g_cEst-0.04f),std::max(ANGLE_MIN,g_angle));
    send(pRotDir,g_lastRot,steer(g_angle-r, segv(seg_at(p))>=FINE? 0.003f:0.006f, 12.f));

    if(now-g_logT>2000){ g_logT=now; char z[64]; segs_str(z,sizeof z);
        KC_DLOG("pass=%d dir=%+d segments=[%s] pos=%.3f angle=%.3f/%.2f eff=%.2f wheel=%.2f stamina=%.0f",g_passes,g_dir,z,p,r,g_angle,eff(),sp,st); }
}

static void UpdateHook(void* self){
    pOrigUpdate(self);
    if(g_dead) return;
    __try{
        if((uintptr_t)self!=g_s){ if(g_on) logf("grindstone changed -> stop"); g_on=false; g_s=(uintptr_t)self; }
        tick();
    }__except(EXCEPTION_EXECUTE_HANDLER){ logf("exception in tick"); g_on=false; }
}
// Getting up after the last pass: the script line runs from the script system, not from inside the minigame.
static void* SsUpdateHook(void* self,void* a,void* b,void* c){
    void* r=g_ssUpdate(self,a,b,c);
    uint64_t at=g_exitAt;
    if(at && g_clock()>=at){
        g_exitAt=0;
        __try{
            if(g_s && state()==ST_SHARPENING && active()){ g_exitReq((void*)g_s); logf("getting up from the grindstone"); }
        }__except(EXCEPTION_EXECUTE_HANDLER){ g_exitReq=nullptr; logf("could not get up automatically - press the exit key"); }
    }
    return r;
}

struct Found { uintptr_t vt, update, pedal, posdir, rotdir; };
static bool resolve(kc::Resolver& R, Found& f){
    f.vt=R.vtable("C_Sharpening",".?AVC_Sharpening@playermodule@wh@@",0);
    f.update=R.slot("C_Sharpening::Update",f.vt,14,SIG_UPDATE);
    f.pedal=R.find("pedal",SIG_PEDAL);
    f.posdir=R.find("blade position",SIG_POSDIR);
    f.rotdir=R.find("blade angle",SIG_ROTDIR);
    return R.ok();
}
// Optional: without it the mod works, Henry just stays seated when the edge is sharp.
struct FoundSS { uintptr_t vt, update, exitreq; };
static bool resolve_exit(kc::Resolver& R, uintptr_t sharpVt, FoundSS& f){
    f.exitreq=R.slot("C_Sharpening exit request",sharpVt,46,SIG_EXITREQ);
    f.vt=R.vtable("CScriptSystem",".?AVCScriptSystem@@",0);
    uintptr_t cur= f.vt? ((uintptr_t*)f.vt)[1] : 0;
    if(cur && !R.im.in_image(cur)){ if(R.find("CScriptSystem::Update",SIG_SS_UPDATE)) f.update=cur; }   // another mod hooked it first: chain
    else f.update=R.slot("CScriptSystem::Update",f.vt,1,SIG_SS_UPDATE);
    return R.ok();
}
#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out){   // offline check (tools/selftest)
    kc::Resolver R; R.im.load(game); Found f{}; FoundSS g{}; bool ok=resolve(R,f) && resolve_exit(R,f.vt,g);
    uintptr_t* p=(uintptr_t*)&f; for(size_t i=0;i<sizeof f/8;i++) out[i]=p[i];
    uintptr_t* q=(uintptr_t*)&g; for(size_t i=0;i<sizeof g/8;i++) out[sizeof f/8+i]=q[i];
    return ok? (int)((sizeof f+sizeof g)/8) : -R.fails;
}
// Offline simulator hooks (tools/sharpsim.cpp): a fake C_Sharpening plus fake pedal/position/angle functions.
extern "C" __declspec(dllexport) void KC_SimInit(uintptr_t s, void* pedal, void* posdir, void* rotdir, uint64_t (*clock)(), const char* logPath){
    g_s=s; pPedal=(Pedal_t)pedal; pPosDir=(Dir_t)posdir; pRotDir=(Dir_t)rotdir; g_clock=clock;
    kc::g_debug=true; if(kc::g_log) fclose(kc::g_log); fopen_s(&kc::g_log,logPath,"w"); g_on=false; g_dead=false;
}
extern "C" __declspec(dllexport) void KC_SimStart(){ start(); }
extern "C" __declspec(dllexport) int KC_SimTick(){ tick(); return g_on? 1 : (g_exitAt? 2 : 0); }
#endif

static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_autosharpen");
    g_key.vk=kc::ini_key("Sharpen",VK_F7);
    g_autoExit=kc::ini_int("Options","AutoExit",1)!=0;
    HMODULE h=kc::wait_game(); if(!h){ logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h); B=R.im.base;
    logf("start v1.0.0 (game ts 0x%08X)",R.im.timestamp);
    Found f{};
    if(!resolve(R,f)){ logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    pPedal=(Pedal_t)f.pedal; pPosDir=(Dir_t)f.posdir; pRotDir=(Dir_t)f.rotdir;
    if(g_autoExit){
        kc::Resolver R2; R2.im=R.im; FoundSS g{};
        if(resolve_exit(R2,f.vt,g)){ g_exitReq=(ExitReq_t)g.exitreq; g_ssUpdate=(SsUpdate_t)g.update; kc::write_ptr(&((void**)g.vt)[1],(void*)&SsUpdateHook); }
        else logf("getting up automatically is not available in this game version (the rest works)");
    }
    void** slot=&((void**)f.vt)[14];
    pOrigUpdate=(Update_t)f.update;
    kc::write_ptr(slot,(void*)&UpdateHook);
    char kn[16]; logf("ready (%s at the grindstone)%s",kc::key_name(g_key.vk,kn,sizeof kn),g_autoExit? ", gets up when done":"");
    return 0;
}
KC_PLUGIN("Theatrical AutoSharpening", KC_AUTHOR, 100)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); } return TRUE; }
