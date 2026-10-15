# C_LockPicking, as decoded (WHGame.dll Steam build 15693, RVAs; image base 0x180000000)

Sources: static analysis of the build-15693 binary in this thread; libKCD2 (JerryYOJ, GPL v3)
`C_LockPicking.h` for the layout names; the 2026-10-14 research handoff (gpt-notes/autolockpick.md).
Nothing here is confirmed in game yet. The shared write-up of the input mechanism is
`/mnt/project-files/theatrical/shared/input-injection.md` in the project.

## Classes and entry points
- `.?AVC_LockPicking@playermodule@wh@@`, main vtable 0x3F6F8B0; vtbl[19] = Update 0x89A940 `(this, float dt)`.
- IActionListener sub-object at +0x10 (vtable 0x3F6F858), OnAction = vtbl[1] 0x16D4AAC
  -> TActionHandler dispatcher 0xA98158 with the handler map at this+0x80.
- Handlers registered in 0x168D6xx (ActionIds from globals()+0x30, +0x428..+0x448):
  0xA98244 for the four pick actions, 0xA98614 for `lock_dir_fwd`.
- Name test 0xA98474 = `_stricmp(*(char**)actionId, name) == 0` (so a `{const char*}` ActionId works).
- Start 0x8986A4 (no picks -> "OB_T17_LockpickNoLockpicks"; the RPG sweet-spot evaluation gets the input device
  +0x188, so the spot size may differ between mouse and gamepad; the mod does not change it).

## Fields
| off | meaning |
|---|---|
| +0x88 (byte) | state: 3 idle (pick off the pin), 4 on the pin, 5 dry run, 6 opened (0.5 s hold, +0x184), 7 commit, 8 broke |
| +0x90 | spot size (normaliser of the error) |
| +0x94 / +0x98 | start angle / open angle (deg) |
| +0x9C | lock angle (deg); +0xA0 last turn step |
| +0xA4 / +0xA8 | error distance / error / spot size |
| +0xAC | pick health |
| +0xB4 | turn speed (100 while the turn key is held) |
| +0xB8 | time in the minigame |
| +0xC4,+0xC8 | pin position at 0 deg |
| +0xD0,+0xD4 | pin position at the current angle |
| +0xDC,+0xE0 | pick position (input) |
| +0xE8,+0xEC | pick position with tremble (judged) |
| +0x138,+0x13C | pending mouse move (consumed by the next Update) |

## Per frame (Update, states 3-5)
1. 0x89AA30: above the start angle the lock springs back by `angle/360 * RPG vtbl[0xB8](actor, difficulty) * dt`;
   if +0xB4 != 0, 0x89A69C turns by `+0xB4 * dt` (in state 3 below start+9 deg -> state 5 dry run, where the
   angle is clamped near the start; in state 3 further in it does not turn). Then the pin is rotated:
   `phi = -angle*pi/180; x = cx*cos(phi) + cy*sin(phi); y = cy*cos(phi) - cx*sin(phi)`.
2. 0x89AE30: pending mouse move added to the pick, clamped to radius 1; tremble from RPG vtbl[0xE0]
   added and clamped to [-1,1] -> +0xE8/+0xEC.
3. 0x89BAEC: error = |trembling pick - pin| / max(spot, 1e-4). Above the start angle pick health -=
   RPG vtbl[0xB0](actor, error) * dt, and a pick at <= 0 breaks (vtbl[2]). Pick within radius 0.2 of the centre
   = withdrawn (state 4 -> 3). Error <= 1: at the open angle -> state 6, else -> state 4 ("onplace").
   Error > 1: state 4 -> 3.

## Inputs (handlers act only in states 3-5)
- `xi_lock_cursor_x/y`: pick x/y = value (absolute, -1..1, y up), pending mouse move cleared.
- `lock_cursor_x/y`: pending move += value * k(0x55EDD88, runtime) * sensitivity (0x82FD18); y inverted.
- `lock_dir_fwd` mode 4 (hold): +0xB4 = 100. Mode 2 (release; any action): +0xB4 = 0, then 0x897AB4 resets the
  lock to the start angle and state 3.

## Not decoded (the RPG tables decide them; the mod reads the outcome instead)
Tremble shape, spring-back rate, wear per error. `tools/lpsim.cpp` uses plausible stand-ins.
The pin's random placement at start was not found (not written by a plain store in 0x896000-0x8A0000).
