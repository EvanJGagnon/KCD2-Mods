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
