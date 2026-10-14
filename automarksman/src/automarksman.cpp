// automarksman.cpp -- Theatrical AutoMarksman for Kingdom Come: Deliverance II (kcd2_automarksman.dll).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// While Henry holds a drawn bow, the camera turns - at a steady, human speed - onto the point that makes
// the arrow hit the head of the person (or the chest of the animal) nearest the centre of the view,
// allowing for the arrow's drop, air drag, the bow sitting beside the eye and a walking target.  That is
// all it does: the player still draws, holds and releases; the game itself flies the arrow, sways the bow
// and decides the hit.  Nothing is teleported, no hit is forced, no item or stat is touched.
//
// How the camera is turned: the game's look-angle state of the player (C_ActorPhysicsState, C_Actor+0x238)
// has an additive input, m_lookAngleAccum (+0x88), that its per-frame tick (0x4415E0 in build 15693)
// adds to the look angles before the game's own view limits are applied, then clears.  The horse camera
// uses the same input.  The mod adds a small turn there each frame, like a hand on the mouse; when the
// player moves the mouse the mod lets go for a moment.
//
// How it knows the bow: nothing about the bow is assumed.  CProjectile::Launch (vtbl[35]) is watched, so
// every arrow Henry shoots tells the mod its real launch speed (bow, arrow, draw), where it left relative
// to the camera and in which direction; the arrow is then followed in flight to measure gravity and drag.
// The first shot of a session uses defaults (ArrowSpeed in the .ini); the next ones use what was measured.
//
// Use: F4 (kcd2_automarksman.ini) switches the assist on or off; it only acts while a bow is drawn.
// Game code is found by RTTI + signature at startup (kcd2_common.h); on a mismatch nothing is hooked.
// Log: kcd2_automarksman.log next to the DLL.
//
// Research credits: the layouts of C_ActorPhysicsState, C_MissileWeaponPlayerController and CProjectile and
// several vtable slots come from libKCD2's public notes (JerryYOJ, GPL-3.0), re-verified against build
// 15693 for this mod (docs/NOTES.md).  Arrow Arc (inkcrow) was used as a concept reference only; no code.
#include "kcd2_common.h"
#include "mk_ballistics.h"

using kc::logf;
using mk::V3;
template<class T> static T& F(uintptr_t p, uintptr_t o) { return *(T*)(p + o); }

// ---------------- signatures (build 15693 addresses in comments; struct offsets kept literal) ----------------
static const char* SIG_SS_UPDATE = "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 3D ?? ?? ?? ?? 48 8B F1 33 D2 0F 29 74 24 20";   // CScriptSystem vtbl[1] 0xA26B94
static const char* SIG_SS_EXEC   = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 50 48 8B F9 48 89 50 E8"; // CScriptSystem vtbl[6] ExecuteBuffer 0x4D4484
static const char* SIG_FRAMEWORK_REF = "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 84 C0";                       // mov rcx,[CCryAction*] (global 0x549D328)
static const char* SIG_GETCLIENTACTOR = "48 83 EC 28 48 8B 89 88 00 00 00 33 C0 48 85 C9 74 ?? E8 ?? ?? ?? ?? 48 83 C4 28 C3";   // CCryAction vtbl[64]
static const char* SIG_GETACTORSYS = "48 8B 81 18 05 00 00 C3";                                            // CCryAction vtbl[25] 0x641150
// gEnv->pSystem->GetViewCamera() (0x393B5C): the mov's rip operand is gEnv.pSystem (gEnv+0xC0 = 0x492D8C0).
static const char* SIG_GENV_SYSTEM = "48 89 5C 24 08 57 48 83 EC 20 48 8B 0D ?? ?? ?? ?? 48 8B FA 48 8B 01 FF 90 38 04 00 00 48 8B D8";
static const char* SIG_VIEWCAMERA  = "48 8D 81 88 02 00 00 C3";                                            // CSystem vtbl[135] 0x5D77F0
static const char* SIG_GETENTITY   = "48 89 5C 24 08 57 48 83 EC 20 8B FA 48 8B D9 B8 01 00 00 00 F0 0F C1 81 20 01 00 00";   // CEntitySystem vtbl[14] 0x4B4F98
static const char* SIG_GETBOUNDS   = "48 89 5C 24 08 48 89 7C 24 10 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00 48 8B FA";        // CEntity vtbl[34] GetWorldBounds 0x63E034
static const char* SIG_GETACTOR    = "89 54 24 10 53 48 83 EC 20 48 8B D9 4C 8D 44 24 38 48 83 C1 38 48 8D 54 24 30";         // CActorSystem vtbl[3] 0x647E40
static const char* SIG_MKITER      = "48 89 74 24 10 57 48 83 EC 20 48 8B 81 90 00 00 00 48 8B F2 48 8B F9 48 39 81 88 00 00 00"; // CActorSystem vtbl[7] 0x1794CB0
static const char* SIG_ITER_NEXT   = "48 8B D1 48 8B 49 10 48 8B 42 08 48 3B 48 40 74 ?? 48 8B 41 18 48 8B 09";                // CActorIterator vtbl[2] 0x4CB02C
static const char* SIG_ITER_REL    = "40 53 48 83 EC 20 FF 49 18 48 8B D9 83 79 18 00 7F ?? 48 8B 01 33 D2 FF 10";              // CActorIterator vtbl[4] 0x16F5194
static const char* SIG_DEADBYHP    = "48 8B C1 48 8B 89 68 06 00 00 48 85 C9 74 ?? 48 8B 01 48 FF 60 30";                      // C_Actor vtbl[38] IsDeadByHealth 0x64583C
static const char* SIG_LAUNCH      = "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 D8 48 81 EC 20 01 00 00 0F 29 70 E8 48 8B D9 0F 29 78 D8"; // CProjectile vtbl[35] 0x2AA85A8
// Layout checks (found, not called): they fail if the fields the mod reads or writes have moved.
// Look-angle tick 0x4415E0: ... m_lookAngles(+8) copies, m_lookDeltaTransient(+0x4C) and m_lookAngleAccum(+0x88) cleared.
static const char* SIG_LOOKTICK = "40 53 48 83 EC 20 F2 0F 10 41 58 0F 28 CA 8B 41 60 48 8B D9 F2 0F 11 41 64 89 41 6C F2 0F 10 42 18 F2 0F 11 41 7C 8B 42 20 89 81 84 00 00 00 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 48 20 83 B9 D0 03 00 00 00 0F 85 ?? ?? ?? ?? 8B 43 08 48 8B CB 89 43 48 8B 43 10 89 43 44 E8 ?? ?? ?? ?? 48 8B CB 48 C7 43 4C 00 00 00 00 C7 43 54 00 00 00 00 48 C7 83 88 00 00 00 00 00 00 00 C7 83 90 00 00 00 00 00 00 00 48 83 C4 20";
// C_Actor vtbl[57] GetViewRotation 0x94DB70: the look state is C_Actor+0x238, view quat at +0x24.
static const char* SIG_GETVIEWROT = "48 8B 81 38 02 00 00 0F 10 40 24 48 8B C2 0F 11 02 C3";
// C_MissileWeaponPlayerController aim start 0x2AAA4C0: m_isAiming is +0x20.
static const char* SIG_AIMSTART = "40 53 48 83 EC 20 48 8B DA C6 41 20 01 E8";
// CProjectile::Launch stores m_initialPosition/Direction/Velocity at +0xB0/+0xBC/+0xC8 (0x2AA8906).
static const char* SIG_LAUNCHSTORE = "F2 0F 10 06 F2 0F 11 83 B0 00 00 00 8B 46 08 89 83 B8 00 00 00 F2 0F 10 07 F2 0F 11 83 BC 00 00 00 8B 47 08 89 83 C4 00 00 00 F2 41 0F 10 06 F2 0F 11 83 C8 00 00 00 41 8B 46 08 89 83 D0 00 00 00";

// ---------------- layouts (build 15693; see docs/NOTES.md) ----------------
static const uintptr_t A_ENTITY = 0x38, A_LOOK = 0x238, P_MWCTRL = 0xD70;        // C_Actor / C_Player
static const uintptr_t MW_OWNER = 0x18, MW_AIMING = 0x20;                          // C_MissileWeaponPlayerController
static const uintptr_t L_OWNER = 0x00, L_ANGLES = 0x08, L_REQUEST = 0x7C, L_ACCUM = 0x88;   // C_ActorPhysicsState (Ang3: x pitch, z yaw)
static const uintptr_t E_ID = 0x0C, E_NAME = 0xE0, E_TM = 0x58;                    // CEntity (world TM rows at +0x58, translation +0x64/+0x74/+0x84)
static const uintptr_t PR_ENTITY = 0x38, PR_OWNER = 0x108;                         // CProjectile
static const uintptr_t GENV_SYSTEM_TO_ENTSYS = 0x20;                               // gEnv.pEntitySystem (+0xA0) = &gEnv.pSystem (+0xC0) - 0x20

typedef void* (*SsUpdate_t)(void*, void*, void*, void*);
typedef bool  (*SsExec_t)(void*, const char*, size_t, const char*, void*);
typedef void* (*GetClientActor_t)(void*);
typedef float* (*GetViewCamera_t)(void*);
typedef void* (*GetEntity_t)(void*, uint32_t);
typedef void  (*GetBounds_t)(void*, float*);
typedef void* (*GetActor_t)(void*, uint32_t);
typedef void** (*MkIter_t)(void*, void**);
typedef void* (*IterNext_t)(void*);
typedef void  (*IterRel_t)(void*);
typedef bool  (*DeadByHp_t)(void*);
typedef void  (*Launch_t)(void*, const float*, const float*, const float*, float);

static struct Game {
    uintptr_t fwGlobal, sysGlobal, vtPlayer, vtMwCtrl, vtEntity, vtEntSys, vtActorSys, vtIter, vtProjectile, vtSS;
    GetClientActor_t clientActor; GetViewCamera_t viewCamera; GetEntity_t getEntity; GetBounds_t bounds;
    GetActor_t getActor; MkIter_t mkIter; IterNext_t iterNext; IterRel_t iterRel; uintptr_t deadByHp;
    Launch_t launch; SsUpdate_t ssUpdate; SsExec_t ssExec;
} G;

// ---------------- settings ----------------
static kc::Hotkey g_key;
static bool g_on = false, g_dead = false, g_people = true, g_animals = true, g_lead = true;
static float g_cone = mk::rad(10.f), g_maxDist = 80.f, g_headDrop = 0.13f, g_overrideS = 0.4f;
static mk::Steer g_steer;
static mk::Calib g_cal;

// ---------------- per-frame state (game thread) ----------------
static LARGE_INTEGER g_qpf;
static double now_s() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)g_qpf.QuadPart; }
static double g_lastT = 0, g_aimT0 = 0, g_userUntil = 0, g_pickT = 0, g_logT = 0;
static uintptr_t g_player = 0, g_checked = 0; static uint32_t g_playerId = 0; static int g_bad = 0;
static bool g_wasAiming = false, g_outOfRange = false;
static uint32_t g_lock = 0; static V3 g_lockPrev, g_lockVel; static bool g_lockHasPrev = false;
static float g_lastYaw = 0, g_lastPitch = 0; static bool g_haveLast = false;
static float g_wrX = 0, g_wrZ = 0;                              // what the mod last put into m_lookAngleAccum
static float g_errStart = 0; static int g_errFrames = 0;        // "does the camera follow?" check
static const char* volatile g_notice = nullptr;

// One arrow in flight (from CProjectile::Launch until it stops or 2 s).
static volatile LONG g_shotReady = 0;
static struct { V3 p0, v0, camPos, camFwd; uint32_t ent; double t0; float hold, speedScale, vlen; } g_newShot;
static mk::ShotTrack g_track; static uint32_t g_trackEnt = 0; static double g_trackT0 = 0;

static V3 rd3(uintptr_t a) { return V3{F<float>(a, 0), F<float>(a, 4), F<float>(a, 8)}; }
static bool camera(V3& pos, V3& fwd) {
    uintptr_t sys = *(uintptr_t*)G.sysGlobal; if (!sys) return false;
    float* m = G.viewCamera((void*)sys); if (!m) return false;      // CCamera::m_Matrix (Matrix34, rows)
    fwd = V3{m[1], m[5], m[9]}; pos = V3{m[3], m[7], m[11]};
    float l = fwd.len(); if (!(l > 0.5f && l < 1.5f)) return false;
    fwd = fwd * (1.f / l); return true;
}
static uintptr_t entity_system() { uintptr_t es = *(uintptr_t*)(G.sysGlobal - GENV_SYSTEM_TO_ENTSYS); return es && kc::is_a(es, G.vtEntSys) ? es : 0; }
static uintptr_t actor_system() { uintptr_t fw = *(uintptr_t*)G.fwGlobal; if (!fw) return 0; uintptr_t as = F<uintptr_t>(fw, 0x518); return as && kc::is_a(as, G.vtActorSys) ? as : 0; }
static const char* ent_name(uintptr_t e, char* buf, size_t cap) {
    __try { const char* n = F<const char*>(e, E_NAME); if (n && kc::readable((uintptr_t)n, 1)) { strncpy_s(buf, cap, n, _TRUNCATE); return buf; } }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    strcpy_s(buf, cap, "?"); return buf;
}

// Aim point on an actor's entity, plus whether it is a person or an animal (by the shape of its box).
static bool aim_point(uintptr_t ent, V3& out, bool& animal, float* dims = nullptr) {
    if (!kc::is_a(ent, G.vtEntity)) return false;
    float b[6] = {0}; G.bounds((void*)ent, b);
    V3 mn{b[0], b[1], b[2]}, mx{b[3], b[4], b[5]};
    float h = mx.z - mn.z, lx = mx.x - mn.x, ly = mx.y - mn.y;
    if (!(h > 0.1f && h < 6.f && lx > 0.f && lx < 8.f && ly > 0.f && ly < 8.f)) return false;
    V3 fwd{F<float>(ent, E_TM + 0x04), F<float>(ent, E_TM + 0x14), F<float>(ent, E_TM + 0x24)};   // column 1
    animal = std::max(lx, ly) > 1.25f * h;
    out = mk::vital_point(mn, mx, fwd, g_headDrop);
    if (dims) { dims[0] = lx; dims[1] = ly; dims[2] = h; }
    return true;
}

// Best target near the centre of the view (the current one keeps the lock unless a much better one appears).
static uint32_t pick(const V3& camPos, const V3& camFwd) {
    uintptr_t as = actor_system(); if (!as) return 0;
    void* it = nullptr; G.mkIter((void*)as, &it);
    if (!it || !kc::is_a((uintptr_t)it, G.vtIter)) return g_lock;
    uint32_t best = 0; float bestScore = 1e9f, curScore = 1e9f; int seen = 0;
    for (void* a; (a = G.iterNext(it)) != nullptr && seen < 4096; seen++) {
        uintptr_t actor = (uintptr_t)a; if (actor == g_player) continue;
        uintptr_t ent = F<uintptr_t>(actor, A_ENTITY); if (!ent) continue;
        V3 p; bool animal;
        if (!aim_point(ent, p, animal)) continue;
        if ((animal && !g_animals) || (!animal && !g_people)) continue;
        V3 r = p - camPos; float d = r.len();
        if (d < 3.f || d > g_maxDist) continue;
        float ang = mk::angle_between(r, camFwd);
        uint32_t id = F<uint32_t>(ent, E_ID);
        if (ang > (id == g_lock ? 1.5f * g_cone : g_cone)) continue;
        bool dead = ((DeadByHp_t)((*(uintptr_t**)actor)[38]))(a);
        if (dead) continue;
        float score = ang + 0.002f * d;
        if (id == g_lock) curScore = score;
        if (score < bestScore) { bestScore = score; best = id; }
    }
    G.iterRel(it);
    if (g_lock && curScore < 1e8f && best != g_lock && bestScore > 0.5f * curScore) return g_lock;   // hysteresis
    return best;
}

static void reset_aim() { g_lock = 0; g_lockHasPrev = false; g_haveLast = false; g_errFrames = 0; g_outOfRange = false; }

// ---------------- arrows: calibration ----------------
static void track_arrows(double now, const V3& camPos, const V3& camFwd) {
    (void)camPos; (void)camFwd;
    if (InterlockedExchange(&g_shotReady, 0)) {
        auto& s = g_newShot;
        if (g_trackEnt) { float g, k; if (mk::fit_flight(g_track, g, k)) { g_cal.on_flight(g, k); } }
        g_cal.on_launch(s.p0, s.v0, s.camPos, s.camFwd);
        logf("arrow %d: speed %.1f m/s (|v| %.1f x %.2f) after %.2f s draw; launch vs camera yaw %+.2f pitch %+.2f deg, from (%.2f %.2f %.2f) m in camera axes -> speed now %.1f",
             g_cal.shots, s.v0.len(), s.vlen, s.speedScale, s.hold, mk::deg(mk::wrap_pi(mk::yaw_of(s.v0) - mk::yaw_of(s.camFwd))), mk::deg(mk::pitch_of(s.v0) - mk::pitch_of(s.camFwd)),
             g_cal.originOff.x, g_cal.originOff.y, g_cal.originOff.z, g_cal.speed);
        g_track = mk::ShotTrack(); g_track.p0 = s.p0; g_track.v0 = s.v0; g_trackEnt = s.ent; g_trackT0 = s.t0;
    }
    if (!g_trackEnt) return;
    uintptr_t es = entity_system(); uintptr_t e = es ? (uintptr_t)G.getEntity((void*)es, g_trackEnt) : 0;
    float t = (float)(now - g_trackT0);
    bool done = !e || t > 2.0f || g_track.n >= mk::ShotTrack::N;
    if (!done) g_track.add(t, V3{F<float>(e, E_TM + 0x0C), F<float>(e, E_TM + 0x1C), F<float>(e, E_TM + 0x2C)});
    if (done) {
        float g, k;
        if (mk::fit_flight(g_track, g, k)) { g_cal.on_flight(g, k); logf("arrow flight: %d samples, gravity %.2f drag %.4f -> using gravity %.2f drag %.4f", g_track.n, g, k, g_cal.flight.g, g_cal.flight.k); }
        else KC_DLOG("arrow flight: %d samples over %.2f s - too short to measure", g_track.n, t);
        g_trackEnt = 0;
    }
}

static void LaunchHook(void* self, const float* pos, const float* dir, const float* vel, float scale) {
    G.launch(self, pos, dir, vel, scale);
    if (g_dead) return;
    __try {
        uintptr_t p = (uintptr_t)self;
        if (!g_playerId || F<uint32_t>(p, PR_OWNER) != g_playerId || g_shotReady) return;
        auto& s = g_newShot;
        s.p0 = V3{pos[0], pos[1], pos[2]}; V3 v{vel[0], vel[1], vel[2]}; s.vlen = v.len(); s.speedScale = scale; s.v0 = v * scale;
        uintptr_t e = F<uintptr_t>(p, PR_ENTITY); s.ent = e ? F<uint32_t>(e, E_ID) : 0;
        if (!camera(s.camPos, s.camFwd)) return;
        s.t0 = now_s(); s.hold = g_wasAiming ? (float)(s.t0 - g_aimT0) : 0.f;
        InterlockedExchange(&g_shotReady, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("exception reading a launched arrow - calibration off"); g_dead = true; }
}

// ---------------- steering ----------------
static void steer(double now, float dt, uintptr_t look, const V3& camPos, const V3& camFwd) {
    // The player is moving the mouse: let go for a moment.
    float rqx = F<float>(look, L_REQUEST), rqz = F<float>(look, L_REQUEST + 8);
    if (fabsf(rqx) + fabsf(rqz) > 0.0015f) { if (now >= g_userUntil) KC_DLOG("player is aiming by hand - letting go"); g_userUntil = now + g_overrideS; g_haveLast = false; g_errFrames = 0; }
    if (now < g_userUntil) return;

    if (now - g_pickT > 0.1 || !g_lock) {
        g_pickT = now;
        uint32_t id = pick(camPos, camFwd);
        if (id != g_lock) {
            g_lock = id; g_lockHasPrev = false; g_haveLast = false; g_errFrames = 0; g_outOfRange = false;
            if (id) {
                uintptr_t as = actor_system(); void* a = as ? G.getActor((void*)as, id) : nullptr;
                uintptr_t ent = a ? F<uintptr_t>((uintptr_t)a, A_ENTITY) : 0; V3 p; bool animal = false; float dims[3] = {0};
                char nm[64]; if (ent && aim_point(ent, p, animal, dims))
                    logf("target: %s (%s) %.1f m away, box %.2f x %.2f x %.2f m", ent_name(ent, nm, sizeof nm), animal ? "animal: chest" : "person: head", (p - camPos).len(), dims[0], dims[1], dims[2]);
            }
        }
    }
    if (!g_lock) return;
    uintptr_t as = actor_system(); void* a = as ? G.getActor((void*)as, g_lock) : nullptr;
    uintptr_t ent = a ? F<uintptr_t>((uintptr_t)a, A_ENTITY) : 0; V3 tp; bool animal;
    if (!ent || !aim_point(ent, tp, animal)) { reset_aim(); return; }
    if (g_lockHasPrev && dt > 0.f) { V3 v = (tp - g_lockPrev) * (1.f / dt); if (v.len() < 15.f) g_lockVel = g_lockVel + (v - g_lockVel) * std::min(1.f, dt * 6.f); }
    else g_lockVel = V3{};
    g_lockPrev = tp; g_lockHasPrev = true;

    V3 origin = g_cal.origin(camPos, camFwd);
    mk::Solution s = mk::solve(origin, tp, g_lead ? g_lockVel : V3{}, g_cal.speed, g_cal.flight);
    if (!s.ok) { if (!g_outOfRange) logf("target out of range for this bow (%.0f m) - not aiming", (tp - origin).len()); g_outOfRange = true; g_haveLast = false; return; }
    g_outOfRange = false;
    float wy = mk::yaw_of(s.dir) - g_cal.yawOff, wp = mk::pitch_of(s.dir) - g_cal.pitchOff;
    float ey = mk::wrap_pi(wy - mk::yaw_of(camFwd)), ep = wp - mk::pitch_of(camFwd);
    float fy = 0, fp = 0;
    if (g_haveLast && dt > 0.f) { fy = mk::wrap_pi(wy - g_lastYaw) / dt; fp = (wp - g_lastPitch) / dt; if (fabsf(fy) > 3.f || fabsf(fp) > 3.f) fy = fp = 0; }
    g_lastYaw = wy; g_lastPitch = wp; g_haveLast = true;
    float dy, dp; g_steer.step(ey, ep, fy, fp, dt, dy, dp);

    // Safety: the camera must come closer to the aim point while the mod turns it.  If after half a second
    // of large errors it has not, something in the game is not as expected: stop and say so.
    float err = sqrtf(ey * ey + ep * ep);
    if (err > mk::rad(2.f)) {
        if (!g_errFrames) g_errStart = err;
        if (++g_errFrames > 30 && err > 0.9f * g_errStart) {
            logf("the camera does not follow the mod's turns (error %.1f -> %.1f deg) - assist disabled for this session", mk::deg(g_errStart), mk::deg(err));
            g_dead = true; return;
        }
    } else g_errFrames = 0;

    // Add the turn to the game's additive look input.  If our last write has not been used yet (no look
    // tick since), replace it instead of adding twice.
    float& ax = F<float>(look, L_ACCUM); float& az = F<float>(look, L_ACCUM + 8);
    if (ax == g_wrX && az == g_wrZ && (g_wrX != 0 || g_wrZ != 0)) { ax = dp; az = dy; }
    else { ax += dp; az += dy; }
    g_wrX = ax; g_wrZ = az;

    if (kc::g_debug && now - g_logT > 0.5) {
        g_logT = now;
        logf("aim: target %.1f m, flight %.2f s, lead (%.1f %.1f) m/s, error yaw %+.2f pitch %+.2f deg, turn %+.2f %+.2f deg",
             (tp - origin).len(), s.tof, g_lockVel.x, g_lockVel.y, mk::deg(ey), mk::deg(ep), mk::deg(dy), mk::deg(dp));
    }
}

static void tick() {
    double now = now_s(); float dt = g_lastT > 0 ? (float)std::min(0.1, now - g_lastT) : 0.f; g_lastT = now;
    if (g_key.pressed()) {
        g_on = !g_on; reset_aim();
        logf("assist %s", g_on ? "ON" : "OFF");
        g_notice = g_on ? "Game.SendInfoText(\"AutoMarksman on\", false)" : "Game.SendInfoText(\"AutoMarksman off\", false)";
    }
    uintptr_t fw = *(uintptr_t*)G.fwGlobal; if (!fw) return;
    uintptr_t pl = (uintptr_t)G.clientActor((void*)fw);
    if (!pl || !kc::is_a(pl, G.vtPlayer)) { g_player = 0; g_playerId = 0; reset_aim(); return; }
    uintptr_t ctrl = pl + P_MWCTRL, look = F<uintptr_t>(pl, A_LOOK);
    if (pl != g_checked) {        // a new player object (load): check the layout once
        bool ok = F<uintptr_t>(ctrl, 0) == G.vtMwCtrl && F<uintptr_t>(ctrl, MW_OWNER) == pl && look && kc::readable(look, 0x98) && F<uintptr_t>(look, L_OWNER) == pl;
        if (!ok) { if (++g_bad >= 3) { g_dead = true; logf("disabled: player data does not look right (game updated?)"); } return; }
        g_checked = pl; uintptr_t e = F<uintptr_t>(pl, A_ENTITY); g_playerId = e ? F<uint32_t>(e, E_ID) : 0;
        logf("player found (entity %u)", g_playerId);
    }
    g_player = pl;
    bool aiming = F<uint8_t>(ctrl, MW_AIMING) != 0;
    if (aiming && !g_wasAiming) { g_aimT0 = now; reset_aim(); }
    g_wasAiming = aiming;
    V3 camPos, camFwd; if (!camera(camPos, camFwd)) return;
    track_arrows(now, camPos, camFwd);
    if (!g_on || !aiming) { if (g_lock) reset_aim(); return; }
    steer(now, dt, look, camPos, camFwd);
}

static void* SsUpdateHook(void* self, void* a, void* b, void* c) {
    void* r = G.ssUpdate(self, a, b, c);
    if (!g_dead) {
        __try { tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { logf("exception in tick - assist disabled for this session"); g_dead = true; }
    }
    const char* line = (const char*)InterlockedExchangePointer((void* volatile*)&g_notice, nullptr);
    if (line && G.ssExec) { __try { G.ssExec(self, line, strlen(line), "AutoMarksman", nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { G.ssExec = nullptr; } }
    return r;
}

// ---------------- resolve ----------------
struct Found { uintptr_t ssUpdate, ssExec, fwGlobal, clientActor, sysGlobal, viewCamera, getEntity, bounds, getActor, mkIter, iterNext, iterRel, deadByHp, launch; };
static bool resolve(kc::Resolver& R, Found& f) {
    G.vtSS = R.vtable("CScriptSystem", ".?AVCScriptSystem@@", 0);
    uintptr_t cur = G.vtSS ? ((uintptr_t*)G.vtSS)[1] : 0;
    if (cur && !R.im.in_image(cur)) { if (R.find("CScriptSystem::Update", SIG_SS_UPDATE)) f.ssUpdate = cur; }   // another mod hooked it first: chain
    else f.ssUpdate = R.slot("CScriptSystem::Update", G.vtSS, 1, SIG_SS_UPDATE);
    f.ssExec = R.slot("CScriptSystem::ExecuteBuffer", G.vtSS, 6, SIG_SS_EXEC);
    uintptr_t fwref = R.find("game framework", SIG_FRAMEWORK_REF); f.fwGlobal = fwref ? R.riprel("game framework", fwref + 3) : 0;
    uintptr_t vtCA = R.vtable("CCryAction", ".?AVCCryAction@@", 0);
    f.clientActor = R.slot("CCryAction::GetClientActor", vtCA, 64, SIG_GETCLIENTACTOR);
    R.slot("CCryAction::GetIActorSystem", vtCA, 25, SIG_GETACTORSYS);
    uintptr_t cam = R.find("gEnv->pSystem", SIG_GENV_SYSTEM); f.sysGlobal = cam ? R.riprel("gEnv->pSystem", cam + 13) : 0;
    f.viewCamera = R.slot("CSystem::GetViewCamera", R.vtable("CSystem", ".?AVCSystem@@", 0), 135, SIG_VIEWCAMERA);
    G.vtEntSys = R.vtable("CEntitySystem", ".?AVCEntitySystem@@", 0); f.getEntity = R.slot("CEntitySystem::GetEntity", G.vtEntSys, 14, SIG_GETENTITY);
    G.vtEntity = R.vtable("CEntity", ".?AVCEntity@@", 0); f.bounds = R.slot("CEntity::GetWorldBounds", G.vtEntity, 34, SIG_GETBOUNDS);
    G.vtActorSys = R.vtable("CActorSystem", ".?AVCActorSystem@@", 0);
    f.getActor = R.slot("CActorSystem::GetActor", G.vtActorSys, 3, SIG_GETACTOR);
    f.mkIter = R.slot("CActorSystem::CreateActorIterator", G.vtActorSys, 7, SIG_MKITER);
    G.vtIter = R.vtable("CActorIterator", ".?AVCActorIterator@CActorSystem@@", 0);
    f.iterNext = R.slot("CActorIterator::Next", G.vtIter, 2, SIG_ITER_NEXT);
    f.iterRel = R.slot("CActorIterator::Release", G.vtIter, 4, SIG_ITER_REL);
    uintptr_t vtActor = R.vtable("C_Actor", ".?AVC_Actor@entitymodule@wh@@", 0);
    f.deadByHp = R.slot("C_Actor::IsDeadByHealth", vtActor, 38, SIG_DEADBYHP);
    R.slot("C_Actor::GetViewRotation (look state at +0x238)", vtActor, 57, SIG_GETVIEWROT);
    G.vtPlayer = R.vtable("C_Player", ".?AVC_Player@entitymodule@wh@@", 0);
    G.vtMwCtrl = R.vtable("C_MissileWeaponPlayerController", ".?AVC_MissileWeaponPlayerController@entitymodule@wh@@", 0);
    G.vtProjectile = R.vtable("CProjectile", ".?AVCProjectile@entitymodule@wh@@", 0);
    f.launch = R.slot("CProjectile::Launch", G.vtProjectile, 35, SIG_LAUNCH);
    R.find("look-angle tick (layout)", SIG_LOOKTICK);
    R.find("bow aim start (layout)", SIG_AIMSTART);
    R.find("arrow launch fields (layout)", SIG_LAUNCHSTORE);
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
    kc::init(g_self, "kcd2_automarksman");
    QueryPerformanceFrequency(&g_qpf);
    g_key.vk = kc::ini_key("Toggle", VK_F4);
    g_on = kc::ini_int("Options", "StartEnabled", 0) != 0;
    g_cone = mk::rad((float)std::max(2, std::min(30, kc::ini_int("Options", "ConeDegrees", 10))));
    g_maxDist = (float)std::max(10, std::min(200, kc::ini_int("Options", "MaxDistance", 80)));
    g_steer.maxRate = mk::rad((float)std::max(20, std::min(400, kc::ini_int("Options", "TurnSpeed", 115))));
    g_steer.gain = (float)std::max(2, std::min(30, kc::ini_int("Options", "Responsiveness", 9)));
    g_headDrop = std::max(0, std::min(40, kc::ini_int("Options", "HeadDropCm", 13))) / 100.f;
    g_overrideS = std::max(0, std::min(3000, kc::ini_int("Options", "HandsOffMs", 400))) / 1000.f;
    g_cal.speed = (float)std::max(15, std::min(150, kc::ini_int("Options", "ArrowSpeed", 55)));
    g_people = kc::ini_int("Options", "TargetPeople", 1) != 0;
    g_animals = kc::ini_int("Options", "TargetAnimals", 1) != 0;
    g_lead = kc::ini_int("Options", "LeadMovingTargets", 1) != 0;
    HMODULE h = kc::wait_game(); if (!h) { logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("start v0.1.0 (game ts 0x%08X)", R.im.timestamp);
    Found f{};
    if (!resolve(R, f)) { logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    G.fwGlobal = f.fwGlobal; G.sysGlobal = f.sysGlobal;
    G.clientActor = (GetClientActor_t)f.clientActor; G.viewCamera = (GetViewCamera_t)f.viewCamera; G.getEntity = (GetEntity_t)f.getEntity;
    G.bounds = (GetBounds_t)f.bounds; G.getActor = (GetActor_t)f.getActor; G.mkIter = (MkIter_t)f.mkIter; G.iterNext = (IterNext_t)f.iterNext;
    G.iterRel = (IterRel_t)f.iterRel; G.deadByHp = f.deadByHp; G.ssExec = (SsExec_t)f.ssExec;
    G.launch = (Launch_t)f.launch; G.ssUpdate = (SsUpdate_t)f.ssUpdate;
    kc::write_ptr(&((void**)G.vtProjectile)[35], (void*)&LaunchHook);
    kc::write_ptr(&((void**)G.vtSS)[1], (void*)&SsUpdateHook);
    char kn[16];
    logf("ready: %s switches the assist %s (now %s); cone %.0f deg, range %.0f m, turn speed %.0f deg/s, first-shot arrow speed %.0f m/s",
         kc::key_name(g_key.vk, kn, sizeof kn), "on/off", g_on ? "ON" : "OFF", mk::deg(g_cone), g_maxDist, mk::deg(g_steer.maxRate), g_cal.speed);
    return 0;
}
KC_PLUGIN("Theatrical AutoMarksman", KC_AUTHOR, 10)
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) { if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_self = h; KC_START(Init); } return TRUE; }
