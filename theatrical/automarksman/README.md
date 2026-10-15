# Theatrical AutoMarksman

KCSE plugin for Kingdom Come: Deliverance II: while a bow, crossbow or hand cannon is aimed, the camera turns onto the
head of the target nearest the centre of the view, using the weapon's live launch pose and the game's own
projectile speed, gravity and drag. F4 toggles. Each weapon family can be switched off in the ini (`Bows`, `Crossbows`, `HandCannons`).

| path | |
|---|---|
| `src/automarksman.cpp` | the plugin (`kcd2_automarksman.dll`): signatures, hooks, targeting, steering, notices |
| `src/mk_ballistics.h`, `mk_pose.h`, `mk_target.h`, `mk_visibility.h` | portable aim maths, servo, target policy, line of sight |
| `src/kcd2_common.h` | copy of the repository's shared runtime (unchanged) |
| `kcd2_automarksman.ini` | the release settings |
| `localization/` | the ON/OFF message table packed into `Localization\*_xml.pak` |
| `tools/marksim.cpp` | offline ballistics and steering tests (run by `build.bat`) |
| `tools/check_sigs.py` | resolves every signature against a WHGame.dll without Windows (`pip install pefile`) |
| `tools/experimental/` | local-only crossbow animation experiments; never packaged (see its README) |
| `docs/NOTES.md`, `docs/research/` | reverse-engineering notes (build 15693) and the round-by-round investigations |
| `docs/theatrical_automarksman.txt` | the README shipped with the mod |

Build: `build.bat` here (VS 2022 x64), or the repository's `build.bat` for every mod; archives come from the
repository's `tools\package.py`. Linux: `g++ -O2 -std=c++17 -Isrc tools/marksim.cpp -o marksim && ./marksim`.
