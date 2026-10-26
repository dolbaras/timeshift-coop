# TimeShift Co-op

A co-op mod for **TimeShift** (2007, Saber Interactive / Sierra): play the whole story campaign together, 2 to 4
players over a local network or the Internet, on the game's **own netcode**, plus local **split screen** on one PC,
and the stock multiplayer brought back to life without GameSpy.

**Status: in development — there is no release yet.** Everything below marked ✅ has been checked in the game on
test PCs; 🟨 is written and partly checked. The first release will be published here once the whole campaign has
been played through in co-op.

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
| Time powers in co-op: slow and stop for everyone, one power at a time, each player's own energy | ✅ |
| Reverse in co-op (the game's own time recorder on the host) | 🟨 |
| AI on clients: movement, deaths and corpses, weapon fire | ✅ |
| Campaign levels for several players: lifts and trains wait for everyone, zeppelin turrets and armoured car for clients, AI aims at the nearest player, late joiners get the level's state | 🟨 |
| Local split screen for two players with gamepads (own HUD, weapons, ammo, effects, pick-ups, saves) | ✅ |
| Split screen together with network play | 🟨 |
| Installer: builds the mod's data from the player's own game files, backups, uninstall, a shortcut that updates the mod | ✅ |

## Requirements

- TimeShift **patch 1.2** (the retail exe the mod is made for). Other exe builds (e.g. Steam) are not supported yet:
  the installer refuses them rather than risk a broken game.
- Windows 10/11. No Python or other tools: the installer brings what it needs.

## For developers

- `native/` — the proxy `dinput8.dll` (C, built with `zig cc`, `native/build.sh`): every engine change is a hook in
  memory; the game files are never patched on disk.
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

**Статус: в разработке — релиза пока нет.** Всё, что отмечено ✅, проверено в игре на тестовых ПК; 🟨 — написано
и проверено частично. Первый релиз появится здесь после прохождения всей кампании в кооперативе.

## Что умеет

| Область | Статус |
|---|---|
| Запуск на современных ПК (больше 8 потоков CPU, проверки драйвера и видеопамяти, видеокарты Intel) | ✅ |
| Direct3D 9Ex: картинка не пропадает, когда другая программа включает полноэкранный режим; режим экрана (полный / без рамки / окно) в стандартных настройках изображения | ✅ |
| Стандартная сетевая игра: список серверов в локальной сети на любом адаптере, интернет-серверы через OpenSpy вместо закрытого GameSpy | ✅ |
| Геймпад (XInput) в игре и меню | ✅ |
| Меню «Кооператив»: создание игры на главе кампании (сложность, 2-4 игрока, LAN/Интернет, пароль), подключение из списка или по адресу; проверка версии | ✅ |
| Сюжет ведёт хост, клиенты повторяют: сюжетные зоны, чекпоинты, переходы, общий рестарт; подключение к уже идущему уровню | ✅ |
| Способности времени в кооперативе: замедление и остановка для всех, одна способность за раз, у каждого своя энергия | ✅ |
| Перемотка в кооперативе (встроенная запись времени игры на хосте) | 🟨 |
| ИИ на клиентах: движение, смерти и тела, стрельба | ✅ |
| Уровни кампании для нескольких игроков: лифты и поезда ждут всех, турели дирижабля и броневик для клиентов, ИИ целится в ближайшего игрока, опоздавшие получают состояние уровня | 🟨 |
| Разделённый экран на двоих с геймпадами (свой HUD, оружие, патроны, эффекты, подбор, сохранения) | ✅ |
| Разделённый экран вместе с сетевой игрой | 🟨 |
| Установщик: собирает данные мода из файлов игры игрока, резервные копии, удаление, ярлык с автообновлением | ✅ |

## Требования

- TimeShift **с патчем 1.2** (розничный exe, под который сделан мод). Другие сборки exe (например, Steam) пока не
  поддерживаются: установщик откажется ставиться, чтобы не сломать игру.
- Windows 10/11. Python и другие программы не нужны: установщик несёт всё нужное с собой.

Лицензия: MIT (см. `LICENSE`). TimeShift — товарный знак своих владельцев; проект с ними не связан.
