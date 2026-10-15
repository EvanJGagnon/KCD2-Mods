# Gun firing target drift, October 14 2026

## Live game evidence: high-skill Henry, AutoMarksman 0.6.0 LOCAL DIAGNOSTIC 2

- From 23:42:56 to 23:43:01 hand cannon was Aiming at kkut_man_125, ~11.5-11.6 m. Pre-trigger predicted misses were usually 2-8 cm.
- Weapon state changed to Firing at 23:43:01.041. The shot was not spawned until 23:43:03.489 (2.448 seconds later).
- During Firing, weapon-to-camera yaw moved between approximately -0.82 and +2.54 degrees, pitch between -0.29 and +3.32 degrees. Aiming controller kept issuing new camera turns; miss estimates surged 16, 22, 40, 38 and 52 cm during that interval.
- At 23:43:03.089, while still waiting on that same shot, candidate selection SWITCHED the target to kkut_strnad, ~16.1 m, because it was closer to the shifted sight line.
- At actual release the shot's tracked target was kkut_strnad, with predicted impact 20 cm from its head. The measured projectile came closest 37 cm to that target's aim point. Log cannot prove exactly whom the user hit.
- Native view sway from the high-level user's gun aim was just 1.2 deg/s unscaled / 0.3 deg/s with INI SwayStrengthGun=25. The much larger gun-pose swing is NOT explained by the sampled native view-sway alone; source remains unproven, likely animation/pose or transient IK.
- Both game behavior and ability scaling should be verified on lower-level Henry later; do not hardcode a baseline from this character's advanced skill.

## Target switching bugfix (in code after aa73fd9)

- While the shooting action is Firing, capture the current target exactly once at state entry and **do not call pick() again** until the action leaves Firing. A shot started with no target cannot acquire another target during its fuse. If the original target becomes unreadable, fail closed.
- If the user moves the mouse during the fuse, cancel auto-follow for the committed shot without reacquiring someone else. Manual aiming continues to work.
- This is not a gun weapon-sway cure. It prevents a class of unintended target jumps and preserves player input without changing projectile physics, weapon pose, or skill-dependent sway.
- Added marksim test 30 for the switched-bystander situation, user override, resumption after Firing, and shot started without target; existing 1-29 and build signatures/selftest pass.
- Next investigation: separate native gun weapon aiming/firing pose animation from player view sway; may need a reversible player-only animation-layer change, but MUST first verify that the gun projectile still spawns from a correct launch axis and unaffected NPCs/bow/crossbow.
