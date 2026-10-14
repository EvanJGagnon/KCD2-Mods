# AutoPickpocket research notes (Steam build 15693)

Static analysis of `WHGame.dll` (image base 0x180000000; RVAs below) and of the game's own
`Pickpocketing.gfx` UI movie (disassembled locally with an AS2 disassembler; the movie itself is game
data and is not in this repository). Field names for `C_Pickpocketing` started from **libKCD2** by
JerryYOJ (GPL v3, https://github.com/JerryYOJ/libKCD2); every offset below was re-checked against the
disassembly. No libKCD2 code is used.

## Charge: C_Pickpocketing (`.?AVC_Pickpocketing@playermodule@wh@@`, vtable 0x3F74DD8)

| field | meaning |
|---|---|
| +0x18 | actor |
| +0x78 | state: 1 charging below the minimum, 2 above it, 3 grid open |
| +0x80 | detection roll, drawn when the charge starts |
| +0x84 | boost (s): grows by dt x rate each frame, clamped to the maximum |
| +0x88 / +0x8C / +0x90 | rate / minimum / maximum |
| +0x98, +0xC0 | timers |
| +0xF0 | `I_UIPickpocketing*` |

* vtbl[19] **Update(this, float dt)** 0x2EB3B34: in states 1/2 adds the boost, pushes the ring to the UI
  (SetBoostProgress), and opens the grid itself at the maximum.
* **Detection** 0x2EA8754, every 1000 ms: passes while `notFail(thief, victim, boost) >= roll`
  (`rpg[+0x1B8]`, notFail 0x2CF64F8).
* **Ring** 0x2CF686C: `safety = remap(notFail, roll + IndicatorSharpness .. 1 -> 0 .. 1)`. 1 green, 0 red;
  the check still passes at 0 (the sharpness is the margin).
* Start 0x2EB0F74 binds action **"charge"** of action map **"pickpocketing"** (`defaultProfile.xml`:
  fires on release). Its functor `_Do_call` 0x203DEE0 reads `[this+8]` = C_Pickpocketing; state 1 calls
  vtbl+0x28 (give up), state 2 jumps to 0x2EB1D84 (open the grid); other states do nothing. The mod calls
  this functor with a stand-in `{nullptr, pp}`, which is exactly what the key release does.

## Grid: C_UIPickpocketing (`.?AVC_UIPickpocketing@guimodule@wh@@`)

`I_UIPickpocketing` is its sub-object at +0x58. RTTI lists **two** vtables for that offset; the one the
game uses is 0x3C33898 (found by its slots, not by RTTI):
`[0] Open(state, ?) 0x2BBA2D4`, `[1] Close 0x2BB8AF8`, `[2] SetBoostProgress(progress, minimum, int seconds, safety) 0x2BBAC78`.

Open's state: +0 maxTime, +4 Time (= boost s, the budget), +8/+0x10 item records (0x18 bytes: +0 slot
0..10 (UI slot = slot+1), +8 `C_Item*`, +0x10 uncovered, +0x14 penalty s), +0x20 selector speed
(slots/s), +0x24 Easy Way Out perk.

`C_UIFlashPickpocketingItem` (vtable 0x3EEBCD0) vtbl[1] **FillUIArgs** 0x2B8D3F4 pushes: +0x18 icon
(string), +0x20 time penalty (float), +0x24 slot (int), +0x28 uncovered (bool), +0x2C health, +0x30
health state (int), +0x34 weight, +0x38 category (string), +0x40 price (float), +0x44 count (int), +0x48 stat.

Inputs: every grid key goes through **`C_UIFlashBase::CallFunction(ui, const CryString& name)`**
0x566BCC (reads only `[name]` as `char*`): left "MoveLeft", right "MoveRight", uncover/take/close
"DoAction" (`Libs/UI/UIElements/Pickpocketing.xml` maps them to `fc_moveLeft`/`fc_moveRight`/`fc_doAction`).

UI events (registered in Init 0xED1D04):
* **OnActionHintState(ui, CryString* action, bool visible, bool enable)** 0x2BB9DAC: toggles the
  `pickpocketing_ui` action-map entries. Used by the mod to learn which commands the movie accepted.
* **OnClose(ui, bool result, CryString* items)** 0x2BB9E50.

## Pickpocketing.gfx (60 fps)

* Slot 0 is EXIT, 1..11 items. Moves and actions are ignored unless the selector queue holds one entry
  (the selector is at rest).
* Accepted move: at once `hint(take, false, false)`.
* DoAction on a hidden item: `(uncover, false, false)`, `(take, true, false)`, state UNCOVERING. On an
  uncovered one: SELECT, `(take, true, false)`, and `penalty*1000 + 1000` ms come off the timer over 5
  frames. On EXIT: `ClosePickpocket(true)`.
* Tiles: frames 1->120 uncovering (1 s), 120->240 shown (1 s), 240->360 covering (1 s).
* Arrival: `(close, true, true)` on EXIT, `(uncover, true, true)` on a hidden item, `(take, true, true)`
  on a shown one.
* Timer on the wall clock; at zero the result is success only if the selector rests on EXIT.
* Selector: `round(r9*2*dir)/2` degrees per frame, `r9 = dt_s*30*speed`, so the step time depends on the
  frame rate. Easy Way Out doubles r9 if `(dir==1 && q0>=6) || (dir==-1 && q0<=6) || (q1==11 && q0==EXIT)`.
* Hint names: `pickpocketing_uncover`, `pickpocketing_take`, `pickpocketing_close`.

## The mod

* Hooks (all verified by signature and by their vtable slot): C_Pickpocketing vtbl[19], I_UIPickpocketing
  [0] and [2], item vtbl[1]; inline (17 / 19 whole-instruction bytes) OnActionHintState and OnClose.
* Charge: release at `safety <= ReleaseSafety` in state 2 (or after MaxChargeSeconds). No ring value
  seen: release half way between minimum and maximum.
* Grid: `pp_logic.h` greedy value/time planner with a feasibility check (`travel + cost + way home +
  SafetyMs`), re-checked every step. Step times start from the nominal speed plus 70 ms and are then
  measured from accepted/ignored moves. A command counts as accepted only when the movie raises its hint;
  if hints arrive late, a command waits up to 150 ms and 3 ticks, and a hint that turns up after that is
  still credited.
* Fair mode (default): covered items are unknown until uncovered (their cost and value are estimated
  from what is visible).

## Simulator (`tools/ppsim.cpp`)

Models the movie above frame by frame. 2000 runs x 3 seeds x 5 variants (60 fps, 60 fps + hitches,
30 fps + hitches, 45 fps with hints a frame late, 60 fps omniscient): **0 caught**, about 0.95 items and
19-20 % of all value per grid (27-28 % omniscient), 3.7-3.9 s of budget left on average.

## Open questions for the test session

* Is FillUIArgs called inside Open (prices known) or later (prices unknown -> value-blind but safe)?
  The log line `grid_start items_described=N` answers it.
* Are hints raised during our CallFunction (sync) as expected? `DebugLog=1` shows `hint ...` lines
  between `send ...` lines.
* Real selector step time vs nominal: logged as `selector step measured`.
