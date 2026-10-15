// autolockpick.cpp -- Theatrical AutoLockpick for Kingdom Come: Deliverance II (kcd2_autolockpick.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// Henry picks the lock himself, through the same C_LockPicking input handlers the gamepad and the turn key
// call: the pick position (xi_lock_cursor_x / xi_lock_cursor_y, an absolute stick position) and the turn key
// (lock_dir_fwd, held and let go).  The game itself adds Henry's tremble, judges whether the pick sits on
// the pin, turns the lock, wears and breaks picks and opens the lock; the mod never writes any of that.
// It ticks just before C_LockPicking's own per-frame Update, on the game thread, like input does.
// Use: start picking a lock, press F2 (kcd2_autolockpick.ini). F2 again stops and gives the pick back.
// Game code is found by RTTI + signature at startup (kcd2_common.h); on a mismatch nothing is hooked.
// Log: kcd2_autolockpick.log next to the DLL.  The minigame, as decoded: see ../NOTES.md.
// C_LockPicking's layout follows libKCD2 by JerryYOJ (GPL v3), checked against the build-15693 binary.
#include "kcd2_common.h"
#include "lp_logic.h"

using kc::logf;
template<class T> static T& F(uintptr_t p, uintptr_t o) { return *(T*)(p + o); }
static uint64_t (*g_clock)() = GetTickCount64;

// Signatures (build 15693 addresses in comments). Struct offsets in them stay literal.
// C_LockPicking::Update(this, float dt), vtbl[19]: runs rotation / pick / judgement in states 3-5.
static const char* SIG_UPDATE = "48 89 5C 24 08 57 48 83 EC 30 48 8D B9 88 00 00 00 0F 29 74 24 20 8A 07 0F 28 F1 48 8B D9 3C 03 74 ?? 2C 04 3C 01 77 ?? "
                                "0F 28 C6 F3 0F 58 81 B8 00 00 00 F3 0F 11 81 B8 00 00 00";        // 0x89A940
// Pick handler, registered for xi_lock_cursor_x/y and lock_cursor_x/y: bool (this, EntityId, const ActionId&, int mode, float value)
static const char* SIG_CURSOR = "48 89 5C 24 08 57 48 83 EC 20 8A 81 88 00 00 00 49 8B F8 48 8B D9 3C 03 74 ?? 2C 04 3C 01 77 ?? "
                                "48 8D 15 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? "
                                "48 8D 15 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8B 4B 18 48 8B 89 88 0B 00 00 E8 ?? ?? ?? ?? "
                                "F3 0F 10 0D ?? ?? ?? ?? F3 0F 59 C8 F3 0F 59 4C 24 50 F3 0F 58 8B 38 01 00 00 F3 0F 11 8B 38 01 00 00";   // 0xA98244
// ...its gamepad branches (cold code): x/y = value, pending mouse move cleared.
static const int   CURSOR_XI_X = 0x33, CURSOR_XI_Y = 0x4A;                                      // rel32 of the two jne
static const char* SIG_XI_X = "F3 0F 10 44 24 50 F3 0F 11 83 DC 00 00 00 C7 83 38 01 00 00 00 00 00 00";   // 0x226F978
static const char* SIG_XI_Y = "F3 0F 10 44 24 50 F3 0F 11 83 E0 00 00 00 C7 83 3C 01 00 00 00 00 00 00";   // 0x226F995
// Turn key handler (lock_dir_fwd): hold (mode 4) sets the turn speed +0xB4 = 100; release (mode 2) lets go.
static const char* SIG_TURN = "40 53 48 83 EC 40 8A 81 88 00 00 00 48 8B D9 3C 03 75 ?? 41 83 F9 04 75 ?? 48 8D 15 ?? ?? ?? ?? 49 8B C8 E8 ?? ?? ?? ?? "
                              "84 C0 74 ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2E 83 B4 00 00 00 7A ?? 75 ?? B0 01 48 83 C4 40 5B C3 2C 04 3C 01 76 ?? EB ?? "
                              "C7 83 B4 00 00 00 00 00 C8 42 EB ?? 41 83 F9 02 75 ?? E9 ?? ?? ?? ??";   // 0xA98614
static const int UPDATE_SLOT = 19;

// ---------------- C_LockPicking layout (build 15693) ----------------
static const uintptr_t L_STATE = 0x88, L_TOL = 0x90, L_START = 0x94, L_END = 0x98, L_ANGLE = 0x9C, L_ERR = 0xA8,
    L_HEALTH = 0xAC, L_TURN = 0xB4, L_BASE = 0xC4, L_PIN = 0xD0, L_PICK = 0xDC, L_SHAKEN = 0xE8;

typedef void (*Update_t)(void*, float);
typedef bool (*Handler_t)(void*, uint32_t, const void*, int, float);
static Update_t pOrigUpdate = nullptr;
static Handler_t pCursor = nullptr, pTurn = nullptr;
// ActionId is a CCryName: its first field is the name, which these handlers compare with _stricmp.
static const char* const A_XI_X = "xi_lock_cursor_x";
static const char* const A_XI_Y = "xi_lock_cursor_y";
static const char* const A_TURN = "lock_dir_fwd";

static kc::Hotkey g_key;
static bool g_autoStart = false, g_dead = false;
static int g_insane = 0;
static uintptr_t g_lp = 0;
static int g_lastState = -1;
static lp::Hand g_hand;
static uint64_t g_t0 = 0, g_logT = 0;
static float g_health0 = 0;

static lp::View read_view(uintptr_t s) {
    lp::View v{};
    v.state = F<uint8_t>(s, L_STATE);
    v.tol = F<float>(s, L_TOL); v.start = F<float>(s, L_START); v.end = F<float>(s, L_END); v.angle = F<float>(s, L_ANGLE);
    v.err = F<float>(s, L_ERR); v.health = F<float>(s, L_HEALTH);
    v.bx = F<float>(s, L_BASE); v.by = F<float>(s, L_BASE + 4);
    v.cx = F<float>(s, L_PICK); v.cy = F<float>(s, L_PICK + 4);
    v.sx = F<float>(s, L_SHAKEN); v.sy = F<float>(s, L_SHAKEN + 4);
    return v;
}
static bool sane(const lp::View& v) {
    auto in = [](float x, float lo, float hi) { return x >= lo && x <= hi; };   // false for NaN
    return in(v.tol, 0.f, 4.f) && in(v.start, 0.f, 360.f) && in(v.end, 0.f, 361.f) && in(v.angle, 0.f, 360.f)
        && in(v.bx, -1.5f, 1.5f) && in(v.by, -1.5f, 1.5f) && in(v.cx, -1.5f, 1.5f) && in(v.cy, -1.5f, 1.5f);
}

static void apply(uintptr_t s, const lp::Cmd& c) {
    if (c.move) {
        pCursor((void*)s, 0, &A_XI_X, lp::TURN_HOLD, c.x);
        pCursor((void*)s, 0, &A_XI_Y, lp::TURN_HOLD, c.y);
    }
    if (c.turn == lp::TURN_HOLD) pTurn((void*)s, 0, &A_TURN, lp::TURN_HOLD, 1.f);
    else if (c.turn == lp::TURN_RELEASE) { pTurn((void*)s, 0, &A_TURN, lp::TURN_RELEASE, 0.f); KC_DLOG("turn key let go"); }
}

static void begin(uintptr_t s, const lp::View& v, const char* why) {
    g_hand.begin(v); g_t0 = g_clock(); g_health0 = v.health;
    float px = F<float>(s, L_PIN), py = F<float>(s, L_PIN + 4), qx, qy; lp::pin_at(v.bx, v.by, v.angle, qx, qy);
    logf("START (%s) state=%d spot=%.3f angle=%.1f (start %.1f, opens at %.1f) pin=(%.3f,%.3f) r=%.2f pick=(%.2f,%.2f) health=%.2f",
         why, v.state, v.tol, v.angle, v.start, v.end, v.bx, v.by, sqrtf(v.bx * v.bx + v.by * v.by), v.cx, v.cy, v.health);
    if (fabsf(px - qx) + fabsf(py - qy) > 0.01f) logf("note: pin rotation differs from the game's (%.3f,%.3f) vs (%.3f,%.3f)", qx, qy, px, py);
}
static void report(const lp::View& v) {
    logf("done: %s after %.1fs, %d tries, pick health %.2f -> %.2f", lp::outcome_name(g_hand.outcome),
         (g_clock() - g_t0) / 1000.0, g_hand.tries, g_health0, v.health);
}

static void tick(uintptr_t s, float dt) {
    bool key = g_key.pressed();
    lp::View v = read_view(s);
    int prev = g_lastState; g_lastState = v.state;
    bool playing = v.state >= lp::ST_IDLE && v.state <= lp::ST_DRYRUN;
    if (g_hand.active()) {
        if (key) { apply(s, g_hand.cancel()); logf("stopped by the player (%s)", "key"); return; }
        if (playing && !sane(v)) {
            apply(s, g_hand.cancel());
            logf("stop: unexpected lock data (spot %.3f angle %.1f pin %.2f,%.2f)", v.tol, v.angle, v.bx, v.by);
            if (++g_insane >= 3) { g_dead = true; logf("disabled: lock data does not look right (game updated?)"); }
            return;
        }
        lp::Cmd c = g_hand.step(v, dt);
        if (playing) apply(s, c);
        if (!g_hand.active()) { report(v); return; }
        if (g_clock() - g_logT > 500) {
            g_logT = g_clock();
            KC_DLOG("ph=%d st=%d angle=%.1f err=%.2f pick=(%.3f,%.3f) shaken=(%.3f,%.3f) pin=(%.3f,%.3f) health=%.3f turn=%.0f",
                    g_hand.phase, v.state, v.angle, v.err, v.cx, v.cy, v.sx, v.sy, F<float>(s, L_PIN), F<float>(s, L_PIN + 4), v.health, F<float>(s, L_TURN));
        }
        return;
    }
    if (!playing) return;
    if (key) {
        if (!sane(v)) { logf("not starting: lock data does not look right (spot %.3f angle %.1f)", v.tol, v.angle); return; }
        begin(s, v, "key");
    } else if (g_autoStart && v.state == lp::ST_IDLE && (prev < lp::ST_IDLE || prev > lp::ST_DRYRUN) && g_hand.outcome != lp::O_BROKE && sane(v)) {
        begin(s, v, "auto");     // a new lock (or a new pick after a let-go): not after a break, the player decides
    }
}

static void UpdateHook(void* self, float dt) {
    if (!g_dead) {
        __try {
            uintptr_t s = (uintptr_t)self;
            if (s != g_lp) { if (g_hand.active()) logf("another lock -> stop"); g_hand.phase = lp::P_OFF; g_lp = s; g_lastState = -1; }
            tick(s, dt);
        } __except (EXCEPTION_EXECUTE_HANDLER) { logf("exception in tick - stopped"); g_hand.phase = lp::P_OFF; }
    }
    pOrigUpdate(self, dt);
}

struct Found { uintptr_t vt, update, cursor, turn, xix, xiy; };
static bool resolve(kc::Resolver& R, Found& f) {
    f.vt = R.vtable("C_LockPicking", ".?AVC_LockPicking@playermodule@wh@@", 0);
    uintptr_t cur = f.vt ? ((uintptr_t*)f.vt)[UPDATE_SLOT] : 0;
    if (cur && !R.im.in_image(cur)) { if (R.find("C_LockPicking::Update", SIG_UPDATE)) f.update = cur; }   // another mod hooked it first: chain
    else f.update = R.slot("C_LockPicking::Update", f.vt, UPDATE_SLOT, SIG_UPDATE);
    f.cursor = R.find("pick handler", SIG_CURSOR);
    f.turn = R.find("turn key handler", SIG_TURN);
    if (f.cursor) {
        f.xix = kc::rel32(f.cursor + CURSOR_XI_X); f.xiy = kc::rel32(f.cursor + CURSOR_XI_Y);
        if (!R.verify("gamepad pick x", f.xix, SIG_XI_X)) f.xix = 0;
        if (!R.verify("gamepad pick y", f.xiy, SIG_XI_Y)) f.xiy = 0;
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

static HMODULE g_self = nullptr;
static DWORD WINAPI Init(LPVOID) {
    kc::init(g_self, "kcd2_autolockpick");
    g_key.vk = kc::ini_key("Pick", VK_F2);
    g_autoStart = kc::ini_int("Options", "AutoStart", 0) != 0;
    g_hand.p.maxTries = std::max(1, std::min(50, kc::ini_int("Options", "MaxTries", 6)));
    HMODULE h = kc::wait_game(); if (!h) { logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("start v1.0.0 (game ts 0x%08X)", R.im.timestamp);
    Found f{};
    if (!resolve(R, f)) { logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    pCursor = (Handler_t)f.cursor; pTurn = (Handler_t)f.turn; pOrigUpdate = (Update_t)f.update;
    kc::write_ptr(&((void**)f.vt)[UPDATE_SLOT], (void*)&UpdateHook);
    char kn[16];
    logf("ready (%s while picking a lock)%s, up to %d tries per lock", kc::key_name(g_key.vk, kn, sizeof kn),
         g_autoStart ? ", starts by itself on every lock" : "", g_hand.p.maxTries);
    return 0;
}
KC_PLUGIN("Theatrical AutoLockpick", KC_AUTHOR, 10)
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_self = h; KC_START(Init); } return TRUE; }
