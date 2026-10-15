# AutoMarksman experimental tools (not part of any release)

- `build_crossbow_shake_experiment.py`, `build_crossbow_idle_experiment_v2.py`: build a local-only test
  `.pak` that swaps the player crossbow's WeaponShake / idle-aim fragments. They read the game's (and another
  mod's) animation database from a local install, so **their output is a derived game asset: never commit
  it, attach it to CI, or upload it anywhere.** V1 is the one Evan keeps installed locally
  (`Mods\theatrical_crossbow_shake_test`); V2 distorted the stance and was reverted. See
  `docs/research/CROSSBOW_SHAKE_*.md`.
- `crossbow_diag_report.py`: summarises the `crossbow diagnostic:` lines that `CrossbowPoseDiagnostics=1`
  (with `DebugLog=1`) writes to `kcd2_automarksman.log`.

The release packaging (`tools/package.py` at the repository root) never includes anything from here.
