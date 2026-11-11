# TimeShift Co-op

[![Latest release](https://img.shields.io/github/v/release/dolbaras/timeshift-coop?label=latest%20release)](https://github.com/dolbaras/timeshift-coop/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

Co-op for **TimeShift** (2007, PC; Saber Interactive / Sierra): the whole story campaign for 2-4 players over a local network or the Internet on the game's **own netcode**, two players on one PC with **split screen**, and the stock multiplayer brought back to life without GameSpy.

## Status

**Version 0.1.5** ([release notes](https://github.com/dolbaras/timeshift-coop/releases/latest)): the mod's texts reviewed in all nine languages of the game (the game's own words where it has them, such as "health" instead of "life" and the game's word for the checkpoint; one word for the host and for the mod in each language; one form of address per language: German "Sie", Spanish "tú"; Italian Regroup lines that read "moved by you" fixed); `coop\Collect logs.bat` now speaks the player's language (16 languages: the game's language, else Windows'); the setup exe's error messages are in 16 languages. The rest of the mod is unchanged from 0.1.4 (the other players on the HUD radar, a Ready step before the first chapter, co-op next to other mods' ASI loaders). __LIVE_CHECK__

How it is tested: the whole campaign was run host + client by an automatic bot (24 of 24 levels); each version's changes are checked with a host and a client on test PCs. Nobody outside the lab has played 0.1.5 yet, and a whole campaign played by people has not been done. Every player needs the same version of the mod.

## What works

✅ checked in the game on test PCs · 🟨 partly checked (the row says what is missing) · ⬜ not tested · — not offered

| Area | Status |
|---|---|
| Starts on today's PCs (more than 8 CPU threads, driver / video-memory checks, Intel GPUs) | ✅ |
| Direct3D 9Ex: the picture survives another program's full screen; screen mode (full screen / borderless / window) in the game's Video options | ✅ |
| Stock multiplayer revived: LAN list on every network adapter, Internet servers through OpenSpy instead of the closed GameSpy | ✅ |
| Gamepad (XInput) in game and menus | ✅ |
| Co-op menu in the main menu: host a campaign chapter (difficulty, 2-4 players, LAN / Internet, password), join by LAN list, Internet list or address; version check | ✅ |
| Story on the host, replayed on clients: story zones, checkpoints, level exits, team restart; joining a level already in progress | ✅ |
| Ready step before the first chapter of a co-op game (the game's own READY / NOT READY in the pause menu, nobody moves until all are ready) | ✅ a split-screen player 2 in a network game is not held by it |
| The other players on the HUD radar (blue blips; network and split screen) | ✅ |
| Time powers in co-op: slow and stop for everyone, one power at a time, each player's own energy | ✅ |
| Reverse in co-op (the game's own time recorder on the host; puzzle actors, force fields, container trap) | ✅ |
| Continue: the host's checkpoint of a co-op game is saved per profile with the real loadout; Co-op -> Continue hosts it again | ✅ |
| AI on clients: movement, deaths and corpses, weapon fire | ✅ |
| Campaign levels for several players: lifts and trains wait for everyone, zeppelin turrets and armoured car for clients, AI aims at the nearest player, late joiners get the level's state | 🟨 |
| Local split screen for two players with gamepads (own HUD, weapons, ammo, effects, pick-ups, saves) | ✅ |
| Split screen together with network play | 🟨 host with split screen and a client works; split screen on a client is basic |
| Play over the Internet (not only LAN) | ⬜ LAN, a virtual LAN and emulated bad links were tested |
| Steam edition (patch 1.0) brought to 1.2 with a community package you supply | 🟨 tested on a simulated Steam folder, not on a real Steam installation |
| Installer: builds the mod's data from the player's own game files, backups, uninstall, a shortcut that updates the mod | ✅ |
| Played through by people | ⬜ |

## Download and install

1. Download from [Releases](https://github.com/dolbaras/timeshift-coop/releases/latest): `timeshift-coop-setup-<version>.exe` (one file, nothing to extract) or `timeshift-coop-<version>.zip` (extract it anywhere and run `Install Co-op.bat`; the same installer).
2. The installer finds the game, builds the mod's data from your own game files, keeps a copy of every file it replaces in `coop-backup` and can uninstall. It follows Windows' language (the game's nine languages).
3. Start the game with the "TimeShift Co-op" shortcut: before the game starts it installs a newer release from GitHub by itself (the download is checked against the SHA-256 GitHub gives for it); offline the game starts as it is. The main menu has a "Co-op" entry.

The setup exe is not code-signed, so Windows SmartScreen may warn ("More info" -> "Run anyway"); the zip with `Install Co-op.bat` does the same. Your saves and the Continue record stay when you update. To remove the mod run `Uninstall Co-op.bat` in the game folder: it puts every replaced file back. Details, network ports and the FAQ are in `README-COOP.txt` / `README-COOP-ru.txt` inside the zip (source `docs/release-readme.txt`, `docs/release-readme-ru.txt`).

## Requirements

- TimeShift **patch 1.2** (the mod is made for the code of the 1.2 `bin\TimeShift.exe`; the community's pre-patched build of it is accepted too). An exe of another version is refused, changing nothing.
- **Steam** copies are patch 1.0 (Steam never got the update): the installer warns first and needs your explicit consent (a tick; `-AcceptSteamUpdate` in quiet mode), then REPLACES `bin\TimeShift.exe`, `bin\steamwars.ini` and the four patch archives with the files of the 1.2 update package for the Steam edition (community-made) that **you supply**; it shows the package's exe SHA-256 and the file list first, backs up every original in `coop-backup\steam10`, and the uninstall restores them exactly. The repository and the releases contain no game files and no such package, and the mod downloads nothing for this.
- Windows 10/11. No Python or other tools: the installer brings what it needs.
- Network: the host needs UDP port 7777 reachable (allow TimeShift in Windows Firewall; over the Internet forward UDP 7777 or use a virtual LAN).
- Every player needs the same version of the mod.
- Split screen: a second XInput gamepad.

## How to play

- **Host:** main menu -> Co-op: pick a campaign chapter (any of the 24), the difficulty, the number of players (2-4), LAN or Internet and an optional password. Co-op servers are named `[COOP]<host's name>`.
- **Join:** the LAN list, the Internet list (through OpenSpy, nothing to set up) or Co-op -> Direct connection by IP address.
- **Ready step:** the first chapter of a co-op game waits until every player (the host too) has picked READY in the pause menu.
- **Continue:** Co-op -> Continue hosts the last checkpoint of the last co-op session again.
- **Split screen:** main menu -> Co-op -> SPLIT SCREEN, pick the chapter and the difficulty, Start. In a network game set "Second player on pad 2" to On in the Co-op menu, then host or join as usual.

### Controls

| Key / setting | What it does |
|---|---|
| Esc -> READY / NOT READY | the Ready step before the first chapter |
| Esc -> Regroup | on a client: moves you to the host's player; on the host: brings every living client to the host |
| Gamepads | Xbox-style (XInput) pads in the game and its menus; split screen: player 1 on keyboard and mouse or pad 1, player 2 on pad 2 |
| Video options -> Screen mode | Full screen, Borderless or Window (applies at the next start) |

## Languages

- **In the game:** the game's nine languages: English, Russian, German, French, Italian, Spanish, Polish, Czech and Hungarian. The co-op menus and hints follow the game's language.
- **Installer and the shortcut's launcher:** the same nine languages, following Windows' language.
- **Setup exe** (its few error messages) and **`coop\Collect logs.bat`**: 16 languages (the nine above and Portuguese, Brazilian Portuguese, Dutch, Japanese, Korean, Simplified and Traditional Chinese). The setup exe follows Windows' language; the log collector speaks the game's language (`bin\steamwars.ini`), else Windows'.

## Known limits / not verified

- **Ready step:** a split-screen player 2 in a network game is not held in the ready room and is no player of the ready check; the Tab scoreboard during the ready room was not checked.
- **ASI loaders:** tested with Ultimate ASI Loader 10.0.2 and a test plugin only; a loader renamed to some other DLL name was not tried.
- **Sound was never judged by ear** (all test windows were muted). The soldiers' own voices and the gibbing sounds are still played on the host only.
- **A bad link:** under about 30 % lost packets a join can fail once; press Join again.
- **Split screen:** cutscenes, the pause menu and loading screens are full screen; each half runs at half of the game's frame rate unless `BothViews=1` helps on your card (tried on an Intel card only); split screen on a client is basic.
- **After the host's rewind** the clients take the rewound machinery's state from the host on level 2 (the walkway) only.
- **Internet play** (through OpenSpy and a forwarded port) is untested, and a whole campaign played by people has not been done.
- **Steam:** tested with the real community package on a simulated Steam folder and on a test PC, not on a real Steam installation.
- **The setup exe is not code-signed** (see above).

## Reporting a bug

Open an [issue](https://github.com/dolbaras/timeshift-coop/issues/new/choose) (a form asks for the version, the game edition, what happened and what you did). Double-click `coop\Collect logs.bat` in the game folder right after the problem: it puts one zip on your desktop with the mod's logs, the install record and a system summary, with your user, computer and player names, Steam names and IDs, paths, e-mail, MAC and public IP addresses removed. Nothing is uploaded; open the zip to check it before you attach it. The logs are the mod's `bin\ts_coop.log` and the four runs before it, the installer's and the launcher's logs; its window speaks your language. A zip from the host AND from a client of the same session helps most.

## For developers

- `native/` — the proxy `dinput8.dll` (C, built with `zig cc`, `native/build.sh`): every engine change is a hook in memory. The one change made to a game file on disk is the installer's: it sets the large-address-aware flag in the PE header of `bin\TimeShift.exe` (a copy of the original is kept in `coop-backup` and put back on uninstall); on a Steam copy it also applies the player's own 1.2 update package, with the same backup.
- `data/` — recipes that edit the player's own archives at install time (`tools/build_pak.py`); this repository holds **no game files**.
- `tools/` — installer, launcher, packaging, archive tools (`tools/mkinstaller`: the setup exe); `tests/` — the installer's and the texts' tests; `native/tests/` — the native tests (`run.sh`).
- `docs/` — how it works: `multiplayer.md` (netcode, co-op session, story sync, joins), `splitscreen.md`, `campaign-audit.md` (every level in co-op), `building.md`, `releasing.md`, `data-format.md`, `gamepad.md`, `translation-style.md` (the style each language keeps).

## Credits and third-party

- demon27248 for the community "CPU fix" for TimeShift (the mod does its own start-up fixes in memory and recognises the pre-patched exe).
- ThirteenAG for the Ultimate ASI Loader (tested with it; not shipped).
- The makers of the community 1.2 update package for the Steam edition (neither shipped nor downloaded by the mod).
- OpenSpy (openspy.net) keeps the GameSpy-era games online.
- The installer builds the mod's data with python.org's embeddable CPython 3.12.10 (`coop\python`, Python Software Foundation License, `coop\python\LICENSE.txt`).

## License

MIT (see `LICENSE`). No game files are in this repository or in the releases; you need your own copy of the game. TimeShift is a trademark of its owners; this project is not affiliated with them.

## По-русски

TimeShift Co-op — кооператив для TimeShift (2007, PC): вся сюжетная кампания для 2-4 игроков по локальной сети или через Интернет на родном сетевом коде игры, разделённый экран на одном ПК и ожившая стандартная сетевая игра без GameSpy. Скачайте `timeshift-coop-setup-<версия>.exe` или zip со страницы [выпусков](https://github.com/dolbaras/timeshift-coop/releases/latest) и запустите установщик; дальше запускайте игру ярлыком «TimeShift Co-op»: он сам ставит новые версии. Меню мода и подсказки — на девяти языках игры, установщик и запуск по ярлыку — на тех же девяти языках, по языку Windows; `coop\Collect logs.bat` для отчёта об ошибке говорит на языке игрока. Подробности на русском — в `README-COOP-ru.txt` в архиве и в описании каждого выпуска.
