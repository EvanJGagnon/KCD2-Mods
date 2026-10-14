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

Not yet known: how speed depends on draw time (the log records hold time and speed per arrow), and
whether the arrow leaves exactly along the camera ray (the log records the yaw/pitch difference;
the mod learns and applies it).

## Ballistics (src/mk_ballistics.h)

Point mass, gravity g, linear drag k: horizontal `vh*E(t)`, height `vz*E(t) - g*(t-E(t))/k`,
`E(t) = (1-e^-kt)/k`. Low-arc pitch by scan + bisection; up to 4 lead iterations with the target's
smoothed velocity. Steering: feed-forward of the aim point's own angular motion plus proportional
correction, capped at TurnSpeed on the combined turn. `tools/marksim.cpp` tests all of it against an
independent step-integrated model (solver within 5 cm, flight fits, learning an unknown bow with
offsets, moving targets, rate cap, bow sway still causing misses).
