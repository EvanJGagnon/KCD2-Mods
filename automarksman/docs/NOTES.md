# Theatrical AutoMarksman: research notes (build 15693)

Static research for the mod, verified against Steam build 15693 `WHGame.dll` (image base 0x180000000;
RVAs below). `tools/check_sigs.py` re-checks every signature and slot against a `WHGame.dll`.
Starting point was libKCD2's public headers (JerryYOJ, GPL-3.0): all its addresses used here match this
build exactly (libKCD2 calls it 1.5.6). Nothing here comes from Arrow Arc's code.

## Camera: the player's look-angle state

`C_Actor+0x238` -> `C_ActorPhysicsState` (0x98 bytes, no vtable, owner back-pointer at +0).

| off | field | |
|---|---|---|
| +0x08 | `Ang3 m_lookAngles` | x = pitch, z = yaw; the persistent look direction |
| +0x24 | `Quat m_viewRotation` | what the camera uses (`C_Actor` vtbl[57] GetViewRotation 0x94DB70 reads it) |
| +0x7C | `Ang3 m_lookDeltaRequest` | this frame's mouse-look request (movement controller) |
| +0x88 | `Ang3 m_lookAngleAccum` | additive input from other systems (horse camera glue); cleared every tick |

Per-frame tick 0x4415E0 (verified by disassembly):
1. pending -> carried (+0x58 -> +0x64), request copied from the movement controller (rdx+0x18 -> +0x7C);
2. integrator 0x441928: `m_lookAngles += (carried + request) + accum` when the hold-counter check
   0x441AFC passes, then the view-limit clamp 0x53B508 (so weapon pitch limits still apply);
3. quat rebuild 0x441730, then `+0x4C` (transient) and `+0x88` (accum) are zeroed (0x441649 / 0x441658).

**Steering = adding a small yaw/pitch to `+0x88` each frame**, the way the horse camera does. The
game's clamps apply on top. Mouse input is detected through `+0x7C` and makes the mod let go.

Sign convention (CryEngine): yaw = atan2(-dir.x, dir.y), pitch = asin(dir.z); positive yaw turns left,
positive pitch looks up. The mod works closed-loop on the real view camera and turns itself off if
the camera does not follow (wrong sign or another system fighting it).

## View camera

`gEnv` is a static struct at RVA 0x492D800; `gEnv.pSystem` (+0xC0, global 0x492D8C0) is read at
0x393B66 by a function that calls `CSystem` vtbl[135] GetViewCamera (0x5D77F0 = `lea rax,[rcx+288h]`).
`CCamera::m_Matrix` is a row-major Matrix34: forward = column 1 (m01,m11,m21), position = column 3.
`gEnv.pEntitySystem` is +0xA0 (0x492D8A0: 1597 references, the ones checked call vtbl[14] GetEntity).

## Player, bow, actors

- `CCryAction*` global 0x549D328 (found via the same signature Horse Route Follow uses).
  vtbl[64] GetClientActor 0x6458C4, vtbl[25] GetIActorSystem 0x641150 (`mov rax,[rcx+518h]`).
- `C_Player+0xD70` = embedded `C_MissileWeaponPlayerController` (vtable 0x183A80E30, owner at +0x18).
  `+0x20 m_isAiming`: set to 1 at aim start 0x2AAA4C0 (`mov byte [rcx+20h],1`), cleared on release/abort.
  The mod checks the vtable and owner pointer at runtime before trusting the layout.
- `CActorSystem` vtbl[3] GetActor(id) 0x647E40; vtbl[7] CreateActorIterator 0x1794CB0 (writes the iterator
  to the hidden return slot and AddRefs it: refcount 1); `CActorIterator@CActorSystem` vtbl[2] Next
  0x4CB02C, vtbl[4] Release 0x16F5194.
- `C_Actor` vtbl[38] IsDeadByHealth 0x64583C (soul at +0x668, else health +0x164 <= 0).
- Actor entity at `C_Actor+0x38` (IGameObjectExtension::m_pEntity). `CEntity`: id +0x0C, name `char*`
  +0xE0, world matrix +0x58 (translation +0x64/+0x74/+0x84), vtbl[34] GetWorldBounds 0x63E034.

Aim point: the entity's world AABB. A box longer than 1.25 x its height is treated as an animal (aim at
the chest/neck, 30% of its length ahead of the centre, 70% up); anything else as a person (aim
`HeadDropCm` below the top). The test session should confirm the boxes in the log ("target: ... box").
Head bones would be more exact; no verified bone accessor is known yet (the look IK at C_Actor+0x288
names "Bip01 Head" but does not cache a position).

## Arrows

`CProjectile` (vtable 0x18471D020). vtbl[35] `Launch(pos, dir, velocity, speedScale)` 0x2AA85A8:
stores pos/dir/velocity at +0xB0/+0xBC/+0xC8 (0x2AA8906) and hands `velocity * speedScale` to the
physics particle (0x2AAD50C multiplies each component by xmm3 = speedScale). Owner entity id +0x108,
entity +0x38. The mod hooks this slot only to read the launch, and follows the arrow's entity
(by id, through `CEntitySystem::GetEntity`) for up to 2 s to fit gravity and linear drag
(`mk::fit_flight`). Bow power, draw time and arrow type therefore never have to be modelled.

**Round 1 finding: arrows are `CArrow`, whose own vtable (0x183ECE6F0) has slot 35 = 0x2ACC880, a 5-byte
`jmp` to `CProjectile::Launch`.** Hooking only the CProjectile vtable slot therefore never saw an arrow, the
mod never measured a bow and kept the 55 m/s first-shot guess, which aims high with a strong bow. Only one
thunk to Launch exists and only the CArrow vtable references it; `Launch` has no direct callers. Both slots
are hooked now; `check_sigs.py` checks that the CArrow slot still jumps to Launch.

Launch speed (for reference; the mod measures it rather than recomputing it). `C_RPGCombat` vtbl[23]
(0x72697C) combines:
- power 0x726C28 from Henry's strength vs the bow's required strength (RPG params `RangedWpnMinStrCoef`
  +0x7D4, `RangedWpnMinPowerCoef` +0x7D0, `RangedWpnPowerConstA` +0x7D8: full power at or above the
  requirement, a quadratic fall-off below it, a minimum below MinStrCoef);
- speed 0x726A94: bow: `sqrt(2 * power * drawWeight * RangedWpnPwrToEnergy / mass)` with mass from the
  arrow's weight (+0x108 of the ammo record) and the bow's limb resistance (`RangedWpnLimbResistanceEffect`),
  then a stat modifier (id 0x5D) and a floor; crossbow (flag +0x140): `power * PwrToSpeed * ...`.
So the speed depends on bow, arrow and strength, not on a fixed number. Draw time is not an input of that
function; the mod still keeps speed per draw time (`mk::BowSpeed`) in case the launch scale depends on it.
RPG parameter offsets were mapped from their registration (0xCEAB00..): BowChargeDurationMin/Max +0x7C4/+0x7C8.

Where the projectile's source weapon is stored is not verified, so the mod cannot tell which bow is in hand
before the first arrow; a clear speed change at a similar draw time (8%) resets the measurement at once.

## Head bone

As the script bind `Entity.GetBonePos` (0x37D3EF8): `CEntity` vtbl[101] GetCharacter(slot) 0x6411AC ->
`CCharInstance` vtbl[11] GetIDefaultSkeleton 0x8A49D0 -> `CDefaultSkeleton` vtbl[7] GetJointIDByName 0x78BC4C
("Bip01 Head", int16); `CCharInstance` vtbl[7] GetISkeletonPose 0x1AA6BC0 (this+0x698) -> `CSkeletonPose`
vtbl[27] GetAbsJointByID 0x641250 (QuatT, t at +0x10); world = `CEntity` vtbl[91] GetSlotWorldTM(0)
0xB7258C (Matrix34) * t. Every object is RTTI-checked before use. The bone is at the base of the skull,
so the mod aims `HeadAboveBoneCm` (8 cm) above it; the result must lie inside the body's box.

## Hostility

Soul script bind `IsInCombatMode` (0x117F1E8): actor = soul's actor; in combat = `(C_Actor+0x278)->vtbl[1]()`.
`HostileOnly=1` targets people only while that is true. `IsPublicEnemy` reads a different object and was
not used.

## Ballistics (src/mk_ballistics.h)

Point mass, gravity g, linear drag k: horizontal `vh*E(t)`, height `vz*E(t) - g*(t-E(t))/k`,
`E(t) = (1-e^-kt)/k`. Low-arc pitch by scan + bisection; up to 4 lead iterations with the target's
smoothed velocity. Steering: feed-forward of the aim point's own angular motion plus proportional
correction, capped at TurnSpeed on the combined turn. `tools/marksim.cpp` tests all of it against an
independent step-integrated model (solver within 5 cm, flight fits, learning an unknown bow with
offsets, moving targets, rate cap, bow sway still causing misses).

## Round 2 findings (0.3.0)

- HostileOnly=1 hid every villager ("9 people in view not in combat"), so the only targets left were Henry's
  dog and horse. Now: HostileOnly=0 by default; actors are classed by their vtable (`C_Human`/`C_NPCActor`
  person, `C_Horse`, `C_Dog`, `C_Animal`); horses and dogs are skipped by default; people beat animals.
- Soft lock (`src/mk_target.h`, tested in marksim): while the mouse moves the mod does not steer and drops a
  target that leaves the cone; afterwards the choice is fresh (no preference for the old target, so no swing
  back); the turn rate eases in over 0.4 s after taking a target or after the mouse.
- The one measured arrow (76.2 m/s) left 2.47 deg above the camera ray. Steering now aims the predicted
  impact (camera + measured offsets + speed + drop) at the head, logs the predicted gap, and can draw it
  (`System.DrawLine` through ExecuteBuffer). Arrow Arc was consulted for behaviour only; no code used.
- "Too short to measure" with 96 samples: the arrow entity moves with the physics step, so at 135 fps the same
  position repeats and the old fit cut the track there. The track now keeps distinct positions only and the
  fit uses height against distance (plus a time-offset-tolerant distance/time term for drag).

## Round 3 findings (0.4.0)

- **The arrow does not start at the camera or fly along the camera ray.** Static trace of the player shot:
  Firing action anim event -> 0x2896788 -> Fire 0x28999C8 -> 0x289F7A0 -> `Launch` via `[rax+0x118]` at 0x289F9DA
  (speedScale 1.0). Pose: GetLaunchPose 0x289B28C(sys, out{pos, up, dir}, actor, bow): the bow's "arrow" attachment
  joint (fallback static helper "arw_01"); dir = joint -Z, up = joint +Y (player with `wh_w_AimByProjectile` 1, the
  default). The launch point is the joint minus the arrow's "slt_0" helper offset in the frame (dir x up, dir, up).
  No random spread for the player (0x2895680 returns dir unchanged); inaccuracy is the bow joint's own sway.
- Speed: 0x289B594(sys, power, bow, ammo, soul) -> C_RPGCombat vtbl[6] 0x726A94; power = draw charge (1.0 when
  fully drawn, from C_ActorActionShootingAiming). vtbl[23] (strength) is not on this path.
- Loadout: `([C_Actor+0x280])->0x7F3310()`: bow +0x40, ammo +0x48; soul = `[C_Actor+0x668]`.
- The game's own preview (cvar `wh_w_DebugTrajectory`, Aiming update 0x28A0E70 -> 0x28962C8) makes exactly these
  calls every frame; the mod finds them through that block (SIG_PREVIEW) and calls them read-only while aiming.
- Arrow physics: `CProjectile+0x68` ammo params -> `+0x88` particle params: air resistance +0x24, gravity Vec3 +0x44.
- Round 3 data: with 0.3.0's averaged camera offset, the pitch offset used vs the real one differed by -0.35..+0.61
  deg; that alone explains the measured misses (4/18/6/3/15 cm explained vs 5/18/10/5/16 cm measured, marksim 21).
- Head bone: every round 3 lookup fell back to the box with no reason logged. The lookup matches Entity.GetBonePos
  step for step; it now logs the failing step, accepts a vtable with the same functions, and searches the
  skeleton's joint names (CDefaultSkeleton vtbl[1] count, vtbl[6] name) for "head" if "Bip01 Head" is absent.

## Round 4 findings (0.5.0)

- 0.4.0's live pose was accurate when the bow was drawn: per-shot misses 1-9 cm at 12-25 m, and the launch matched
  the mod's last aimed frame (2-3 ms earlier) to 0.00 deg, so the marker is exact at release.
- **Jolt:** the first frames of each draw (and after each shot, while the next arrow is nocked) put the "arrow"
  joint far from the view: pitch -12 to -66 deg, yaw up to +152 deg. 0.4.0 steered on them at once (wanted turns
  of 30-71 deg). 0.5.0 gates the pose (`PoseGate`, mk_pose.h): within 10 deg of the view, changing under 25 deg/s,
  for 200 ms before any turn; once settled, up to 0.1 s of odd frames are skipped, more re-start the wait.
- **Shake:** 0.4.0 followed each frame's offset (tau 0.03 s) and fed its derivative forward, so tremor and joint
  noise became camera acceleration (marksim 25: 253 deg/s^2 RMS). 0.5.0 tracks the offset with a critically
  damped filter (`SwayTrack`, bandwidth 1/SwaySmoothingMs) and feeds forward only its smooth rate and the
  target's motion; a soft dead zone (HoldWithinCm) and an acceleration limit (570 deg/s^2) on top (32 deg/s^2).
  The marker and the per-shot log still use the raw pose of the frame.
- **Self-disable:** the "camera does not follow" check fired after a shot when the bow pose went far off
  (error 22.5 -> 99.4 deg) and set the global off switch. Now it pauses 0.5 s and re-settles; only 6 trips in a row
  with a settled, plausible bow give up steering (tracking and logging go on). Pose read faults back off 1-60 s
  and retry; tick faults restart the aim (20 to give up). The offset yaw is wrapped (0.4.0 logged +360 deg).
- **Crossbows and firearms** (static RE, /mnt/project-files/theatrical/shared/automarksman-weapons-re.md): same
  Fire/Launch path, same GetLaunchPose (no weapon branch: "arrow" attachment, fallback "arw_01", dir = -Z) and the
  same speed function (+0x140 is the OldFormula switch, linear in power; power is 1.0 from Aiming). Bolts are CArrow,
  balls CProjectile: both slot-35 hooks cover them. The controller's aiming flag stays 1 during a crossbow/gun
  reload, so 0.5.0 steers only while the Main shooting action's sub-action (`[[loadout]+0x50]+0x98`) is
  C_ActorActionShootingAiming (RTTI, optional: without it the pose gate alone decides). Projectile gravity and
  drag are read at each launch and kept per ammo item. Multi-pellet ammo (class 4) has real random spread.

## Round 5 findings (0.6.0)

- Bow good (most shots within 2-16 cm); crossbow "bouncy"; hand cannon stopped aiming at the click. Weapon
  family was not logged; now each shot says bow / crossbow / hand cannon (C_MissileWeaponPlayerController+0x28,
  CryString "combat_bow" / "combat_crossbow" / "combat_gun").
- **Native aim sway is a camera turn** (shared/automarksman-sway-re.md): C_ActorActionShootingAiming vtbl[55]
  0x289C0D8 returns a look rate s*(cos(pi t)/3, 0, sin(pi t/2)), s from stat 0x45 (AimSpreadMax, skill, fatigue);
  Main::Update 0x28A12E4 adds dt*rate to the look accumulator (+0x88) every frame, player only. Crossbows and guns
  enter Aiming at once (peak rate), bows only at full draw. 0.6.0 points that vtable slot at a wrapper that
  scales the returned rate by SwayStrength (default 25%) only while the assist is on; the original pointer is
  put back when it is switched off, on unload and if the mod stops. No data or stats are changed.
- **Hand cannon fuse** (shared/automarksman-firearm-fuse-re.md): the click goes straight to Firing; animation
  event "ignite" sets +0xB4, "firearrow" sets +0xA4 and calls Fire -> GetLaunchPose -> Launch, ~2.45 s later.
  The direction is read at "firearrow" and the view is not locked, so 0.6.0 keeps aiming in Firing until +0xA4
  is set or this shot launches (max 4 s). During the fuse the game drifts the view at the sway rate frozen at
  the click (Firing+0xA8, also scaled by SwayStrength). Whether the gun joint follows the view during the fuse
  is animation data: round 5's shot left +1.24 deg above the view, as during aiming, which suggests it does.
- Crossbows launch ~25 ms into Firing; 0.5.0 stopped at Firing and the joint moved -0.15 deg meanwhile
  (13-15 cm low at 48-58 m). Now aimed until the launch too.
- The projectile helper offset differs per weapon (bow 64.5 cm, crossbow 15.2 cm, gun 0): kept per weapon now
  (one shared value put crossbow shots' start 41 cm off). The calibration file (fallback only) learns from
  bow shots only.
- Arrows 37 and 42 left their launch plane mid-flight (deflected 54-116 cm sideways): now reported as brushed/hit.

### LOCAL ROUND-6 DIAGNOSTIC PATCH (not release-ready)
- Fix false "bow measured" notification after crossbow and gun shots: only notify on first real bow measurement.
- Gate first-bow measurement prompt to actual bow family.
- Notices=0 by default, because current SendInfoText literal text can display raw @ in-game. F4 continues to work and is logged.
- CrossbowPoseDiagnostics=1 logs firing-pose yaw/pitch, camera movement and mouse input at 4 Hz even if no target; compare F4 OFF and ON, and aim at empty space. This is read-only diagnostics, not a claimed weapon-sway fix.
- Do not publish/merge before live diagnosis; preserve four other mods.
