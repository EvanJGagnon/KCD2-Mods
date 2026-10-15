# Theatrical AutoPickpocket

KCSE plugin for Kingdom Come: Deliverance II. Henry pickpockets with the game's own controls: the mod
releases the charge key when the safety ring reaches a chosen value, then plays the item grid through the
same UI calls the grid keys make. Detection rolls, timers, item time costs and failure are all the game's.
Details and addresses: [NOTES.md](NOTES.md).

Self-contained: its own copy of `kcd2_common.h` (an older one: symbol keys are parsed by this copy) and its
own `build.bat`. The release archive is made by the repository's `tools\package.py`.

```bat
set KC_AUTHOR=Flubbermunchkin
build.bat
out\dev\selftest.exe "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll" out\dev\kcd2_autopickpocket.dll 2eb3b34 203dee0 566bcc 2bb9dac 2bb9e50 2bba2d4 2bbac78 2b8d3f4 3f74dd8 3c33898 3eebcd0
python tools\check_sigs.py "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll"
out\dev\ppsim.exe 2000 1            :: offline grid simulator; exit code 1 if any run got caught
```

The grid logic (`src/pp_logic.h`) has no Windows or game code, so the simulator runs exactly what the mod
runs. On Linux: `g++ -O2 -std=c++17 -Isrc tools/ppsim.cpp -o ppsim && ./ppsim 2000 1`.

User-facing README: [docs/theatrical_autopickpocket.txt](docs/theatrical_autopickpocket.txt).
