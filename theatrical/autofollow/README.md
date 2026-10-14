# Theatrical AutoFollow (in development)

KCSE plugin for Kingdom Come: Deliverance II: Henry can follow any walking NPC with the game's own follow
action. It turns on the game's own `wh_player_FollowAlwaysCanFollow` switch, which is cheat-locked in the
release game (see [NOTES.md](NOTES.md)). Nothing else changes; the game does the following.

Isolated from the published mods in this repository: its own copy of `kcd2_common.h`, build and
packaging. It changes no file outside `theatrical/autofollow/`.

```bat
set KC_AUTHOR=Flubbermunchkin
build.bat
python tools\package.py            :: dist\TheatricalAutoFollow-0.1.0.zip
out\dev\selftest.exe "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll" out\dev\kcd2_autofollow.dll 1064609 4aade0 15a7864 15a8337 55dbec0 55dc2a0 15a8361 55dc2a4
python tools\check_sigs.py "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll"
```

User-facing README: [docs/theatrical_autofollow.txt](docs/theatrical_autofollow.txt).
