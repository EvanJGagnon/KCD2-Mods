// autofollow.cpp -- Theatrical AutoFollow for Kingdom Come: Deliverance II (kcd2_autofollow.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Lets Henry follow any walking NPC with the game's own follow action, not just the ones the game marks
// as followable. The game already has the switch for this, a player console variable:
//   wh_player_FollowAlwaysCanFollow  "When enabled player can follow anyone as if they had followable
//                                     NPC side effect"
// The mod turns it on and keeps it on; the game does all of the following itself (the follow prompt,
// Henry walking/running after the NPC, cancelling when the player moves). No pathfinding of our own, no
// teleporting, nothing written into NPCs, quests or saves.
//
// Why not just "wh_player_FollowAlwaysCanFollow 1" in user.cfg: the whole player cvar group is
// registered with flags 2 (VF_CHEAT), so the console, user.cfg and Lua's System.SetCVar refuse to change
// it outside developer mode. The variable is a plain int the game reads directly (registered by pointer,
// no change callback), so the mod writes that int - exactly what the console does in developer mode.
//
// How it is found (build 15693, static analysis of WHGame.dll, see NOTES.md):
//   * player cvar registration 0x1815A7864(this = player cvar block) registers, at 0x1815A8337,
//       Register("wh_player_FollowAlwaysCanFollow", &this->+0x3E0, default 0, flags ebp=2, help, no callback)
//     and right after it (0x1815A8361) "wh_player_FollowDebugDraw" at +0x3E4.
//   * the block is a function-local static returned by getter 0x1804AADE0 (lea rax,[block] at +0x27),
//     called and passed to the registration at 0x181064609:  call getter; mov rcx,rax; call register.
//   The registration site is checked against the cvar's own name and help text at runtime, and its
//   offset (+0x3E0) is read from the instruction, so a layout change is followed or refused - never guessed.
//
// Use: on by default. F10 (kcd2_autofollow.ini) switches it off and back on; off restores the value the
// variable had before. Optional on-screen notice through the script system (as Horse Route Follow).
// Log: kcd2_autofollow.log next to the DLL.
#include "kcd2_common.h"

using kc::logf;

#define CVAR_NAME "wh_player_FollowAlwaysCanFollow"
#define CVAR_HELP "When enabled player can follow anyone"
#define DRAW_NAME "wh_player_FollowDebugDraw"

// Signatures (build 15693 addresses in comments). Struct offsets stay literal except the cvar's own
// field offset, which is read from the matched instruction and range-checked.
// call <player cvar block getter>; mov rcx,rax; call <player cvar registration>; ...  (0x181064609)
static const char* SIG_CVARS_CALL = "E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 90 B8 00 00 00 48 8B C8 BA 07 00 00 00";
// getter: function-local static (thread-safe init); "lea rax,[block]" at +0x27                (0x1804AADE0)
static const char* SIG_GETTER = "48 83 EC 28 8B 0D ?? ?? ?? ?? 65 48 8B 04 25 58 00 00 00 BA 18 00 00 00 48 8B 04 C8 8B 04 02 39 05 ?? ?? ?? ?? 7F 0C 48 8D 05";
// lea rax,[help]; mov [rsp+28],rdi(0: no callback); lea rdx,[r14+0x3E0]; mov [rsp+20],rax; mov r9d,ebp(flags);
// lea rcx,[name]; xor r8d,r8d (default 0); call RegisterInt                                     (0x1815A8337)
static const char* SIG_REGISTER = "48 8D 05 ?? ?? ?? ?? 48 89 7C 24 28 49 8D 96 E0 03 00 00 48 89 44 24 20 44 8B CD 48 8D 0D ?? ?? ?? ?? 45 33 C0 E8";
// the next registration, same shape, field +0x3E4 (wildcarded here, checked by name)               (0x1815A8361)
static const char* SIG_REGISTER_NEXT = "48 8D 05 ?? ?? ?? ?? 48 89 7C 24 28 49 8D 96 ?? ?? 00 00 48 89 44 24 20 44 8B CD 48 8D 0D ?? ?? ?? ?? 45 33 C0 E8";
// CScriptSystem, as in Horse Route Follow and AutoSharpening: only for the optional on-screen notice.
static const char* SIG_SS_UPDATE = "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 48 8B F1 33 D2 0F 29 74 24 20";   // vtbl[1] 0xA26B94
static const char* SIG_SS_EXEC   = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 48 8B F9 48 89 50 E8"; // vtbl[6] ExecuteBuffer 0x4D4484

// ---------------- settings and state ----------------
static kc::Hotkey g_key;
static volatile bool g_on=true;           // [Options] Enabled
static bool g_debugDraw=false;            // [Options] DebugDraw: the game's own follow debug drawing
static bool g_notices=true;               // [Options] Notices
static volatile int32_t* g_var=nullptr;   // wh_player_FollowAlwaysCanFollow
static volatile int32_t* g_draw=nullptr;  // wh_player_FollowDebugDraw (optional)

// ---------------- on-screen notice (optional) ----------------
typedef void* (*SsUpdate_t)(void*,void*,void*,void*);
typedef bool  (*SsExec_t)(void*,const char*,size_t,const char*,void*);
static SsUpdate_t g_ssUpdate=nullptr; static SsExec_t g_ssExec=nullptr;
static const char* volatile g_notice=nullptr;
static const char* const NOTE_ON  = "Game.SendInfoText(\"@taf_on\", true)";
static const char* const NOTE_OFF = "Game.SendInfoText(\"@taf_off\", true)";
static void* SsUpdateHook(void* self,void* a,void* b,void* c){
    void* r=g_ssUpdate(self,a,b,c);
    if(g_notice && g_ssExec){
        const char* line=(const char*)InterlockedExchangePointer((void* volatile*)&g_notice,nullptr);
        __try { if(line) g_ssExec(self,line,strlen(line),"TheatricalAutoFollow",nullptr); }
        __except(EXCEPTION_EXECUTE_HANDLER){ g_ssExec=nullptr; logf("{\"ev\":\"notice_error\"}"); }
    }
    return r;
}
// Hook vtbl[1], chaining to whatever is there. Our other mods hook the same slot from their own init
// threads, so the swap is a compare-exchange: if the slot changed under us, chain to the new occupant.
static bool hook_update(uintptr_t vt, uintptr_t expect){
    void** slot=&((void**)vt)[1];
    DWORD old; if(!VirtualProtect(slot,8,PAGE_READWRITE,&old)) return false;
    void* cur=(void*)expect; bool ok=false;
    for(int i=0;i<8 && !ok;i++){
        g_ssUpdate=(SsUpdate_t)cur;
        void* was=InterlockedCompareExchangePointer(slot,(void*)&SsUpdateHook,cur);
        if(was==cur) ok=true; else cur=was;
    }
    VirtualProtect(slot,8,old,&old);
    return ok;
}

// ---------------- resolve ----------------
static bool str_at(const kc::Resolver& R, uintptr_t a, const char* want, bool prefix){
    size_t n=strlen(want)+(prefix? 0:1);
    return a && R.im.in_image(a) && kc::readable(a,n) && !memcmp((const void*)a,want,n);
}
// The cvar field at a registration site: name checked, offset from "lea rdx,[r14+disp32]" (site+0x0F).
static uintptr_t field_at(kc::Resolver& R, const char* what, uintptr_t site, uintptr_t block, const char* name, const char* help){
    if(!site || !block) return 0;
    uintptr_t nm=R.riprel(what,site+0x1E), hp=R.riprel(what,site+0x03);
    if(!str_at(R,nm,name,false)){ R.fail(what,"registration is for another variable"); return 0; }
    if(help && !str_at(R,hp,help,true)){ R.fail(what,"help text differs"); return 0; }
    int32_t off=*(const int32_t*)(site+0x0F);
    if(off<0 || off>0x4000 || (off&3)){ R.fail(what,"field offset out of range"); return 0; }
    return block+(uint32_t)off;
}
struct Found { uintptr_t call, getter, regfn, reg, block, var, reg2, draw; };
static bool resolve(kc::Resolver& R, Found& f){
    f.call=R.find("player cvar setup call",SIG_CVARS_CALL);
    f.getter=R.branch("player cvar getter",f.call);
    f.regfn=f.call? R.branch("player cvar registration",f.call+8) : 0;
    if(R.verify("player cvar getter",f.getter,SIG_GETTER)) f.block=R.riprel("player cvar block",f.getter+0x2A);
    f.reg=R.find(CVAR_NAME " registration",SIG_REGISTER);
    if(f.reg && f.regfn && (f.reg<f.regfn || f.reg>f.regfn+0x4000)) R.fail(CVAR_NAME " registration","not inside the player cvar registration");
    f.var=field_at(R,CVAR_NAME,f.reg,f.block,CVAR_NAME,CVAR_HELP);
    if(f.var && (R.im.in_text(f.var) || !kc::readable(f.var,4))) R.fail(CVAR_NAME,"field is not data");
    return R.ok();
}
static void resolve_draw(const kc::Image& im, const Found& f){     // optional
    kc::Resolver R; R.im=im;
    uintptr_t s=f.reg+0x2A;
    R.verify(DRAW_NAME " registration",s,SIG_REGISTER_NEXT);
    uintptr_t d=R.ok()? field_at(R,DRAW_NAME,s,f.block,DRAW_NAME,nullptr) : 0;
    if(R.ok() && d) g_draw=(volatile int32_t*)d;
    else logf("{\"ev\":\"debug_draw_off\",\"msg\":\"" DRAW_NAME " not found - DebugDraw ignored\"}");
}
static void install_notices(const kc::Image& im){                  // optional
    kc::Resolver R; R.im=im;
    uintptr_t vt=R.vtable("CScriptSystem",".?AVCScriptSystem@@",0), up=0;
    uintptr_t cur= vt? ((uintptr_t*)vt)[1] : 0;
    if(cur && !R.im.in_image(cur)){ if(R.find("CScriptSystem::Update",SIG_SS_UPDATE)) up=cur; }   // another mod hooked it first: chain
    else up=R.slot("CScriptSystem::Update",vt,1,SIG_SS_UPDATE);
    uintptr_t ex=R.slot("CScriptSystem::ExecuteBuffer",vt,6,SIG_SS_EXEC);
    if(!R.ok()){ logf("{\"ev\":\"notices_off\",\"msg\":\"on-screen notices not available in this game version\"}"); return; }
    g_ssExec=(SsExec_t)ex;
    if(!hook_update(vt,up)){ g_ssExec=nullptr; logf("{\"ev\":\"notices_off\",\"msg\":\"could not hook the script system\"}"); }
}

#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out){   // offline check (tools/selftest)
    kc::Resolver R; R.im.load(game); Found f{}; bool ok=resolve(R,f);
    if(ok){ kc::Resolver R2; R2.im=R.im; f.reg2=f.reg+0x2A; R2.verify("next registration",f.reg2,SIG_REGISTER_NEXT);
            f.draw=field_at(R2,DRAW_NAME,f.reg2,f.block,DRAW_NAME,nullptr); }
    uintptr_t* p=(uintptr_t*)&f; for(size_t i=0;i<sizeof f/8;i++) out[i]=p[i];
    return ok? (int)(sizeof f/8) : -R.fails;
}
#endif

// ---------------- the loop (own thread, 4x a second) ----------------
// The game registers the variable (and so resets it to 0) when it sets up the player cvars, which may be
// after this thread starts; the loop simply puts it back whenever it is not what the player chose.
static void loop(){
    int32_t orig=*g_var, origDraw= g_draw? *g_draw : 0; bool haveOrig=false;
    int sets=0; bool was=!g_on;
    for(;;){
        if(g_key.vk && g_key.pressed()){
            g_on=!g_on; logf("{\"ev\":\"toggle\",\"on\":%d}",(int)g_on);
            if(g_notices && g_ssExec) g_notice= g_on? NOTE_ON : NOTE_OFF;
        }
        if(g_on){
            int32_t v=*g_var;
            if(v!=1){
                if(!haveOrig){ orig=v; haveOrig=true; }
                *g_var=1; sets++;
                if(sets<=5 || was!=g_on) logf("{\"ev\":\"set\",\"cvar\":\"" CVAR_NAME "\",\"from\":%d,\"to\":1,\"n\":%d}",v,sets);
                else if(sets==6) logf("{\"ev\":\"set\",\"msg\":\"the game keeps resetting the variable - further resets not logged\"}");
            }
            if(g_draw && g_debugDraw && *g_draw!=1){ origDraw=*g_draw; *g_draw=1; }
        } else if(was!=g_on){
            if(haveOrig){ *g_var=orig; logf("{\"ev\":\"restore\",\"cvar\":\"" CVAR_NAME "\",\"to\":%d}",orig); }
            if(g_draw && g_debugDraw) *g_draw=origDraw;
        }
        was=g_on;
        Sleep(250);
    }
}

static void run_loop(){
    __try { loop(); } __except(EXCEPTION_EXECUTE_HANDLER){ logf("{\"ev\":\"error\",\"msg\":\"stopped after an exception - the variable keeps its current value\"}"); }
}

static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_autofollow");
    g_key.vk=kc::ini_key("Toggle",VK_F10);
    g_on=kc::ini_int("Options","Enabled",1)!=0;
    g_debugDraw=kc::ini_int("Options","DebugDraw",0)!=0;
    g_notices=kc::ini_int("Options","Notices",1)!=0;
    HMODULE h=kc::wait_game(); if(!h){ logf("{\"ev\":\"abort\",\"msg\":\"WHGame.dll not loaded\"}"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("{\"ev\":\"start\",\"version\":\"0.1.0\",\"game_ts\":\"0x%08X\"}",R.im.timestamp);
    Found f{};
    if(!resolve(R,f)){ logf("{\"ev\":\"disabled\",\"msg\":\"this game version is not supported yet - nothing was changed\"}"); return 0; }
    g_var=(volatile int32_t*)f.var;
    if(g_debugDraw) resolve_draw(R.im,f);
    if(g_notices) install_notices(R.im);
    char kn[16];
    logf("{\"ev\":\"ready\",\"on\":%d,\"key\":\"%s\",\"var_rva\":\"0x%llX\",\"value\":%d,\"debug_draw\":%d,\"notices\":%d}",
         (int)g_on,kc::key_name(g_key.vk,kn,sizeof kn),(unsigned long long)(f.var-R.im.base),(int)*g_var,(int)(g_draw!=nullptr),(int)(g_ssExec!=nullptr));
    run_loop();
    return 0;
}
KC_PLUGIN("Theatrical AutoFollow", KC_AUTHOR, 10)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); } return TRUE; }
