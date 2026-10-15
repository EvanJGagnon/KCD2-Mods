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
// How it knows the bow: nothing about the bow is assumed.  The arrow launch (CProjectile::Launch, which
// arrows reach through their own CArrow vtable slot 35) is watched, so every arrow Henry shoots tells the
// mod its real launch speed (the game derives it from bow, arrow and Henry's strength), how long the bow
// was drawn, where the arrow left relative to the camera and in which direction; the arrow is then followed
// in flight to measure gravity and drag and how far it passed from the aim point.  What was measured is
// kept in kcd2_automarksman_calibration.txt, so only the very first arrow (or the first after a change of
// bow) uses the ArrowSpeed guess from the .ini; the game says so on screen when that is the case.
//
// Aim point: the "Bip01 Head" bone of a person (read from the character's skeleton pose, like the game's
// own Entity.GetBonePos), the chest of an animal.  Which target: the person (or, failing that, the
// animal) nearest the centre of the view; horses and dogs are skipped by default (mk_target.h).  The
// lock is soft: moving the mouse pauses the assist, looking away from the target releases it, and the
// next choice starts from wherever the player now looks.
//
// What is steered: not the centre of the screen but the predicted impact.  The game launches the player's
// arrow from the bow's "arrow" attachment joint along that joint's -Z axis (no camera ray is involved), at the
// speed of its RPG formula for this bow, arrow and draw.  Every frame the mod reads that pose and speed with
// the same calls the game's own trajectory preview makes (GetLaunchPose 0x289B28C, speed 0x289B594), works
// out where an arrow loosed now would cross the target, and turns the camera until that point is on the head
// (round 4; before, it used the camera plus an average offset, which round 3 showed to be off by up to 0.6
// degrees from shot to shot).  ShowImpactMarker=1 draws that point (and the head) with System.DrawLine.
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
#include "mk_target.h"
#include "mk_pose.h"
#include "mk_visibility.h"
#include <cctype>
#include <cstring>

using kc::logf;
using mk::V3;
template<class T> static T& F(uintptr_t p, uintptr_t o) { return *(T*)(p + o); }

// ---------------- signatures (build 15693 addresses in comments; struct offsets kept literal) ----------------
// C_ActorActionShootingAiming vtbl[55] 0x289C0D8 (player only): the game's own aim sway, a look rate
// amp * (cos(pi t)/3, 0, sin(pi t / 2)) that Main::Update adds to the look input every frame.
static const char* SIG_AIMSWAY = "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC 60 0F 29 78 E8 48 8B E9 48 8B 49 60 4C 8B F2";
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
// This game's own physics ray-cast wrapper, verified by a unique caller in
// C_ActorActionShootingFiring; exact Win64 argument ABI is established by
// WHGame.dll build 15693's wrapper, not assumed from a different CryEngine SDK.
static const char* SIG_PHYS_REF = "48 8B 0D ?? ?? ?? ?? 4C 8D 45 B0 48 89 45 78 48 8D 55 98 48 8D 05 ?? ?? ?? ?? 41 B9 1F 01 00 00";
static const char* SIG_PHYS_RAY = "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 F1 48 81 EC 90 00 00 00 48 8B DA 49 8B F8 33 D2 4C 8B F1";
static const char* SIG_LAUNCH      = "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 70 20 55 48 8D 68 D8 48 81 EC 20 01 00 00 0F 29 70 E8 48 8B D9 0F 29 78 D8"; // CProjectile vtbl[35] 0x2AA85A8
// Arrows: CArrow vtbl[35] is a 5-byte jmp to CProjectile::Launch (checked at startup); hooking only the
// CProjectile slot misses every arrow (round 1).
// Head bone, as the script bind Entity.GetBonePos (0x37D3EF8) does it:
static const char* SIG_GETCHAR  = "40 53 48 83 EC 20 8B DA 33 D2 E8 ?? ?? ?? ?? 4C 8D 40 B0 48 F7 D8 48 1B C9 49 23 C8 74 ?? 8B D3 48 83 C4 20 5B E9 ?? ?? ?? ?? 33 C0 48 83 C4 20 5B C3";            // CEntity vtbl[101] GetCharacter(slot) 0x6411AC
static const char* SIG_SLOTTM   = "40 53 48 83 EC 20 8B DA 33 D2 E8 ?? ?? ?? ?? 4C 8D 40 B0 48 F7 D8 48 1B C9 49 23 C8 74 ?? 8B D3 48 83 C4 20 5B E9 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 83 C4 20 5B C3"; // CEntity vtbl[91] GetSlotWorldTM(slot) 0xB7258C
static const char* SIG_CI_POSE  = "48 8D 81 98 06 00 00 C3";                                                 // CCharInstance vtbl[7] GetISkeletonPose 0x1AA6BC0
static const char* SIG_CI_SKEL  = "48 8B 81 70 0A 00 00 C3";                                                 // CCharInstance vtbl[11] GetIDefaultSkeleton 0x8A49D0
static const char* SIG_JOINTID  = "4C 8B C2 4C 8B C9 83 CA FF 41 8A 00 84 C0 74 ?? 3C 5A 7E ?? 41 8A 00 0F B6 C8";   // CDefaultSkeleton vtbl[7] GetJointIDByName 0x78BC4C
static const char* SIG_JOINTCOUNT = "48 8B 41 08 8B 40 FC C3";                                             // CDefaultSkeleton vtbl[1] GetJointCount 0x1AAF020
static const char* SIG_JOINTNAME  = "66 85 D2 78 ?? 4C 8B 41 08 0F BF CA 41 3B 48 FC 7D ?? 48 0F BF CA 48 8D 04 49 48 03 C0 49 8B 04 C0 C3";   // CDefaultSkeleton vtbl[6] GetJointNameByID 0x5AF5BC
static const char* SIG_ABSJOINT = "48 89 5C 24 08 57 48 83 EC 20 48 8D 99 48 02 00 00 8B FA 85 D2 78 ?? 48 8B 03";   // CSkeletonPose vtbl[27] GetAbsJointByID 0x641250
// Layout checks (found, not called): they fail if the fields the mod reads or writes have moved.
// Look-angle tick 0x4415E0: ... m_lookAngles(+8) copies, m_lookDeltaTransient(+0x4C) and m_lookAngleAccum(+0x88) cleared.
static const char* SIG_LOOKTICK = "40 53 48 83 EC 20 F2 0F 10 41 58 0F 28 CA 8B 41 60 48 8B D9 F2 0F 11 41 64 89 41 6C F2 0F 10 42 18 F2 0F 11 41 7C 8B 42 20 89 81 84 00 00 00 E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 48 20 83 B9 D0 03 00 00 00 0F 85 ?? ?? ?? ?? 8B 43 08 48 8B CB 89 43 48 8B 43 10 89 43 44 E8 ?? ?? ?? ?? 48 8B CB 48 C7 43 4C 00 00 00 00 C7 43 54 00 00 00 00 48 C7 83 88 00 00 00 00 00 00 00 C7 83 90 00 00 00 00 00 00 00 48 83 C4 20";
// C_Actor vtbl[57] GetViewRotation 0x94DB70: the look state is C_Actor+0x238, view quat at +0x24.
static const char* SIG_GETVIEWROT = "48 8B 81 38 02 00 00 0F 10 40 24 48 8B C2 0F 11 02 C3";
// C_MissileWeaponPlayerController aim start 0x2AAA4C0: m_isAiming is +0x20.
static const char* SIG_AIMSTART = "40 53 48 83 EC 20 48 8B DA C6 41 20 01 E8";
// Soul script bind IsInCombatMode 0x117F1E8: actor = soul's actor, in combat = (actor+0x278)->vtbl[1]().
static const char* SIG_COMBATMODE = "48 89 5C 24 08 57 48 83 EC 20 48 8B CA 48 8B FA E8 ?? ?? ?? ?? 33 DB 48 85 C0 0F 84 ?? ?? ?? ?? 33 D2 48 8B C8 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 78 02 00 00 48 85 C9 74 ?? 48 8B 01 FF 50 08 84 C0";
// CProjectile::Launch stores m_initialPosition/Direction/Velocity at +0xB0/+0xBC/+0xC8 (0x2AA8906).
// The bow's launch pose and arrow speed, from the game's own trajectory preview (C_ActorActionShootingAiming,
// 0x28962C8, the same calls Fire 0x28999C8 makes): loadout = ([actor+0x280])->0x7F3310, bow +0x40, ammo +0x48;
// GetLaunchPose 0x289B28C(sys, out{pos,up,dir}, actor, bow); speed 0x289B594(sys, power, bow, ammo, soul [actor+0x668]).
static const char* SIG_PREVIEW = "48 8B 88 80 02 00 00 E8 ?? ?? ?? ?? 48 8B 4E 60 4C 8B F0 48 8B 11 FF 52 10 48 8B B8 68 06 00 00 E8 ?? ?? ?? ?? 48 8B 4E 60 49 8B 5E 40 48 8B 11 FF 52 10 4C 8B CB 48 8D 55 A8 4C 8B C0 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 3D ?? ?? ?? ?? 4D 8B 4E 48 0F 28 CF 4D 8B 46 40 48 89 7C 24 20 E8";
static const char* SIG_LOADOUT    = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B 01 B2 04 48 8B F1 FF 50 70";
static const char* SIG_LAUNCHPOSE = "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF FF 48 81 EC C0 01 00 00";
static const char* SIG_BOWSPEED   = "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 41 56 48 83 EC 40 0F 29 70 E8 49 8B F9 0F 28 F1 49 8B D8 E8";
static const char* SIG_LAUNCHSTORE = "F2 0F 10 06 F2 0F 11 83 B0 00 00 00 8B 46 08 89 83 B8 00 00 00 F2 0F 10 07 F2 0F 11 83 BC 00 00 00 8B 47 08 89 83 C4 00 00 00 F2 41 0F 10 06 F2 0F 11 83 C8 00 00 00 41 8B 46 08 89 83 D0 00 00 00";

// ---------------- layouts (build 15693; see docs/NOTES.md) ----------------
static const uintptr_t A_ENTITY = 0x38, A_LOOK = 0x238, A_COMBAT = 0x278, P_MWCTRL = 0xD70;   // C_Actor / C_Player
static const uintptr_t MW_OWNER = 0x18, MW_AIMING = 0x20;                          // C_MissileWeaponPlayerController
static const uintptr_t L_OWNER = 0x00, L_ANGLES = 0x08, L_REQUEST = 0x7C, L_ACCUM = 0x88;   // C_ActorPhysicsState (Ang3: x pitch, z yaw)
static const uintptr_t E_ID = 0x0C, E_NAME = 0xE0, E_TM = 0x58;                    // CEntity (world TM rows at +0x58, translation +0x64/+0x74/+0x84)
static const uintptr_t PR_ENTITY = 0x38, PR_OWNER = 0x108;                         // CProjectile
static const uintptr_t GENV_SYSTEM_TO_ENTSYS = 0x20;
static const uintptr_t A_LOADOUT = 0x280, A_SOUL = 0x668, LO_BOW = 0x40, LO_AMMO = 0x48;          // C_Actor -> loadout (SIG_PREVIEW)
// The loadout is C_ActorShootingExpansion; its Main shooting action (+0x50, null when idle) holds the current
// sub-action at +0x98 (Charging = drawing a bow, InstantCharging = raising a crossbow or gun, Aiming = ready to
// loose, Firing, Reloading, Unloading, ...).  Bows, crossbows and firearms all use these classes.
static const uintptr_t SE_MAIN = 0x50, MAIN_SUB = 0x98;
static const uintptr_t FIRING_FIRED = 0xA4;        // C_ActorActionShootingFiring: 1 once "firearrow" launched the shot
static const uintptr_t MW_FAMILY = 0x28;           // C_MissileWeaponPlayerController: CryString "combat_bow" / "combat_crossbow" / "combat_gun"
enum { FAM_UNKNOWN, FAM_BOW, FAM_CROSSBOW, FAM_GUN };
static const char* const FAM_NAME[4] = {"weapon", "bow", "crossbow", "hand cannon"};
static const uintptr_t PR_AMMOPARAMS = 0x68, AP_PARTICLE = 0x88, PP_AIRRES = 0x24, PP_GRAVITY = 0x44;   // CProjectile -> ammo params -> pe_params_particle                               // gEnv.pEntitySystem (+0xA0) = &gEnv.pSystem (+0xC0) - 0x20

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
typedef void* (*GetChar_t)(void*, int);
typedef const float* (*SlotTM_t)(void*, int);
typedef void* (*CiGet_t)(void*);
typedef int16_t (*JointId_t)(void*, const char*);
typedef const float* (*AbsJoint_t)(void*, int);
typedef bool (*InCombat_t)(void*);
typedef void* (*Loadout_t)(void*);
// WHGame wrapper accepts Vec3* origin/dir, objTypes, flags, hit buffer, maxHits,
// skip entities/count, foreign data/id, and a debug tag (12 arguments).
typedef int (*PhysRay_t)(void*, const V3*, const V3*, int, unsigned, void*,
                         int, void*, int, void*, int, const char*);
typedef void* (*WSys_t)();
typedef void  (*LaunchPose_t)(void*, float*, void*, void*);
typedef float (*BowSpeedFn_t)(void*, float, void*, void*, void*);

static struct Game {
    uintptr_t fwGlobal, sysGlobal, vtPlayer, vtMwCtrl, vtEntity, vtEntSys, vtActorSys, vtIter, vtProjectile, vtArrow, vtSS;
    uintptr_t vtChar, vtDefSkel, vtPose, imgLo, imgHi, textLo, textHi;
    uintptr_t vtHuman, vtNpc, vtHorse, vtDog, vtAnimal;
    uintptr_t physicsGlobal = 0; PhysRay_t physRay = nullptr;
    GetChar_t getChar; SlotTM_t slotTM; CiGet_t ciPose, ciSkel; JointId_t jointId; AbsJoint_t absJoint; Launch_t launchArrow;
    GetClientActor_t clientActor; GetViewCamera_t viewCamera; GetEntity_t getEntity; GetBounds_t bounds;
    GetActor_t getActor; MkIter_t mkIter; IterNext_t iterNext; IterRel_t iterRel; uintptr_t deadByHp;
    Launch_t launch; SsUpdate_t ssUpdate; SsExec_t ssExec;
    Loadout_t loadout; WSys_t wsys; LaunchPose_t launchPose; BowSpeedFn_t bowSpeed;
    uintptr_t vtShootExp, vtShootMain, vtSub[9];       // optional: weapon state (0 = not found, the pose gate alone decides)
} G;
enum { SUB_CHARGING, SUB_INSTANT, SUB_AIMING, SUB_FIRING, SUB_RELOADING, SUB_UNLOADING, SUB_UNCHARGING, SUB_TOIDLE, SUB_BASE, SUB_N };
static const char* const SUB_NAME[SUB_N] = {"drawing", "raising the weapon", "aiming", "firing", "reloading", "unloading", "lowering", "putting away", "between actions"};

// ---------------- settings ----------------
static kc::Hotkey g_key;
static bool g_on = false, g_dead = false, g_lead = true, g_hostileOnly = false, g_headBone = true, g_remember = true, g_marker = true;
static bool g_losEnabled = true, g_losFaulted = false; static double g_losLogT = -100;
static mk::TargetPolicy g_policy; static mk::Lock g_lockState;
static float g_maxDist = 80.f, g_headDrop = 0.13f, g_headAbove = 0.08f, g_overrideS = 0.4f;
static mk::Steer g_steer;
static mk::Calib g_cal;
// Live bow pose (round 4): the arrow leaves the bow's "arrow" joint along its -Z axis, at the game's own speed.
static bool g_live = true; static mk::Backoff g_poseBack;
// Round 5: steer only on a settled bow pose, on a slowly smoothed offset, with a dead zone and an acceleration
// limit; checks that trip pause the assist and start over instead of switching it off.
static mk::AimCtl g_ctl; static mk::Trips g_trips;
static bool g_gaveUp = false;                                    // steering off for the session (tracking and log go on)
static double g_pauseUntil = 0; static int g_tickFaults = 0;
static double g_noPoseT = -1;                                    // since when the pose has not been readable (-1: it is)
static bool g_waitLogged = false;
// Weapon state (-1 unknown layout, -2 idle/none, else SUB_*), and projectile physics per ammo (arrows, bolts and
// balls fly differently; the game's own parameters are read at each launch and kept per ammo item).
static int g_sub = -1, g_subLogged = -3; static bool g_toldNoState = false;
static uintptr_t g_subPtr = 0; static int g_family = FAM_UNKNOWN, g_familyLogged = -1, g_shotNo = 0;
// Native sway reduction: the Aiming vtable's sway slot is pointed at SwayHook only while the assist is on;
// the hook calls the game's own function and scales the look rate it returns (0 = no sway, 1 = vanilla).
typedef uint8_t* (*AimSway_t)(void* self, uint8_t* out);
static AimSway_t g_swayOrig = nullptr; static uintptr_t* g_swaySlot = nullptr; static bool g_swayHooked = false;
static float g_swayStrength[4] = {0.25f, 0.25f, 0.25f, 0.25f};  // per family (SwayStrength, SwayStrengthBow/Crossbow/Gun)
static volatile float g_swayScale = 1.f; static float g_swayPeakRaw = 0;
static void* g_curAmmo = nullptr;
struct PhysEntry { void* ammo; mk::Flight f; };
static PhysEntry g_physCache[8]; static int g_physN = 0;
// Projectile helper offset in the joint frame, per weapon (bow 64.5 cm, crossbow 15 cm, hand cannon 0 in round 5:
// one shared value put the start point up to 50 cm off after a weapon change).
struct SlotEntry { void* weapon; V3 slot; };
static SlotEntry g_slots[8]; static int g_slotN = 0;
static void* g_curWeapon = nullptr;
static const V3* slot_for(void* w) { for (int i = 0; i < g_slotN; i++) if (g_slots[i].weapon == w) return &g_slots[i].slot; return nullptr; }
static void slot_store(void* w, const V3& sl) {
    if (!w) return;
    for (int i = 0; i < g_slotN; i++) if (g_slots[i].weapon == w) { g_slots[i].slot = g_slots[i].slot + (sl - g_slots[i].slot) * 0.5f; return; }
    int i = g_slotN < 8 ? g_slotN++ : 7; g_slots[i].weapon = w; g_slots[i].slot = sl;
}
// Firing phase: the shot is not out yet until the projectile launches (a hand cannon burns its fuse for ~2.5 s
// after the click; a crossbow launches ~25 ms into Firing).  The aim is kept until then.
static double g_firingT0 = -10, g_lastLaunchT = -10;
static mk::SpeedCheck g_speedChk; static mk::PoseOffset g_off;
static mk::Flight g_phys; static bool g_physGame = false;        // gravity and air resistance from the arrow's own physics params

// ---------------- per-frame state (game thread) ----------------
static LARGE_INTEGER g_qpf;
static double now_s() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)g_qpf.QuadPart; }
static double g_lastT = 0, g_aimT0 = 0, g_userUntil = 0, g_pickT = 0, g_logT = 0, g_acqT = 0;
static char g_markerLua[1024]; static volatile LONG g_markerReady = 0;
static uintptr_t g_player = 0, g_checked = 0; static uint32_t g_playerId = 0; static int g_bad = 0;
static bool g_wasAiming = false, g_outOfRange = false, g_toldUncal = false;
static double g_releaseT = -10;                                  // last time the bow stopped being drawn
static uint32_t g_lock = 0; static V3 g_lockPrev, g_lockVel; static bool g_lockHasPrev = false;
static mk::FiringTargetPin g_firingPin;  // preserve the click-time target across a delayed gun ignition
static float g_lastYaw = 0, g_lastPitch = 0; static bool g_haveLast = false;
static float g_wrX = 0, g_wrZ = 0;                              // what the mod last put into m_lookAngleAccum
static float g_errStart = 0; static int g_errFrames = 0;        // "does the camera follow?" check
static const char* volatile g_notice = nullptr;
// KCD2 SendInfoText resolves @localization_keys. Plain text produces stray '@' in the HUD.
// The corresponding keys live in Localization/*_xml.pak (all supported game languages).
// Notices can still be suppressed by Notices=0, without affecting F4 or targeting.
static bool g_notices = true;
static const char* const NOTICE_ON   = "Game.SendInfoText(\"@tam_on\", true)";
static const char* const NOTICE_OFF  = "Game.SendInfoText(\"@tam_off\", true)";
static const char* const NOTICE_BOW  = "Game.SendInfoText(\"@tam_bow_measured\", true)";
static const char* const NOTICE_FIRST= "Game.SendInfoText(\"@tam_first_arrow\", true)";
// Crossbow diagnostic captures the actual, unmodified firing-pose direction even when
// nothing is targeted. It does not alter the camera, weapon skeleton or projectile.
static bool g_crossbowDiag = false;
static double g_diagNext = 0, g_diagPrevT = 0;
static bool g_diagPrevValid = false, g_diagPrevOn = false;
static V3 g_diagPrevPose, g_diagPrevView;
static float g_diagSwayPeakRaw = 0.f;   // max raw native view sway during this diagnostic sample

// What the aim looked like on the last frame the mod steered (for the per-shot log).
static struct { uint32_t target; V3 point, at, origin, dir; float dist, errYaw, errPitch, speed, gap; bool bone, live; double t; } g_aim;
// One arrow in flight (from the launch until it stops or 2 s).
static volatile LONG g_shotReady = 0;
static struct { V3 p0, v0, camPos, camFwd; uint32_t ent, owner; double t0; float hold, speedScale, vlen, gameSpeed; char name[48];
                mk::LivePose fire; float physK; V3 physG; bool physOk; void* ammo; void* weapon; int sub, family; double firingT0; } g_newShot;
static struct { bool valid; V3 at; } g_shotPred;                   // predicted impact at release, for the flight check
static V3 g_tgtP[mk::ShotTrack::N]; static float g_tgtT[mk::ShotTrack::N]; static int g_tgtN = 0;   // the target during the flight
static mk::ShotTrack g_track; static uint32_t g_trackEnt = 0, g_trackTarget = 0; static double g_trackT0 = 0; static int g_trackFamily = 0;
static float g_trackMiss = 1e9f; static V3 g_trackPrev; static bool g_trackHasPrev = false;

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

static bool in_image(uintptr_t a) { return a >= G.imgLo && a < G.imgHi; }
static bool in_text(uintptr_t a) { return a >= G.textLo && a < G.textHi; }

// ---------------- the bow's live launch pose ----------------
// The same two calls the game's trajectory preview makes every frame while the bow is drawn: where the bow's
// "arrow" joint is and where it points, and the arrow speed the game's formula gives for this bow, this arrow
// and a full draw (power 1).  Read-only: nothing is changed.  Faults switch it off (the measured camera
// offsets of the calibration are used instead).
static bool read_pose_raw(mk::LivePose& lp, float* speed) {
    lp.ok = false; if (speed) *speed = 0;
    uintptr_t holder = F<uintptr_t>(g_player, A_LOADOUT); if (!holder || !kc::readable(holder, 8) || !in_image(*(uintptr_t*)holder)) return false;
    uintptr_t lo = (uintptr_t)G.loadout((void*)holder); if (!lo || !kc::readable(lo + LO_BOW, 16)) return false;
    void* bow = F<void*>(lo, LO_BOW); void* ammo = F<void*>(lo, LO_AMMO); if (!bow) return false;
    g_curAmmo = ammo; g_curWeapon = bow;
    void* sys = G.wsys();
    float out[9] = {0};
    G.launchPose(sys, out, (void*)g_player, bow);
    lp.pos = V3{out[0], out[1], out[2]}; lp.up = V3{out[3], out[4], out[5]}; lp.dir = V3{out[6], out[7], out[8]};
    float dl = lp.dir.len(), ul = lp.up.len();
    if (!std::isfinite(dl + ul + lp.pos.x + lp.pos.y + lp.pos.z) || dl < 0.9f || dl > 1.1f || ul < 0.5f || ul > 1.5f) return false;
    lp.dir = lp.dir * (1.f / dl); lp.up = lp.up * (1.f / ul);
    lp.ok = true;
    if (speed && ammo) {
        void* soul = F<void*>(g_player, A_SOUL);
        if (soul) { float v = G.bowSpeed(sys, 1.f, bow, ammo, soul); if (std::isfinite(v) && v > 5.f && v < 300.f) *speed = v; }
    }
    return true;
}
static bool read_pose(mk::LivePose& lp, float* speed) {
    if (!g_live) return false;
    double now = now_s(); if (g_poseBack.blocked(now)) return false;
    __try { bool ok = read_pose_raw(lp, speed); if (ok) g_poseBack.ok(); return ok; }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        lp.ok = false;
        if (g_poseBack.fault(now)) { g_live = false; logf("bow pose: %d faults while reading it - aiming with the measured camera offsets from now on", g_poseBack.total); }
        else logf("bow pose: fault while reading it (%d in a row) - camera offsets for %.0f s, then trying again", g_poseBack.inRow, g_poseBack.until - now);
        return false;
    }
}
// Gravity and drag to aim with: the arrow's own physics parameters once an arrow has shown them, else the
// measured flights.
static const mk::Flight& flight_use() {
    for (int i = 0; i < g_physN; i++) if (g_physCache[i].ammo == g_curAmmo) return g_physCache[i].f;
    return g_physGame ? g_phys : g_cal.flight;
}
static void phys_store(void* ammo, const mk::Flight& f) {
    if (!ammo) return;
    for (int i = 0; i < g_physN; i++) if (g_physCache[i].ammo == ammo) { g_physCache[i].f = f; return; }
    int i = g_physN < 8 ? g_physN++ : 7; g_physCache[i].ammo = ammo; g_physCache[i].f = f;
}
// The weapon's shooting sub-action right now (SUB_*), -2 when no shooting action runs, -1 when the layout is
// not the one this build knows (then only the pose gate decides).
static int weapon_state_raw() {
    if (!G.vtShootExp || !G.vtShootMain) return -1;
    uintptr_t holder = F<uintptr_t>(g_player, A_LOADOUT); if (!holder || !kc::readable(holder, 8) || !in_image(*(uintptr_t*)holder)) return -1;
    uintptr_t exp = (uintptr_t)G.loadout((void*)holder); if (!exp || !kc::is_a(exp, G.vtShootExp) || !kc::readable(exp + SE_MAIN, 8)) return -1;
    uintptr_t m = F<uintptr_t>(exp, SE_MAIN); if (!m) return -2;
    if (!kc::is_a(m, G.vtShootMain) || !kc::readable(m + MAIN_SUB, 8)) return -1;
    uintptr_t sub = F<uintptr_t>(m, MAIN_SUB); g_subPtr = sub; if (!sub) return -2;
    uintptr_t vt = 0; if (!kc::readable(sub, 8)) return -1; vt = *(uintptr_t*)sub;
    for (int i = 0; i < SUB_N; i++) if (G.vtSub[i] && vt == G.vtSub[i]) return i;
    return -1;
}
static int weapon_state() { g_subPtr = 0; __try { return weapon_state_raw(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_subPtr = 0; return -1; } }
// Firing and the shot not launched yet (the hand cannon's fuse burns inside Firing until "firearrow").
// -1 when the flag cannot be read.
static int firing_unfired() {
    __try { if (g_sub != SUB_FIRING || !g_subPtr || !kc::readable(g_subPtr + FIRING_FIRED, 1)) return -1; return F<uint8_t>(g_subPtr, FIRING_FIRED) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
// Bow, crossbow or hand cannon: the name of the action map the game picked for the weapon in hand.
static int weapon_family(uintptr_t ctrl) {
    __try {
        const char* n = F<const char*>(ctrl, MW_FAMILY);
        if (!n || !kc::readable((uintptr_t)n, 16)) return FAM_UNKNOWN;
        if (!strncmp(n, "combat_bow", 10)) return FAM_BOW;
        if (!strncmp(n, "combat_crossbow", 15)) return FAM_CROSSBOW;
        if (!strncmp(n, "combat_gun", 10)) return FAM_GUN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return FAM_UNKNOWN;
}

static uint8_t* SwayHook(void* self, uint8_t* out) {
    uint8_t* r = g_swayOrig(self, out);
    __try {
        if (out && out[0]) {
            float* v = (float*)(out + 4); float k = g_swayScale;
            float m = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (m > g_swayPeakRaw) g_swayPeakRaw = m;
            // The native sway sample already incorporates Henry's live stats/condition.
            // Measure it without changing the source or assuming a particular skill level.
            if (g_crossbowDiag && g_family == FAM_CROSSBOW && m > g_diagSwayPeakRaw) g_diagSwayPeakRaw = m;
            v[0] *= k; v[1] *= k; v[2] *= k;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}
// Point the sway slot at the hook (on) or back at what was there before (off).  Only our own pointer is ever
// replaced on the way back, so another mod's hook in the same slot is left alone.
static void sway_hook(bool on) {
    if (!g_swaySlot || !g_swayOrig || on == g_swayHooked) return;
    if (on) { kc::write_ptr((void**)g_swaySlot, (void*)&SwayHook); g_swayHooked = true; }
    else { if (*g_swaySlot == (uintptr_t)&SwayHook) kc::write_ptr((void**)g_swaySlot, (void*)g_swayOrig); g_swayHooked = false; }
}

// ---------------- skeleton bones ----------------
// A joint of an entity's character, as the script bind Entity.GetBonePos does it: CEntity::GetCharacter(slot)
// -> CCharInstance (GetIDefaultSkeleton -> GetJointIDByName; GetISkeletonPose -> GetAbsJointByID: QuatT in
// model space) -> CEntity::GetSlotWorldTM(slot).  Round 3 found every head lookup failing silently, so each
// step now reports why it failed (bone_why), the class checks accept any object whose vtable has the very
// functions the mod resolved (a subclass of the checked class), and when no joint has the asked name the
// skeleton is searched for one whose name contains the given word ("head", "arrow").
enum BoneStep { BS_OK, BS_NOCHAR, BS_CHARTYPE, BS_NOSKEL, BS_SKELTYPE, BS_NOJOINT, BS_NOPOSE, BS_POSETYPE, BS_NOTM, BS_BADVAL, BS_FAULT };
static const char* bone_why(int s) {
    static const char* w[] = {"ok", "entity has no character in that slot", "character is not a CCharInstance", "character has no skeleton",
        "skeleton is not a CDefaultSkeleton", "no joint of that name", "character has no pose", "pose is not a CSkeletonPose",
        "no slot transform", "joint position not finite or zero", "fault while reading"};
    return s >= 0 && s <= BS_FAULT ? w[s] : "?";
}
typedef int (*JointCount_t)(void*); typedef const char* (*JointName_t)(void*, int);
static JointCount_t g_jointCount = nullptr; static JointName_t g_jointName = nullptr;   // CDefaultSkeleton vtbl[1], [6]
struct Bone { V3 pos; float rot[9]; int id; uintptr_t chVt, skVt, poVt; V3 local; };   // rot: world rotation, row-major
// "Is this a CCharInstance?": its own vtable, or one with the same functions in the slots the mod calls.
static bool vt_has(uintptr_t obj, uintptr_t vt, int i1, uintptr_t f1, int i2 = -1, uintptr_t f2 = 0) {
    uintptr_t v = *(uintptr_t*)obj; if (v == vt) return true;
    if (!in_image(v)) return false;
    const uintptr_t* t = (const uintptr_t*)v;
    return t[i1] == f1 && (i2 < 0 || t[i2] == f2);
}
static struct { uintptr_t skel; char key[16]; int16_t id; } g_jointCache[48]; static int g_jointNext = 0;
static int find_joint(uintptr_t skel, const char* const* names, const char* word) {
    for (auto& c : g_jointCache) if (c.skel == skel && !strcmp(c.key, names[0])) return c.id;
    int id = -1;
    for (int i = 0; names[i] && id < 0; i++) id = G.jointId((void*)skel, names[i]);
    if (id < 0 && word && g_jointCount && g_jointName) {        // search the skeleton's joint names
        int n = g_jointCount((void*)skel);
        for (int j = 0; j < n && j < 1024 && id < 0; j++) {
            const char* nm = g_jointName((void*)skel, j); if (!nm || !kc::readable((uintptr_t)nm, 1)) continue;
            char low[64]; int k = 0; for (; nm[k] && k < 63; k++) low[k] = (char)tolower((unsigned char)nm[k]); low[k] = 0;
            if (strstr(low, word) && !strstr(low, "nub") && !strstr(low, "end") && !strstr(low, "twist")) { id = j; logf("   skeleton joint %d \"%s\" used for \"%s\"", j, nm, names[0]); }
        }
    }
    auto& c = g_jointCache[g_jointNext]; g_jointNext = (g_jointNext + 1) % 48;
    c.skel = skel; strncpy_s(c.key, names[0], _TRUNCATE); c.id = (int16_t)id;
    return id;
}
static int bone_raw(uintptr_t ent, int slot, const char* const* names, const char* word, Bone& b) {
    b.chVt = b.skVt = b.poVt = 0; b.id = -1;
    uintptr_t ch = (uintptr_t)G.getChar((void*)ent, slot); if (!ch) return BS_NOCHAR;
    b.chVt = *(uintptr_t*)ch - G.imgLo;
    if (!vt_has(ch, G.vtChar, 7, (uintptr_t)G.ciPose, 11, (uintptr_t)G.ciSkel)) return BS_CHARTYPE;
    uintptr_t skel = (uintptr_t)G.ciSkel((void*)ch); if (!skel) return BS_NOSKEL;
    b.skVt = *(uintptr_t*)skel - G.imgLo;
    if (!vt_has(skel, G.vtDefSkel, 7, (uintptr_t)G.jointId)) return BS_SKELTYPE;
    int id = find_joint(skel, names, word); b.id = id; if (id < 0) return BS_NOJOINT;
    uintptr_t pose = (uintptr_t)G.ciPose((void*)ch); if (!pose) return BS_NOPOSE;
    b.poVt = *(uintptr_t*)pose - G.imgLo;
    if (!vt_has(pose, G.vtPose, 27, (uintptr_t)G.absJoint)) return BS_POSETYPE;
    const float* qt = G.absJoint((void*)pose, id); const float* m = G.slotTM((void*)ent, slot);
    if (!qt || !m) return BS_NOTM;
    float qx = qt[0], qy = qt[1], qz = qt[2], qw = qt[3], x = qt[4], y = qt[5], z = qt[6];   // QuatT: q (x y z w), then t
    b.local = V3{x, y, z};
    if (!std::isfinite(x + y + z + qx + qy + qz + qw) || (x == 0 && y == 0 && z == 0)) return BS_BADVAL;
    b.pos = V3{m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11]};
    // Joint rotation (quaternion -> matrix), then into the world with the slot's rotation.
    float r[9] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw),
                  2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw),
                  2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) b.rot[i * 3 + j] = m[i * 4] * r[j] + m[i * 4 + 1] * r[3 + j] + m[i * 4 + 2] * r[6 + j];
    return BS_OK;
}
static int bone(uintptr_t ent, int slot, const char* const* names, const char* word, Bone& b) {
    __try { return bone_raw(ent, slot, names, word, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return BS_FAULT; }
}
static const char* const HEAD_NAMES[] = {"Bip01 Head", "Bip01_Head", "head", nullptr};
static int g_boneFaults = 0;
static int g_boneWhy = -1; static Bone g_boneInfo; static V3 g_boneAt;   // the last head lookup, for the log
// Head of a person: the "Bip01 Head" joint (base of the skull) plus HeadAboveBoneCm.  why = BoneStep.
static bool head_bone(uintptr_t ent, V3& out, int* why = nullptr, Bone* info = nullptr) {
    Bone b; int s = bone(ent, 0, HEAD_NAMES, "head", b);
    if (s == BS_FAULT && ++g_boneFaults >= 20) { g_headBone = false; logf("head bone: 20 faults while reading skeletons - using the body box from now on"); }
    if (why) *why = s; if (info) *info = b;
    if (s != BS_OK) return false;
    out = b.pos; out.z += g_headAbove;           // the joint sits at the base of the skull; aim at its middle
    return true;
}

// Is this actor in combat (fighting or alerted to a fight)?  Same test as the script bind soul:IsInCombatMode().
static bool in_combat_raw(uintptr_t actor) {
    uintptr_t o = F<uintptr_t>(actor, A_COMBAT); if (!o || !kc::readable(o, 8)) return false;
    uintptr_t vt = *(uintptr_t*)o; if (!in_image(vt)) return false;
    uintptr_t fn = ((uintptr_t*)vt)[1]; if (!in_text(fn)) return false;
    return ((InCombat_t)fn)((void*)o);
}
static bool in_combat(uintptr_t actor) {
    __try { return in_combat_raw(actor); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_hostileOnly = false; logf("exception reading combat state - HostileOnly switched off for this session"); return true; }
}

// What kind of actor: by its class (C_Horse, C_Dog, C_Animal, C_Human / C_NPCActor); -1 = unknown class.
static int actor_kind(uintptr_t actor) {
    uintptr_t vt = 0; __try { vt = *(uintptr_t*)actor; } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (vt == G.vtHorse) return mk::K_HORSE;
    if (vt == G.vtDog) return mk::K_DOG;
    if (vt == G.vtAnimal) return mk::K_ANIMAL;
    if (vt == G.vtHuman || vt == G.vtNpc) return mk::K_PERSON;
    return -1;
}

// Aim point on an actor's entity, plus whether it is a person or an animal (by the shape of its box).
static bool aim_point(uintptr_t ent, V3& out, bool& animal, float* dims = nullptr, bool* bone = nullptr, bool useBone = true) {
    if (!kc::is_a(ent, G.vtEntity)) return false;
    float b[6] = {0}; G.bounds((void*)ent, b);
    V3 mn{b[0], b[1], b[2]}, mx{b[3], b[4], b[5]};
    float h = mx.z - mn.z, lx = mx.x - mn.x, ly = mx.y - mn.y;
    if (!(h > 0.1f && h < 6.f && lx > 0.f && lx < 8.f && ly > 0.f && ly < 8.f)) return false;
    V3 fwd{F<float>(ent, E_TM + 0x04), F<float>(ent, E_TM + 0x14), F<float>(ent, E_TM + 0x24)};   // column 1
    animal = std::max(lx, ly) > 1.25f * h;
    out = mk::vital_point(mn, mx, fwd, g_headDrop);
    bool usedBone = false;
    g_boneWhy = -1;
    if (!animal && g_headBone && useBone) {      // the head bone, if it is plausibly on this body (else keep the box estimate)
        V3 hb; int why = 0;
        if (head_bone(ent, hb, &why, &g_boneInfo)) {
            bool inside = hb.x > mn.x - 0.5f && hb.x < mx.x + 0.5f && hb.y > mn.y - 0.5f && hb.y < mx.y + 0.5f && hb.z > mn.z + 0.3f * h && hb.z < mx.z + 0.35f;
            if (inside) { out = hb; usedBone = true; } else why = 100;     // 100: outside the body's box
        }
        g_boneWhy = why; g_boneAt = hb;
    }
    if (dims) { dims[0] = lx; dims[1] = ly; dims[2] = h; }
    if (bone) *bone = usedBone;
    return true;
}

// Why the head bone was (not) used for this target: every step of the lookup, so a failure can be fixed.
static void log_head(uintptr_t ent, bool used) {
    const Bone& b = g_boneInfo; float bb[6] = {0}; G.bounds((void*)ent, bb);
    if (g_boneWhy < 0) { logf("   head: bone lookup off (HeadBone=0)"); return; }
    if (used) { logf("   head: joint %d at (%.2f %.2f %.2f), %.2f m above the feet of a %.2f m box; model-space (%.2f %.2f %.2f)",
                     b.id, b.pos.x, b.pos.y, b.pos.z, b.pos.z - bb[2], bb[5] - bb[2], b.local.x, b.local.y, b.local.z); return; }
    logf("   head: NOT the bone (%s) - aiming %.0f cm below the top of the box instead; character vt +%llx, skeleton vt +%llx, pose vt +%llx, joint %d, "
         "bone (%.2f %.2f %.2f) model (%.2f %.2f %.2f), box z %.2f..%.2f",
         g_boneWhy == 100 ? "bone is outside the body's box" : bone_why(g_boneWhy), g_headDrop * 100.f, (unsigned long long)b.chVt, (unsigned long long)b.skVt,
         (unsigned long long)b.poVt, b.id, g_boneAt.x, g_boneAt.y, g_boneAt.z, b.local.x, b.local.y, b.local.z, bb[2], bb[5]);
}

// Solid-world ray query: static props, terrain, doors, rigid/sleeping physics
// (ent_static|ent_sleeping_rigid|ent_rigid|ent_terrain = 0x107).
// Excludes characters to avoid immediately hitting Henry's own physical body,
// and intentionally errs on denying a lock if the query cannot be verified.
// Read only, no changes to any game physical entities or projectile velocity.
static bool solid_segment_clear(const V3& from, const V3& to) {
    if (!g_losEnabled) return true; // configurable compatibility opt-out
    if (g_losFaulted || !G.physicsGlobal || !G.physRay ||
        !kc::readable(G.physicsGlobal, sizeof(void*))) return false;
    V3 ray = to - from; float dist = ray.len();
    if (!std::isfinite(dist) || dist < 0.001f) return false;
    // End 10 cm before the requested point: a solid target-adjacent structure
    // should not appear to occlude its own visible surface.
    ray = ray * (std::max(0.001f, dist - 0.10f) / dist);
    alignas(16) uint8_t hit[128] = {};
    __try {
        void* world = *(void**)G.physicsGlobal;
        if (!world || !kc::readable((uintptr_t)world, 8)) { g_losFaulted = true; return false; }
        // rwi_stop_at_pierceable=0xf, one solid hit. The game's raycast wrapper
        // calls IPhysicalWorld virtual slot +0x118 (confirmed in build 15693).
        int n = G.physRay(world, &from, &ray, 0x107, 0xF,
                          hit, 1, nullptr, 0, nullptr, 0, "AutoMarksmanLOS");
        if (n == 0) return true;
        if (n != 1) { g_losFaulted = true; logf("visibility physics ray returned %d hits for a 1-hit buffer; all locks disabled", n); return false; }
        float d = *(const float*)hit; // ray_hit.dist is at offset zero in this game.
        if (!std::isfinite(d) || d < -0.001f || d > dist + 1.f) {
            g_losFaulted = true; logf("visibility ray data invalid; all locks disabled"); return false;
        }
        if (kc::g_debug && now_s() - g_losLogT > 3.0) {
            g_losLogT = now_s();
            logf("visibility: solid collision at %.1f m of %.1f m; actor behind obstruction excluded", d, dist);
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_losFaulted = true;
        logf("visibility physics query faulted; target locking disabled for this session (safe fail-closed)");
        return false;
    }
}
// Candidates near the centre of the view, then the soft-lock decision (mk::Lock).
static mk::Lock::Why pick(const V3& camPos, const V3& camFwd, bool handsOn) {
    uintptr_t as = actor_system(); if (!as) return mk::Lock::NONE;
    void* it = nullptr; G.mkIter((void*)as, &it);
    if (!it || !kc::is_a((uintptr_t)it, G.vtIter)) return mk::Lock::KEPT;
    static mk::Cand c[256], visible[256]; static V3 places[256];
    int n = 0, seen = 0, calm = 0;
    float wide = std::max(g_policy.cone * g_policy.keep, mk::rad(20.f));
    for (void* a; (a = G.iterNext(it)) != nullptr && seen < 4096 && n < 256; seen++) {
        uintptr_t actor = (uintptr_t)a; if (actor == g_player) continue;
        uintptr_t ent = F<uintptr_t>(actor, A_ENTITY); if (!ent) continue;
        V3 p; bool boxAnimal;
        if (!aim_point(ent, p, boxAnimal, nullptr, nullptr, false)) continue;   // the box is enough to choose
        int kind = actor_kind(actor); if (kind < 0) kind = boxAnimal ? mk::K_ANIMAL : mk::K_PERSON;
        V3 r = p - camPos; float d = r.len();
        if (d < 2.f || d > g_maxDist) continue;
        float ang = mk::angle_between(r, camFwd);
        if (ang > wide) continue;
        bool dead = ((DeadByHp_t)((*(uintptr_t**)actor)[38]))(a);
        if (dead) continue;
        if (kind == mk::K_PERSON && g_hostileOnly && !in_combat(actor)) { calm++; continue; }   // optional: fights only
        c[n] = mk::Cand{F<uint32_t>(ent, E_ID), ang, d, kind};
        places[n] = p; ++n;
    }
    G.iterRel(it);
    // Traces only centre-ranked candidates, prioritizing the current lock.
    // A target not traced because of the bounded 8-candidate budget is
    // deliberately omitted, never implicitly treated as visible.
    mk::LivePose launch{}; float speed = 0;
    bool haveLaunch = g_losEnabled && g_sub == SUB_AIMING &&
        read_pose(launch, &speed) && speed > 5.f && (launch.pos - camPos).len() < 3.f;
    const mk::Flight& fl = flight_use();
    int clearN = g_losEnabled ? mk::visible_candidates(c, places, n, g_policy, g_lock, visible, 8,
        [&](const mk::Cand& candidate, const V3& pt) {
            if (!solid_segment_clear(camPos, pt)) return false; // visual LOS first
            if (!haveLaunch) return true; // not yet in a stable shooting state
            mk::Solution shot = mk::solve(launch.pos, pt, V3{}, speed, fl);
            return mk::projectile_path_clear(launch.pos, shot, speed, fl,
                [&](const V3& a, const V3& b) { return solid_segment_clear(a,b); });
        }) : n;
    mk::Lock::Why why = g_lockState.update(g_losEnabled ? visible : c,
                                         clearN, g_policy, handsOn);
    static double s_calmLogT = -100;
    if (!g_lockState.id && calm && now_s() - s_calmLogT > 5.0) { s_calmLogT = now_s(); logf("%d %s in view not in combat - not targeted (HostileOnly=1)", calm, calm == 1 ? "person" : "people"); }
    return why;
}

static void reset_steer() { g_ctl.reset(); g_errFrames = 0; g_haveLast = false; g_waitLogged = false; g_noPoseT = -1; }
static void reset_aim() { reset_steer(); g_off.have = false; g_lock = 0; g_lockState = mk::Lock(); g_lockHasPrev = false; g_haveLast = false; g_errFrames = 0; g_outOfRange = false; }

// ---------------- arrows: calibration ----------------
static void save_calib() {
    if (!g_remember) return;
    char path[MAX_PATH], line[256]; kc::path(path, sizeof path, "kcd2_automarksman_calibration.txt");
    g_cal.save(line, sizeof line);
    FILE* f = nullptr; if (fopen_s(&f, path, "w") == 0 && f) { fprintf(f, "%s\n", line); fclose(f); }
}
static void load_calib() {
    if (!g_remember) return;
    char path[MAX_PATH], line[256] = {0}; kc::path(path, sizeof path, "kcd2_automarksman_calibration.txt");
    FILE* f = nullptr; if (fopen_s(&f, path, "r") != 0 || !f) { logf("calibration: none saved yet - the first arrow measures the bow"); return; }
    bool ok = fgets(line, sizeof line, f) && g_cal.load(line); fclose(f);
    if (ok) logf("calibration loaded from an earlier session: %d arrows, speed %.1f m/s at full draw, gravity %.2f drag %.4f, bow %.2f m right %.2f m up of the eye",
                 g_cal.shots, g_cal.speed_at(10.f), g_cal.flight.g, g_cal.flight.k, g_cal.originOff.x, g_cal.originOff.z);
    else logf("calibration file unreadable - ignored, the first arrow measures the bow");
}

// Distance from point q to segment a-b.
static float seg_dist(const V3& a, const V3& b, const V3& q) {
    V3 ab = b - a; float l2 = ab.dot(ab), t = l2 > 1e-9f ? std::max(0.f, std::min(1.f, (q - a).dot(ab) / l2)) : 0.f;
    return (a + ab * t - q).len();
}
static bool target_point(uint32_t id, V3& p) {
    uintptr_t as = actor_system(); void* a = as && id ? G.getActor((void*)as, id) : nullptr;
    uintptr_t ent = a ? F<uintptr_t>((uintptr_t)a, A_ENTITY) : 0; bool animal;
    return ent && aim_point(ent, p, animal);
}

// After the flight: where the real arrow crossed the target's distance, against the head and against where the
// mod predicted at release (signed, in cm: up/right as seen from the bow).  This is the check that does not
// depend on the mod's own model.
static void finish_track(float t) {
    float dfAt = 0, dfSide = 0;
    if (mk::deflection(g_track, dfAt, dfSide))
        logf("   the projectile had left its flight line by %.1f m (%.0f cm to the %s): it brushed or hit something there - the miss below is not the aim's",
             dfAt, fabsf(dfSide) * 100.f, dfSide >= 0 ? "right" : "left");
    if (g_trackTarget && g_tgtN > 0) {
        V3 h = mk::heading(g_track.v0), at; float tc = 0;
        // the target where it was when the arrow got there (it may walk)
        auto tgt_at = [&](float tt) { int i = 0; while (i + 1 < g_tgtN && g_tgtT[i + 1] <= tt) i++; return g_tgtP[i]; };
        V3 tp = g_tgtP[0]; float d = 0;
        for (int it = 0; it < 3; it++) { V3 r = tp - g_track.p0; d = r.x * h.x + r.y * h.y; if (!mk::crossing(g_track, g_track.p0, h, d, at, tc)) break; tp = tgt_at(tc); }
        if (mk::crossing(g_track, g_track.p0, h, d, at, tc)) {
            float up, right; mk::miss_parts(h, at, tp, up, right);
            logf("   at the target (%.1f m, %.2f s): the arrow passed %+.0f cm %s, %+.0f cm %s of the %s",
                 d, tc, fabsf(up) * 100.f, up >= 0 ? "high" : "low", fabsf(right) * 100.f, right >= 0 ? "right" : "left", g_aim.bone ? "head bone" : "box estimate");
            if (g_shotPred.valid) {
                V3 r = g_shotPred.at - g_track.p0; float dp = r.x * h.x + r.y * h.y; V3 ap; float tp2;
                if (mk::crossing(g_track, g_track.p0, h, dp, ap, tp2)) {
                    float pu, pr; mk::miss_parts(h, ap, g_shotPred.at, pu, pr);
                    logf("   prediction check: at %.1f m the real arrow was %+.0f cm up, %+.0f cm right of the impact the mod predicted at release", dp, pu * 100.f, pr * 100.f);
                }
            }
        } else logf("   at the target: the arrow never reached its distance (%.1f m) - it hit something first or fell short", d);
    }
    float g, k, span = 0, rms = 0;
    if (mk::fit_flight(g_track, g, k, &span, &rms)) {
        if (!g_physGame && g_trackFamily != FAM_CROSSBOW && g_trackFamily != FAM_GUN) g_cal.on_flight(g, k);
        logf("flight: %d positions over %.0f m, measured gravity %.2f drag %.4f (fit within %.1f cm); aiming with gravity %.2f drag %.4f (%s)", g_track.n, span, g, k, rms * 100.f,
             flight_use().g, flight_use().k, g_physGame ? "the arrow's physics parameters" : "measured flights");
    } else logf("flight: %d positions over %.2f s, %.0f m of clean flight - not enough to measure the drop", g_track.n, t, span);
    if (g_trackTarget && g_trackMiss < 1e8f) logf("   closest approach to the aim point: %.0f cm", g_trackMiss * 100.f);
    save_calib();
    g_trackEnt = 0; g_trackTarget = 0; g_tgtN = 0;
}

static void track_arrows(double now) {
    if (InterlockedExchange(&g_shotReady, 0)) {
        auto& s = g_newShot;
        if (g_trackEnt) finish_track((float)(now - g_trackT0));
        float predicted = g_aim.live && now - g_aim.t < 1.0 ? g_aim.speed : g_cal.speed_at(s.hold); bool wasMeasured = g_cal.measured();
        // The measured fallback (calibration file) is the bow's: crossbow and hand cannon shots do not change it.
        bool bowShot = s.family == FAM_BOW || s.family == FAM_UNKNOWN;
        bool changed = bowShot && g_cal.on_launch(s.p0, s.v0, s.camPos, s.camFwd, s.hold);
        float sp = s.v0.len();
        logf("shot %d (%s, %s): speed %.1f m/s after %.2f s %s; the game's formula says %.1f m/s for a full draw%s",
             ++g_shotNo, FAM_NAME[s.family], s.name, sp, s.hold, s.family == FAM_BOW ? "draw" : "aim", s.gameSpeed, s.gameSpeed > 0 && fabsf(sp / s.gameSpeed - 1.f) > 0.03f ? " - DIFFERENT (early release, or an effect the formula call misses)" : "");
        if (s.gameSpeed > 0) { bool used = g_speedChk.add(sp, s.gameSpeed); if (!used) logf("   speed differs by %+.0f%% - not learnt from", 100.f * (sp / s.gameSpeed - 1.f)); }
        logf("   launch vs camera: yaw %+.2f pitch %+.2f deg (the bow's own aim; the camera is not the arrow's path)",
             mk::deg(mk::wrap_pi(mk::yaw_of(s.v0) - mk::yaw_of(s.camFwd))), mk::deg(mk::pitch_of(s.v0) - mk::pitch_of(s.camFwd)));
        if (s.fire.ok) {
            // The arrow helper's offset: joint minus launch point, in the joint frame (constant per arrow model).
            V3 sl = mk::slot_learn(s.fire.dir, s.fire.up, s.fire.pos - s.p0);
            if (sl.len() < 1.5f) slot_store(s.weapon, sl);
            float da = mk::angle_between(s.v0, s.fire.dir);
            logf("   bow pose at the shot: arrow leaves %.1f cm from the bow's arrow joint, %.2f deg off its axis (arrow helper offset %.1f %.1f %.1f cm)",
                 (s.fire.pos - s.p0).len() * 100.f, mk::deg(da), sl.x * 100.f, sl.y * 100.f, sl.z * 100.f);
        } else logf("   bow pose at the shot: not readable%s", g_live ? "" : " (switched off)");
        if (g_aim.live && now - g_aim.t < 1.0) {
            V3 h = mk::heading(s.v0), d = s.p0 - g_aim.origin;
            logf("   vs the mod's last aimed frame (%.0f ms earlier): direction yaw %+.2f pitch %+.2f deg, start point %.0f cm, speed %+.1f%%",
                 (s.t0 - g_aim.t) * 1000.0, mk::deg(mk::wrap_pi(mk::yaw_of(s.v0) - mk::yaw_of(g_aim.dir))), mk::deg(mk::pitch_of(s.v0) - mk::pitch_of(g_aim.dir)),
                 d.len() * 100.f, 100.f * (sp / std::max(1.f, predicted) - 1.f));
            (void)h;
        }
        if (s.physOk) {
            float gz = -s.physG.z;
            if (gz > 1.f && gz < 30.f && s.physK >= 0.f && s.physK < 2.f) {
                if (!g_physGame || fabsf(g_phys.g - gz) > 0.01f || fabsf(g_phys.k - s.physK) > 1e-4f)
                    logf("   projectile physics (ammo parameters): gravity %.2f, air resistance %.4f - used for this ammo from now on", gz, s.physK);
                g_phys.g = gz; g_phys.k = s.physK; g_physGame = true; phys_store(s.ammo, g_phys);
            } else if (!g_physGame) logf("   arrow physics (ammo parameters): gravity (%.2f %.2f %.2f), air resistance %.4f - not usable, the measured flights are used", s.physG.x, s.physG.y, s.physG.z, s.physK);
        }
        g_trackTarget = 0; g_shotPred.valid = false;
        if (g_aim.target && s.t0 - g_aim.t < 0.5) {
            char nm[64] = "?"; uintptr_t as = actor_system(); void* a = as ? G.getActor((void*)as, g_aim.target) : nullptr;
            uintptr_t ent = a ? F<uintptr_t>((uintptr_t)a, A_ENTITY) : 0; if (ent) ent_name(ent, nm, sizeof nm);
            logf("   at release: target %s, %s at (%.2f %.2f %.2f), %.1f m, aim error yaw %+.2f pitch %+.2f deg, predicted impact %.0f cm from it (%s)",
                 nm, g_aim.bone ? "head bone" : "box estimate", g_aim.point.x, g_aim.point.y, g_aim.point.z, g_aim.dist, mk::deg(g_aim.errYaw), mk::deg(g_aim.errPitch), g_aim.gap * 100.f,
                 g_aim.live ? "from the bow's live pose" : "from the camera and measured offsets");
            g_trackTarget = g_aim.target; g_shotPred.valid = g_aim.gap >= 0; g_shotPred.at = g_aim.at;
        } else logf("   at release: the assist was not aiming (off, hands-on or no target)");
        if (bowShot && changed && !wasMeasured && g_notices) g_notice = NOTICE_BOW;
        g_track = mk::ShotTrack(); g_track.p0 = s.p0; g_track.v0 = s.v0; g_trackEnt = s.ent; g_trackT0 = s.t0; g_trackFamily = s.family;
        g_trackMiss = 1e9f; g_trackPrev = s.p0; g_trackHasPrev = true; g_tgtN = 0;
        save_calib();
    }
    if (!g_trackEnt) return;
    uintptr_t es = entity_system(); uintptr_t e = es ? (uintptr_t)G.getEntity((void*)es, g_trackEnt) : 0;
    float t = (float)(now - g_trackT0);
    bool done = !e || t > 2.0f || g_track.n >= mk::ShotTrack::N;
    if (!done) {
        V3 p{F<float>(e, E_TM + 0x0C), F<float>(e, E_TM + 0x1C), F<float>(e, E_TM + 0x2C)};
        g_track.add(t, p);
        V3 tp; if (g_trackTarget && target_point(g_trackTarget, tp)) {
            g_trackMiss = std::min(g_trackMiss, seg_dist(g_trackPrev, p, tp));
            if (g_tgtN < mk::ShotTrack::N) { g_tgtT[g_tgtN] = t; g_tgtP[g_tgtN] = tp; g_tgtN++; }
        }
        g_trackPrev = p;
    }
    if (done) finish_track(t);
}

static int g_otherLaunches = 0;
static void on_launch(void* self, const float* pos, const float* vel, float scale) {
    if (g_dead) return;
    __try {
        uintptr_t p = (uintptr_t)self;
        if (!g_playerId || g_shotReady) return;
        uint32_t owner = F<uint32_t>(p, PR_OWNER);
        V3 camPos, camFwd; if (!camera(camPos, camFwd)) return;
        V3 p0{pos[0], pos[1], pos[2]};
        double t = now_s();
        // Henry's arrow: owned by him, or (should the owner not be filled in yet) leaving right at his eye
        // while he draws or has just released.
        bool mine = owner == g_playerId || ((p0 - camPos).len() < 2.5f && (g_wasAiming || t - g_releaseT < 0.3));
        if (!mine) { if (kc::g_debug && g_otherLaunches++ < 5) logf("launch by someone else (owner %u) - ignored", owner); return; }
        auto& s = g_newShot;
        s.p0 = p0; V3 v{vel[0], vel[1], vel[2]}; s.vlen = v.len(); s.speedScale = scale; s.v0 = v * scale; s.owner = owner;
        uintptr_t e = F<uintptr_t>(p, PR_ENTITY); s.ent = e ? F<uint32_t>(e, E_ID) : 0;
        if (e) ent_name(e, s.name, sizeof s.name); else strcpy_s(s.name, "?");
        s.camPos = camPos; s.camFwd = camFwd;
        s.t0 = t; s.hold = g_wasAiming ? (float)(t - g_aimT0) : (t - g_releaseT < 0.3 ? (float)(g_releaseT - g_aimT0) : 0.f);
        // The bow's pose and the game's speed right now (Launch runs inside the game's Fire, after the pose was
        // taken): the truth to check the prediction against, and the arrow helper's offset.
        s.fire.ok = false; s.gameSpeed = 0; read_pose(s.fire, &s.gameSpeed); s.ammo = g_curAmmo; s.weapon = g_curWeapon;
        s.sub = g_sub; s.family = g_family; s.firingT0 = g_firingT0; g_lastLaunchT = t;
        // The arrow's physics parameters (ammo params -> particle params: air resistance, gravity vector).
        s.physOk = false;
        uintptr_t ap = F<uintptr_t>(p, PR_AMMOPARAMS), pp = ap && kc::readable(ap + AP_PARTICLE, 8) ? F<uintptr_t>(ap, AP_PARTICLE) : 0;
        if (pp && kc::readable(pp, PP_GRAVITY + 12)) { s.physK = F<float>(pp, PP_AIRRES); s.physG = rd3(pp + PP_GRAVITY); s.physOk = std::isfinite(s.physK + s.physG.x + s.physG.y + s.physG.z); }
        InterlockedExchange(&g_shotReady, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedExchange(&g_shotReady, 0); logf("exception reading a launched arrow - this shot is not measured"); }
}
static void LaunchHook(void* self, const float* pos, const float* dir, const float* vel, float scale) {        // CProjectile vtbl[35]
    G.launch(self, pos, dir, vel, scale); on_launch(self, pos, vel, scale);
}
static void LaunchHookArrow(void* self, const float* pos, const float* dir, const float* vel, float scale) {   // CArrow vtbl[35]
    G.launchArrow(self, pos, dir, vel, scale); on_launch(self, pos, vel, scale);
}

// The weapon's shooting state, every frame while the bow/crossbow/gun is up.
static void update_weapon(double now) {
    g_sub = weapon_state();
    if (g_sub != g_subLogged) {
        if (g_sub == SUB_FIRING) {
            g_firingT0 = now;
            KC_DLOG("shot committed: target id %u; holding selection until Firing ends", g_lock);
        }
        if (g_sub >= 0 || g_sub == -2) KC_DLOG("weapon: %s", g_sub >= 0 ? SUB_NAME[g_sub] : "no shooting action");
        else if (!g_toldNoState) { g_toldNoState = true; logf("weapon state: not readable here - steering on the bow pose check alone"); }
        g_subLogged = g_sub;
    }
    g_firingPin.state(g_sub == SUB_FIRING, g_lock);
}

// ---------------- steering ----------------
static void steer(double now, float dt, uintptr_t look, const V3& camPos, const V3& camFwd) {
    // The player is moving the mouse: let go (the lock decides whether the target is still wanted).
    float rqx = F<float>(look, L_REQUEST), rqz = F<float>(look, L_REQUEST + 8);
    bool moving = fabsf(rqx) + fabsf(rqz) > 0.0015f;
    if (moving) { if (now >= g_userUntil) KC_DLOG("player is aiming by hand - letting go"); g_userUntil = now + g_overrideS; g_haveLast = false; g_errFrames = 0;
                  g_ctl.hands_on();
                  // User input during ignition cancels auto-steering of the committed shot.
                  // Never acquire a different target before this projectile launches.
                  if (g_firingPin.active && !g_firingPin.cancelled) {
                      g_firingPin.user_moved(); g_lock = 0; g_lockState.id = 0;
                      KC_DLOG("shot target follow cancelled by manual aim; no reacquisition before launch");
                  }
                }
    bool hands = now < g_userUntil;
    static bool s_hands = false; if (s_hands && !hands) g_acqT = now; s_hands = hands;   // ease back in after the mouse

    if (g_firingPin.may_select() && now - g_pickT > (g_lock && !moving ? 0.1 : 0.05)) {
        g_pickT = now;
        mk::Lock::Why why = pick(camPos, camFwd, hands);
        uint32_t id = g_lockState.id;
        if (id != g_lock) {
            g_lock = id; g_lockHasPrev = false; g_haveLast = false; g_errFrames = 0; g_outOfRange = false; g_acqT = now;
            if (why == mk::Lock::LOOKED_AWAY) logf("released: you looked away from the target");
            else if (why == mk::Lock::LOST) logf("target lost (dead, gone or out of view)");
            if (id) {
                uintptr_t as = actor_system(); void* a = as ? G.getActor((void*)as, id) : nullptr;
                uintptr_t ent = a ? F<uintptr_t>((uintptr_t)a, A_ENTITY) : 0; V3 p; bool animal = false; float dims[3] = {0};
                char nm[64]; bool bone = false; if (ent && aim_point(ent, p, animal, dims, &bone)) {
                    int kind = actor_kind((uintptr_t)a);
                    logf("target%s: %s (%s%s) %.1f m away at (%.2f %.2f %.2f), %.1f deg from the centre, box %.2f x %.2f x %.2f m",
                         why == mk::Lock::SWITCHED ? " (switched)" : "", ent_name(ent, nm, sizeof nm), kind < 0 ? (animal ? "animal?" : "person?") : mk::kind_name(kind),
                         animal ? ": chest" : bone ? ": head bone" : ": head from box", (p - camPos).len(), p.x, p.y, p.z, mk::deg(mk::angle_between(p - camPos, camFwd)), dims[0], dims[1], dims[2]);
                    if (!animal) log_head(ent, bone);
                }
            }
        }
    }
    if (hands || !g_firingPin.may_steer()) return;
    if (g_firingPin.active && g_lock != g_firingPin.id) return;  // fail closed: no substitute
    if (!g_lock) return;
    uintptr_t as = actor_system(); void* a = as ? G.getActor((void*)as, g_lock) : nullptr;
    uintptr_t ent = a ? F<uintptr_t>((uintptr_t)a, A_ENTITY) : 0; V3 tp; bool animal, bone = false;
    if (!ent || !aim_point(ent, tp, animal, nullptr, &bone)) { g_lock = 0; g_lockState.id = 0; return; }
    if (g_lockHasPrev && dt > 0.f) { V3 v = (tp - g_lockPrev) * (1.f / dt); if (v.len() < 15.f) g_lockVel = g_lockVel + (v - g_lockVel) * std::min(1.f, dt * 6.f); }
    else g_lockVel = V3{};
    g_lockPrev = tp; g_lockHasPrev = true;

    if (g_gaveUp || now < g_pauseUntil) return;

    // Weapon state (update_weapon(), every frame): steer only while the game's shooting action is in Aiming.
    // Drawing a bow, raising a crossbow or gun and reloading are never steered on.  (The controller's aiming
    // flag stays set through a crossbow or gun reload.)
    // Firing: keep aiming until this shot's projectile is out (a launch up to 0.15 s before Firing began is the
    // bow's, which launches first), at most 4 s.  The game decides whether the weapon still follows the view.
    int unfired = firing_unfired();
    bool waitingForLaunch = g_sub == SUB_FIRING && unfired != 0 && g_lastLaunchT < g_firingT0 - 0.15 && now - g_firingT0 < 4.0;
    if (g_sub != -1 && g_sub != SUB_AIMING && !waitingForLaunch) { reset_steer(); return; }

    // Where the arrow will start, which way it will go and how fast: the bow's live pose and the game's speed
    // formula when readable (round 4), else the camera plus the offsets measured from earlier arrows.
    mk::LivePose lp; float gameSpeed = 0;
    bool got = read_pose(lp, &gameSpeed) && (lp.pos - camPos).len() < 3.f;
    // Unreadable for a whole second (switched off, faulting, or a weapon without the joint): measured offsets.
    if (got) g_noPoseT = -1; else if (g_noPoseT < 0) g_noPoseT = now;
    bool live = got, fallback = !got && (!g_live || g_poseBack.blocked(now) || now - g_noPoseT > 1.0);
    float rawY = 0, rawP = 0;
    if (!fallback) {
        // Round 5: is this a drawn bow?  Not while it is being raised, nocked or blended (no turn at all then).
        bool was = g_ctl.gate.ready;
        mk::PoseGate::Why gw = g_ctl.pose(live, lp.dir, camFwd, dt, rawY, rawP);
        if (live && (gw == mk::PoseGate::OK || gw == mk::PoseGate::SETTLING)) g_off.update(lp.dir, camFwd, dt, 0.03f);
        if (gw != mk::PoseGate::OK) {
            if (!g_waitLogged || was) {
                g_waitLogged = true;
                logf("not steering: the bow's pose is %s%s (bow vs camera yaw %+.1f pitch %+.1f deg)", mk::gate_why(gw), was ? " - pausing" : "", mk::deg(rawY), mk::deg(rawP));
            }
            g_errFrames = 0;
            return;
        }
        if (!was) { logf("bow pose settled after %.2f s - aiming (bow vs camera yaw %+.2f pitch %+.2f deg)", g_ctl.gate.waited, mk::deg(rawY), mk::deg(rawP)); g_waitLogged = false; }
    }
    V3 origin, launchDir; float speed;
    if (live) {
        const V3* sl = slot_for(g_curWeapon);
        origin = mk::launch_point(lp, sl ? *sl : V3{});
        launchDir = lp.dir;                                             // honest: the marker and the log use this frame's pose
        speed = gameSpeed > 0 ? gameSpeed * g_speedChk.ratio : g_cal.speed_at((float)(now - g_aimT0));
    } else {
        g_off.have = false;
        origin = g_cal.origin(camPos, camFwd);
        launchDir = mk::dir_of(mk::yaw_of(camFwd) + g_cal.yawOff, mk::pitch_of(camFwd) + g_cal.pitchOff);
        speed = g_cal.speed_at((float)(now - g_aimT0));
    }
    // Steering uses the tracked offset: the camera follows the bow's slow sway, not its tremor or each frame's wobble.
    float offY = live ? g_ctl.off.yaw : g_cal.yawOff, offP = live ? g_ctl.off.pitch : g_cal.pitchOff;
    float offVY = live ? g_ctl.off.vy : 0.f, offVP = live ? g_ctl.off.vp : 0.f;
    const mk::Flight& fl = flight_use();
    mk::Solution s = mk::solve(origin, tp, g_lead ? g_lockVel : V3{}, speed, fl);
    if (!s.ok) { if (!g_outOfRange) logf("target out of range for this bow (%.0f m) - not aiming", (tp - origin).len()); g_outOfRange = true; g_haveLast = false; g_ctl.glide.reset(); return; }
    g_outOfRange = false;
    // The camera turn that makes the bow point along the wanted launch direction (smoothed offset, dead zone,
    // acceleration limit; gentle start: the turn speed builds up over 0.4 s after a target is taken).
    float dist = (tp - origin).len(), dy = 0, dp = 0, ey, ep;
    float ramp = std::min(1.f, 0.3f + 0.7f * (float)(now - g_acqT) / 0.4f);
    ey = mk::wrap_pi(mk::yaw_of(s.dir) - offY - mk::yaw_of(camFwd)); ep = mk::pitch_of(s.dir) - offP - mk::pitch_of(camFwd);
    float err = sqrtf(ey * ey + ep * ep);
    auto pause_aim = [&](const char* why) {
        bool settled = g_ctl.gate.ready && fabsf(offY) < mk::rad(5.f) && fabsf(offP) < mk::rad(5.f);
        reset_steer(); g_pauseUntil = now + 0.5;
        if (g_trips.trip(settled)) { g_gaveUp = true; logf("%s - %d times in a row with a settled bow: steering off for this session (restart the game)", why, g_trips.inRow); }
        else logf("%s - pausing, then aiming again once the bow is settled", why);
    };
    // A wanted turn far bigger than the target cone: the pose or the target is not what it seems.
    if (err > mk::rad(25.f)) { char b[128]; sprintf_s(b, "the wanted turn is %.0f deg (more than the target cone)", mk::deg(err)); pause_aim(b); return; }
    bool moveCam = g_ctl.turn(s.dir, camFwd, offY, offP, offVY, offVP, dist, ramp, dt, dy, dp, ey, ep);
    g_lastYaw = mk::yaw_of(s.dir) - offY; g_lastPitch = mk::pitch_of(s.dir) - offP; g_haveLast = true;

    // Where would an arrow loosed right now cross the target?  (the bow's pose as it is this frame)
    V3 lead = g_lead ? g_lockVel * s.tof : V3{}, tFut = tp + lead, at;
    float gap = mk::impact_near(origin, launchDir, speed, fl, tFut, at) ? (at - tFut).len() : -1.f;
    g_aim.target = g_lock; g_aim.point = tp; g_aim.dist = dist; g_aim.errYaw = ey; g_aim.errPitch = ep; g_aim.speed = speed; g_aim.bone = bone; g_aim.t = now;
    g_aim.gap = gap; g_aim.at = at; g_aim.origin = origin; g_aim.dir = launchDir; g_aim.live = live;
    if (g_marker && gap >= 0 && !InterlockedCompareExchange(&g_markerReady, 0, 0)) {
        // Impact: a cross where this arrow would land - green when within 8 cm of the head BONE, yellow when off,
        // orange when the head is only estimated from the body's box (never green then).  Head: a small white cross.
        float r = 0.06f + 0.002f * g_aim.dist, h = 0.04f + 0.001f * g_aim.dist; bool on = gap < 0.08f;
        float cr = 1, cg = 1, cb = 0;
        if (!bone && !animal) { cr = 1; cg = 0.5f; cb = 0; } else if (on) { cr = 0; cg = 1; cb = 0; }
        sprintf_s(g_markerLua, "local a,b=%.3f,%.3f;local x,y,z=%.3f,%.3f,%.3f;System.DrawLine({x=x-a,y=y,z=z},{x=x+a,y=y,z=z},%.1f,%.1f,%.1f,1);System.DrawLine({x=x,y=y,z=z-a},{x=x,y=y,z=z+a},%.1f,%.1f,%.1f,1);"
                  "x,y,z=%.3f,%.3f,%.3f;System.DrawLine({x=x-b,y=y,z=z},{x=x+b,y=y,z=z},1,1,1,1);System.DrawLine({x=x,y=y,z=z-b},{x=x,y=y,z=z+b},1,1,1,1)",
                  r, h, at.x, at.y, at.z, cr, cg, cb, cr, cg, cb, tFut.x, tFut.y, tFut.z);
        InterlockedExchange(&g_markerReady, 1);
    }

    // Safety: the camera must come closer to the aim point while the mod turns it.  If after half a second
    // of large errors it has not, pause and start over (round 4 switched the mod off here for good).
    if (err > mk::rad(2.f) && now - g_acqT > 0.3) {
        if (!g_errFrames) g_errStart = err;
        if (++g_errFrames > 30 && err > 0.9f * g_errStart) {
            char b[128]; sprintf_s(b, "the camera does not follow the mod's turns (error %.1f -> %.1f deg)", mk::deg(g_errStart), mk::deg(err)); pause_aim(b); return;
        }
    } else { g_errFrames = 0; if (err < mk::rad(0.5f)) g_trips.success(); }

    // Add the turn to the game's additive look input.  If our last write has not been used yet (no look
    // tick since), replace it instead of adding twice.
    float& ax = F<float>(look, L_ACCUM); float& az = F<float>(look, L_ACCUM + 8);
    if (ax == g_wrX && az == g_wrZ && (g_wrX != 0 || g_wrZ != 0)) { ax = dp; az = dy; }
    else { ax += dp; az += dy; }
    g_wrX = ax; g_wrZ = az;

    if (kc::g_debug && now - g_logT > (waitingForLaunch ? 0.1 : 0.5)) {
        g_logT = now;
        logf("aim%s: target %.1f m (%s), arrow %.1f m/s (%s), bow vs camera yaw %+.2f pitch %+.2f deg (steering on %+.2f %+.2f), flight %.2f s, lead (%.1f %.1f) m/s, error yaw %+.2f pitch %+.2f deg, turn %+.2f %+.2f deg%s, predicted impact %.0f cm from the aim point",
             waitingForLaunch ? " (fired, waiting for the launch)" : "", (tp - origin).len(), bone ? "head bone" : animal ? "chest" : "box", speed, live ? (gameSpeed > 0 ? "game formula" : "bow pose, measured speed") : (g_cal.measured() ? "camera + measured" : "GUESS"),
             mk::deg(live ? rawY : offY), mk::deg(live ? rawP : offP), mk::deg(offY), mk::deg(offP), s.tof, g_lockVel.x, g_lockVel.y, mk::deg(ey), mk::deg(ep), mk::deg(dy), mk::deg(dp), moveCam ? "" : " (on the head: following the sway only)", gap * 100.f);
    }
}

static void crossbow_diagnostic(double now, uintptr_t look, const V3& camPos, const V3& camFwd, int family) {
    if (!g_crossbowDiag || !kc::g_debug || family != FAM_CROSSBOW || g_sub != SUB_AIMING) {
        g_diagPrevValid = false; return;
    }
    if (now < g_diagNext) return;
    g_diagNext = now + 0.25;
    mk::LivePose lp; float predictedSpeed = 0;
    bool got = read_pose(lp, &predictedSpeed) && (lp.pos - camPos).len() < 3.f;
    float cameraStep = 0, poseStep = 0;
    float camDYaw = 0, camDPitch = 0, poseDYaw = 0, poseDPitch = 0;
    bool continuous = g_diagPrevValid && g_diagPrevOn == g_on && got;
    if (continuous) {
        cameraStep = mk::deg(mk::angle_between(camFwd, g_diagPrevView));
        poseStep = mk::deg(mk::angle_between(lp.dir, g_diagPrevPose));
        camDYaw = mk::deg(mk::wrap_pi(mk::yaw_of(camFwd) - mk::yaw_of(g_diagPrevView)));
        camDPitch = mk::deg(mk::pitch_of(camFwd) - mk::pitch_of(g_diagPrevView));
        poseDYaw = mk::deg(mk::wrap_pi(mk::yaw_of(lp.dir) - mk::yaw_of(g_diagPrevPose)));
        poseDPitch = mk::deg(mk::pitch_of(lp.dir) - mk::pitch_of(g_diagPrevPose));
    }
    float reqX = F<float>(look, L_REQUEST), reqZ = F<float>(look, L_REQUEST + 8);
    float rawNativeSway = g_on ? mk::deg(g_diagSwayPeakRaw) : -1.f;
    g_diagSwayPeakRaw = 0.f;
    if (got) {
        float offY = mk::deg(mk::wrap_pi(mk::yaw_of(lp.dir) - mk::yaw_of(camFwd)));
        float offP = mk::deg(mk::pitch_of(lp.dir) - mk::pitch_of(camFwd));
        // Signed deltas distinguish camera/weapon movement from relative aim drift.
        // Relative drift can also come from pose lag/IK; it is NOT proof of an independent animation.
        KC_DLOG("crossbow diagnostic: F4=%s target=%s sample=%s dt=%.3f input=(%.4f %.4f) camera_step=%.3f deg weapon_step=%.3f deg cam_delta=(yaw %+.3f pitch %+.3f) weapon_delta=(yaw %+.3f pitch %+.3f) relative_delta=(yaw %+.3f pitch %+.3f) native_sway_peak_deg_s=%.3f weapon_vs_camera=(yaw %+.3f pitch %+.3f) speed=%.1f",
                 g_on ? "ON" : "OFF", g_lock ? "YES" : "NONE", continuous ? "OK" : "FIRST",
                 continuous ? (float)(now - g_diagPrevT) : 0.f, reqX, reqZ, cameraStep, poseStep,
                 camDYaw, camDPitch, poseDYaw, poseDPitch, poseDYaw - camDYaw, poseDPitch - camDPitch,
                 rawNativeSway, offY, offP, predictedSpeed);
        g_diagPrevPose = lp.dir;
    } else {
        KC_DLOG("crossbow diagnostic: F4=%s target=%s live firing pose UNAVAILABLE",
                 g_on ? "ON" : "OFF", g_lock ? "YES" : "NONE");
    }
    g_diagPrevView = camFwd; g_diagPrevT = now;
    g_diagPrevOn = g_on; g_diagPrevValid = got;
}

static void tick() {
    double now = now_s(); float dt = g_lastT > 0 ? (float)std::min(0.1, now - g_lastT) : 0.f; g_lastT = now;
    if (g_key.pressed()) {
        g_on = !g_on; reset_aim(); sway_hook(g_on);
        logf("assist %s", g_on ? "ON" : "OFF");
        if (g_notices) g_notice = g_on ? NOTICE_ON : NOTICE_OFF;
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
        // Announce startup state only after a real game player exists (not in the main menu).
        if (g_notices) g_notice = g_on ? NOTICE_ON : NOTICE_OFF;
    }
    g_player = pl;
    bool aiming = F<uint8_t>(ctrl, MW_AIMING) != 0;
    if (aiming && !g_wasAiming) {
        g_aimT0 = now; reset_aim();
        if (g_on && g_notices && weapon_family(ctrl) == FAM_BOW && !g_cal.measured() && !g_toldUncal) { g_toldUncal = true; g_notice = NOTICE_FIRST;
            logf("aiming with the first-shot guess (%.0f m/s) - the first arrow will measure the bow", g_cal.speed); }
    }
    if (!aiming && g_wasAiming) g_releaseT = now;
    bool wasAiming = g_wasAiming; g_wasAiming = aiming;
    V3 camPos, camFwd; if (!camera(camPos, camFwd)) return;
    track_arrows(now);
    if (aiming) {
        update_weapon(now);
        int fam = weapon_family(ctrl);
        if (fam != g_familyLogged) {
            g_familyLogged = fam;
            if (fam != FAM_UNKNOWN) logf("weapon: %s", FAM_NAME[fam]);
            if (g_swaySlot && fam != FAM_UNKNOWN) logf("   native sway: %.0f%% of the game's (SwayStrength%s)", g_swayStrength[fam] * 100.f,
                                                       fam == FAM_BOW ? "Bow" : fam == FAM_CROSSBOW ? "Crossbow" : "Gun");
        }
        g_family = fam; g_swayScale = g_swayStrength[fam];
        crossbow_diagnostic(now, look, camPos, camFwd, fam);
    } else {
        if (wasAiming && g_swayPeakRaw > 0 && g_swayHooked)
            KC_DLOG("native sway this aim: peak %.1f deg/s from the game, %.1f deg/s after SwayStrength", mk::deg(g_swayPeakRaw), mk::deg(g_swayPeakRaw * g_swayScale));
        g_swayPeakRaw = 0; g_sub = -2; g_subLogged = -2;
    }
    if (!g_on || !aiming) { if (g_lock) reset_aim(); return; }
    steer(now, dt, look, camPos, camFwd);
}

static volatile LONG g_ssTickOnce = 0;
static void* SsUpdateHook(void* self, void* a, void* b, void* c) {
    if (InterlockedCompareExchange(&g_ssTickOnce, 1, 0) == 0)
        KC_DLOG("first CScriptSystem::Update tick seen; gameplay hook active");
    void* r = G.ssUpdate(self, a, b, c);
    if (!g_dead) {
        __try { tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // A fault in the mod's own reads: start over (the next frame re-reads everything).  Only a fault that
            // keeps coming back switches the mod off.
            reset_aim();
            if (++g_tickFaults >= 20) { logf("exception in tick (%d times) - assist disabled for this session", g_tickFaults); g_dead = true; sway_hook(false); }
            else logf("exception in tick (%d) - starting the aim over", g_tickFaults);
        }
    }
    if (InterlockedCompareExchange(&g_markerReady, 0, 0) && G.ssExec) {
        __try { G.ssExec(self, g_markerLua, strlen(g_markerLua), "AutoMarksman marker", nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { g_marker = false; }
        InterlockedExchange(&g_markerReady, 0);
    }
    const char* line = (const char*)InterlockedExchangePointer((void* volatile*)&g_notice, nullptr);
    if (line && G.ssExec) { __try { G.ssExec(self, line, strlen(line), "AutoMarksman", nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { G.ssExec = nullptr; } }
    return r;
}

// Audit ONLY: if another plugin replaces the shared slot later, we do not blindly
// overwrite it (that could introduce a cyclic chain). Record the ownership and tick
// count for diagnosis, even when nobody presses F4.
static DWORD WINAPI ScriptUpdateAudit(LPVOID) {
    Sleep(30000);
    for (int i = 0; i < 2; ++i) {
        uintptr_t p = G.vtSS ? ((uintptr_t*)G.vtSS)[1] : 0;
        logf("script-update audit (+%ds): first-tick=%s, slot=%s (other mods may chain)",
             i ? 90 : 30,
             InterlockedCompareExchange(&g_ssTickOnce, 0, 0) ? "seen" : "NOT SEEN",
             p == (uintptr_t)&SsUpdateHook ? "ours" : "another plugin");
        if (i == 0) Sleep(60000);
    }
    return 0;
}

// ---------------- resolve ----------------
struct Found { uintptr_t ssUpdate, ssExec, fwGlobal, clientActor, sysGlobal, viewCamera, getEntity, bounds, getActor, mkIter, iterNext, iterRel, deadByHp, launch,
                launchArrow, getChar, slotTM, ciPose, ciSkel, jointId, absJoint, jointCount, jointName, loadout, wsys, launchPose, bowSpeed, physGlobal, physRay; };
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
    G.vtHuman = R.vtable("C_Human", ".?AVC_Human@entitymodule@wh@@", 0);
    G.vtNpc = R.vtable("C_NPCActor", ".?AVC_NPCActor@entitymodule@wh@@", 0);
    G.vtHorse = R.vtable("C_Horse", ".?AVC_Horse@entitymodule@wh@@", 0);
    G.vtDog = R.vtable("C_Dog", ".?AVC_Dog@entitymodule@wh@@", 0);
    G.vtAnimal = R.vtable("C_Animal", ".?AVC_Animal@entitymodule@wh@@", 0);
    G.vtMwCtrl = R.vtable("C_MissileWeaponPlayerController", ".?AVC_MissileWeaponPlayerController@entitymodule@wh@@", 0);
    {   // Optional (weapon state): a miss only turns the check off.
        int f0 = R.fails;
        G.vtShootExp = R.vtable("C_ActorShootingExpansion", ".?AVC_ActorShootingExpansion@entitymodule@wh@@", 0);
        G.vtShootMain = R.vtable("C_ActorActionShootingMain", ".?AVC_ActorActionShootingMain@entitymodule@wh@@", 0);
        static const char* const subCls[SUB_N] = {".?AVC_ActorActionShootingCharging@entitymodule@wh@@", ".?AVC_ActorActionShootingInstantCharging@entitymodule@wh@@",
            ".?AVC_ActorActionShootingAiming@entitymodule@wh@@", ".?AVC_ActorActionShootingFiring@entitymodule@wh@@", ".?AVC_ActorActionShootingReloading@entitymodule@wh@@",
            ".?AVC_ActorActionShootingUnloading@entitymodule@wh@@", ".?AVC_ActorActionShootingUncharging@entitymodule@wh@@",
            ".?AVC_ActorActionShootingTransitioningToIdle@entitymodule@wh@@", ".?AVC_ActorActionShootingBase@entitymodule@wh@@"};
        for (int i = 0; i < SUB_N; i++) G.vtSub[i] = R.vtable(SUB_NAME[i], subCls[i], 0);
        if (R.fails != f0) { logf("  (weapon state classes missing: optional, steering uses the bow pose check alone)"); G.vtShootExp = 0; }
        R.fails = f0;
        // Native sway (optional too): the Aiming vtable's slot 55.
        if (G.vtSub[SUB_AIMING]) {
            uintptr_t* slot = &((uintptr_t*)G.vtSub[SUB_AIMING])[55];
            if (R.verify("Aiming sway rate (vtbl[55])", *slot, SIG_AIMSWAY)) { g_swaySlot = slot; g_swayOrig = (AimSway_t)*slot; }
            else logf("  (native sway reduction unavailable: SwayStrength is ignored)");
        }
        R.fails = f0;
    }
    G.vtProjectile = R.vtable("CProjectile", ".?AVCProjectile@entitymodule@wh@@", 0);
    f.launch = R.slot("CProjectile::Launch", G.vtProjectile, 35, SIG_LAUNCH);
    // CArrow's slot 35 must be a jmp to that same Launch (or already hooked by another mod: then chained).
    G.vtArrow = R.vtable("CArrow", ".?AVCArrow@entitymodule@wh@@", 0);
    if (G.vtArrow && f.launch) {
        uintptr_t a = ((uintptr_t*)G.vtArrow)[35];
        if (a == f.launch || !R.im.in_image(a) || (R.im.in_text(a) && *(uint8_t*)a == 0xE9 && kc::rel32(a + 1) == f.launch)) f.launchArrow = a;
        else R.fail("CArrow::Launch", "slot 35 is not the arrow launch (game updated?)");
    }
    f.getChar = R.slot("CEntity::GetCharacter", G.vtEntity, 101, SIG_GETCHAR);
    f.slotTM = R.slot("CEntity::GetSlotWorldTM", G.vtEntity, 91, SIG_SLOTTM);
    G.vtChar = R.vtable("CCharInstance", ".?AVCCharInstance@@", 0);
    f.ciPose = R.slot("CCharInstance::GetISkeletonPose", G.vtChar, 7, SIG_CI_POSE);
    f.ciSkel = R.slot("CCharInstance::GetIDefaultSkeleton", G.vtChar, 11, SIG_CI_SKEL);
    G.vtDefSkel = R.vtable("CDefaultSkeleton", ".?AVCDefaultSkeleton@@", 0);
    f.jointId = R.slot("CDefaultSkeleton::GetJointIDByName", G.vtDefSkel, 7, SIG_JOINTID);
    f.jointCount = R.slot("CDefaultSkeleton::GetJointCount", G.vtDefSkel, 1, SIG_JOINTCOUNT);
    f.jointName = R.slot("CDefaultSkeleton::GetJointNameByID", G.vtDefSkel, 6, SIG_JOINTNAME);
    G.vtPose = R.vtable("CSkeletonPose", ".?AVCSkeletonPose@@", 0);
    f.absJoint = R.slot("CSkeletonPose::GetAbsJointByID", G.vtPose, 27, SIG_ABSJOINT);
    // The bow's launch pose and speed: the calls of the game's own trajectory preview.
    uintptr_t pv = R.find("bow trajectory preview", SIG_PREVIEW);
    if (pv) {
        f.loadout = R.branch("actor loadout", pv + 7); f.wsys = R.branch("weapon system", pv + 0x20);
        f.launchPose = R.branch("bow launch pose", pv + 0x3D); f.bowSpeed = R.branch("arrow speed formula", pv + 0x5F);
        if (!R.verify("actor loadout", f.loadout, SIG_LOADOUT)) f.loadout = 0;
        if (!R.verify("bow launch pose", f.launchPose, SIG_LAUNCHPOSE)) f.launchPose = 0;
        if (!R.verify("arrow speed formula", f.bowSpeed, SIG_BOWSPEED)) f.bowSpeed = 0;
    }
    // The build's own shooting function calls the same 12-argument physics
    // wrapper, making the raycast ABI and world-global signature verifiable.
    uintptr_t rayCaller = R.find("ballistic physics ray caller", SIG_PHYS_REF);
    if (rayCaller) {
        f.physGlobal = R.riprel("gEnv->pPhysicalWorld", rayCaller + 3);
        f.physRay = R.branch("IPhysicalWorld raycast wrapper", rayCaller + 0x63);
        if (!R.verify("IPhysicalWorld raycast wrapper signature", f.physRay, SIG_PHYS_RAY)) f.physRay = 0;
    }
    R.find("actor combat state (layout)", SIG_COMBATMODE);
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
    g_notices = kc::ini_int("Options", "Notices", 1) != 0;
    g_crossbowDiag = kc::ini_int("Options", "CrossbowPoseDiagnostics", 0) != 0;
    g_on = kc::ini_int("Options", "StartEnabled", 0) != 0;
    g_policy.cone = mk::rad((float)std::max(2, std::min(30, kc::ini_int("Options", "ConeDegrees", 10))));
    g_maxDist = (float)std::max(10, std::min(200, kc::ini_int("Options", "MaxDistance", 80)));
    g_steer.maxRate = mk::rad((float)std::max(20, std::min(400, kc::ini_int("Options", "TurnSpeed", 115))));
    g_steer.gain = (float)std::max(2, std::min(30, kc::ini_int("Options", "Responsiveness", 9)));
    g_headDrop = std::max(0, std::min(40, kc::ini_int("Options", "HeadDropCm", 13))) / 100.f;
    g_overrideS = std::max(0, std::min(3000, kc::ini_int("Options", "HandsOffMs", 400))) / 1000.f;
    g_cal.speed = (float)std::max(15, std::min(150, kc::ini_int("Options", "ArrowSpeed", 55)));
    g_policy.people = kc::ini_int("Options", "TargetPeople", 1) != 0;
    g_policy.horses = kc::ini_int("Options", "TargetHorses", 0) != 0;
    g_policy.dogs = kc::ini_int("Options", "TargetDogs", 0) != 0;
    g_marker = kc::ini_int("Options", "ShowImpactMarker", 1) != 0;
    g_hostileOnly = kc::ini_int("Options", "HostileOnly", 0) != 0;
    g_headBone = kc::ini_int("Options", "HeadBone", 1) != 0;
    g_headAbove = std::max(-10, std::min(20, kc::ini_int("Options", "HeadAboveBoneCm", 8))) / 100.f;
    g_remember = kc::ini_int("Options", "RememberBow", 1) != 0;
    g_policy.animals = kc::ini_int("Options", "TargetAnimals", 1) != 0;
    g_lead = kc::ini_int("Options", "LeadMovingTargets", 1) != 0;
    g_losEnabled = kc::ini_int("Options", "RequireClearShot", 1) != 0;
    g_live = kc::ini_int("Options", "BowPose", 1) != 0;
    {   // Native sway while the assist is on: % of the game's own (100 = vanilla, 0 = none).
        int all = std::max(0, std::min(100, kc::ini_int("Options", "SwayStrength", 25)));
        static const char* const key[4] = {nullptr, "SwayStrengthBow", "SwayStrengthCrossbow", "SwayStrengthGun"};
        g_swayStrength[0] = all / 100.f;
        for (int i = 1; i < 4; i++) { int v = kc::ini_int("Options", key[i], -1); g_swayStrength[i] = (v < 0 ? all : std::min(100, v)) / 100.f; }
    }
    g_ctl.st = g_steer;
    g_ctl.swayTau = std::max(20, std::min(1000, kc::ini_int("Options", "SwaySmoothingMs", 100))) / 1000.f;
    g_ctl.glide.tol = std::max(0, std::min(20, kc::ini_int("Options", "HoldWithinCm", 2))) / 100.f;
    g_ctl.gate.settle = std::max(0, std::min(2000, kc::ini_int("Options", "SettleMs", 200))) / 1000.f;
    HMODULE h = kc::wait_game(); if (!h) { logf("abort: WHGame.dll not loaded"); return 0; }
    kc::Resolver R; R.im.load(h);
    logf("start v0.6.0 LOCAL DIAGNOSTIC 6 (physics line of sight) (game ts 0x%08X)", R.im.timestamp);
    // Other Theatrical mods and Horse Route Follow also install on CScriptSystem::Update.
    // Their init threads are concurrent, so installing immediately after KCSE load may
    // leave our hook underneath a later non-chaining hook. Let other plugins settle first.
    // This is still well before any save loads on the observed game (previously ~60s).
    Sleep(3000);
    Found f{};
    if (!resolve(R, f)) { logf("disabled: this game version is not supported yet - nothing was changed"); return 0; }
    G.fwGlobal = f.fwGlobal; G.sysGlobal = f.sysGlobal;
    G.clientActor = (GetClientActor_t)f.clientActor; G.viewCamera = (GetViewCamera_t)f.viewCamera; G.getEntity = (GetEntity_t)f.getEntity;
    G.bounds = (GetBounds_t)f.bounds; G.getActor = (GetActor_t)f.getActor; G.mkIter = (MkIter_t)f.mkIter; G.iterNext = (IterNext_t)f.iterNext;
    G.iterRel = (IterRel_t)f.iterRel; G.deadByHp = f.deadByHp; G.ssExec = (SsExec_t)f.ssExec;
    G.launch = (Launch_t)f.launch; G.ssUpdate = (SsUpdate_t)f.ssUpdate; G.launchArrow = (Launch_t)f.launchArrow;
    G.getChar = (GetChar_t)f.getChar; G.slotTM = (SlotTM_t)f.slotTM; G.ciPose = (CiGet_t)f.ciPose; G.ciSkel = (CiGet_t)f.ciSkel;
    G.jointId = (JointId_t)f.jointId; G.absJoint = (AbsJoint_t)f.absJoint;
    g_jointCount = (JointCount_t)f.jointCount; g_jointName = (JointName_t)f.jointName;
    G.loadout = (Loadout_t)f.loadout; G.wsys = (WSys_t)f.wsys; G.launchPose = (LaunchPose_t)f.launchPose; G.bowSpeed = (BowSpeedFn_t)f.bowSpeed;
    G.imgLo = R.im.base; G.imgHi = R.im.end; G.textLo = R.im.text0; G.textHi = R.im.text1;
    G.physicsGlobal = f.physGlobal; G.physRay = (PhysRay_t)f.physRay;
    load_calib();
    kc::write_ptr(&((void**)G.vtProjectile)[35], (void*)&LaunchHook);
    kc::write_ptr(&((void**)G.vtArrow)[35], (void*)&LaunchHookArrow);
    // Use the live slot at install time; another mod can hook between resolve and install.
    // Chain to the latest predecessor, but never recursively chain to ourselves.
    uintptr_t liveSS = ((uintptr_t*)G.vtSS)[1];
    if (liveSS != (uintptr_t)&SsUpdateHook && liveSS != f.ssUpdate) {
        if (R.im.in_image(liveSS)) {
            if (!R.verify("latest CScriptSystem::Update", liveSS, SIG_SS_UPDATE)) {
                logf("disabled: latest script-update slot was changed to unrecognized game code");
                return 0;
            }
        } else if (!kc::readable(liveSS, 1)) {
            logf("disabled: latest script-update hook is not readable");
            return 0;
        }
        G.ssUpdate = (SsUpdate_t)liveSS;
        logf("chaining to another plugin's newer script-update hook");
    }
    kc::write_ptr(&((void**)G.vtSS)[1], (void*)&SsUpdateHook);
    if (((void**)G.vtSS)[1] != (void*)&SsUpdateHook) {
        logf("disabled: script-update hook installation did not stick"); return 0;
    }
    logf("script-update hook installed after 3 s startup settlement; first gameplay tick pending");
    if (g_on) sway_hook(true);  // StartEnabled=1 must apply native-sway scaling too, without an F4 press
    HANDLE audit = CreateThread(nullptr, 0, ScriptUpdateAudit, nullptr, 0, nullptr);
    if (audit) CloseHandle(audit);
    char kn[16];
    logf("ready: %s switches the assist %s (now %s); cone %.0f deg, range %.0f m, turn speed %.0f deg/s, first-shot arrow speed %.0f m/s; %s, %s",
         kc::key_name(g_key.vk, kn, sizeof kn), "on/off", g_on ? "ON" : "OFF", mk::deg(g_policy.cone), g_maxDist, mk::deg(g_steer.maxRate), g_cal.speed,
         g_hostileOnly ? "people only when in combat" : "anyone you aim at", g_headBone ? "head bone" : "head from box");
    logf("local diagnostics: crossbow firing pose %s, in-game notices %s", g_crossbowDiag ? "ON" : "OFF", g_notices ? "ON" : "OFF");
    logf("visibility: %s, physics ray = %s, solid props/terrain/rigid blockers; max 8 candidates per choice",
         g_losEnabled ? "required" : "not required", G.physRay && G.physicsGlobal ? "verified" : "unavailable");
    logf("targets: people %s, animals %s, horses %s, dogs %s; impact marker %s", g_policy.people ? "yes" : "no", g_policy.animals ? "yes" : "no",
         g_policy.horses ? "yes" : "no", g_policy.dogs ? "yes" : "no", g_marker ? "on" : "off");
    logf("native aim sway while the assist is on: bow %.0f%%, crossbow %.0f%%, hand cannon %.0f%% of the game's%s", g_swayStrength[1] * 100.f, g_swayStrength[2] * 100.f,
         g_swayStrength[3] * 100.f, g_swaySlot ? "" : " (NOT available on this game version)");
    logf("camera: follows the bow's slow sway (tracking filter %.0f ms, tremor filtered out), does not chase the last %.0f cm, steers only after the bow has been steady for %.0f ms",
         g_ctl.swayTau * 1000.f, g_ctl.glide.tol * 100.f, g_ctl.gate.settle * 1000.f);
    logf("arrow path: %s", g_live ? "from the bow's live arrow joint and the game's speed formula (BowPose=1)" : "from the camera plus measured offsets (BowPose=0)");
    return 0;
}
KC_PLUGIN("Theatrical AutoMarksman", KC_AUTHOR, 10)
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); g_self = h; KC_START(Init); }
    else if (r == DLL_PROCESS_DETACH) sway_hook(false);           // the game's sway as it was
    return TRUE;
}
