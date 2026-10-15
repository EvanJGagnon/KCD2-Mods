# Crossbow remaining bounce, local experiment V2 (2026-10-15)

## User's first animation test
With standalone player-specific neutral WeaponShake V1 installed, user reports crossbow still bounces, **less than before**, but F4 ON with "no one" can be dramatic and F4 ON with a person less so. This suggests subjective benefit from the V1 animation change, not proof the full sway is understood.

## New evidence
Log after V1 (00:53-00:56) contains F4=OFF target=NONE but **no F4=ON target=NONE steady-aim diagnostic samples**. At 00:55:57 F4 ON instantly acquired kkut_executioner 79.5m away and only 0.7 degrees off camera center. User perceived empty space, but AutoMarksman selected a distant NPC. Camera sometimes moved 1-3 degrees per 250ms even with reduced native view sway, indicating feedback from auto-aim. Current pick uses no occlusion check.

## Attempted servo change rejected
Prototype exact critically-damped replacement in mk_pose.h plus crossbow-specific 300ms tracking made camera RMS calmer (1.07 vs 1.70 deg/sec at 100ms) but simulated crossbow headshot fraction collapsed to zero and prior test #25 failed. Slow camera correction sacrifices aim. This change, including simulator edits and INI change, was entirely RESTORED in Git without installation. Hand cannon remains unchanged.

## V2 animation isolation
Builder: automarksman/tools/build_crossbow_idle_experiment_v2.py. Reads user's installed No Herb Picking Animation ADB, and adds exactly four more-specific player crossbow fragments:
1. WeaponShake standing / stealth loops become neutral Blank (same as V1).
2. MotionIdle standing crossbow aim loop crossbow_idle_aim becomes crossbow_idle_loaded while preserving vanilla CameraNear.
3. MotionIdle stealth aim loop crossbow_stealth_idle_aim becomes crossbow_stealth_idle_loaded while preserving holster AnimationOverride.

The base XML is byte-for-byte unchanged apart from inserted fragments. XML parsed, ZIP CRC validated. Proprietary merged ADB exists only in untracked out/ and installed test; do not distribute.
V2 installed over V1 while game closed under Mods/theatrical_crossbow_shake_test/Data/CrossbowShakeExperiment.pak, SHA256 08F7DED8E83CCE6F911B52989F5C143E42A9C1D3FB7AEB2A86DFF4CEDF6DF878.
V1 backed up under C:/Users/Acer/Documents/Mods/_backups/theatrical-crossbow-idle-v2-20261015. Other Theatrical DLL/INIs (16 files), NoHerb, mod order, game and calibration untouched. F4 AutoMarksman targeting/indicators unchanged. This standalone animation experiment is NOT an F4 toggle.

## One next gameplay test
1. Aim crossbow into empty space and look at stance/weapon bounce. If arm or crossbow is lowered unnaturally, do not shoot; report and revert V2.
2. If stance normal, shoot ONE controlled bolt with F4 ON and a stationary target, then hold aim F4 OFF a few seconds. User reports bounce better/unchanged/worse and shot orientation. Agent reads AutoMarksman log directly through Remote Desktop Commander.
3. If abnormal, remove test folder and restore V1 folder from backup while game closed.

Future: investigate procedural Aiming and line-of-sight / far-away target acquisition; do not worsen established hand-cannon accuracy.
