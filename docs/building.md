# Building the mod's data from the player's game

The mod ships no game files. Its data changes are **recipes** in `data/`: short anchored find/replace
edits of members of the player's own archives, plus files written for the mod. `tools/build_pak.py`
applies them to the player's copy of the game and writes the mod's archives. The installer runs it
on the player's PC with the embeddable Python that comes with the mod (docs/releasing.md); for development it runs on a test
copy.

```
python -I tools/build_pak.py <game folder> -o <out dir> [--recipe FILE ...] [--keep-work DIR]
```

| Output | What it is |
|---|---|
| `coop.s3darc` | patch archive over the game's `patch.s3darc` (data-format.md section 7): the changed and new `ps` members and string tables, every other entry a reference |
| `coop_ati.s3darc`, `coop_nv.s3darc` | directory-only patches over `patch_ati` / `patch_nv` (the engine derives the texture archive name from `arcName`) |
| `coop_build.txt` | version, hashes of `patch.s3darc` and the exe, recipes, every changed or new member with size and CRC |

Installing (the installer's job; never into the owner's game): copy the three archives into
`preload\paks\` and set `[Preload] arcName = coop.s3darc` in `bin\steamwars.ini`, keeping its CRLF line
ends (the game's ini parser skips bare-LF lines). Uninstall: `arcName = patch.s3darc`, delete the three
files. For tests: `lab\pak.ps1`.

## What the build does

1. Opens `<game>\preload\paks\patch.s3darc` (and its base `out.s3darc`) with `tools/s3darc.py`. It fails if
   `patch.s3darc`, `patch_ati`, `patch_nv` or `out.s3darc` is missing (not a 1.2 game folder).
2. Loads the recipes: `data/__init__.py` lists the mod's (`RECIPES`), `--recipe` adds more after them
   (the lab's test automation).
3. Applies them in order to the members they name. **Every anchor must occur exactly once**; otherwise the
   build stops and names the member and the anchor's first line (another game version, or another mod
   changed that text). New members must not exist in the game yet; a new string key must not exist in a
   table yet.
4. Writes the changed members (`ps` texts with CRLF, string tables UTF-16LE with BOM and CRLF) into a work
   folder laid out like `s3darc.py extract` output and packs it with `s3darc.pack_patch`.
5. Reads `coop.s3darc` back and compares every changed member byte for byte.

The data version is `TS_VERSION` from `native/ts.h`: the DLL and the data always carry the same version
(the main menu hands it to the DLL as the UI string `CoopDataVersion`; the co-op menu shows it).

## Recipe format

A recipe is a Python module in `data/` with any of:

```python
EDITS = [("CacheBlock/ps/contwosaves.ui", [(anchor, replacement), ...]), ...]
NEW_FILES = [("CacheBlock/ps/<new name>.ui", "ui/<file in data>"), ...]
def build(ctx): ...        # generated content
```

- Members: `CacheBlock/ps/<file name>` (unnamed members by name too: the key is the CRC-64 of the name,
  e.g. `commsettings_pc.ui`; or the 16-hex-digit key) and `Strings/<table>` (`strings_english`, ...).
- Anchors: the text to find. Runs of blanks match any run of blanks, trailing blanks and CR/LF
  differences are ignored. Keep them short but unique; they are the only game text in the repository.
- Replacements are inserted as written; `\n` becomes CRLF in `ps` members.
- `ctx` (see `tools/build_pak.py`): `ctx.text(member)`, `ctx.edit(member, edits)`, `ctx.add(member, text)`,
  `ctx.set_text(member, text)`, `ctx.data_file(rel)`, `ctx.string_tables()`, `ctx.archive`, `ctx.version`.

## The mod's recipes

| Recipe | Members | Effect |
|---|---|---|
| `coop_rules` | `contwosaves.ui`, `ai_brain.cls` | in a multiplayer session a failed autosave no longer opens the empty "continue without saves" menu (multiplayer.md section 8); `isSyncMP = Yes` on the AI (AI replicated to clients, with `native/aisync.c`) |
| `coop_menu` | `mainmenu.ui`, `multiplayer.ui`, `creategame.ui`, `commsettings_pc.ui`, `servermodifiers.ui`; new `coopmenu.ui`, `coophost.ui` (from `data/ui/`) | the Co-op menu (below) |
| `coop_strings` | every `Strings/strings_<language>` | the menu's texts in all 9 languages of the game (en, ru, de, fr, it, es, pl, cs, hu); the game shows the table of its own language |

What stays stock: the map list (`gs_map_list.cls` is read, not changed: the campaign levels are not
offered in the stock server set-up), the multiplayer rule defaults (`gs_mp_modifier.cls`), the stock
frames' behaviour while no co-op step runs, and the player's own multiplayer settings (set aside and put
back, below).

### The Co-op menu

```
Main menu: SINGLE PLAYER / MULTIPLAYER / CO-OP / OPTIONS / ...
CoopMenu:  HOST GAME / JOIN LAN / JOIN INTERNET / DIRECT CONNECTION / Back      (+ "Co-op mod <version>")
CoopHost:  level list (24 campaign levels, campaign order, the game's level names, chapter picture)
           Difficulty (Casual / Normal / Hard), Max players (2-4), Server type (LAN / Internet), Password
           CREATE
```

- **Host**: `CoopHost` stores the choices in UI strings (`CoopLevel`, `CoopDifficulty`, ...) and the
  profile (`CoopDifficulty`, `CoopMaxPlayers`, `CoopNetwork`: the next visit starts with them), sets the
  game's difficulty (`$ui.SetVariable("Multiplayer", "Difficulty", ...)`, as the single-player New Game
  does), calls `PreCreateGame("Create")` and opens the stock `CreateGame`. The stock frames' natives
  (`AcceptMap`, `SaveValues`, `StartCreating` ...) exist only in frames of their type, so the co-op set-up
  goes through them: while `CoopStage` is `host_create` / `host_comm` / `host_modifiers`, `CreateGame`,
  `CommSettings` and `ServerModifiers` each do their "Next"/"Create" step in their first update (they flash
  by for a frame each), then `WaitCreate` creates the server as usual.
- `CommSettings` first sets the player's own values aside (`CoopKeep = 1`, `CoopKeep_<key>` for
  `ServerMinPlayers`, `ServerMaxPlayers`, `ServerTimeLimit`, `ServerFragLimit`, `ServerMaxSpectators`,
  `ServerName`, `NetworkMode`), fills its controls (server name `[COOP]<PlayerName>`, LAN/Internet,
  password, max players, no spectators, no limits), saves them as its "Next" does, then writes the co-op
  values the controls cannot hold: `ServerMinPlayers = 1`, `ServerTimeLimit = 0`, `ServerFragLimit = 0`,
  `ServerLocation = <level>` (the level the server loads, read by `CreateServer("End")`) and
  `ServerMapId = <the level's singleplayer id>` (the map the session reports).
- The main menu's and the Multiplayer frame's `OnInit` put the kept values back (`CoopKeep = 0`) and clear
  `CoopSession` / `CoopStage`: the stock multiplayer always starts from the player's own settings.
- **Join**: the stock LAN list, Internet list (OpenSpy, `native/netfix.c`) and direct connection. Co-op
  servers are marked by their name (`[COOP]...`) and show the campaign level as their map.
- **In the session**: the DLL (`native/coopsession.c`) knows a co-op session by the level (a network game
  on a `ts2_` level), switches off warm-up, ready system and friendly fire there, sends each joining client
  the host's settings and version, and a client with another version leaves with the message
  `COOP_ERR_VERSION` (main menu, UI string `CoopError`). The stock in-game multiplayer menu and score
  board are used.

UI strings (live while the game runs) shared by the frames, the DLL and the lab recipe:

| UI string | Set by | Meaning |
|---|---|---|
| `CoopSession` | CoopHost (`host`), DLL on a client (`client`), main menu (`""`) | this machine is in a co-op session |
| `CoopLevel`, `CoopMapId`, `CoopDifficulty`, `CoopMaxPlayers`, `CoopNetwork`, `CoopPassword`, `CoopServerName` | CoopHost | the host's choices (the DLL reads difficulty and max players) |
| `CoopStage` | CoopHost, the stock frames | step of the server set-up |
| `CoopAuto` | lab recipe only | `host` / `join`: CoopMenu and CoopHost start that step by themselves |
| `CoopError` | DLL | why the last co-op session ended (`COOP_ERR_VERSION,<host>,<own>`), shown by the main menu |
| `CoopDataVersion` | main menu | the data's version, for the DLL |

## The DLL side (`native/coopsession.c`)

- `ts_coop_active()`: network game (game mode 2/3/4) on a campaign level. Synchronous, the same on every
  machine, no message needed.
- `ts_coop_feature(COOP_F_*)`: the session's settings: the host's (environment overrides for tests), on a
  client the ones the host sent.
- Rules, co-op sessions only: no friendly fire (MP damage factor 0 between different players), no warm-up,
  no ready system (the game option reads see 0). `TS_COOP_FF=1` / `TS_COOP_MPRULES=1` keep the stock rules;
  `TS_COOP=0` switches all of it off.
- Session info and version check: coopnet subtypes 40 (server -> client: flags, difficulty, max players,
  version) and 41 (client -> server: same / other version), see the file's header.
- `TS_TIMESYNC` defaults to `1` (timesync.c acts on campaign levels in network games only, so the stock
  multiplayer is untouched). `TS_STORYSYNC` stays opt-in until storysync.c asks `ts_coop_active()`:
  it acts in every network game, stock maps too.

What timesync.c and storysync.c need to read the session instead of their environment variables (not done
here; those files are being tested by another session):

- timesync.c: install its hooks unless `TS_TIMESYNC=0`, and in `tsync_coop_now()` use
  `ts_coop_feature(COOP_F_TIMESYNC)` (or `any_level` for `TS_TIMESYNC=all`) instead of
  `enabled && game mode && campaign_level()`.
- storysync.c: install unless `TS_STORYSYNC=0`; `net_role()` returns 0 unless
  `ts_coop_feature(COOP_F_STORYSYNC)`. Then coopsession.c sets `COOP_F_STORYSYNC` by default like
  `COOP_F_TIMESYNC`.
- split.c (local split screen, not a network game): friendly fire between the two local players is a
  separate rule (the MP damage factor is not used in single player).

## Test builds

The lab's `autotest_recipe.py` adds the profile keys `AutoTest` / `AutoTestMap` / `AutoTestAddress` on
top of the production recipes (it starts the production co-op set-up through `CoopAuto`, or the stock
Multiplayer set-up for a multiplayer map):

```
python -I mod\tools\build_pak.py work\game -o work\coop_test_pak --recipe lab\autotest_recipe.py
```

It is never part of a release build.
