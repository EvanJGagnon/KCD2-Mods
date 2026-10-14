# Theatrical AutoDice: reverse-engineering notes (WHGame.dll build 15693)

Static analysis only; nothing below has been observed in a running game yet. Addresses are RVAs.

## Input actions -> handlers (C_Dice constructor 0x2E7EC5C registers them, action map "dice_interaction")
| action | handler | what it does |
|---|---|---|
| dice_hold | 0x2E8FA84 | if `vtbl[47]` (input state `+0x498 == 5`): die under the cursor (`+0x4F0` index) -> its C_DieModel (`+0x48`) -> `HoldDie(model, die+0x28 id, !(die+0x40))` 0x2E97F64 |
| dice_score | 0x2E91B3C | gate, then model `0x2E9848C`: state `CanScore` (vtbl[18]) then `Score` (vtbl[19]) = set held dice aside and throw the rest; then UI refresh 0x2E92D20 |
| dice_pass | 0x2E919E0 | gate, then state `Pass(delegate = model+0x18, false)` (vtbl[22]); UI refresh |
| dice_badge_use | 0x2E8F86C | not used by the mod |
| dice_exit_badge_effect | 0x2E90388 | not used |

`HoldDie` 0x2E97F64 = `state(model+0x108)->vtbl[12](model+0x18, id, hold)`; the player-turn state queues a
hold command through the delegate (0x2E764B8 -> delegate vtbl[19]). The mod calls 0x2E97F64 directly with
the die id instead of moving the cursor: same function, same command, no cursor needed.

## Objects
- **C_Dice** (minigame, RTTI `.?AVC_Dice@playermodule@wh@@`, vtable 0x3F72098). vtbl[14] 0x2E9272C =
  per-frame update (same slot as C_Sharpening's). `+0x498` input state (5 = player may act; vtbl[47]),
  `+0x4A0` C_DiceGameModel* (getter 0x2E8B054 creates it lazily: the mod only reads the field),
  `+0x4B0 + 16*i` per-player visual objects (dice entities at `+0xF0`), `+0x4F0` hovered die index.
  vtbl[46] 0x2E91AD0 = give up (uses model `+0x20`, so player 0 is the human).
- **C_DiceGameModel** (vtable 0x473F178, delegate vtable at `+0x18` 0x473F368).
  `+0x20`/`+0x30` shared_ptr<C_DiceGamePlayerModel> player 0 (Henry) / 1 (from Lua `Dice.SetScore`
  0x2ECF8C8), `+0x50 + 0x40*i` per-player combination tables, `+0x100` target score (cvar
  `wh_pl_DiceTargetScore`, clamped to 100000; ctor 0x2E7F41C), `+0x108` current state (shared_ptr).
  Delegate vtbl[10] 0x2E947F4 = score the dice with held == flag: `(delegate, flag, int* score, bool, bool)
  -> bool all-scoring` (CanScore calls it with `(true, &s, 0, 0)`).
- **States** (I_DiceGameModelState, layout from libKCD2): `+0x18` = the player model the state is for.
  vtbl[4] returns it. PlayerTurn (vtable 0x3F711E0) is the state in which a player picks dice.
  Others: Initial, PlayerChange, CheckPassiveBadge, Rolling, Holding, Scoring, Bust, Passing, Ending, End,
  CheckBustBadge, BadgeDiceSelecting, ChangingDieValue, ExtraRolling, ReRolling, Null.
- **C_DiceGamePlayerModel** (vtable 0x473F198, ctor 0x2E7F608): `+0x18` points this turn (0x2E8B508 adds a
  selection to it), `+0x30` banked total (setter 0x2E95554), `+0x50` model, `+0x58` player index,
  dice: small-vector header at `+0xD0` with begin `+0xD8`, end `+0xE0` (shared_ptr<C_DieModel>, 16 bytes);
  a second one at `+0x160` (begin `+0x168`) - role not confirmed (dice set aside?).
- **C_DieModel** (vtable 0x473F160, ctor 0x2E7F76C, alloc 0xC8 incl. 0x10 control block):
  `+0x28` id, `+0x40` held, `+0x58` face 0..5, `+0x88` float side weights[6] (ctor sets 1.0),
  `+0xA0` first side value: 6 = joker face (devil's head die; item SideValues "6 1 2 3 4 5").
  Collect 0x2E89FF4 builds `{face, side0 == 6}` pairs from the dice with held == flag.

## Scoring (0x2E8DC9C builds each player's table; matcher 0x2E94138 is greedy over that table)
single 1 = 100, single 5 = 50, 1-5 = 500, 2-6 = 750, 1-6 = 1500, three of a kind 1 = 1000 / n = n*100,
each extra die doubles. One extra combination depends on a badge (global 0x55EDDDC). Joker matching is
in 0x2E86078. Badges can change per-player tables; the mod reads the game's own score of the held set
(delegate vtbl[10]) before acting and logs any difference from its own count.

## Assumptions to confirm in the test session
1. The table vector (`+0xD8`) holds only the dice on the table; dice set aside earlier in the turn are
   elsewhere. (Evidence: the scoring of held dice collects every die with held = 1 from it.)
2. PlayerTurn is entered after the dice have landed (no manual first throw).
3. `dice_pass` banks the held selection together with the turn points. (Static evidence: Passing state
   OnEnter 0x2E91F28 calls delegate vtbl[9] 0x2E93EE4, which scores the held dice and applies them
   unless the player model's `+0x198` already-scored flag is set.)
4. Player 0 (`model+0x20`) is Henry.
The log prints faces, held flags, turn/total before and after every action to check each of these.
