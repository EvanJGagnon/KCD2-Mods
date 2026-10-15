# AutoMarksman: crossbow sway investigation, 2026-10-14 23:xx
LOCAL RESEARCH ONLY. Do not publish or claim the physical weapon wobble is fixed.

## Latest live evidence (LOCAL DIAGNOSTIC 0.6.0)
Game log: Mods/theatrical_automarksman/KCSE/Plugins/kcd2_automarksman.log
Session 23:11:28 through 23:15:18 (build 15693). Local source commit 6f4023c, on branch local/automarksman-round6-fix; not pushed.

The read-only diagnostic is active; F4 defaults OFF, [Options] Notices=0, CrossbowPoseDiagnostics=1, native view sway 25% when F4 ON.

- Crossbow, F4 OFF, no target: 25 diagnostic samples. Weapon-world orientation change averages 0.230 deg per ~250 ms (peak 0.502); camera-world orientation change averages 0.182 deg (peak 0.428). Relative projectile-axis pitch versus camera ranged +2.328 to +3.215 deg.
- Crossbow, F4 ON with target: 27 samples, though includes pose-settling transition. Native view sway scales to 25%; the live firing pose still varies around camera and residual predicted shot misses were observed.
- F4 ON without target has only three samples and includes one 39.46 deg camera turn with nonzero manual input. This segment is NOT a controlled comparison. Do not claim it proves independent oscillation under F4 ON.

IMPORTANT INTERPRETATION: the moving projectile axis and varying angle from the camera demonstrate that view sway alone is an insufficient mechanical guarantee of accuracy. They do not by themselves distinguish independent skeletal animation from aim lag/IK response to native view movement. Neither should be asserted as proven root cause yet.

## Skill/stats (important user observation)
User expects native CAMERA and physical WEAPON sway to vary with Henry's marksmanship and/or fatigue/strength. This matters when developing a universal mod rather than tuning just this high-level save.

Previous static reverse engineering of C_ActorActionShootingAiming vtable[55] at WHGame.dll RVA 0x289C0D8 showed that the native camera look-sway rate derives from the gameplay stat AimSpreadMax (id 0x45, influenced by skill/fatigue), then from harmonic phase. Main::Update RVA 0x28A12E4 integrates that returned rate into look-angle accumulator +0x88. This was independently inspected by Capstone on local WHGame.dll; vtbl[55] is the source the existing SwayHook scales only when F4 is ON. The physical weapon/IK sway path is not yet mapped, and no claim is made that it uses the same stat.

Theatrical AutoMarksman should never hardcode a view offset/amplitude measured only on high-skill Henry. Prefer an independently configurable, safe, player-specific multiplier of the game's own native WEAPON / IK sway, *after* locating and validating that source, retaining dynamic skill/condition effects and correct real projectile pose. If the native source is additive animation/IK, validate output and guard against NPC effects.

## Nearby animation assets inspected
Data/Animations.pak includes Animations/Mannequin/ADB/kcd_crossbow_database.adb with player-specific l_crossbow+player Fire fragment and crossbow_loaded looping Load fragments. It also contains crossbow aim-idle TurnAngle blendspaces. These documents alone do NOT identify the procedural wobble driver, and globally overriding the animation assets would affect NPCs. Do not patch animations speculatively.

## Safe next phase
1. Staticaly/dynamically trace weapon projectile joint/pose production during *steady* Aiming, including procedural weapon IK/animation. Distinguish steady camera-driven motion versus bone-relative motion. Avoid permanently changing the player stats, animation assets or real ballistic marker.
2. If validated, add a separate crossbow-only WeaponSwayStrengthCrossbow setting (100 = vanilla, 0 = no extra sway), with safe pass-through when F4 OFF, guarded pointer restoration, skill/condition-sensitive vanilla baseline, and frame/rate-safety.
3. Build + signatures + selftest + offline regression, then one in-game batch comparing stable empty-space F4 OFF and ON, stationary target, bow and delayed hand cannon shots. Preserve all previous fixes, F4 binding, calibration, four other mods and game save.

## No regressions observed / remaining
No new software changes or game files were made in this investigation. The local round-6 build still includes fixes for false bow-measured notices and workaround Notices=0 to hide raw @ text (not a localization fix). Bow measured and gun fuse control remain important regression gates. Do not claim it fixes physical crossbow bouncing until proven in game.
