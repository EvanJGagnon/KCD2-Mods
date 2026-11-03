# AutoForge 2.0.1-test release candidate

## Actual tested implementation

This supersedes PR #12's original post-quench design. The forge update stops while
its native result dialog is open; continuation therefore also runs through the
main-window message pump. Native result capture/close is guarded; after closing,
the retired forge object is **not dereferenced** to restart. The production
restart routine reacquires the current player's forge component through the same
manager lookup used by native interaction/script callers, checks type/owner,
refuses an already-active session, and validates lifetime again after start.
Unsupported lookup groups disable their optional functionality safely.

The crafting engine and result-dismissal implementation are preserved from the
installed successful test. Do not substitute the old PR binary or a rebuilt DLL
for the exact gameplay-verified binary without explicitly recording the change.

## In-game evidence

- Game timestamp `0x6A350E20`, Steam build 15693.
- Live startup: `v2.0.1-test native-reacquire`.
- Queue completion records: `done, 3 made` at 13:50:10 and `done, 4 made` at
  14:09:07; five successful native component reacquisitions between pieces.
- The user independently confirms all seven items completed successfully.
- Verified installed DLL SHA256:
  `479f7d814efb6faeb5b54aa67e40b1fca94783193c23e795b13aa2eef14dfea3`.
- Logs/binaries are private evidence, not files to commit or distribute as source.

## Reproducible checks

`build.bat` now builds `forgequeuetest`, `pumptest`, `restarttest` and `hookaudit`.
CI runs the first three (production restart is exercised with offline mocks,
including an inaccessible retired pointer). Against a local, legally installed
WHGame.dll, `selftest.exe WHGame.dll out/dev/kcd2_autoforge.dll` resolves the
native signatures; `hookaudit` audits actual modal call-site patches offline.
The existing private validation runner also compares 24 forging cases against
the previous crafting implementation (four pieces, three flip settings, two
orientations), checks installer guards/lifecycle and verifies original addresses.
These are offline checks, not additional gameplay tests.

Current local result: standard `build.bat` and all three queue/pump/restart
executables pass; the private complete validation passes (33 native addresses,
modal hook audit, 24 baseline-identical crafting cases, install guards/lifecycle).
Both candidate archives pass CRC/content/version audits and exact comparison of
all 16 nested localization packs against the header. All 443 snapshotted installed
non-log files remain unchanged. MSVC environment initialization prints a missing
`vswhere.exe` advisory on this machine, but compilation and tests exit successfully.

## Packaging

`python tools/package_forge_rc.py` packages a fresh build only. To preserve the
proven binary, provide `--tested-dll PATH --sha256 HASH` with the hash above.
The command refuses to overwrite candidates and does not delete existing dist
archives. It audits both normal/NoFlip variants, version/readme/manifest,
localization layout, forbidden build/game assets and exact DLL bytes. Both
variants share a mod folder and must not be installed together. NoFlip is a
configuration alternative, not separately in-game verified here.

The package retains **2.0.1-test**, the actual binary identity. Stable-version
renaming/recompilation, public release, merge and Nexus upload await approval.
The seven-item acceptance does not require repeating. Optional later coverage:
F9 cancellation/resume and insufficient-material handling; NoFlip if desired.
