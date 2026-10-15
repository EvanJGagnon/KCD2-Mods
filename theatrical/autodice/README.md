# Theatrical AutoDice

Henry plays his own turns at dice in Kingdom Come: Deliverance II: after each throw he sets aside the dice
worth keeping, one at a time, then throws again or passes, through the game's own dice actions. The game
throws, scores, busts and pays; the mod only chooses. KCSE plugin, same architecture as the published mods
in this repository (signature + RTTI resolution, fail-safe when the game changes).

| path | |
|---|---|
| `src/autodice.cpp` | the plugin (`kcd2_autodice.dll`), F3 starts/stops |
| `src/dice_engine.h` | scoring rules and the decision engine (portable C++, no game code) |
| `src/kcd2_common.h` | copy of the repository's shared runtime (unchanged) |
| `tools/dicesim.cpp` | engine tests and whole-game simulations against simple strategies |
| `tools/check_sigs.py` | resolves every signature against a WHGame.dll without Windows |
| `tools/selftest.cpp` | resolves the plugin's signatures inside Windows (`out\dev`) |
| `NOTES.md` | reverse-engineering notes (build 15693) and what the first game test must confirm |
| `docs/theatrical_autodice.txt` | the README shipped with the mod |

Build (Windows, VS 2022): `build.bat` here, or the repository's `build.bat` for every mod; the release archive
(`dist\TheatricalAutoDice-<ver>.zip`) comes from the repository's `tools\package.py`. The build also runs `dicesim test`.
Linux/macOS: `g++ -O2 -std=c++17 -Isrc tools/dicesim.cpp -o dicesim && ./dicesim test && ./dicesim duel 1000`.

Simulated results (1000 games each, `dicesim duel 1000 7`): with six normal dice the engine beats
"keep the most points, bank at 300 / 500 / 1000" bots 63-74% of games; with a lucky set 77-89%.
