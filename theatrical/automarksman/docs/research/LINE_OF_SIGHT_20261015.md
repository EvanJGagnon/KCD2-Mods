# AutoMarksman physics line-of-sight, 2026-10-15

LOCAL TEST BUILD. Do not publish until verified in-game.

## Reverted distorted crossbow V2
User reported strangely oriented weapon with V2 idle animation. Restored previously helpful V1 experiment while game closed; SHA256 1CF17333D72C439B96C464462AA8CF97F6BBF4FDF00F40A0D312CAA8692A4E2E. No Herb Picking Animation and other mods unchanged.

## Target through walls
Previously selected candidates only by camera angle, 80m distance, and living/dead state, without actual LOS. On one recorded test it selected kkut_executioner 79.5m away at 0.7 degrees while the user thought there was no one in view. Whether that specific NPC was behind a wall was not proven, but the algorithm allowed it.

New local AutoMarksman build 0.6.0 LOCAL DIAGNOSTIC 6:
- Resolves KCD2 WHGame.dll native physics wrapper through unique byte signature of actual game's shooting caller at RVA 0x2899B02; physics world global RVA 0x492D830 and wrapper RVA 0x4846e4 in build 15693. Signature resolution and ABI verified against actual binary and selftest. The wrapper passes to IPhysicalWorld vtable +0x118.
- Ray object flags 0x107 include solid static/terrain and physical rigid/sleeping props, excluding living characters to avoid Henry's own body blocking view. Ray flags 0xF stop at solid collision. Does not currently check NPCs obstructing other NPCs.
- During target selection, verifies real ray from camera to head approximation before acquiring; and when valid projectile pose and speed are available, checks the predicted low-arc ballistic trajectory from weapon joint to target in 2-12 short collision-ray segments.
- Current lock gets priority; the best-ranked 8 candidates may be tested per selection tick; all unchecked/blocked targets are denied by default.
- Physics unavailable / faulted => no acquisition, fail closed. INI RequireClearShot=1 default; set 0 only as a troubleshooting opt-out.
- No changes to hand-cannon fuse, projectile launch, aim animation, F4 hotkey or indicators. V1 standalone crossbow shake experiment remains installed separately.
- New tests 31 and 32 simulate blockers and clear targets; 30 original tests must still pass.

## One-launch verification
With game loaded: aim crossbow at visible isolated target, then near someone behind a real wall. Unobstructed NPC should lock; wall-hidden NPC must not. Verify crossbow pose is normal (V1), F4 toggle indicators work. Agent reads AutoMarksman log directly, no user inspection of logs needed.

## Rollback
Timestamped backup of previous AutoMarksman DLL/INI on user PC. New ray query is experimental until actual game session. If it throws/produces bad distances it refuses locks. No game save or original game archive is modified.
