// autofollow.cpp -- Theatrical AutoFollow for Kingdom Come: Deliverance II (kcd2_autofollow.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Lets Henry follow any NPC with the game's own follow action ("Follow" in the chat focus, the same
// prompt the game shows for guides), not just the ones the game marks as followable. The game itself
// does all of the following (Henry walking/running after the NPC, cancelling when the player moves). No
// pathfinding of our own, no teleporting, nothing written into NPCs, quests or saves.
//
// How the game decides who can be followed (build 15693, static analysis, see NOTES.md):
//   CanFollowTarget 0x18082E974 (chat focus prompt, and every update of the follow action 0x18287DE7C)
//   asks 0x18129DA18(out, entityId) whether the target's soul carries a "followable NPC" side effect:
//   type 0x38 followableNPCCenter / 0x39 followableNPCLeft / 0x3A followableNPCRight (the side-effect
//   name table at 0x180BE51C3). Result {position, valid}. A dead or otherwise excluded soul is refused
//   before that (soul vtbl[0x38]/[0x30]), and CanFollowTarget keeps all its other gates (target state,
//   distance, mounted, ...).
//   The mod changes one jump: when none of the three side effects is present, instead of "not
//   followable" the function answers "followable, center" - exactly what the game's own (unused)
//   switch promises: wh_player_FollowAlwaysCanFollow "When enabled player can follow anyone as if they
//   had followable NPC side effect". 2 bytes, written atomically, put back when switched off.
//
// The switch itself (round 1): the mod also still sets wh_player_FollowAlwaysCanFollow (cheat-locked,
// VF_CHEAT, so written at its storage). Round 1 showed it set to 1 with no effect; no code reading it
// was found in this build. Kept, harmless, in case some path does read it.
//
// Use: on by default. ] (kcd2_autofollow.ini) switches it off and back on.
// Log: kcd2_autofollow.log next to the DLL.
#include "kcd2_common.h"
#include <intrin.h>

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
// The followable side-effect checks in 0x18129DA18: HasSideEffect(0x38) -> jne center; 0x39 -> jne left;
// 0x3A; test; mov rax,rdi; jne right; mov byte [rdi+1],0 (not followable) <- patched at +0x51.  (0x18129DAD1)
static const char* SIG_FOLLOWABLE = "B2 38 49 8B CE 4C 8B 00 FF D3 84 C0 75 ?? 49 8B 06 48 8B CE 48 8B 98 80 00 00 00 48 8B 06 FF 10 "
                                    "B2 39 49 8B CE 4C 8B 00 FF D3 84 C0 75 ?? 49 8B 06 48 8B CE 48 8B 98 80 00 00 00 48 8B 06 FF 10 "
                                    "B2 3A 49 8B CE 4C 8B 00 FF D3 84 C0 48 8B C7 75 ?? C6 47 01 00";
static const char* SIG_CENTER = "66 C7 07 00 01";      // mov word [rdi],0x100: {center, followable}
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
static volatile uint16_t* g_patch=nullptr; // the "not followable" store in 0x18129DA18
static uint16_t g_patchOrig=0, g_patchNew=0;

// ---------------- on-screen notice (optional) ----------------
typedef void* (*SsUpdate_t)(void*,void*,void*,void*);
typedef bool  (*SsExec_t)(void*,const char*,size_t,const char*,void*);
static SsUpdate_t g_ssUpdate=nullptr; static SsExec_t g_ssExec=nullptr;
static const char* volatile g_notice=nullptr;
// Plain text, not a localization key: round 2 showed the "@taf_on" keys unresolved on screen.
static const char* const NOTE_ON  = "Game.SendInfoText(\"AutoFollow: you can follow anyone\", false)";
static const char* const NOTE_OFF = "Game.SendInfoText(\"AutoFollow: off (vanilla following)\", false)";
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
struct Found { uintptr_t call, getter, regfn, reg, block, var, reg2, draw; };   // the cvar (optional)
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
struct FoundF { uintptr_t site, patch, center; };
static bool resolve_followable(kc::Resolver& R, FoundF& f){
    f.site=R.find("followable side-effect check",SIG_FOLLOWABLE);
    if(f.site){
        f.patch=f.site+0x51;
        f.center=f.site+0x0E+(int8_t)*(const uint8_t*)(f.site+0x0D);
        R.verify("followable: center result",f.center,SIG_CENTER);
        intptr_t d=(intptr_t)f.center-(intptr_t)(f.patch+2);
        if(d<-128 || d>127) R.fail("followable","jump out of range");
        if(f.patch&1) R.fail("followable","patch site not 2-byte aligned");
    }
    return R.ok();
}
// Atomic 2-byte code write (aligned, so a running thread sees either the old or the new instruction).
static void write_patch(uint16_t v){
    if(!g_patch || *g_patch==v) return;
    DWORD old; VirtualProtect((void*)g_patch,2,PAGE_EXECUTE_READWRITE,&old);
    _InterlockedExchange16((volatile short*)g_patch,(short)v);
    VirtualProtect((void*)g_patch,2,old,&old); FlushInstructionCache(GetCurrentProcess(),(void*)g_patch,2);
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
    kc::Resolver R0; R0.im.load(game); FoundF ff{}; bool okF=resolve_followable(R0,ff);
    out[0]=ff.site; out[1]=ff.patch; out[2]=ff.center; out+=3;
    if(!okF) return -R0.fails;
    kc::Resolver R; R.im=R0.im; Found f{}; bool ok=resolve(R,f);
    if(ok){ kc::Resolver R2; R2.im=R.im; f.reg2=f.reg+0x2A; R2.verify("next registration",f.reg2,SIG_REGISTER_NEXT);
            f.draw=field_at(R2,DRAW_NAME,f.reg2,f.block,DRAW_NAME,nullptr); }
    uintptr_t* p=(uintptr_t*)&f; for(size_t i=0;i<sizeof f/8;i++) out[i]=p[i];
    return ok? (int)(sizeof f/8)+3 : -R.fails;
}
#endif

// ---------------- the loop (own thread, 4x a second) ----------------
// The followable patch is put in or taken out when the key switches. The cvar (if found) is re-checked:
// the game registers it (and so resets it to 0) when it sets up the player cvars, possibly later.
static void apply(bool on){
    write_patch(on? g_patchNew : g_patchOrig);
    logf("{\"ev\":\"follow_anyone\",\"on\":%d}",(int)on);
}
static void loop(){
    int32_t orig=0, origDraw=0; bool haveOrig=false;
    int sets=0; bool was=g_on;
    for(;;){
        if(g_key.vk && g_key.pressed()){
            g_on=!g_on; logf("{\"ev\":\"toggle\",\"on\":%d}",(int)g_on);
            apply(g_on);
            if(g_notices && g_ssExec) g_notice= g_on? NOTE_ON : NOTE_OFF;
        }
        if(g_on && g_var){
            int32_t v=*g_var;
            if(v!=1){
                if(!haveOrig){ orig=v; haveOrig=true; }
                *g_var=1; sets++;
                if(sets<=5) KC_DLOG("{\"ev\":\"set\",\"cvar\":\"" CVAR_NAME "\",\"from\":%d,\"to\":1,\"n\":%d}",v,sets);
            }
            if(g_draw && g_debugDraw && *g_draw!=1){ origDraw=*g_draw; *g_draw=1; }
        } else if(!g_on && was && g_var){
            if(haveOrig) *g_var=orig;
            if(g_draw && g_debugDraw) *g_draw=origDraw;
        }
        was=g_on;
        Sleep(250);
    }
}
#ifdef KC_DEVTOOLS
// tools/fulltest: what the toggle key does, without a keyboard.
extern "C" __declspec(dllexport) void KC_DevToggle(){ g_on=!g_on; logf("{\"ev\":\"toggle\",\"on\":%d,\"dev\":1}",(int)g_on); apply(g_on); }
#endif
static void run_loop(){
    __try { loop(); } __except(EXCEPTION_EXECUTE_HANDLER){ logf("{\"ev\":\"error\",\"msg\":\"key handling stopped after an exception - follow-anyone stays as it is\"}"); }
}

static HMODULE g_self=nullptr;
static DWORD WINAPI Init(LPVOID){
    kc::init(g_self,"kcd2_autofollow");
    int defKey=kc::parse_key("]",0); if(!defKey) defKey=VK_OEM_6;   // ] on the active keyboard layout
    g_key.vk=kc::ini_key("Toggle",defKey);
    g_on=kc::ini_int("Options","Enabled",1)!=0;
    g_debugDraw=kc::ini_int("Options","DebugDraw",0)!=0;
    g_notices=kc::ini_int("Options","Notices",1)!=0;
    bool useCvar=kc::ini_int("Options","SetCvar",1)!=0;
    HMODULE h=kc::wait_game(); if(!h){ logf("{\"ev\":\"abort\",\"msg\":\"WHGame.dll not loaded\"}"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("{\"ev\":\"start\",\"version\":\"1.0.0\",\"game_ts\":\"0x%08X\"}",R.im.timestamp);
    FoundF ff{};
    if(!resolve_followable(R,ff)){ logf("{\"ev\":\"disabled\",\"msg\":\"this game version is not supported yet - nothing was changed\"}"); return 0; }
    g_patch=(volatile uint16_t*)ff.patch;
    uint16_t cur=*g_patch;
    g_patchNew=(uint16_t)(0xEB | ((uint8_t)(int8_t)(ff.center-(ff.patch+2))<<8));   // jmp center
    if(cur==g_patchNew) g_patchOrig=0x47C6;                  // already patched (live reload)
    else if(cur==0x47C6) g_patchOrig=cur;                    // C6 47: mov byte [rdi+1],0
    else { logf("{\"ev\":\"disabled\",\"msg\":\"followable check has unexpected code (0x%04X) - nothing was changed\"}",cur); return 0; }
    if(useCvar){
        kc::Resolver Rc; Rc.im=R.im; Found f{};
        if(resolve(Rc,f)){ g_var=(volatile int32_t*)f.var; if(g_debugDraw) resolve_draw(R.im,f); }
        else logf("{\"ev\":\"cvar_off\",\"msg\":\"" CVAR_NAME " not found - not needed\"}");
    }
    if(g_notices) install_notices(R.im);
    if(g_on) apply(true);
    char kn[24];
    logf("{\"ev\":\"ready\",\"on\":%d,\"key\":\"%s\",\"patch_rva\":\"0x%llX\",\"cvar\":%d,\"debug_draw\":%d,\"notices\":%d}",
         (int)g_on,kc::key_name(g_key.vk,kn,sizeof kn),(unsigned long long)(ff.patch-R.im.base),(int)(g_var!=nullptr),(int)(g_draw!=nullptr),(int)(g_ssExec!=nullptr));
    run_loop();
    return 0;
}
KC_PLUGIN("Theatrical AutoFollow", KC_AUTHOR, 21)
BOOL WINAPI DllMain(HINSTANCE h,DWORD r,LPVOID){ if(r==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); g_self=h; KC_START(Init); } return TRUE; }
