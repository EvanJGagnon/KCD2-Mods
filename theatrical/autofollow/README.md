# Theatrical AutoFollow (in development)

KCSE plugin for Kingdom Come: Deliverance II: Henry can follow any walking NPC with the game's own follow
action: the game's "followable NPC" check answers yes for everyone, as its own (unused, cheat-locked)
`wh_player_FollowAlwaysCanFollow` switch describes (see [NOTES.md](NOTES.md)). The game does the following.

Isolated from the published mods in this repository: its own copy of `kcd2_common.h`, build and
packaging. It changes no file outside `theatrical/autofollow/`.

```bat
set KC_AUTHOR=Flubbermunchkin
build.bat
python tools\package.py            :: dist\TheatricalAutoFollow-0.2.0.zip
out\dev\selftest.exe "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll" out\dev\kcd2_autofollow.dll 129dad1 129db22 129db45 1064609 4aade0 15a7864 15a8337 55dbec0 55dc2a0 15a8361 55dc2a4
out\dev\fulltest.exe "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll" out\dev\kcd2_autofollow.dll
python tools\check_sigs.py "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll"
```

User-facing README: [docs/theatrical_autofollow.txt](docs/theatrical_autofollow.txt).
