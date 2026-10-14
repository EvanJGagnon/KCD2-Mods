// autopickpocket.cpp -- Theatrical AutoPickpocket for Kingdom Come: Deliverance II (kcd2_autopickpocket.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Plays the pickpocketing minigame for Henry with the game's own inputs, the way a careful player would:
//  * Charge: the player starts the pickpocket and holds the key as usual. The mod watches the same safety
//    ring the player sees and lets go of the key for them (the game's own "charge" release action) once the
//    ring falls to the configured safety. The victim's detection roll, the timers and the skill checks are
//    the game's; a bad roll still gets Henry caught.
//  * Grid: the mod moves the selector, uncovers and takes items with the same UI calls the left/right/
//    uncover/take keys make (MoveLeft, MoveRight, DoAction). The selector turns at the game's speed, every
//    item costs its time, covered items stay unknown until uncovered, and it heads for the exit with time
//    to spare. Run out of time away from the exit and Henry is caught, exactly as for a player.
// Nothing is written into the minigame, the items, the inventory or the victim. Items move only when the
// game itself closes the grid.
//
// How it hooks in (build 15693, see NOTES.md for addresses and how they were found):
//   C_Pickpocketing vtbl[19] Update(this, dt)          - our tick, after the game's own
//   I_UIPickpocketing [0] Open(state) [2] SetBoostProgress(progress, minimum, seconds, safety)
//                                                        - the grid's contents / the safety ring
//   C_UIFlashPickpocketingItem vtbl[1] FillUIArgs      - item names and prices as the UI shows them
//   C_UIPickpocketing::OnActionHintState, ::OnClose    - (inline) the UI reporting what it accepted
//   C_UIFlashBase::CallFunction(ui, name)               - (called) how the game sends a key to the grid
//   charge action functor                               - (called) how the game releases the charge key
//
// Use: off by default; F5 (kcd2_autopickpocket.ini) switches it on and off at any time. While it plays the
// grid keep your hands off the grid keys (switching it off hands the grid back to you at once).
// Log: kcd2_autopickpocket.log next to the DLL.
#include "kcd2_common.h"
#include "pp_logic.h"

using kc::logf;

#define VERSION "0.1.0"

// ---------------- signatures (build 15693 RVAs in comments) ----------------
// C_Pickpocketing::Update: mov ecx,[rcx+78h] (state) right after the stack cookie                 0x2EB3B34
static const char* SIG_UPDATE = "48 89 5C 24 10 55 48 8D 6C 24 A9 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 47 48 8B D9 8B 49 78";
// "charge" action functor _Do_call: this->pp; state 1 -> give up (vtbl+28h), state 2 -> open the grid   0x203DEE0
static const char* SIG_CHARGE = "48 8B 49 08 8B 51 78 83 EA 01 74 ?? 83 FA 01 75 ?? E9 ?? ?? ?? ?? 48 8B 01";
// C_UIFlashBase::CallFunction(ui, const CryString& name)                                          0x566BCC
static const char* SIG_CALLFN = "48 89 5C 24 08 57 48 83 EC 60 48 8B 01 48 8B FA 48 8B D9 FF 50 60 48 8B 5B 48 48 85 DB 74 ??";
// C_UIPickpocketing::OnActionHintState(ui, CryString* action, bool visible, bool enable)          0x2BB9DAC
static const char* SIG_HINT = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 30 41 8A F1 41 8A D8";
// C_UIPickpocketing::OnClose(ui, bool result, CryString* items)                                   0x2BB9E50
static const char* SIG_CLOSE = "40 55 53 56 57 41 56 48 8D 6C 24 C9 48 81 EC F0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 27 48 8D 45 97";
// I_UIPickpocketing::Open(state, ?)                                                                0x2BBA2D4
static const char* SIG_OPEN = "48 89 5C 24 20 55 56 57 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 27 48 8B 72 10";
// I_UIPickpocketing::SetBoostProgress(progress, minimum, seconds, safety)                          0x2BBAC78
static const char* SIG_BOOST = "44 89 4C 24 20 F3 0F 11 54 24 18 F3 0F 11 4C 24 10 53 48 83 EC 30 48 8D 59 A8";
// C_UIFlashPickpocketingItem::FillUIArgs                                                          0x2B8D3F4
static const char* SIG_FILL = "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8 ?? ?? ?? ?? 48 8D 53 18 41 B8 04 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 20 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 24 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 28 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 2C 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 30 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 34 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 38 41 B8 04 00 00 00 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 40 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 44";
// CScriptSystem (as in AutoFollow / Horse Route Follow): only for the optional on-screen notice.
static const char* SIG_SS_UPDATE = "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 48 8B F1 33 D2 0F 29 74 24 20";   // vtbl[1] 0xA26B94
static const char* SIG_SS_EXEC   = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 48 8B F9 48 89 50 E8"; // vtbl[6] 0x4D4484
static const int HINT_COPY = 17, CLOSE_COPY = 19;   // whole instructions, position independent

// ---------------- game layout (build 15693) ----------------
namespace off {
    const int ppState = 0x78, ppBoost = 0x84, ppMin = 0x8C, ppMax = 0x90, ppUI = 0xF0;
    const int uiPrimary = 0x58;                                // I_UIPickpocketing sub-object inside C_UIPickpocketing
    const int gMaxTime = 0x00, gTime = 0x04, gItems = 0x08, gItemsEnd = 0x10, gSpeed = 0x20, gEasy = 0x24;
    const int recSize = 0x18, recSlot = 0x00, recItem = 0x08, recUncovered = 0x10, recPenalty = 0x14;
    const int fIcon = 0x18, fPenalty = 0x20, fSlot = 0x24, fUncovered = 0x28, fWeight = 0x34, fPrice = 0x40, fCount = 0x44;
}

// ---------------- settings ----------------
static kc::Hotkey g_key;
static volatile bool g_on = false;   // [Options] Enabled
static bool g_notices = true;        // [Options] Notices
static bool g_charge = true;         // [Options] Charge: release the charge key at the configured safety
static bool g_grid = true;           // [Options] Grid: play the item grid
static pp::Config g_cfg;

// ---------------- game functions ----------------
typedef void (*Update_t)(void*, float);
typedef bool (*Open_t)(void*, void*, void*);
typedef void (*Boost_t)(void*, float, float, int, float);
typedef void (*Fill_t)(void*, void*);
typedef void (*Hint_t)(void*, void*, bool, bool);
typedef void (*Close_t)(void*, bool, void*);
typedef void (*CallFn_t)(void*, const void*);
typedef void (*Charge_t)(void*);
static Update_t g_update; static Open_t g_open; static Boost_t g_boost; static Fill_t g_fill;
static Hint_t g_hintTramp; static Close_t g_closeTramp; static CallFn_t g_callFn; static Charge_t g_chargeFn;
static uintptr_t g_base, g_end;     // WHGame.dll image (runtime type checks)

// ---------------- state (game thread; the lock covers UI callbacks from elsewhere) ----------------
static CRITICAL_SECTION g_lock;
static pp::Controller g_ctl;
static bool g_driving = false;       // the controller owns the current grid
static void* g_ui = nullptr;         // C_UIPickpocketing of the open grid
static uint64_t g_openAt = 0;
static int g_lastState = 0;
static float g_safety = -1; static uint64_t g_safetyAt = 0;
static bool g_released = false;      // our charge release for this charge (once)
static bool g_closeHooked = false;
// item details captured from FillUIArgs while the grid opens
struct Shown { bool used; int slot; bool uncovered; float penalty, weight, price; int count; char name[48]; };
static Shown g_shown[16]; static int g_nShown = 0; static bool g_capturing = false;

static uint64_t now_ms() {
    static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (uint64_t)(c.QuadPart * 1000 / f.QuadPart);
}
static void ctl_log(const char* s) { logf("  %s", s); }

// ---------------- on-screen notice (optional, as AutoFollow) ----------------
typedef void* (*SsUpdate_t)(void*, void*, void*, void*);
typedef bool (*SsExec_t)(void*, const char*, size_t, const char*, void*);
static SsUpdate_t g_ssUpdate = nullptr; static SsExec_t g_ssExec = nullptr;
static const char* volatile g_notice = nullptr;
static const char* const NOTE_ON = "Game.SendInfoText(\"@tap_on\", true)";
static const char* const NOTE_OFF = "Game.SendInfoText(\"@tap_off\", true)";
static void* SsUpdateHook(void* self, void* a, void* b, void* c) {
    void* r = g_ssUpdate(self, a, b, c);
    if (g_notice && g_ssExec) {
        const char* line = (const char*)InterlockedExchangePointer((void* volatile*)&g_notice, nullptr);
        __try { if (line) g_ssExec(self, line, strlen(line), "TheatricalAutoPickpocket", nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_ssExec = nullptr; logf("{\"ev\":\"notice_error\"}"); }
    }
    return r;
}
static bool hook_update(uintptr_t vt, uintptr_t expect) {   // compare-exchange: other mods hook this slot too
    void** slot = &((void**)vt)[1];
    DWORD old; if (!VirtualProtect(slot, 8, PAGE_READWRITE, &old)) return false;
    void* cur = (void*)expect; bool ok = false;
    for (int i = 0; i < 8 && !ok; i++) {
        g_ssUpdate = (SsUpdate_t)cur;
        void* was = InterlockedCompareExchangePointer(slot, (void*)&SsUpdateHook, cur);
        if (was == cur) ok = true; else cur = was;
    }
    VirtualProtect(slot, 8, old, &old);
    return ok;
}

// ---------------- sending input the way the game does ----------------
// Runtime type check through the object's RTTI: vtable[-1] is its CompleteObjectLocator (offset, type).
static bool class_is(void* obj, const char* mangled) {
    __try {
        uintptr_t vt = *(uintptr_t*)obj; if (vt < g_base || vt >= g_end) return false;
        const uint32_t* col = *(const uint32_t**)(vt - 8);
        if ((uintptr_t)col < g_base || (uintptr_t)col >= g_end || col[0] != 1 || col[1] != 0) return false;
        return !strcmp((const char*)(g_base + col[3] + 0x10), mangled);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ui_ok(void* ui) { return ui && class_is(ui, ".?AVC_UIPickpocketing@guimodule@wh@@"); }
static bool send_cmd(void* ui, int cmd) {
    const char* name = cmd == pp::C_LEFT ? "MoveLeft" : cmd == pp::C_RIGHT ? "MoveRight" : "DoAction";
    const char* str = name;                       // CryString: one char* (CallFunction reads only that)
    __try { g_callFn(ui, &str); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf("{\"ev\":\"error\",\"msg\":\"grid command failed - handing the grid back\"}"); return false; }
}
static bool release_charge(void* pp) {
    void* functor[2] = { nullptr, pp };           // the action functor's only field: the C_Pickpocketing
    __try { g_chargeFn(functor); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { logf("{\"ev\":\"error\",\"msg\":\"charge release failed\"}"); return false; }
}

// ---------------- the grid's contents ----------------
static bool read_grid(void* state, pp::Grid& g) {
    __try {
        uintptr_t s = (uintptr_t)state;
        g = pp::Grid();
        g.maxTimeSec = *(float*)(s + off::gMaxTime); g.timeSec = *(float*)(s + off::gTime);
        g.selectorSpeed = *(float*)(s + off::gSpeed); g.easyWayOut = *(uint8_t*)(s + off::gEasy) != 0;
        uintptr_t b = *(uintptr_t*)(s + off::gItems), e = *(uintptr_t*)(s + off::gItemsEnd);
        if (e < b || (e - b) % off::recSize || (e - b) / off::recSize > 11) return false;
        if (!(g.timeSec > 0 && g.timeSec < 120 && g.selectorSpeed > 0.01f && g.selectorSpeed < 50)) return false;
        int n = (int)((e - b) / off::recSize);
        for (int i = 0; i < n; i++) {
            uintptr_t r = b + (uintptr_t)i * off::recSize;
            int slot = *(int*)(r + off::recSlot);
            if (slot < 0 || slot > 10) return false;
            pp::Item& it = g.slot[slot + 1];                 // the UI's slot 0 is the exit
            it.present = true; it.uncovered = *(uint8_t*)(r + off::recUncovered) != 0;
            it.penalty = *(float*)(r + off::recPenalty); it.count = 1;
            sprintf_s(it.name, "item%d", slot + 1);
            // what the UI was given for this record: same slot, else the same position
            const Shown* sh = nullptr;
            for (int k = 0; k < g_nShown && !sh; k++) if (!g_shown[k].used && g_shown[k].slot == slot) sh = &g_shown[k];
            if (!sh && i < g_nShown && !g_shown[i].used) sh = &g_shown[i];
            if (sh) {
                ((Shown*)sh)->used = true;
                it.price = sh->price; it.weight = sh->weight; it.count = sh->count > 0 ? sh->count : 1;
                if (sh->name[0]) strcpy_s(it.name, sh->name);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void stop_driving(const char* why) {
    if (!g_driving) return;
    g_driving = false; g_ui = nullptr;
    logf("{\"ev\":\"grid_end\",\"why\":\"%s\",\"taken\":%d,\"value\":%.1f,\"moves\":%d,\"ignored\":%d}", why, g_ctl.taken, g_ctl.value, g_ctl.moves, g_ctl.rejected);
}

// ---------------- hooks ----------------
static void FillHook(void* item, void* args) {
    if (g_capturing && g_nShown < 16) {
        __try {
            uintptr_t p = (uintptr_t)item; Shown& s = g_shown[g_nShown];
            s = Shown{}; s.slot = *(int*)(p + off::fSlot); s.uncovered = *(uint8_t*)(p + off::fUncovered) != 0;
            s.penalty = *(float*)(p + off::fPenalty); s.weight = *(float*)(p + off::fWeight);
            s.price = *(float*)(p + off::fPrice); s.count = *(int*)(p + off::fCount);
            const char* icon = *(const char**)(p + off::fIcon);
            if (icon && kc::readable((uintptr_t)icon, 1)) { strncpy_s(s.name, icon, _TRUNCATE); }
            g_nShown++;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_fill(item, args);
}

static bool OpenHook(void* self, void* state, void* extra) {
    EnterCriticalSection(&g_lock);
    g_nShown = 0; g_capturing = true;
    LeaveCriticalSection(&g_lock);
    bool r = g_open(self, state, extra);
    EnterCriticalSection(&g_lock);
    g_capturing = false;
    stop_driving("a new grid opened");
    void* ui = (void*)((uintptr_t)self - off::uiPrimary);
    pp::Grid g;
    if (!read_grid(state, g)) logf("{\"ev\":\"grid\",\"msg\":\"could not read the grid - playing it is up to you\"}");
    else if (g_on && g_grid && ui_ok(ui)) {
        g_ctl.cfg = g_cfg; g_ctl.log = ctl_log; g_ctl.closeAck = g_closeHooked;
        g_ctl.start(g, now_ms()); g_ui = ui; g_driving = true; g_openAt = now_ms();
        logf("{\"ev\":\"grid_start\",\"items_described\":%d}", g_nShown);
    } else {
        int n = 0; for (int s = 1; s < pp::NSLOT; s++) n += g.slot[s].present;
        logf("{\"ev\":\"grid\",\"on\":%d,\"time\":%.2f,\"items\":%d,\"msg\":\"not playing this grid\"}", (int)g_on, g.timeSec, n);
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

static void BoostHook(void* self, float progress, float minimum, int seconds, float safety) {
    g_safety = safety; g_safetyAt = now_ms();
    KC_DLOG("boost progress=%.3f min=%.3f t=%d safety=%.3f", progress, minimum, seconds, safety);
    g_boost(self, progress, minimum, seconds, safety);
}

static void HintHook(void* ui, void* name, bool visible, bool enable) {
    g_hintTramp(ui, name, visible, enable);
    EnterCriticalSection(&g_lock);
    if (g_driving) {
        const char* n = nullptr;
        __try { n = name ? *(const char**)name : nullptr; if (n && !kc::readable((uintptr_t)n, 1)) n = nullptr; }
        __except (EXCEPTION_EXECUTE_HANDLER) { n = nullptr; }
        pp::Hint h{ pp::hint_action(n), visible, enable };
        KC_DLOG("hint %s visible=%d enable=%d", n ? n : "?", (int)visible, (int)enable);
        if (h.action != pp::H_OTHER) g_ctl.hint(h, now_ms());
    }
    LeaveCriticalSection(&g_lock);
}

static void CloseHook(void* ui, bool result, void* items) {
    EnterCriticalSection(&g_lock);
    if (g_driving) { g_ctl.closed(now_ms(), result); stop_driving(result ? "closed: success" : "closed: caught"); }
    LeaveCriticalSection(&g_lock);
    g_closeTramp(ui, result, items);
}

static void tick(void* pp) {
    uintptr_t p = (uintptr_t)pp;
    int state = *(int*)(p + off::ppState);
    if (state != g_lastState) {
        KC_DLOG("state %d -> %d", g_lastState, state);
        if (state == 1 || state == 2) { if (g_lastState != 1 && g_lastState != 2) { g_released = false; g_safety = -1; } }
        g_lastState = state;
    }
    // charge
    if (g_on && g_charge && (state == 1 || state == 2) && !g_released) {
        float boost = *(float*)(p + off::ppBoost), mn = *(float*)(p + off::ppMin), mx = *(float*)(p + off::ppMax);
        float safety = (now_ms() - g_safetyAt < 500) ? g_safety : -1.f;
        int d = pp::charge_decide(g_cfg, state, boost, mn, mx, safety);
        if (d != pp::CD_WAIT) {
            g_released = true;
            logf("{\"ev\":\"%s\",\"boost\":%.2f,\"min\":%.2f,\"max\":%.2f,\"safety\":%.2f}", d == pp::CD_RELEASE ? "charge_release" : "charge_abort", boost, mn, mx, safety);
            release_charge(pp);
            return;
        }
    }
    // grid
    if (!g_driving) return;
    if (!g_on || !g_grid) { stop_driving("switched off - the grid is yours"); return; }
    if (state != 3 && now_ms() - g_openAt > 300) { g_ctl.closed(now_ms(), true); stop_driving("the minigame ended"); return; }
    if (!ui_ok(g_ui)) { stop_driving("the grid's UI went away"); return; }
    uint64_t t = now_ms();
    int cmd = g_ctl.step(t);
    if (cmd == pp::C_NONE) return;
    KC_DLOG("send %s (model slot %d)", pp::cmd_name(cmd), g_ctl.pos);
    g_ctl.begin(cmd);
    if (!send_cmd(g_ui, cmd)) { stop_driving("command failed"); return; }
    if (g_driving) g_ctl.sent(now_ms());
}

static void UpdateHook(void* pp, float dt) {
    g_update(pp, dt);
    EnterCriticalSection(&g_lock);
    __try { tick(pp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_driving = false; g_on = false; logf("{\"ev\":\"error\",\"msg\":\"exception in the tick - switched off\"}"); }
    LeaveCriticalSection(&g_lock);
}

// ---------------- inline detour (two UI callbacks that are not virtual) ----------------
static void put_jmp(uint8_t* at, uintptr_t to) {        // jmp qword ptr [rip+0]; dq to
    at[0] = 0xFF; at[1] = 0x25; *(uint32_t*)(at + 2) = 0; *(uint64_t*)(at + 6) = to;
}
static void* detour(uintptr_t fn, int len, void* hook) {
    uint8_t* tr = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return nullptr;
    memcpy(tr, (void*)fn, len); put_jmp(tr + len, fn + len);
    uint8_t patch[32]; put_jmp(patch, (uintptr_t)hook); memset(patch + 14, 0xCC, len - 14);
    DWORD old; if (!VirtualProtect((void*)fn, len, PAGE_EXECUTE_READWRITE, &old)) { VirtualFree(tr, 0, MEM_RELEASE); return nullptr; }
    memcpy((void*)fn, patch, len);
    VirtualProtect((void*)fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)fn, len);
    return tr;
}

// ---------------- resolve ----------------
// The vtable whose first three slots are a, b, c (C_UIPickpocketing has two COLs for its I_UIPickpocketing
// sub-object, so RTTI alone is ambiguous; the slots are not).
static uintptr_t vtable_with(kc::Resolver& R, const char* what, uintptr_t a, uintptr_t b, uintptr_t c) {
    if (!a || !b || !c) return 0;
    uintptr_t hit = 0; int n = 0;
    for (auto& r : R.im.data)
        for (uintptr_t v = (r.lo + 7) & ~(uintptr_t)7; v + 24 <= r.hi; v += 8) {
            const uintptr_t* s = (const uintptr_t*)v;
            if (s[0] == a && s[1] == b && s[2] == c) { hit = v; n++; }
        }
    if (n != 1) { R.fail(what, n ? "found more than once" : "not found"); return 0; }
    return hit;
}
struct Found { uintptr_t update, charge, callfn, hint, close, open, boost, fill, ppVt, uiVt, itemVt; };
static bool resolve(kc::Resolver& R, Found& f) {
    f.update = R.find("C_Pickpocketing::Update", SIG_UPDATE);
    f.charge = R.find("charge action", SIG_CHARGE);
    f.callfn = R.find("C_UIFlashBase::CallFunction", SIG_CALLFN);
    f.hint = R.find("OnActionHintState", SIG_HINT);
    f.close = R.find("OnClose", SIG_CLOSE);
    f.open = R.find("I_UIPickpocketing::Open", SIG_OPEN);
    f.boost = R.find("I_UIPickpocketing::SetBoostProgress", SIG_BOOST);
    f.fill = R.find("FillUIArgs", SIG_FILL);
    f.ppVt = R.vtable("C_Pickpocketing", ".?AVC_Pickpocketing@playermodule@wh@@", 0);
    f.itemVt = R.vtable("C_UIFlashPickpocketingItem", ".?AVC_UIFlashPickpocketingItem@guimodule@wh@@", 0);
    if (f.ppVt && f.update && ((uintptr_t*)f.ppVt)[19] != f.update) R.fail("C_Pickpocketing::Update", "not in vtable slot 19");
    if (f.itemVt && f.fill && ((uintptr_t*)f.itemVt)[1] != f.fill) R.fail("FillUIArgs", "not in vtable slot 1");
    if (f.open && f.boost) {
        uintptr_t c = 0;   // slot 1 (Close) is whatever sits between Open and SetBoostProgress
        for (auto& r : R.im.data) {
            for (uintptr_t v = (r.lo + 7) & ~(uintptr_t)7; v + 24 <= r.hi && !c; v += 8)
                if (((const uintptr_t*)v)[0] == f.open && ((const uintptr_t*)v)[2] == f.boost) c = ((const uintptr_t*)v)[1];
            if (c) break;
        }
        f.uiVt = vtable_with(R, "I_UIPickpocketing vtable", f.open, c, f.boost);
    }
    return R.ok();
}

#ifdef KC_DEVTOOLS
extern "C" __declspec(dllexport) int KC_SelfTest(HMODULE game, uintptr_t* out) {   // offline check (tools/selftest)
    kc::Resolver R; R.im.load(game); Found f{}; bool ok = resolve(R, f);
    uintptr_t* p = (uintptr_t*)&f; for (size_t i = 0; i < sizeof f / 8; i++) out[i] = p[i];
    return ok ? (int)(sizeof f / 8) : -R.fails;
}
#endif

static void install_notices(const kc::Image& im) {      // optional
    kc::Resolver R; R.im = im;
    uintptr_t vt = R.vtable("CScriptSystem", ".?AVCScriptSystem@@", 0), up = 0;
    uintptr_t cur = vt ? ((uintptr_t*)vt)[1] : 0;
    if (cur && !R.im.in_image(cur)) { if (R.find("CScriptSystem::Update", SIG_SS_UPDATE)) up = cur; }
    else up = R.slot("CScriptSystem::Update", vt, 1, SIG_SS_UPDATE);
    uintptr_t ex = R.slot("CScriptSystem::ExecuteBuffer", vt, 6, SIG_SS_EXEC);
    if (!R.ok()) { logf("{\"ev\":\"notices_off\",\"msg\":\"on-screen notices not available in this game version\"}"); return; }
    g_ssExec = (SsExec_t)ex;
    if (!hook_update(vt, up)) { g_ssExec = nullptr; logf("{\"ev\":\"notices_off\",\"msg\":\"could not hook the script system\"}"); }
}

static float ini_float(const char* key, float def) {
    char p[MAX_PATH], v[32] = {0}; kc::path(p, sizeof p, "kcd2_autopickpocket.ini");
    GetPrivateProfileStringA("Options", key, "", v, sizeof v, p);
    return v[0] ? (float)atof(v) : def;
}
static void load_settings() {
    g_key.vk = kc::ini_key("Toggle", VK_F5);
    g_on = kc::ini_int("Options", "Enabled", 0) != 0;
    g_notices = kc::ini_int("Options", "Notices", 1) != 0;
    g_charge = kc::ini_int("Options", "Charge", 1) != 0;
    g_grid = kc::ini_int("Options", "Grid", 1) != 0;
    float rs = ini_float("ReleaseSafety", g_cfg.releaseSafety);
    g_cfg.releaseSafety = rs < 0.05f ? 0.05f : rs > 1.f ? 1.f : rs;
    g_cfg.maxChargeSec = ini_float("MaxChargeSeconds", 0);
    g_cfg.abortSafety = ini_float("AbortSafety", -1);
    int sm = kc::ini_int("Options", "SafetyMs", g_cfg.safetyMs); g_cfg.safetyMs = sm < 800 ? 800 : sm > 6000 ? 6000 : sm;
    g_cfg.minValue = ini_float("MinValue", 0);
    g_cfg.uncover = kc::ini_int("Options", "Uncover", 1) != 0;
}

static HMODULE g_self = nullptr;
static DWORD WINAPI Init(LPVOID) {
    kc::init(g_self, "kcd2_autopickpocket");
    InitializeCriticalSection(&g_lock);
    load_settings();
    HMODULE h = kc::wait_game(); if (!h) { logf("{\"ev\":\"abort\",\"msg\":\"WHGame.dll not loaded\"}"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("{\"ev\":\"start\",\"version\":\"" VERSION "\",\"game_ts\":\"0x%08X\"}", R.im.timestamp);
    Found f{};
    if (!resolve(R, f)) { logf("{\"ev\":\"disabled\",\"msg\":\"this game version is not supported yet - nothing was changed\"}"); return 0; }
    g_callFn = (CallFn_t)f.callfn; g_chargeFn = (Charge_t)f.charge; g_base = R.im.base; g_end = R.im.end;
    g_update = (Update_t)f.update; g_open = (Open_t)f.open; g_boost = (Boost_t)f.boost; g_fill = (Fill_t)f.fill;
    g_hintTramp = (Hint_t)detour(f.hint, HINT_COPY, (void*)&HintHook);
    if (!g_hintTramp) { logf("{\"ev\":\"disabled\",\"msg\":\"could not hook the grid's hints - nothing was changed\"}"); return 0; }
    g_closeTramp = (Close_t)detour(f.close, CLOSE_COPY, (void*)&CloseHook);
    g_closeHooked = g_closeTramp != nullptr;
    kc::write_ptr(&((void**)f.itemVt)[1], (void*)&FillHook);
    kc::write_ptr(&((void**)f.uiVt)[0], (void*)&OpenHook);
    kc::write_ptr(&((void**)f.uiVt)[2], (void*)&BoostHook);
    kc::write_ptr(&((void**)f.ppVt)[19], (void*)&UpdateHook);
    if (g_notices) install_notices(R.im);
    char kn[16];
    logf("{\"ev\":\"ready\",\"on\":%d,\"key\":\"%s\",\"charge\":%d,\"grid\":%d,\"release_safety\":%.2f,\"safety_ms\":%d,\"close_hook\":%d,\"notices\":%d}",
         (int)g_on, kc::key_name(g_key.vk, kn, sizeof kn), (int)g_charge, (int)g_grid, g_cfg.releaseSafety, g_cfg.safetyMs, (int)g_closeHooked, (int)(g_ssExec != nullptr));
    for (;;) {                                           // the hotkey (the minigame ticks only while it runs)
        if (g_key.vk && g_key.pressed()) {
            g_on = !g_on; logf("{\"ev\":\"toggle\",\"on\":%d}", (int)g_on);
            if (g_notices && g_ssExec) g_notice = g_on ? NOTE_ON : NOTE_OFF;
        }
        Sleep(30);
    }
}
KC_PLUGIN("Theatrical AutoPickpocket", KC_AUTHOR, 10)
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_self = h; KC_START(Init); } return TRUE; }
