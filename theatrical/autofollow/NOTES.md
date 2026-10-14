# Theatrical AutoFollow: research notes (build 15693)

Static analysis of `WHGame.dll` (Steam build 15693, image base 0x180000000). Addresses are VAs.
Reproduce with `python tools/check_sigs.py <WHGame.dll>`.

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

## Not yet found

The reader of block+0x3E0 (the "can follow" check). The Lua binding `CanFollow` (name 0x183ECBBF0, used at
0x18128A33C) goes through a virtual call (`[vtbl+0xA0]`, thunk 0x18213A0C0), so it was not traced
statically. The in-game test settles whether the switch does what its description says.

## Unrelated look-alikes

* `wh_pl_FollowEntity` / `wh_pl_CameraFollowEntity`: debug camera following, not Henry walking.
* `wh_horse_Follow*`, `wh_rpg_FollowDrain*Stamina`: horse road following (Horse Route Follow territory).
