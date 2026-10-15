# Crossbow F4-on bounce reduction, 2026-10-15

STATUS: LOCAL TEST BUILD, not published and not yet in-game validated.

## User's decisive comparison
User confirmed physics LOS now works. When F4 OFF, crossbow does not visibly bounce; F4 ON brings the bouncing back. Current log at 01:26-01:27 shows crossbow camera moving up/down 0.2-1.6 degrees per 0.25 seconds with no mouse motion; during manual-free aiming it switched from kkut_additive_man_67 to kkut_strnad to kkut_man_125. This was the mod's selection and steering, not evidence that the weapon's native idle loop must be removed.

## Two specific crossbow-only corrections
1. The target policy has an opt-in sticky lock: once crossbow has acquired a live unobstructed person, it will not jump to another NPC unless the player moves the mouse, or the first NPC goes away, becomes occluded, or leaves the keep cone. This prevents sudden camera yanks to bystanders. Other bow and gun targeting is unchanged; firing pin protection remains.
2. Attenuate the crossbow servo's animated-pose velocity feed-forward to CrossbowSwayRateGain=40 percent (default). Keep positional auto-aim error correction at full strength, normal bow/gun velocity compensation at 100 percent, F4 and localized status, and the collision LOS physics filter.

## Testing and limits
All 34 offline simulations pass, native binary selftest and 15693 signature checks pass. A controlled simulated crossbow at 22 metres measured baseline 28.5 deg/s2 camera acceleration and 89.4% 8cm head hits. Revised rate compensation 24.3 deg/s2 and 76.6% 8cm head hits, mean miss 6.2cm in both. These are model results, NOT in-game accuracy guarantees. Simpler over-smoothing, altered aim idle animations and a wider deadzone were rejected because they sacrificed aim or distorted stance. Preserved original successful V1 standalone neutral WeaponShake mod; reverted problematic V2 earlier.

## One gameplay check
Launch game, keep F4 ON; hold the loaded crossbow steady at one visible person near others for several seconds, and compare visible bouncing with F4 OFF. Check whether the targeting sticks instead of skipping across NPCs. One stationary bolt shot if stance is normal. Agent reads logs directly. If worse or invisible, restore previous DLL/INI from backup, without altering calibration and other mods. This build has only been installed locally, no GitHub or Nexus publication.
