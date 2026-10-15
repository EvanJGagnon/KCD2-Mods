# Theatrical AutoLockpick

KCSE plugin `kcd2_autolockpick.dll`: Henry plays the lockpicking minigame through C_LockPicking's own input
handlers (gamepad pick position + the `lock_dir_fwd` turn key). F2 starts and stops. Status: 1.0.0, passed
the in-game test (round 1, 2026-10-14).

- `src/autolockpick.cpp` -- the mod: signatures, Update hook (C_LockPicking vtbl[19]), handler calls.
- `src/lp_logic.h` -- the hand: seek the pin, settle, hold the turn key and track, let go and retry.
- `tools/lpsim.cpp` -- the hand against a model of the game's lock (`lpsim [locks] [seed] [verbose]`).
- `tools/check_sigs.py` -- resolves the signatures against a WHGame.dll on disk (no Windows needed).
- `tools/selftest.cpp` -- the same on Windows through the dev DLL (`selftest WHGame.dll out\dev\kcd2_autolockpick.dll`).
- `NOTES.md` -- the minigame as decoded from build 15693.

Build: `build.bat` (VS 2022 x64) here, or the repository's `build.bat` for every mod. The release archive comes from
the repository's `tools\package.py`; CI (`.github/workflows/build.yml`) builds, runs the simulator and packages it.
