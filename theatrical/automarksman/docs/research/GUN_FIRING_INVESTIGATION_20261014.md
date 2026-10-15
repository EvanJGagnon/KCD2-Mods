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

## Original firearm animation asset audit

- Inspected original Data/Animations.pak without modifying/installing animation files.
- In Animations/Mannequin/ADB/kcd_male_database.adb: player rifle aim scope r_rifle+player+prime+Scope_AimPose blends rifle_aimposes_aim_upperbody_player (Loop), with AimSmoothing explicitly SmoothEnable=false and CameraNear.
- r_rifle+charge+prime uses rifle_idle_aim (Loop). Player/non-player r_rifle shooting fragments both reference rifle_shot_over (one-shot); stealth references rifle_stealth_shot. Rifle shot animation therefore is not proven player-only; DO NOT replace it globally without guarding NPCs.
- Animations/assets/firearms/hand_cannon.animevents refers to animations/assets/firearms/rifle_shot_over.caf with ignition effect at normalized time 0.145 and muzzle-front effect around 0.58. Effects alone do not identify the game's projectile launch time, which is measured by CProjectile hook.
- Likely avenue for large post-click aim deviation: separate the rifle_shot_over pose motion and the player rifle upperbody aim-pose layer from native camera sway (which was only ~0.3 deg/s scaled). Needs isolated reversible animation/IK test and projectile-pose verification before a sway remedy. Do not copy unsupported edits from third-party mods or suppress essential firing animation events.
