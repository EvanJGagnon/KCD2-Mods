# Theatrical AutoFollow

KCSE plugin for Kingdom Come: Deliverance II: Henry can follow any walking NPC with the game's own follow
action: the game's "followable NPC" check answers yes for everyone, as its own (unused, cheat-locked)
`wh_player_FollowAlwaysCanFollow` switch describes (see [NOTES.md](NOTES.md)). The game does the following.

Self-contained: its own copy of `kcd2_common.h` and its own `build.bat`. The release archive is made by the
repository's `tools\package.py`.

```bat
set KC_AUTHOR=Flubbermunchkin
build.bat
out\dev\selftest.exe "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll" out\dev\kcd2_autofollow.dll 129dad1 129db22 129db45 1064609 4aade0 15a7864 15a8337 55dbec0 55dc2a0 15a8361 55dc2a4
out\dev\fulltest.exe "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll" out\dev\kcd2_autofollow.dll
python tools\check_sigs.py "<game>\Bin\Win64MasterMasterSteamPGO\WHGame.dll"
```

User-facing README: [docs/theatrical_autofollow.txt](docs/theatrical_autofollow.txt).
