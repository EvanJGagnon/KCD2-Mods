# Crossbow physical weapon shake experiment: 2026-10-15

STATUS: **LOCAL IN-GAME TEST REQUIRED. NOT RELEASE READY.**

## Findings
- AutoMarksman's camera-view sway reduction alone did not remove visible weapon-axis wobble. On user's high-skill save, measured clean no-mouse samples: with F4 ON and no target, camera vertical RMS was 0.022 degrees per 0.25 s, whereas weapon vertical RMS stayed 0.226 degrees.
- Original KCD2 Animations.pak `Animations/Mannequin/ADB/kcd_male_database.adb`, WeaponShake fragment has `l_crossbow+charge+prime` -> looping `crossbow_idle_pulled_shaking_add`, plus separate `stealth+l_crossbow+charge+prime` -> `crossbow_stealth_idle_pulled_shaking_add`. Other weapon families have independent WeaponShake fragments.
- Controller definition `kcd_male_controllerdefs.xml` assigns `WeaponShake` with `l_crossbow+charge+prime` to `GameLogic` scope. GameLogic is Char3P layer 10 with 1 animation layer; AimPose is Char3P layer 14. This makes WeaponShake a stronger animation candidate than native view sway and allows an experiment without replacing AimPose.
- No Herb Picking Animation already overrides the same ADB through `Mods/no_herb_picking_animation/Data/FPHerbPick.pak`. Every other mod PAK was scanned (43 total), and no other copy was found. A raw vanilla animation ADB replacement would clobber this mod.

## Built experimental implementation
- Script: `automarksman/tools/build_crossbow_shake_experiment.py`. It loads the INSTALLED No Herb Picking Animation's ADB (rather than vanilla) and inserts **two more-specific player-tagged** WeaponShake fragments containing existing neutral `Blank` clip: `l_crossbow+player+charge+prime`, `stealth+l_crossbow+player+charge+prime`. It changes no existing fragment and verifies the original ADB is reproduced byte-for-byte after removing only the insertion. Unique deterministic GUIDs; parses the XML; verifies ZIP CRC.
- Generated proprietary base game ADB is NEVER committed or released. Output lives under `automarksman/out/crossbow_shake_experiment/theatrical_crossbow_shake_test`.
- Test mod installed (game was closed) at `Mods/theatrical_crossbow_shake_test`, appended as final mod in `Mods/mod_order.txt`. Original mod order preserved, backup `C:/Users/Acer/Documents/Mods/_backups/theatrical-crossbow-shake-experiment-20261015`. Original NoHerb archive, other 16 Theatrical DLL/INIs, existing AutoMarksman DLL/INI, and calibration hash-verified unchanged.
- F4 STILL toggles original AutoMarksman camera correction and target locking. The experimental crossbow animation replacement is **not yet F4 toggleable** because PAK overrides load at game startup. It is not part of the AutoMarksman DLL.
- Risk: no guarantee that player tag is set on WeaponShake selection; no guarantee Blank clip eliminates all physical sway. Still needs real crossbow aiming and bolt-path test; do not claim fixed or publish.
- Other behavior: local game logs show two hand-cannon shots aimed at approximately 16m and 22m during 00:44-00:45. User reported two hits in two shots near 20m. AutoMarksman logs still show large gun-pose swings during fuse; preserve current gun behavior until separately understood.

## One-launch test for user
1. Launch KCD2 normally. Aim loaded crossbow at empty space for ~10 seconds without moving mouse, then at stationary distant target with F4 ON and make one bolt shot. Observe whether crossbow still bobs and whether projectile follows normal ballistic path.
2. Press F4 OFF and aim at empty space for ~10 seconds (this should still benefit from the experimental animation change even with target lock OFF). F4 ON/OFF notifications should keep working.
3. Report only whether wobble was improved, worsened or unchanged, and whether shot went roughly where indicated. Agent reads game logs via Remote Desktop Commander and checks for ADB errors and diagnostics.
4. If unstable, remove entire `Mods/theatrical_crossbow_shake_test` folder and restore backup `mod_order_previous.txt` to `Mods/mod_order.txt` while game closed.

## Next proper implementation
If this proves the culprit, research player-only runtime selection/weight of the WeaponShake fragment so native sway (including Henry's level, stamina, condition) can be scaled rather than blanked globally. Keep F4 behavior meaningful and isolate to player, no NPC side effects, and preserve NoHerb mod. Do not install this experiment as the final published solution.
