# KCD2 Mods - Horse Route Follow and the Theatrical mods

Native (C++) mods for **Kingdom Come: Deliverance II**, loaded by the
[Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332).

> [!WARNING]
> **Only download these mods from Nexus Mods (Flubbermunchkin) or this repository.** They never come
> with an installer or `.exe`. Copies of Theatrical AutoForge and Theatrical Autobrew on other sites that
> tell you to "run the installer" are not ours. Don't run them.

| Mod | What it does | Key |
|---|---|---|
| **[Horse Route Follow](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3742)** | Horse auto-follow takes the right turn at every fork to reach your custom map marker. The route is drawn on the map like fast travel, the route is recalculated only if you leave it, and the horse stops on the road closest to the marker. Always on while a marker is placed. | none |
| **Hardcore Map Markers** (optional file of Horse Route Follow) | Allows custom map markers in hardcore mode (map only, not the compass), so Horse Route Follow works there too. | none |
| **[Theatrical AutoForge](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3743)** | Henry forges the chosen piece himself, stroke by stroke: heating, bellows, hammering in rhythm, turning it over halfway (a NoFlip download skips that), reheating and quench - all through the game's own forge actions. | F9 |
| **[Theatrical Autobrew](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3744)** | Henry brews the recipe open in the alchemy book on screen, step by step and in real time. **An adaptation of [Autobrew](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3394) by JerryYOJ** (see below). | F8 |
| **[Theatrical AutoSharpening](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3802)** | Henry sharpens the weapon himself at the grindstone: pedalling, holding the angle and working the edge end to end until it is at 100%, then gets up. Idea by [cpk](https://www.nexusmods.com/profile/cpk). | F7 |
| **Theatrical AutoLockpick** (new) | Henry picks the lock himself with the game's own pick and turn controls: his tremble, pick wear and broken picks stay. | F2 |
| **Theatrical AutoDice** (new) | Henry plays his own turns at dice: he sets aside the dice worth keeping and decides when to pass. The game throws and scores. | F3 |
| **Theatrical AutoMarksman** (new) | While a bow, crossbow or hand cannon is aimed, the camera turns onto the head of the target nearest the centre, allowing for drop and movement; nobody behind cover. You still draw and release. | F4 |
| **Theatrical AutoPickpocket** (new) | Henry lets go of the pickpocket charge at a safe moment and plays the item grid, keeping every risk. | [ |
| **Theatrical AutoFollow** (new) | Henry can follow anyone with the game's own follow action, as if they were a guide. | ] |

![Horse Route Follow - route drawn on the map](media/horse_route_follow_map.jpg)

> Most of the code and text in this project was written with AI assistance (Claude); the mods are tagged accordingly on Nexus.
>
> These are our first mods for this game and have been tested on one setup (Steam, game 1.5.x build 15693).
> Expect issues we have not run into yet - bug reports with the mod's log are very welcome.

## Download and install

Downloads, install instructions and support are on Nexus Mods:
[Horse Route Follow](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3742) -
[Theatrical AutoForge](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3743) -
[Theatrical Autobrew](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3744) -
[Theatrical AutoSharpening](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3802)
Each mod needs:

- [Kingdom Come Script Extender (KCSE)](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3332) by JerryYOJ
- [Address Library For KCSE](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3381) by JerryYOJ (required by KCSE)

The user-facing README of each mod (usage, settings, known issues, changes) is in [`docs/`](docs) and
`theatrical/<mod>/docs/`. Every hotkey is set in the mod's `.ini` and no two mods share one.

## Update safety

The mods do not use fixed memory addresses. At startup they locate the game code they need by byte
signature (rip-relative displacements and branch targets wildcarded, struct offsets kept literal) and
by RTTI class name, and every signature must match exactly once. If anything does not resolve - for
example after a game update - the mod logs why and changes nothing, so the game plays as vanilla
instead of crashing. See [`src/kcd2_common.h`](src/kcd2_common.h).

## Repository layout

```
src/kcd2_common.h    shared runtime: signature scanner, RTTI lookup, fail-safe resolver,
                     .ini settings, logging, KCSE plugin entry points
src/autotravel.cpp   Horse Route Follow    -> kcd2_autotravel.dll
src/hardcore_markers.cpp  Hardcore Map Markers  -> kcd2_hardcore_markers.dll
src/autoforge.cpp    Theatrical AutoForge  -> kcd2_autoforge.dll
src/alchemy.cpp      Theatrical Autobrew   -> kcd2_alchemy.dll
src/autosharpen.cpp  Theatrical AutoSharpening -> kcd2_autosharpen.dll
theatrical/<mod>/    the Theatrical Collection: autolockpick, autodice, automarksman, autopickpocket, autofollow,
                     each with its own src/ (and copy of kcd2_common.h), build.bat, simulator, check_sigs.py,
                     NOTES.md (research) and docs/ (the shipped README)
data/                road graphs (*.amg) and the recipe table, generated from the game's own data
docs/                the README shipped with each mod
tools/
  mksig.py, whimg.py, xref.py    generate update-tolerant signatures from WHGame.dll (capstone, pefile)
  selftest.cpp                   resolve every signature of a mod against a WHGame.dll, offline
  routetest.cpp, sim_route.py    run the horse route planner offline
  forgesim.cpp, sharpsim.cpp     run AutoForge / AutoSharpening against a simulated minigame
  gen_graph.py, ubernav.py       road graphs from the game's ubernav.tmm road data
  gen_recipes.py                 recipe table from the game's AlchemyRecipe.xml
  package.py                     build and audit every release archive (dist\, SHA256SUMS.txt)
```

The data generators read the game files from `KCD2_DIR` (default: the Steam install folder).

## Building

Visual Studio 2022 with the x64 C++ tools.

```bat
set KC_AUTHOR=YourName
build.bat
python tools\package.py
```

- `out\kcd2_*.dll`, `theatrical\<mod>\out\kcd2_*.dll` - release builds
- `out\dev\`, `theatrical\<mod>\out\dev\` - development builds (`/DKC_DEVTOOLS`: self-test exports, horse-mod live reload) plus `selftest.exe`, `routetest.exe` and the simulators
- `dist\` - the mod archives, `SHA256SUMS.txt` and `RELEASE_CONTENTS.txt` (every file of every archive). `package.py`
  also audits them: required files present, manifest and README versions match, no hotkey used twice, `DebugLog=0`,
  and nothing that looks like a game asset or a build leftover. `TheatricalAutoMarksman-ExperimentalCrossbow-*.zip`
  is the optional crossbow test build (detailed crossbow log on) and is uploaded by CI as a separate artifact.

`build.bat` expects Visual Studio 2022 Community; for another edition or the Build Tools, set `KC_VCVARS`
to its `vcvars64.bat` first. Every push is also built on GitHub Actions
([`.github/workflows/build.yml`](.github/workflows/build.yml)): a Windows runner runs `build.bat` and
`tools\package.py`, runs the simulators and attaches the mod archives, the experimental archive and the
development builds to the run.

Self-test against a game DLL:

```bat
out\dev\selftest.exe "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll" out\dev\kcd2_autotravel.dll
```

After a game update, run the self-test against the new `WHGame.dll`; for each signature that no longer
resolves, find the function again (reference addresses for build 15693 are in the source comments) and
regenerate its pattern with `tools/mksig.py`.

## Credits

- **JerryYOJ** - [Autobrew](https://www.nexusmods.com/kingdomcomedeliverance2/mods/3394): Theatrical Autobrew is
  an adaptation of it; its recipe planner and brewing logic (`src/alchemy.cpp`) are ported from Autobrew's
  public source in [libKCD2](https://github.com/JerryYOJ/libKCD2) (Projects/Autobrew, GPL-3.0). Also KCSE, the
  Address Library and libKCD2, whose public research notes and class layouts helped with parts of the other
  mods (the lock, dice and pickpocket layouts, the player's look angles, bow controller and projectiles).
- **inkcrow** - Arrow Arc inspired AutoMarksman's arc prediction (idea only; no code was used).
- **cpk** - suggested AutoSharpening and AutoForge's halfway flip.
- **Warhorse Studios** - Kingdom Come: Deliverance II.

## License

[GNU GPL v3](LICENSE). Theatrical Autobrew contains code ported from Autobrew (GPL-3.0); the whole
repository is released under the same license.
