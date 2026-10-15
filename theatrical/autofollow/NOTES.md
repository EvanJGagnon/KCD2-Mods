# Theatrical AutoFollow: research notes (build 15693)

Static analysis of `WHGame.dll` (Steam build 15693, image base 0x180000000). Addresses are VAs.
Reproduce with `python tools/check_sigs.py <WHGame.dll>`; `tools/fulltest` runs the real startup on a mapped WHGame.dll (also under Wine).

## The switch

`wh_player_FollowAlwaysCanFollow`, help "When enabled player can follow anyone as if they had followable
NPC side effect". Registered in the player cvar registration `0x1815A7864` (this = player cvar block):

```
0x1815A8337  lea  rax,[help 0x183EA27B0]
0x1815A833E  mov  [rsp+28h],rdi          ; rdi = 0: no on-change callback
0x1815A8343  lea  rdx,[r14+3E0h]          ; int* storage: player cvar block + 0x3E0
0x1815A834A  mov  [rsp+20h],rax          ; help
0x1815A834F  mov  r9d,ebp                ; flags: ebp = 2 (lea ebp,[rsi+2] at 0x1815A78D4, rsi = 0)
0x1815A8352  lea  rcx,[name 0x183EA2870]
0x1815A8359  xor  r8d,r8d                ; default 0
0x1815A835C  call 0x1823D8168            ; -> 0x180B941C0 -> IConsole vtbl[8] Register(name, int*, def, flags, help, cb, true)
```

* **Default is 0 (off)**, confirmed statically.
* **Flags = 2 = VF_CHEAT** for the whole player cvar group (every registration in the function uses
  `r9d=ebp`, and ebp is never written after `lea ebp,[rsi+2]`). In CryEngine `OnBeforeVarChange` refuses
  cheat cvars outside dev mode, so the console, user.cfg and Lua `System.SetCVar` cannot turn this on in a
  normal game. That is why the mod writes the storage directly instead of setting the cvar.
* The returned ICVar* is discarded, and the name string has a single reference (the registration), so
  the game reads the value through the int, not through a by-name lookup: writing the int is the same as
  a successful `wh_player_FollowAlwaysCanFollow 1`.
* Next registration (0x1815A8361): `wh_player_FollowDebugDraw` at block + 0x3E4, "Enables Follow DebugDraw".

## The player cvar block

Function-local static, returned by getter `0x1804AADE0` (`lea rax,[0x1855DBEC0]` at +0x27, thread-safe
static init guard before it). The game calls it at `0x181064609`:
`call getter; mov rcx,rax; call 0x1815A7864` (the registration above), inside game setup `0x1810645B8`.
So the variable is at **0x1855DC2A0** (.data, writable) in this build.

Registering an int cvar by pointer writes the default into it, so a value written before the game
registers the group is reset to 0. The mod re-checks four times a second and puts it back.

## Round 1 (2026-10-14): the switch alone does nothing

In game the mod set the variable to 1 (log: 0 -> 1, again after load) but no NPC could be followed. No
code reading block+0x3E0 was found: no rip-relative reference to the field, no `[reg+3E0h]` read within
~200 instructions of any of the 222 getter calls (also through one level of argument passing), and the
name string is only referenced by the registration. The cvar looks registered but unused in the release
build. The mod still sets it (`SetCvar=1`), harmless.

## Who can be followed (the real gate)

The chat-focus HUD/action update 0x18082DFA4 (strings `chat_focus_follow_init[_press]`,
`ui_hud_follow_start/stop`, `ui_hud_focus_start`) shows "follow start" when
`CanFollowTarget 0x18082E974(this, targetId) && 0x182AA83F4(this, targetId)` (r12b).
`CanFollowTarget`: player not in excluded states (0x18082EA40, player vtbl checks), target entity exists,
target actor's `[0x990]->vtbl[0x298]` in state 0/5/0x25 (tail 0x182249EA4), ... and
**0x18129DA18(out, targetId)**: target soul (actor+0x668) not excluded (soul vtbl[0x38], vtbl[0x30]),
then `HasSideEffect(0x38/0x39/0x3A)` = followableNPCCenter/Left/Right (name table 0x180BE51C3..F9).
Result `{u8 position, u8 valid}`: 0x100 center, 0x101 left, 0x102 right, valid=0 otherwise.
The follow action's update 0x18287DE7C calls the same two functions every tick (position stored at +0x78).

**0.2.0 fix:** at 0x18129DB22 the "none of the three" path `C6 47 01 00` (mov byte [rdi+1],0) becomes
`EB 21` (jmp 0x18129DB45: mov word [rdi],0x100 = center, followable). Early refusals (no soul, excluded
soul) keep their own path. Signature anchors on the three side-effect ids; the jump distance is computed
from the center branch and the target bytes verified. Aligned 2-byte atomic write; restored when off.

## Unrelated look-alikes

* `wh_pl_FollowEntity` / `wh_pl_CameraFollowEntity`: debug camera following, not Henry walking.
* `wh_horse_Follow*`, `wh_rpg_FollowDrain*Stamina`: horse road following (Horse Route Follow territory).
