# TimeShift Co-op

A co-op mod for **TimeShift** (2007, Saber Interactive / Sierra): play the whole story campaign together, 2 to 4
players over a local network or the Internet, on the game's **own netcode**, plus local **split screen** on one PC,
and the stock multiplayer brought back to life without GameSpy.

**Status: [version 0.1.4](https://github.com/dolbaras/timeshift-coop/releases/latest) is out** — the other players on the HUD
radar (network and split screen), a Ready step before the first chapter of a co-op game, co-op next to other mods' ASI loaders
(Ultimate ASI Loader), and the installer and texts in all nine languages of the game; 0.1.1 added the Steam edition (with your
consent and a community update package), a one-file setup exe and a fuller Continue. Every player needs the same version. ✅
below means checked in the game on test PCs (the whole campaign was run host + client by an automatic bot, 24 of 24 levels);
🟨 is written and checked only partly. Not yet done: a whole campaign played by people,
play over the Internet (LAN, virtual LAN and emulated bad links were tested), sound judged by ear (soldiers' voices and gibbing
sounds are still host-only), a real Steam installation. The full list is in the release notes. Please report what goes wrong
in the [issues](https://github.com/dolbaras/timeshift-coop/issues).

**Install:** download `timeshift-coop-setup-<version>.exe` from the release and run it (one file, nothing to extract; it is
not code-signed, so Windows SmartScreen may warn: "More info" -> "Run anyway"), or download `timeshift-coop-<version>.zip`,
extract it anywhere and run `Install Co-op.bat` (the same installer). Start the game with the "TimeShift Co-op" shortcut (it
updates the mod by itself, from the zip). Details: `README-COOP.txt` in the archive.

[Русская версия ниже](#timeshift-co-op-по-русски)

## What it does

| Area | Status |
|---|---|
| Starts on today's PCs (more than 8 CPU threads, driver/video-memory checks, Intel GPUs) | ✅ |
| Direct3D 9Ex: the picture survives another program's full screen; screen mode (full screen / borderless / window) in the game's Video options | ✅ |
| Stock multiplayer revived: LAN list on every network adapter, Internet servers through OpenSpy instead of the closed GameSpy | ✅ |
| Gamepad (XInput) in game and menus | ✅ |
| Co-op menu in the main menu: host a campaign chapter (difficulty, 2-4 players, LAN/Internet, password), join by LAN list, Internet list or address; version check | ✅ |
| Story on the host, replayed on clients: story zones, checkpoints, level exits, team restart; joining a level already in progress | ✅ |
| Ready step before the first chapter of a co-op game (the game's own READY / NOT READY in the pause menu, nobody moves until all are ready) | ✅ |
| The other players on the HUD radar (blue blips; network and split screen) | ✅ |
| Time powers in co-op: slow and stop for everyone, one power at a time, each player's own energy | ✅ |
| Reverse in co-op (the game's own time recorder on the host; puzzle actors, force fields, container trap) | ✅ |
| Continue: the host's checkpoint of a co-op game is saved per profile with the real loadout (weapons, ammunition, grenades; clients' too), Co-op -> Continue hosts it again | ✅ |
| AI on clients: movement, deaths and corpses, weapon fire | ✅ |
| Campaign levels for several players: lifts and trains wait for everyone, zeppelin turrets and armoured car for clients, AI aims at the nearest player, late joiners get the level's state | 🟨 |
| Local split screen for two players with gamepads (own HUD, weapons, ammo, effects, pick-ups, saves) | ✅ |
| Split screen together with network play (host with split screen and a client works; split screen on a client is basic: pick-ups of its second player work, its time power and kills over the network are not proven) | 🟨 |
| Play over the Internet (not only LAN) | 🟨 not tested |
| Installer: builds the mod's data from the player's own game files, backups, uninstall, a shortcut that updates the mod | ✅ |

## Requirements

- TimeShift **patch 1.2** (the mod is made for the code of the 1.2 `bin\TimeShift.exe`; the community's pre-patched
  build of it is accepted too). **Steam** copies are patch 1.0 (Steam never got the update): the installer warns first and
  needs your explicit consent (a tick; `-AcceptSteamUpdate` in quiet mode), then REPLACES `bin\TimeShift.exe`,
  `bin\steamwars.ini` and the four patch archives with the files of the 1.2 update package for the Steam edition
  (community-made) that **you supply**; it shows the package's exe SHA-256 and the file list first, backs up every
  original in `coop-backup\steam10` and the uninstall restores them exactly. The mod's repository and releases contain
  no game files and no such package, and the mod downloads nothing for this. Tested on a simulated Steam folder and with
  the package's files, not on a real Steam installation. An exe of another version is refused, changing nothing.
- Windows 10/11. No Python or other tools: the installer brings what it needs.
- Languages: the game's nine (English, Russian, German, French, Italian, Spanish, Polish, Czech, Hungarian): the co-op
  menus and hints follow the game's language, the installer, the shortcut's launcher and the setup exe Windows' one.

## For developers

- `native/` — the proxy `dinput8.dll` (C, built with `zig cc`, `native/build.sh`): every engine change is a hook in
  memory. The one change made to a game file on disk is the installer's: it sets the large-address-aware flag in the
  PE header of `bin\TimeShift.exe` (a copy of the original is kept in `coop-backup` and put back on uninstall); on a
  Steam copy it also applies the player's own 1.2 update package, with the same backup.
- `data/` — recipes that edit the player's own archives at install time (`tools/build_pak.py`); this repository holds
  **no game files**.
- `tools/` — installer, launcher, packaging, archive tools.
- `docs/` — how it works: `multiplayer.md` (netcode, co-op session, story sync, joins), `splitscreen.md`,
  `campaign-audit.md` (every level in co-op), `building.md`, `releasing.md`, `data-format.md`, `gamepad.md`.

License: MIT (see `LICENSE`). TimeShift is a trademark of its owners; this project is not affiliated with them.

---

# TimeShift Co-op (по-русски)

Кооперативный мод для **TimeShift** (2007, Saber Interactive / Sierra): вся сюжетная кампания вместе, от 2 до 4 игроков
по локальной сети или через Интернет, на **родном сетевом коде** игры, а также **разделённый экран** на одном ПК и
ожившая стандартная сетевая игра без GameSpy.

**Статус: [версия 0.1.4](https://github.com/dolbaras/timeshift-coop/releases/latest) вышла** — остальные игроки на радаре
(по сети и на разделённом экране), шаг «Готов» перед первой главой кооп-игры, кооп рядом с загрузчиками ASI других модов
(Ultimate ASI Loader), установщик и тексты на всех девяти языках игры; в 0.1.1 появились издание Steam (с вашего согласия и
пакетом обновления от сообщества), установщик одним файлом и более полное «Продолжить». У всех игроков должна быть одна
версия. ✅ ниже — проверено в игре на тестовых ПК (всю кампанию автоматический бот прошёл хостом и клиентом, 24 уровня из 24);
🟨 — написано и проверено частично. Ещё не
сделано: полное прохождение кампании живыми людьми, игра через Интернет (проверены LAN, виртуальная LAN и эмулируемые плохие
каналы), оценка звука на слух (голоса солдат и звуки расчленения пока только у хоста), настоящая установка Steam. Полный
список — в описании релиза. О проблемах пишите в [issues](https://github.com/dolbaras/timeshift-coop/issues).

**Установка:** скачайте `timeshift-coop-setup-<версия>.exe` из релиза и запустите его (один файл, распаковывать не нужно; он
без цифровой подписи, поэтому SmartScreen может предупредить: «Подробнее» -> «Выполнить в любом случае») или скачайте
`timeshift-coop-<версия>.zip`, распакуйте куда угодно и запустите `Install Co-op.bat` (тот же установщик). Запускайте игру
ярлыком «TimeShift Co-op» (он сам обновляет мод, из zip). Подробности — `README-COOP.txt` в архиве.

## Что умеет

| Область | Статус |
|---|---|
| Запуск на современных ПК (больше 8 потоков CPU, проверки драйвера и видеопамяти, видеокарты Intel) | ✅ |
| Direct3D 9Ex: картинка не пропадает, когда другая программа включает полноэкранный режим; режим экрана (полный / без рамки / окно) в стандартных настройках изображения | ✅ |
| Стандартная сетевая игра: список серверов в локальной сети на любом адаптере, интернет-серверы через OpenSpy вместо закрытого GameSpy | ✅ |
| Геймпад (XInput) в игре и меню | ✅ |
| Меню «Кооператив»: создание игры на главе кампании (сложность, 2-4 игрока, LAN/Интернет, пароль), подключение из списка или по адресу; проверка версии | ✅ |
| Сюжет ведёт хост, клиенты повторяют: сюжетные зоны, чекпоинты, переходы, общий рестарт; подключение к уже идущему уровню | ✅ |
| Шаг «Готов» перед первой главой кооп-игры (собственные «ГОТОВ» / «НЕ ГОТОВ» игры в меню паузы, никто не двигается, пока не готовы все) | ✅ |
| Остальные игроки на радаре (синие отметки; по сети и на разделённом экране) | ✅ |
| Способности времени в кооперативе: замедление и остановка для всех, одна способность за раз, у каждого своя энергия | ✅ |
| Перемотка в кооперативе (встроенная запись времени игры на хосте; объекты головоломок, силовые поля, ловушка контейнера) | ✅ |
| Продолжить: контрольная точка хоста кооп-игры сохраняется для профиля с настоящим снаряжением (оружие, патроны, гранаты; у клиентов тоже), «Кооператив» -> «Продолжить» запускает её снова | ✅ |
| ИИ на клиентах: движение, смерти и тела, стрельба | ✅ |
| Уровни кампании для нескольких игроков: лифты и поезда ждут всех, турели дирижабля и броневик для клиентов, ИИ целится в ближайшего игрока, опоздавшие получают состояние уровня | 🟨 |
| Разделённый экран на двоих с геймпадами (свой HUD, оружие, патроны, эффекты, подбор, сохранения) | ✅ |
| Разделённый экран вместе с сетевой игрой (хост с разделённым экраном и клиент работают; у клиента — базовый: подбор предметов его вторым игроком работает, его сила времени и убийства по сети не доказаны) | 🟨 |
| Игра через Интернет (не только LAN) | 🟨 не проверялась |
| Установщик: собирает данные мода из файлов игры игрока, резервные копии, удаление, ярлык с автообновлением | ✅ |

## Требования

- TimeShift **с патчем 1.2** (мод сделан для кода `bin\TimeShift.exe` версии 1.2; готовая сборка сообщества на его
  основе тоже принимается). Копии **Steam** - патч 1.0 (обновления в Steam так и не было): установщик сначала
  предупреждает и требует вашего явного согласия (галочка; в тихом режиме `-AcceptSteamUpdate`), затем ЗАМЕНЯЕТ
  `bin\TimeShift.exe`, `bin\steamwars.ini` и четыре архива патча файлами пакета обновления 1.2 для издания Steam
  (сделан сообществом), который **вы указываете сами**; заранее показывает SHA-256 exe из пакета и список файлов,
  сохраняет каждый оригинал в `coop-backup\steam10`, а удаление возвращает их в точности. В репозитории и
  выпусках мода нет ни файлов игры, ни такого пакета, и мод для этого ничего не скачивает. Проверено на имитации
  папки Steam и на файлах самого пакета, но не на настоящей установке Steam. Exe другой версии отклоняется,
  ничего не меняя.
- Windows 10/11. Python и другие программы не нужны: установщик несёт всё нужное с собой.
- Языки: установщик и запуск по ярлыку следуют языку Windows (английский, русский, немецкий, французский,
  испанский, итальянский); меню игры - на девяти языках игры.

Лицензия: MIT (см. `LICENSE`). TimeShift — товарный знак своих владельцев; проект с ними не связан.
