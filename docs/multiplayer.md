# TimeShift multiplayer: menu flow, automation, network facts

What the game's own multiplayer (patch 1.2, PC) does today, worked out from the menu scripts and from
LAN matches between two PCs (2026-10-25). Everything marked **(observed)** was seen in a test run with the
mod's socket log (`TS_NETLOG=1`, see the end); everything else comes from reading the scripts and the
executable. The GameSpy online service the game was built for is gone; this file says what still works
without it.

## 1. Menu frames and where they live

Menu frames are `.ui` members of the `ps` cache block (data-format.md, section 4): a property tree plus an
`ssl = <<< ... >>>` script. Per platform the engine has variants `<name>_pc.ui`, `<name>_xenon.ui` and
`<name>_ps3.ui` (suffixes in the executable); a `_pc` variant, where it exists, is the one the PC game uses,
otherwise the plain `<name>.ui`. Most `_pc` members are unnamed in the archive and are found by the hash of
the name (`s3darc.py` accepts the name, e.g. `CacheBlock/ps/commsettings_pc.ui`).

| Frame (`name`) | File on PC | Frame `type` (gives its native functions) |
|---|---|---|
| MainMenu | `mainmenu.ui` | (none) |
| MultiPlayer | `multiplayer.ui` | `PreCreateGame` |
| CreateGame | `creategame.ui` | `CreateGame` |
| CommSettings | `commsettings_pc.ui` | `CommSettings` |
| ServerModifiers | `servermodifiers.ui` | `ServerModifiers` |
| WaitCreate | `waitcreate.ui` | `CreateGame` |
| AddressInputFrame | `addressinputframe.ui` | `JoinServer` |
| WaitJoin | `waitjoin.ui` | `JoinServer` |
| JoinGameLAN | `joingamelan.ui` | `JoinServer` |

Functions such as `CreateServer`, `JoinGame`, `PreCreateGame`, `AcceptMap` are natives of the frame
*type*: a script can call them only inside a frame of that type (they are not methods of `$ui`).

## 2. Hosting a LAN server (menu flow)

1. **MultiPlayer**, button "LAN server" (`CreateGame`): `PreCreateGame("Create")`, then
   `$ui.SwitchFrame("CreateGame")`.
2. **CreateGame**: three `select` controls with `section = Multiplayer`: `CreateGameType` (game mode),
   `ServerLocation` (map or "Map List"), `CreateModifier` (preset). `InitVal`, `LoadModifiers`, `LoadMaps`
   fill them from the profile. "Next": `AcceptVal()` (false when no map is selected), `AcceptMap()`,
   `AcceptModifier()`, `AcceptValues()`, then `CommSettings`.
3. **CommSettings**: server name (must not be empty), `NetworkMode` (`0` = LAN, `1` = Internet),
   `DedicatedMode` (`0`/`1`), min/max players, time and frag limits, spectators, password.
   `SaveValues()` writes them to the profile (`$ui.FlushCfg()`), then `ServerModifiers`.
4. **ServerModifiers**, button "Create": `AcceptVal(true)` and `$ui.ShowFrame("WaitCreate")`.
5. **WaitCreate**: `CreateServer("Begin")` in `OnInit`, then every frame `CreateServer("Wait")` until it
   returns true, then `CreateServer("End")`; "Cancel" calls `CreateServer("Stop")`. On an error the frame
   closes with `$ui.GetLastError()` (a string id such as `UI_NET_ERR_UNABLE_TO_START_SERVER`) and
   ServerModifiers shows it.

`CreateServer(cmd)` takes only the step name. Everything about the server (mode, map, limits, LAN or
Internet) comes from the values the earlier frames accepted and saved in the profile. The server then
loads the map and the host plays in it. With a fresh profile the map select starts on the first map
(level file `multiplayer2.lg`, "mp1" on its loading screen) **(observed)**.

Profile keys (`UserConfigs\<profile>\user.ini`, written by the frames): `ServerName`, `NetworkMode`
(`no` = LAN, `yes` = Internet), `DedicatedMode`, `ServerMaxPlayers`, `ServerMinPlayers`, `ServerTimeLimit`,
`ServerFragLimit`, `ServerMaxSpectators`, `GameType`, `PlayerName`, `PlayerGUID`. `DedicatedMode` is only
an option of this flow: nothing at start-up reads it, so it does not start a server by itself. (The
executable also has the strings `Mode, ServerPort, ServerName, NetworkMode, DedicatedMode,
ServerFragLimit, ServerPassword, File, ServerIP` together, possibly a dedicated-server configuration; not
examined.)

## 3. Joining

- **By address** ("Direct connect"): MultiPlayer shows `AddressInputFrame`; its "Yes" button accepts the
  `DirectAddress` input and opens `WaitJoin` with the frame strings `DirectConnect = "Yes"`,
  `serverid = <typed address>`, `Password`. WaitJoin calls `JoinGame("BeginDirect", serverid, password)`
  once, then `JoinGame("Wait", ...)` every frame; on success `JoinGame("End", serverid)` and the client
  loads the server's map. Errors close the frame with `GetLastError()`.
- **LAN list** (`JoinGameLAN`, `joinType = "Local"`): `FillServerList` starts a LAN query; a selected entry
  goes through `TryJoin` and the `JoinInfo` frame.
- **Internet list** (`JoinGame`) and quick match use the GameSpy master server (gone).

## 3a. The mod's Co-op menu

The mod's data (built from the player's own archives by `tools/build_pak.py`, see `building.md`) adds a
"Co-op" entry to the main menu: host a campaign level (level, difficulty, 2-4 players, LAN or Internet,
password) or join through the stock LAN list, Internet list or direct connection. The host's choices go
through the stock server set-up of sections 2 and 8 (CreateGame, CommSettings and ServerModifiers do their
step by themselves while the UI string `CoopStage` says so); CommSettings then points the server at the
campaign level (`ServerLocation`, `ServerMapId`) with co-op limits and the player's own multiplayer
settings are put back at the main menu. The stock map list stays as shipped. In the session the DLL
(`native/coopsession.c`) switches off warm-up, ready system and friendly fire on campaign levels only and
checks the mod version of every joining client (section 9c). Not yet run in the game.

## 4. Automation for tests (no keyboard or mouse)

Since the Co-op menu (section 3a) the automation is the lab recipe `lab/autotest_recipe.py` for
`build_pak.py` (same profile keys; a campaign `AutoTestMap` starts the Co-op menu's host). The frames
described below were the first version (`lab/autotest_mod.py`).

A test build changes six frames so that a game started with one profile key hosts or joins by itself
(each step does exactly what its button would do). Without the key every frame behaves as before.

| Profile key | Effect |
|---|---|
| `AutoTest = "host"` | MainMenu -> MultiPlayer -> `PreCreateGame("Create")` -> CreateGame (Next) -> CommSettings (`SaveValues`) -> ServerModifiers (Create) -> WaitCreate |
| `AutoTest = "join"` | MainMenu -> MultiPlayer -> AddressInputFrame -> WaitJoin with `AutoTestAddress` |
| `AutoTestAddress = "192.168.0.10"` | (join) the server address, as typed into "Direct connect" |
| `AutoTestMap = "<value>"` | (host, optional) value for the CreateGame `ServerLocation` select; with the co-op test patch also a campaign level and co-op server limits (section 8) |

How it is built:

- Scripts read any profile key with `$ui.GetUserVar("<key>")`; a missing key gives `null` (assigned to a
  `string` variable it reads `"null"`, compared directly with a string it is a script error).
  `$ui.GetVariable(section, key, type)` did not return these keys.
- MainMenu's `OnInit` stores the key once per game start in `$ui.SetStr("AutoTest", ...)` (UI strings live
  as long as the game runs), so returning to the main menu later does not start again. Each frame's
  `override OnUpdate` advances the stage (`host` -> `host_mp` -> `host_create` -> `host_comm` ->
  `host_modifiers` -> `host_create_wait`; `join` -> `join_mp` -> `join_address` -> `join_wait`) and calls
  the frame's own function for the button. `OnUpdate` runs in frames without a type (MainMenu) too.
- The changed frames go into a patch archive over `patch.s3darc` (data-format.md, section 7).
- The profile parser skips lines that end in a bare LF (the key keeps its default): test scripts that
  edit `user.ini` must write CRLF lines.

Measured (two PCs on one LAN, host on a 4-core N100, client on a desktop PC) **(observed)**:

| Step | Time |
|---|---|
| host: process start -> main menu (`menu.lg`) | 24 s |
| host: main menu -> server socket open | 0.2 s |
| host: main menu -> in the map | 6-50 s (shader compilation on the first load) |
| client: process start -> main menu | 25-40 s |
| client: main menu -> connected (handshake done) | 0.05-1.1 s |
| client: connected -> in the map | 5-10 s |

In the map both players are first in warm-up ("press READY"); alone, the host sees "one more player is
needed". The match starts when every player is ready (`$mp_rules_sys.Ready()`, the in-game "Ready"
button), with a short "game starting" countdown **(observed)**.

## 5. Network facts **(observed)**

- **Transport**: UDP only, one socket bound to `0.0.0.0:7777` on the server *and* on the client (the client
  sends from local port 7777 too). No TCP during LAN or direct play.
- **Handshake**: GameSpy Transport 2 style datagrams starting with `fe fe` and a type byte: client `01`
  (39 bytes), server `02` (71 bytes), client `03` (70 bytes), server `04` (7 bytes) = accepted, or `05`
  followed by a reason string = rejected (seen: `UI_NET_ERR_DUPLICATE_PROFILE`), client `68` = close. Game
  data follows in other datagrams (tens of packets per second each way).
- **Duplicate profile**: two copies of the same profile (same `PlayerGUID` and `PlayerName`) cannot play
  together; the server rejects the second with `UI_NET_ERR_DUPLICATE_PROFILE`. Giving the client its own
  `PlayerGUID` (and name) fixed it.
- **Rejoining** works: a client that left (process killed) could join the same server again; the server
  kept running alone.
- **LAN list query**: the browser opens new sockets (UDP plus an ICMP socket, probably for ping), enables
  `SO_BROADCAST` and sends the 8-byte query `fe fd 02 xx xx xx xx 00` to `255.255.255.255` on every port
  from 7777 to 7877. A limited broadcast leaves through one network adapter only (the route with the lowest
  metric): on a PC with a VPN adapter the query never reached the LAN and the list stayed empty. The answer
  format was not captured yet.
- **Firewall**: the server needs inbound UDP 7777 (the LAN list also needs the browser's replies to come
  back). A client needs no inbound rule for direct connect.
- **Name lookups in LAN mode**: only the PC's own host name. No GameSpy host is contacted when hosting with
  `NetworkMode = no`, joining by address, or browsing the LAN list.

## 6. GameSpy today

| Situation | GameSpy calls | Result |
|---|---|---|
| LAN server (`NetworkMode = no`) | none | works |
| Direct connect by address | none | works |
| LAN list | none (broadcast only) | works only where the broadcast reaches the LAN (see above) |
| Internet server (`NetworkMode = yes`) | DNS lookups of `timeshift.available.gamespy.com`, `timeshift.master.gamespy.com`, `motd.gamespy.com` (all fail) | "Unable to start server"; no server |
| Internet list, quick match | master server (`%s.master.gamespy.com`, `%s.ms%d.gamespy.com`) | not tested; cannot work without a replacement |

The executable also contains `natneg1..3.gamespy.com` (NAT negotiation), the MOTD/version-check URLs and
the CD-key code of the GameSpy SDK ("Received cdkey query but not using qr2-cdkey integration"); none of
them was used in LAN or direct play.

### What the mod does (`native/netfix.c`, checked on two PCs 2026-10-25)

- **LAN list:** every query broadcast to 255.255.255.255 is repeated to the directed broadcast address of
  each IPv4 network of the PC. With a VPN adapter on the querying PC the list was empty; with the fix the
  other PC's server shows (name, map, mode, players).
- **Internet:** every name under `gamespy.com` is looked up under `openspy.net` (the community's GameSpy
  replacement; `TS_MASTER=<domain>` or `off`). Hosting with `NetworkMode = yes` now works: the game passes
  the availability check (`timeshift.available`, UDP 27900), registers with the master server (UDP 27900
  heartbeat and challenge, then UDP 29910), and the server shows in another PC's Internet list (TCP 28910
  to `timeshift.ms<N>`, then the list pings the server's public address). Joining over the Internet was
  not tested yet (both test PCs share one router; a host needs UDP 7777 reachable from outside).

| Situation | Before | With the mod |
|---|---|---|
| LAN server, direct connect | works | works |
| LAN list | empty behind a VPN adapter | lists the LAN's servers |
| Internet server | "Unable to start server" | registers on OpenSpy |
| Internet list | could not work | lists OpenSpy servers |

## 6a. The game's own windowed mode

A windowed game whose Direct3D device is lost never resets it: `TestCooperativeLevel` keeps returning
`D3DERR_DEVICELOST`, the game runs on and draws nothing. On the test PC this happened at the first level load
in some runs and not in others; the losses lined up with another program entering or leaving exclusive full
screen on the same PC (Intel UHD 630, 2026-10-25), not with `VIDEO_FullScr` as first thought. Players who
alt-tab from another full-screen game would hit it too. The game has no reset path at all (its present step
0x79bf50 only skips Present while the device is lost), so the mod runs it on Direct3D 9Ex instead
(native/d3dex.c, on by default): a windowed 9Ex device is not lost. Checked 2026-10-25 with lab/fsgrab, which
holds exclusive full screen on the test monitor for 3 s: plain D3D9 went black for good, 9Ex kept drawing.

## 7. Test tools in the mod's DLL

| Environment | Effect |
|---|---|
| `TS_NETLOG=1` | socket calls in the log: socket, bind, connect, listen, accept, setsockopt, close, name lookups, and the first datagrams per socket and peer (then the 10th, 100th, ...) with their first bytes |
| `TS_SHOTDIR=<dir>`, `TS_SHOTS=s1,s2,...` | rendered frames saved by the DLL as `<dir>\shotNNN.bmp` at those seconds, and whenever a file `<dir>\shot.req` appears (window captures of the game return blank or stale pictures) |
| `TS_CONSOLE=<port>` | the script console: e.g. `$mp_rules_sys.Ready()`, `$mp_rules_sys.Spectate()` (the host then follows the other players' views), `return $ui.GetGameInfo("IsServer")` |

## 8. Campaign levels on a server

The game's own LAN server runs a campaign level (first step of a campaign co-op that reuses the native
netcode). Checked on the host only (one PC, 2026-10-25, level `ts2_city_part02`); clients are not tested
yet.

### How the server picks its level

- `CreateServer("End")` reads the profile key `ServerLocation` (default `multiplayer1.lg`) and loads that
  level as the server's map; joining clients are told the same file name. The name is not checked against
  the map list: any level file loads this way.
- `ServerLocation` is written by the CreateGame frame: `AcceptMap()` looks up the id of the map selected in
  the `ServerLocation` select in `gs_map_list.cls` and stores that map's name. The select (filled by
  `LoadMaps()`) only offers the entries of the `multiplayer` list. Writing `ServerLocation` into the profile
  by hand is therefore not enough when the menu is used: the select overwrites it.
- So a data patch adds the campaign levels to the `multiplayer` list of `gs_map_list.cls` (a `.cls` member
  of the `ps` block, patched like the menu frames, data-format.md section 7). Map ids are looked up in one
  table for all maps (menu, multiplayer, singleplayer), so an added entry must not take a multiplayer map's
  id: it reuses the id of the level's own `singleplayer` entry (`ts2_city_part01` = 30 ... `ts2_cityfinal_part01`
  = 53), which points at the same level name.
- `$ui.LoadLevel(...)` is not a way to change the server's level: it is the single-player path and ends the
  session (clients see "server shut down").

### Steps and keys

Test build (lab tool `coopexp_mod.py`, packed over `patch.s3darc` like the automation patch): the six
automation frames of section 4, the 24 campaign levels as entries "Co-op: <level>" in the multiplayer list,
the co-op rule defaults below and the `ContWOSaves` change (see "Saves and checkpoints"); `--sync-ai` also
sets `isSyncMP = Yes` in `ai_brain.cls`. Profile keys for a host that starts by itself:

```
AutoTest    = "host"
AutoTestMap = "ts2_city_part02"
```

The CreateGame step selects the map, CommSettings saves the server settings and, because `AutoTestMap` is
set, also writes `ServerMinPlayers = 1`, `ServerTimeLimit = 0`, `ServerFragLimit = 0` into the profile
(`$ui.SetUserVar`), then the server is created as usual. By hand with the same patch: Multiplayer -> LAN
server -> map "Co-op: ts2_city_part02" -> settings -> Create (the menu's minimum-players control cannot go
below 2, see the rules below).

Measured on the 4-core test PC: main menu 24-28 s after the process start, the campaign level started 7-10 s
later (35-38 s after the start).

### What runs **(observed)**

| Check | Result |
|---|---|
| Server mode | `return $ui.GetGameInfo("IsServer")` -> `Yes`, `IsMultiplayer` -> `Yes`; the engine's game mode is "server" (2), level `ts2_city_part02.lg`, the MP-session flag is set |
| Local player | the campaign class `player` (`$player`), with the MP HUD (score, match clock, radar) and the time-power HUD |
| Spawn | multiplayer spawn points are level objects whose name starts with `start_pos`; this level has one, so there is one spawn point, at the single-player start (same x/z as in a single-player load; the server placed the player about 4 m higher, on the structure above it). Respawns use the same point |
| AI | present and active: soldiers attack, the idle host was shot dead twice within four minutes |
| Mission scripts | run on the server: SAM messages, tutorial hints ("press F to use the chronoshift"), objectives; the level's own behaviour scripts log the same errors at the same moment as in a single-player load of the level (`$rebel_city2_play_start_rebel2_v2_CR.OnBhvEnd`, `Time_to_Fall_v2` missing: a level bug, not a server effect) |
| Death | multiplayer rules: red screen, "respawn in N s", click to respawn now; no checkpoint menu |
| Saving | the level's autosaves fail on the server (see "Saves and checkpoints"); with one byte of the executable changed they work |

### Multiplayer rules and how to switch them off

With the shipped settings the host stays in warm-up: "one more player is needed to start" (profile
`ServerMinPlayers = 2`); with one player allowed, a 2-minute warm-up with "select READY to end the warm-up"
follows (game options `bWarmUp`, `bReadySystem`, both on by default), then the round runs with the profile's
time and score limits (at the time limit the server changes the map).

| Rule | Where it comes from | Off for co-op |
|---|---|---|
| Minimum players | profile `ServerMinPlayers`, read again at every check | `1` (the CommSettings control has `minVal = 2`, so only the profile or `$ui.SetUserVar("ServerMinPlayers", 1)`) |
| Time limit, score limit | profile `ServerTimeLimit` (minutes), `ServerFragLimit` | `0` = none (the clock then counts up) |
| Warm-up, ready system | game options `bWarmUp`, `WarmUpTime`, `bReadySystem`; their defaults are `defVal` in `gs_mp_modifier.cls` (class `Params`, group `UI_MISC_GROUP`) and a game type in its `Modifiers` list can override them | data patch `defVal = "No"`; at run time `$mp_rules_sys.Ready()` ends the warm-up for that player |
| Friendly fire | game option `bFriendlyFire` (same file, `UI_WEAPON_GROUP`) | data patch `defVal = "No"` (no effect seen yet: needs a second player) |

Profile values changed at run time with `$ui.SetUserVar` take effect at once (the rules read them on every
check): setting `ServerMinPlayers` to 1 in a waiting server started the warm-up, `Ready()` then started the
match ("Game started") without reloading the level. With the patched defaults and the profile values above
the match runs from the moment the level is loaded. `$ui.GetVariable(...)` does not read these values (it
returned 0 or `multiplayer1`); `$ui.GetUserVar(key)` reads profile keys.

### The empty menu after the level start: a failed autosave

About 18 s after the level start (when the start camera hands over to the player) an empty menu opened on the
host: the "Ver 1.02" menu background, one highlighted bar, the mouse cursor, no text. It does not open on
multiplayer maps. A trace build (every menu frame of the `ps` block records its `OnInit`/`OnUpdate` in UI
strings; lab tool `frametrace_mod.py`) named the frames: `Startup_Background` and **`ContWOSaves`** ("continue
without saves?"). The engine opens `ContWOSaves` when a save fails: single player saves the game at that
moment ("Saving game..."), and on the server that save fails (`$ui.GetStr("saveSuccess")` = `No`,
`saveReason` = `NoReason`). The frame's own error text never showed, so the menu looked empty.

Fix in the test build: `ContWOSaves` checks `$ui.GetGameInfo("IsMultiplayer")` first and, in a multiplayer
session, takes the "continue without saves" answer at once (`$saves_mng.ResetDevice()`,
`$ui.SetVariable("CONFIG", "WorkWOSaves", "bool", "Yes")`, `$ui.CloseMenu()`). Checked: no menu, the game
goes on **(observed)**. This also covers clients, which run the same level scripts. Without that answer the
save system stays waiting: later `AutoSave`/`SaveGame` calls did nothing.

### Saves and checkpoints on the server

How a save runs (static, addresses of patch 1.2):

1. `$saves_mng.AutoSave(id)` (native `0x6ce030`; level scripts call it in checkpoint domains, ids 2-8 in
   `ts2_city_part02`) marks a save as pending when `CanSave` holds (`0x6d09d0`: no global time stop/slow, no
   save or load in progress, and the player's own check).
2. The save manager's update (`0x6cfab0`) runs it (`0x6ced90`: "Saving game..." message) and queues a save
   job; the job is sent as engine message `0x3ee` to the level loader (`0x734bf0`).
3. The level loader's handler (`0x6e3960`, case `0x3ee`) calls `0x6e3510`, which **returns failure unless
   the game mode is 1 (single player)**:
   `0x6e3516 cmp dword [eax+0x3c], 1 / 0x6e351a je 0x6e3520` (otherwise `xor eax, eax`).
4. The result handler (`0x6cf330`) sets `saveSuccess`/`saveReason` and, on failure, the update opens
   `ContWOSaves`.

Console test on the host with that one byte changed in memory (`je` -> `jmp`, `74` -> `eb` at `0x6e351a`)
before the level start **(observed)**: the level-start autosave succeeded (`saveSuccess` = `Yes`, a 1.1 MB
`save_DDHHMMSS.tss` in the profile folder, no menu), and a checkpoint domain reached later saved again
("Game saved"). So a host can save at the campaign checkpoints with that patch (a job for the mod's DLL).

Loading is the open part: `$saves_mng.LoadGame(<save>)` on the server reloaded the level in **single-player
mode** (game mode 1, single-player death screen), i.e. it ends the server session like `$ui.LoadLevel`. A
checkpoint restart in co-op needs its own path: load the save on the host while keeping the server mode (or
restart the server on the level and restore the checkpoint state), then bring the clients back in. What a save
written in server mode contains (remote players, MP state) was not examined.

### Factions

Teams are class data (`idTeam`, read by `0x667b30` into the class descriptor `+0xac`): `plr_comm.cls` sets
`idTeam = PLAYERS`, and every player class inherits it (`player`, `plr_model`, `player_ghost`, `player_mp`);
`rebel_new_base.cls` and `main_char.cls` set `ALLY`, `soldier_new_base.cls` sets `OPP`; `NEUTRAL` exists too.
So the server's own `player` and, by the same data, remote `player_mp` are on the players' side, like in
the campaign. **Observed** on the host: near rebels created with `$farm.Create("ai_trench_rebel_1")` and
`$player.Transport("ai_trench_rebel_1")`, the radar showed the rebels as friendly (green) and the soldiers as
enemies (red); soldiers shot the player as in single player. Not checked: whether the multiplayer rules
change anything for remote players (no code that rewrites a player's team was looked for), and how AI picks
targets among several players.

### AI replication data switch on the host

With `--sync-ai` (`isSyncMP = Yes` in `ai_brain.cls`) the host behaved the same **(observed)**: soldiers
present and attacking, the same script errors at the same moments as without it, no new errors. What clients
receive is the next test (section 8a).

## 8a. Co-op on a campaign level: what syncs

What the game's own netcode already carries when a client joins a server that runs a campaign level, measured
2026-10-25 **(observed)** with the section 8 test build, level `ts2_city_part02`, both instances on one test PC
(the server, and a second copy that joins it by address with shifted ports). Six sessions: four with
`isSyncMP = Yes` on `ai_brain.cls` (`--sync-ai`), one with the shipped AI data, plus one first exploratory run.
Players and AI were moved and turned with the test console (`$player.Transport(<object>)`,
`$player.DoRotateView(<object>, time)`), positions read from memory, and the rendered frames of both instances
taken at the same moment.

Where the numbers come from: the local player's position is at `[0x1095b30] + 0xb0` (3 floats); on the
server, the client slot table `[0xfcab34] + 0x28` (stride `0xa4`, `+4` = the player entity of that slot; slot 0 =
the server's own player, slot 1 = the first client) gives the remote `player_mp` and its position at `+0xb0`; on
a client, slot 0 is the server and its `+4` is the host's `player_mp`. An AI's position on each machine was taken
by moving that machine's player onto it (`Transport("<ai name>")`) and reading the player's position.

### Summary

| Feature | Syncs? | Evidence | What the mod must add |
|---|---|---|---|
| Remote player body and position | **Yes, both ways** | Client moved 50 m by `Transport`: its `player_mp` on the server followed within 0.03 m; the same for the host as seen on the client. Frames: each sees the other's white suit standing where expected; the host shows with a red name tag ("Default", its `PlayerName`). | Nothing for movement. A way for scripts to address remote players (below). |
| Naming / listing remote players in script | **No** | `player_mp` objects have an empty name (`string(p)` = `""`); `foreach p : @player_mp`, `@player_ghost`, `@plr_model` iterate *every* object of the level (these are not script classes, the class filter is ignored); `@player` lists only the local `player`. A domain's `IsBelong()` does see the remote body (listed as `""` plus its `wpn_mp_pistol`). | Native: name the remote players (e.g. `player_mp<slot>`) or a native that returns them, so C3/C10 of `player-audit.md` can be written. |
| AI movement and animation | **No** (with or without `isSyncMP` on `ai_brain`); **yes with the mod's `aisync.c` and `isSyncMP`** (see "AI on clients") | Same position and view direction on both machines: on the server the soldiers fight and fall, on the client the same soldiers stand frozen in their first pose for minutes; positions differ by 3-15 m (e.g. police3: server 196-207, client stays at 191-194). The shipped data and `--sync-ai` gave the same frozen picture. | Find why the AI regular update does not move client proxies (the AI classes have the update code, `netcode.md` section 4); else mirror AI transforms/animations over mod messages. |
| AI death | **Only with `isSyncMP`**, and it **crashes the client** | With `--sync-ai`, `Die()` on the server made `IsLive()` false on the client too (the figure kept standing); without it the client still said `true`. With `--sync-ai` and no fix the client crashed 1-4 minutes after joining, when AI died on the server: `AccessViolation at 0x8496D7 (read 0)` (2 of 3 unpatched sessions; the first, exploratory one ran 8 minutes without). | Native fix (below), then death animation/ragdoll on the client (the proxy stays standing). AIs that died before the client joined still count as alive there (`slo_*`): the join state must carry deaths. |
| Client shooting AI | **Yes** (damage reaches the server) | The client fired (input written in memory, ammo 15 -> 4 / 112 -> 96) at a soldier it saw (the frozen proxy); the soldier, alive on the server just before, was dead there right after, with no other shooter left (rebels removed by script, host far away with `SetGOD`). Twice, once with and once without `isSyncMP`. Without it the client did not learn of the death. | Make the client aim at where the AI really is (done: AI movement sync, a client burst killed a moving soldier); show the death on the client (open). |
| AI shooting the client | **Not seen** (frozen AI); with AI replication on: yes, the client was shot dead several times | ~1 minute next to live hostile soldiers (server side 8 m away, host elsewhere): client health stayed full; one earlier session showed the red damage tint once while soldiers fought rebels. AI target natives take names and remote players have none (`SetTarget("player")` = the host, audit D7). | Check AI target selection over `player_mp` (C10 of the audit); probably native. |
| Pickups / dropped weapons | **Yes** | Weapons dropped by AI killed on the server appeared on the client's ground (`isSyncMP` items). | - |
| Time powers | **No** (each machine only) | Client `StartTControl("tslow", true)`: the client's flags `[0xfcace0]` = 0x41 and world scale 0.2 with the blue effect on its screen; the server stayed 0x40 / 1.0, no effect. Host `tslow`/`tstop`: server 0x41 / 0.2 and its grey screen; the client stayed 0x40 / 1.0, no effect. Host-side slow-down of AI cannot be seen on the client while AI do not move there. | Mod message: a client asks the host to run the power, the host broadcasts flags/effect to every client. Built: section 9a (not yet run). |
| Level triggers (audit D1) | **Local only** | Client alone entered `dom_farm_first_room` (its `OnEnter` creates `ai_house_first_room_2`): the client ran it (the soldier existed on the client), the server did not (`unknown object`), although `IsBelong` on the server saw the client's body inside. Then the host entered: the server created the soldier and the client got a second, unnamed replica next to its own local copy (`object ... already exist, new one added as unnamed`). | C1 (host-only story handlers on clients: client-run `$farm.Create` makes local, non-networked duplicates) and C4 (a client's entry forwarded to the host) of `player-audit.md`, both native. |
| Level scripts on the client (audit D2) | **Yes, they run** | The client's log shows the same domain handlers and their errors as the server's (`dom_BRIDGE.OnEnter`, `dom_kill_mech_and_street_guys.OnEnter`, ...). | C1. |
| Mission failure (`$player.FailMission`) | MP death, session stays | On the client: red death screen, kill feed "Client2", score -1, respawn in 3 s; on the host the same for "Default"; both still connected, server mode unchanged. No mission-failed menu. | C6/C7 (team restart) of the audit. |
| Level exit (audit D5) | not reached | `dom_reload` is locked; unlocking it on the client and moving to the object inside it left the player just outside the domain. | Next test. |
| Respawn | MP rules | After a death both respawn at the single `start_pos` (+4 m) with the MP pistol. | C6. |

### The client crash with replicated AI

Stack (client): net handler `0x6fba90` -> dispatch `0x946ec4` -> AI message handler `0x83a510` (case `0x439`, NOTIFY)
-> AI NOTIFY receiver `0x859090` -> state change `[ai+0x290]->vtbl[1](state, 0)` at `0x8590d9` -> death state
handler `0x8496c0`, whose first statement reads its third argument (hit info), NULL on a client:
`0x8496cb mov edi,[esp+0x2c]` / `0x8496d7 mov eax,[edi]`.

Experiment (lab only, the running client patched in memory): the 23 bytes at `0x8590d9` rewritten to pass a pointer
to a zeroed 4 KB block instead of NULL (`8b 89 90 02 00 00 8b 40 08 8b 11 68 <block> 50 ff 52 04 c2 04 00`). With it
the client survived several AI deaths (by script and by its own shots) for the rest of two sessions (8-9 minutes each). The mod needs the same in its
DLL (or a NULL check at `0x8496c0`) before AI replication can be switched on. Without `isSyncMP` no crash happened
(one session with AI deaths).

### AI on clients (`native/aisync.c`)

With `isSyncMP = Yes` on `ai_brain.cls` the server gives every soldier a net id and the client matches its own
copy of the level's soldier to it, but the soldiers still stood in their first pose on the client. The AI classes
contain finished-looking network code (a state write/read pair on top of the base entity's, `netcode.md` section 4);
three gaps kept it from running, all found in the executable and confirmed with counters in the mod's log:

1. **Nothing starts an AI's network updates.** Replicated objects call their "net start" virtual (entity vtable
   `+0x44`), which creates a *motion component* for the transform (a writer on the server, an interpolating
   reader on a client) and sets the entity's role bits for "in the regular updates" (`0x10`) and "has motion data"
   (`0x40`). Players, items, doors and physics objects do; no AI code does. On the server the AI write was called
   0 times and the soldiers' role was `0x406`. The mod calls the **item classes' net start** (`0x4f6b60`, kind 2,
   motion on) for every AI that has a server net id, on both machines: the server then sends position and
   rotation of each soldier, and the client's reader interpolates and places the body every frame. The
   component's transform state is generic (position, rotation, velocities read through entity virtuals); it worked
   for soldiers unchanged.
2. **The AI's animation bytes are never sent.** The AI write puts three bytes after the base message (the body's
   animation id, the animation its behaviour wants, a speed), and a client's AI does play exactly these (it
   picks one of the two ids and starts it); but the update codec (`0x693240`/`0x6932b0`) serialises only the
   sequence number, the length and the motion bytes. A client therefore read whatever its static receive buffer
   held: animation 0 at time 0, the "first pose". The mod appends the two ids (16 bits each, the AI's own bytes
   cannot hold ids above 127) and the speed to the motion bytes with a 2-byte marker, and gives them back to the
   AI's own read on the client (a message without the marker gets "no animation" instead of buffer leftovers).
3. **A soldier that stands still is never sent again** (the transform did not change, so its relevance is 0),
   so a new idle, aim or crouch animation would not arrive. The server sets the entity's "send now" bit (`8`) on
   an AI whose animation changed since its last update.

The client also copies the body's position into the entity's own position field (`+0xb0`, which the server's AI
code keeps equal to the body and scripts read).

Measured **(observed)**, level `ts2_city_part02`, both instances on one test PC, positions and animations of all
soldiers logged by both machines at the same clock seconds (`TS_AIDEBUG`):

| Check | Result |
|---|---|
| Position | after the join (the first ~30 s, while the server works through its update queue, some soldiers still lag), client body within 0.02-0.18 m of the server's for moving soldiers (one update behind; 0.58 m once for a running one) and within 0.03-0.1 m for standing ones, over 4 sessions (e.g. police3 `200.98/109.50` server, `200.97/109.47` client at the same second) |
| Animation | same animation id on both in 95-99 % of the samples (walk, run, aim, fire, idle variants: 12, 25, 26, 42, 60, 69, 82, 161, 181, 187, 262 ...), times 0.05-0.5 s apart; a switch shows on the client one update later |
| Frames of the same scene | the soldier the host sees running and firing is at the same spot on the client, in a running pose, not in its first pose (muzzle flashes are not shown on the client, see below) |
| Client shooting a moving soldier | a 1.5 s burst from the client at the soldier it saw killed it on the server in one session (alive before, dead right after), missed in another |
| AI shooting the client | soldiers now shoot the client's body: the client died several times within minutes (`SetGOD` on the client does not protect its body on the server) |
| Update rate | ~9 updates per second per moving soldier (the engine's priority scheduler); standing soldiers only when their animation changes |
| Stability | 10 minutes (and 9.5 minutes with the default build, no test variables but the diagnostics) with AI updates, AI deaths (script and client fire), client deaths and respawns: no crash on either side |

### Corpses, deaths before a join, names of objects created in play (2026-10-25)

**Corpses.** A death reaches a client as the death state (`0x859090` case 2 -> `[ai+0x290]->vtbl[1]` = `0x8496c0`,
with `netfix.c`'s empty hit info); without the authority bit that handler shows nothing, so the soldier stood in
its idle. The visual part is the class's virtual `+0x2e8` (`0x466b90` for the police/soldier classes: hit
reaction, death animation or ragdoll from the damage; `0x8498c0` "R.I.P." for simpler classes). A client now runs
it itself for every dead soldier (`AI_FLAGS 0x538` bit `0x20000`, set by the death state on both machines), with
the authority bit set for that call only and a neutral damage (no attacker, no damage type, a push of 1): the
soldier falls as a ragdoll where it stands, a few cm from the server's. That call also drops the soldier's weapon
and grenade pack as pickups; the server's own pickups reach the client through the network, so the client's
copies (objects the call created that would be networked, `isSyncMP`, but have no net id) are ended at once
(entity `vtbl+0xc0` = Terminate). Without that the client picked up a duplicate carbine (seen in a run).
Experiments that did not work: the body component's ragdoll call (`+0x4fc` -> `vtbl+4`, mode 12) with authority
made the body vanish.

**Dead before the join.** Two paths, both in `aisync.c`:
- the AI updates carry a "dead on the server" bit (one byte more after the animation ids). A client that gets it
  for a soldier that lives there gives it the death state and shows the death; a dead soldier gets no more
  animations from the server. The level's posed corpses `slo_*` (spawned from `SceneSLO/<level>.slo`, saved
  ragdoll poses, `0x612670`) are ragdolls on both machines from the start, but the server's animation id (175/176)
  stood them up on clients: they now keep their pose ("a ragdoll already");
- a client asks for the dead 4 s and 15 s into each level (message 52); the server answers with the names and
  positions of every dead soldier among the script manager's named objects (53; a dead soldier leaves the
  scheduler's lists), the client moves a living copy to the server's body and kills it.

**Names.** An object the server creates in play reaches a client as an unnamed replica (objects that exist when a
client joins do arrive with their names). A client asks for the names of networked objects whose script instance
has no name (50, net ids), the server answers with the names it has (51), and the client names its instance and
inserts it into the script manager's sorted list of named objects as CreateObject (`0x8fd2f0`) does (compare
`0x8fb500`, insert `0x8fb000`). A name a local object already has is not given twice.

| Subtype | Direction | Body |
|---|---|---|
| `50` | client -> server | `u8 1, u8 n, i32 net id x n` |
| `51` | server -> client | `u8 1, u8 n, (i32 net id, u8 len, name) x n` |
| `52` | client -> server | `u8 1, u8 0` |
| `53` | server -> client | `u8 1, u8 n, (u8 len, name, f32 x y z) x n` |

Messages stay below 200 payload bytes.

Measured on the N100 pair **(observed)**, level `ts2_city_part02`, run `work\runs\n100pair-20261025-112642` (frames in
`shots-20261025-112642`), confirmed in `n100pair-20261025-113324`, `-114638`, `-115442`:

| Check | Result |
|---|---|
| Death by script on the host (`police3.Die()`) | client: "died: death shown here (animation 181 -> -1)", ragdoll on the ground (client `shot143`, host `shot188`); the client's copies of `_arifle_itm` removed, the server's pickup then named on the client |
| Death by the client's fire (3 bursts at `police4_v2`) | dead on the host, ragdoll on the client (client `shot181`, host `shot226`); weapon and grenade pack copies removed |
| Killed on the host before the client joined (`police4_v3`) | client: "is dead on the server: death shown here (animation 175 -> -1)"; in other runs the server had already removed the body and the client did not have it either |
| Level corpses `slo_0`..`slo_8` | client: "is dead on the server: a ragdoll already", pose kept (before: standing) |
| `$farm.Create("ai_house_second_room_4")` after the join | client: "object net 2570 named ai_house_second_room_4"; `string($ai_house_second_room_4)` gives the name on both |
| Earlier experiment `n100pair-20261025-104724` | the visual death with authority turned `police4_v2` into a ragdoll matching the host's (client `shot181`, host `shot204`) |

### AI weapon fire on clients (`aisync.c`, 2026-10-25)

A soldier's shot runs on the server only. Its strike logic (authority only) sends engine message `0x4e4` with a
strike record to the weapon (`0x84f0d0`: `0x946ee0(weapon, 0x4e4, record, 0)`, reached from a scheduled `0x4e4` to the
soldier, `0x83a510`). The weapon's handler (weapon vtbl `+0x200` = `0x82cf90`, thiscall (weapon, record), ret 4) calls
`0x827c60` on the weapon's attack set (`+0x278`):

1. the attack's fire (`attack->vtbl+0x24` with the shot info at record `+8`; `0x5417e0` -> `0x8c1fa0` -> the projectile
   dispatcher `0x7f7ee0`/`0x7f8340`, which creates the projectile entity, here `wpn_ai_tracer`: tracer, flash and sound
   of the shot). Both dispatchers **return at once in game mode 3**;
2. the ammunition (skipped for a proxy on a client);
3. the weapon's shot effects `0x7fa420` (edi = the attack's clip entry, stdcall (owner, count), each listener's
   `+0x44`): only `ai_shells` (vtable `0xdc6800`), the ejected cases.

Layouts **(observed, host log)**: record = `14 40 0c 00 | u8 attack | u8 1 | 3f 01 | shot info *`; shot info (a
global, `0x10cc180` on the server) = `+0 0x0054400d, +4 2, +8 weapon, +0xc shooter, +0x10 1.0, +0x18 origin (the
muzzle, ~1.35 m above the body), +0x24 direction (unit), +0x3c target point, +0x4c 0x2002`. A soldier has two weapon
objects (`!ai weapons`), both owned by it (owner = the weapon's interface `+0x274`, vtbl `+0xc`); the one that shoots
is the one with shot effects. The soldier's own fields did not hold it at a fixed place (`+0x270 -> +0xe8` in one
run, nowhere in another), so a client searches the objects by owner.

What the mod does: the server hooks `+0x200` of the six AI weapon vtables (`0xd92e78` arifle/crb/pmg/rlauncher/grenade/
shotgun, `0xd93158` pistol, `0xd93438` uzi, `0xd939f8`, `0xd93d28` sniper, `0xdbefa0` `gs_wpn_ai`), counts each
soldier's shots and keeps the last shot's origin, direction and target distance. The count (6 bits) and the attack
(1 bit) ride in the AI update's state byte; when the count changed since the last update, 14 more bytes (origin
relative to the body in cm, direction, distance in dm; marker `a5 5b` instead of `a5 5a`) and the soldier is marked
"send now". A client that sees the count grow runs the weapon's own handler `0x82cf90` on its copy of that weapon
with a record and shot info built the same way (one shot per ~70 ms, at most 6 queued), with the two dispatchers'
mode test (`0x7f7f25`, `0x7f838e`, now a call) reading mode 3 as 1 during that call only. The projectile is a local
object of the client: role `0x40`, no authority bit, net id -1 (logged); the server's own shot does the damage
(the client never had an authority-side projectile before; the server's projectiles also exist on the client as
replicas, role `0x43`, but drew nothing there). `TS_AIFIRE=1` shows only the shells, `TS_AIFIRE=0` nothing.

Measured **(observed)**, N100 pair, `ts2_city_part02`, the host in god mode among the start soldiers, the client next
to it looking at a soldier, frames saved right after each shot shown (`TS_FIREDEBUG=2`):

| Check | Result | Run (`work\runs\`) |
|---|---|---|
| Server counts | ~10 shots/s per firing soldier; `police3`, `police4_v2_MM`, `police4_v3_MM` | n100pair-20261025-163246 |
| Shells only (first build) | 40 shots shown as "effects", no tracers in the client's frames | n100pair-20261025-162752 |
| Whole shot | 40 of 40 shots "effects, projectile"; client frames `shot7811`-`shot7920` show `police3` firing with yellow/red tracers leaving its weapon towards the host, several per frame (the stock look of AI fire); no crash | n100pair-20261025-164556, -164115 |
| Local projectiles | `wpn_ai_tracer` role `0x40`, net -1 (no authority); the server's replicas role `0x43` with net ids | n100pair-20261025-164556 |

Not checked: the shot sound (test instances are muted); a separate muzzle-flash sprite is not distinguishable in
the frames (the muzzle is small at that distance), the tracers start at the muzzle.

Still open:

- Soldiers playing scripted sequences (the level-start actors `*_MM`/`*_CR`, body animation id -2 on the server)
  stand in their idle on the client: the server plays an animation that is not one of the AI's animation ids (-2),
  started by the level script, which storysync runs on the host only; showing it needs the sequence's name and
  time from the server. Not done.
- The client's ragdoll falls from its own pose with a neutral push, not with the server's hit direction (the hit
  info layout of the death call is not known yet). Not done.

| Environment | Effect |
|---|---|
| `TS_AISYNC=0` | leave the AI as the game has it (the mod's AI replication is on by default; it acts only on AI with `isSyncMP` in a network session) |
| `TS_AIDEBUG=<s>` | every `<s>` seconds (on the clock) each AI's net id, role bits, body position, animation and update counters in the log; with it a client also reads `<TS_SHOTDIR>\follow.req` (`side back up` in metres) and puts its player next to the host's body, facing the same way, so both machines can render the same scene |
| `TS_AIFIRE=0/1/2` | AI shots on clients: nothing / shells only / the whole shot (default) |
| `TS_FIREDEBUG=1` | (lab) shots, shot info, weapons and projectiles in the log; `=2` also a frame after each shot shown (and each server shot); `=3` the code addresses on the stack of the first 12 projectile dispatches |

Test console (`TS_CONSOLE`): `!ai list <part of a name>|*` (each soldier's net id, role, flags, animation, ragdoll,
corpse state), `!ai kill <ai>` (the death state as the server's message sets it), `!ai show <ai> [push]` (show
its death here), `!ai named` (client: objects asked for and named), `!ai mem <object> <hex offset> [dwords]`,
`!ai wpn <ai>`, `!ai strikes <ai>` (weapon holder `+0x520`, strike manager `+0x530`), `!ai weapons` (every AI weapon
object with its owner), `!ai fire <ai>` (shells of one shot here).

### Script facts found on the way

- `string(obj)` turns an object into its name (`"" + obj` is a type error).
- Console snippets cannot declare `var` locals (only class members do); counting has to go through `$ui.SetStr`.
- `$player.Transport()` to a domain does nothing; to an AI, locator or other placed object it works.
- `IsAnyoneInside` needs an argument (`IsAnyoneInside(ent : object)`).
- Script `Terminate()` on a domain removes it on that machine only (used to keep the client from creating local AI).

### Test input for unattended instances

Test windows run with `TS_BACKGROUND=1` and ignore synthetic input even when brought to the front (a SendInput
click and a key reached the window but the game did not react). Firing worked by writing 1.0 into the raw input
value of the left mouse button, `[[0xfcabb4] + 0x3c] + 0x10 + 0x100 * 4`, in a loop for the length of a burst
(the background instance never acquires the mouse, so nothing overwrites it).

## 9. Mod messages (the mod's own data between the machines)

The mod sends its own messages inside the game's network messages instead of opening a second
connection, so they share the game's connection, reliability and ordering, and a joining client needs
nothing extra. Built and checked against the executable only; not yet run in a game.

- **Carrier**: the game's per-peer message sender with the generic small-message format the game uses for
  its own "notify" events: `{ codec index, 0x40, size = 6 + payload, 16-bit subtype, payload }`, at most
  250 payload bytes, reliable. Message id `0x7C01` (the game's ids are `0x3E8`-`0x4FA`; ids from `0x8000`
  up cannot be used because the reader stops at a negative id).
- **Routes**: a client talks only to the server; the server talks to each joined client. A message from a
  client meant for everyone goes to the server, which passes it on.
- **Receiving**: the mod puts its own function in front of the game's message handler of the server's and
  of the client's network object; every message that is not the mod's goes on to the game unchanged. This
  is always on (`TS_COOPNET=0` leaves the game's handlers alone).
- **Subtypes**: `1` = run this SSL text on the receiving machine; `2` = (client to server) run it on every
  machine: the server runs it and passes it on to the other clients as `1`. Received SSL runs in the
  per-frame script tick, like the test console, and is logged as
  `coopnet: ran from peer N: <text> -> <result>`. It runs only with `TS_COOPNET=1` or the test console
  (`TS_CONSOLE`); otherwise it is logged and dropped. Subtypes from `10` up belong to other parts of the mod
  (`coopnet_listen`): `10`/`11` are the time powers of section 9a, `20`-`22` the story of section 9b.
- **Test console** (section 7, `TS_CONSOLE`): a line starting with `@all ` runs the rest here and on every
  other machine, `@peers ` only on the other machines, `@srv ` on the server. The reply is `sent to N`
  (plus the local result for `@all`), `not in a network game`, or `too long`.

| Environment | Effect |
|---|---|
| `TS_COOPNET=1` | run SSL received from the other machines (also on with `TS_CONSOLE`) |
| `TS_COOPNET=0` | no mod messages at all (the game's handlers stay as they are) |

The log shows the first 8 messages sent and received (`coopnet: sent sub ...`, `coopnet: got sub ...`).

## 9a. Time powers in co-op

`native/timesync.c`, on in co-op sessions (`TS_TIMESYNC`, default `1` since `coopsession.c`; it acts only when
`ts_coop_feature(COOP_F_TIMESYNC)`). Run live on the N100 pair 2026-10-25 (results at the end of this section).
Addresses are those of patch 1.2.

### The rule

- **One world power at a time**, as the engine itself allows only one (its script call refuses a second
  power while one runs). The server decides who owns it: first come, first served.
- The owner uses the power exactly as in the campaign: its own key, its own energy, its own screen effect.
- On every other machine the **world** (enemies, projectiles, physics, scripted objects) is slowed or
  stopped the same way, with the same screen effect, so everyone sees what is happening.
- **Every human player keeps normal speed**, the owner and the others alike (as in Jericho's co-op slow
  motion): each machine's own player runs on real time, and a remote player's body follows the positions
  its owner sends.
- **Energy is per player**: only the owner spends energy. While a teammate's power runs, the power keys do
  nothing (no energy is spent). When the owner ends the power, runs out of energy, dies or leaves, the world
  returns to normal speed on every machine.
- **Time reversal is not shared yet**: in co-op the reverse key does nothing (see "Reversal" below).
- Scripted slow-motion moments of the levels are not part of this: level scripts run on every machine and
  start them there themselves.

### What the engine does (static analysis)

| Part | Where | Notes |
|---|---|---|
| World clock | per frame `0x6494c0` | world frame time `[0xfcab8c]` = real frame time `[0xfcab74]` x current world scale `[0xec00e0]`; the scale moves towards the target `[0xebfbb4]` (1.0 normal, 0.2 slow, 0.001 stop) at the rate `[0x1095f88]` |
| Global time flags | `[0xfcace0]` | bit 0 slow, bit 1 stop (bit 0x40 is set during play; 0x40 = nothing active, 0x41 slow, 0x42 stop) |
| World start | `0x441700` | `eax` = mode (0 slow, 1 stop), `ecx` = the player; sets the flag and the target scale and tells every object with the in-process messages `0x3fd` (slow) / `0x401` (stop); nothing is sent over the network |
| World end | `0x4419b0` | `eax` = mode, `ecx` = the player, one stack argument (1 = at once, 0 = back with the normal ramp), `ret 4`; messages `0x3fe` / `0x402` |
| Player speed | player code | the player's own code reads the **real** frame time, never the world frame time: the player keeps normal speed during slow and stop. Remote player bodies are placed from their owners' updates |
| Screen effects | manager `0x1096f18` | effect slot `mode + 3` at `+0x14f4 + slot*4` (slow 3, stop 4, reverse 5); start `0x665f70` (`esi` = manager, `edi` = slot, one float), stop = the slot's virtual function 2 while `+8 & 1` (both as the script `StartTControl(mode, true)` / `StopTControl(mode)` do) |
| A player's own power | controller at player `+0x280` | `+0` active mode (-1 none, 0 slow, 1 stop, 2 reverse), `+0x2c` energy (0..1); every start goes through `0x43e1b0` (`eax` = mode, `esi` = controller, stack = player, `ret 4`), which calls the world start; the end is `0x43e2b0` (`ecx` = player, `esi` = controller, `edx` = 0) |
| Script calls | `StartTControl` `0x43a8b0` -> `0x43c640`; `StopTControl` `0x43aa50` -> `0x43c6c0` | only `"tslow"` and `"tstop"`; the start is refused while a power runs (`[0xfcace0] & 3`); they do not touch the player's controller |
| Power keys in a network game | input function `0x40d2c0` | **in game modes 2/3/4 the keys never reach the campaign powers**: the function takes its multiplayer branch (chrono grenades, time shield). The energy set-up at spawn (`0x43de20`), its regeneration (`0x43e3b0`, `0x43eae0`) and the start threshold (`0x43e1b0`) also switch to the multiplayer rules (`StartEnergy`, `bPlayersTimeEnergy`, `bTimeControl`, ...). So on a campaign level hosted on a server, players had no campaign powers at all; only the script call worked |

### What the mod does

- **Campaign powers on a server.** In a network game on a campaign level (level file name containing
  `ts2_`), the six game-mode reads named in the last row see "single player": the keys, energy (full at
  spawn, regeneration) and HUD behave as in the campaign. Other maps keep the multiplayer rules
  (`TS_TIMESYNC=all` applies it to every network map).
- **Detection**: every frame (in the script tick, main thread) each machine reads its own player's active
  power at `+0x280`.
- **Messages** (mod messages of section 9, reliable):

  | Subtype | Direction | Body | Meaning |
  |---|---|---|---|
  | `10` | client -> server | `u8 version (1), i8 mode` | "my own power is now `mode`" (-1 none, 0 slow, 1 stop) |
  | `11` | server -> each client | `u8 version, i8 mode, u8 yours, u8 0, i32 owner` | "the world power is `mode`, owned by client `owner` (0 = the server's player); `yours` = it is your own" |

- **Server**: keeps the owner and the mode. Its own player's power, or a client's request, is accepted when
  nobody owns the world power (or the same player changes it) and sent to every joined client; a refused
  request is answered with the current state, and the client ends its own power (both players pressed in
  the same moment). The state is repeated every second while a power runs (for clients that join during
  it); a power whose owner left ends.
- **Every machine**: when the world power belongs to someone else, the machine runs it for them with the
  world start/end calls above (the player argument is its own player) and the screen effect, and refuses
  its own player's power starts (a gate in front of `0x43e1b0`). If the world loses that power (a death or
  a level script resets the clock), it is started again within half a second.

| Environment | Effect |
|---|---|
| `TS_TIMESYNC=1` | shared time powers on campaign levels hosted on a server (all machines need it) |
| `TS_TIMESYNC=all` | the same on every network map (multiplayer maps then lose the chrono grenades) |
| `TS_TIMESYNC_FX=0` | no screen effect on the machines of the other players |
| `TS_TIMESYNC_REVERSE=1` | the reverse key works in co-op, on the local machine only (experiment) |

The log has every decision as `timesync: ...` (co-op powers on/off with the level name, local power changes,
requests, refusals, world power changes, re-starts).

### Reversal (next step)

The campaign rewinds every object recorded with `isSyncTR` (`tr_writer`, `0x586000`) and plays the record
back; the player is not rewound. In a network game the recorder is never started (`0x5846b0`, which starts it,
does so only in game mode 1), so a reverse cannot start (`0x5847c0` needs recorded frames). Planned rule: the server's world
rewinds (the server owns AI, physics and doors, so clients see the result through the normal updates), the
players' own bodies do not (they are owned by their machines), the reverse screen effect shows on every
machine, and clients pause their local-only objects. Needs the recorder on the server and a check of what a
rewind does to replicated objects.

### Live test (N100 pair, level `ts2_city_part02`, 2026-10-25) **(observed)**

Keys were pressed with `lab\n100_key.ps1` (fixed in this session: a key press needs the engine's
"last frame" value at `+0x6b4 + code*4` to be 0 and the key's "used" mark `+0x1154 + code` bit 8 cleared;
writing only the current value gave "held" without a press). World state read from memory
(`[0xfcace0]`, `[0xebfbb4]`, controller `+0x280`, energy `+0x2ac`).

| Test | Expected | Observed | Run (`work\runs\`) |
|---|---|---|---|
| T1 keys on a server | campaign powers on the keys, full energy, HUD | V/G start slow/stop on host and client, energy drains and regenerates, HUD energy bar shown | n100pair-20261025-074231 |
| T2 host's power | client's world slows/stops, screen effect | client `0x4041`/target 0.2 (stop `0x4042`/0.001) within ~0.1 s; client frame shows the blue effect | 074231, 075251 (client shot056) |
| T3 client's power | host's world slows/stops, effect | host `0x41`/0.2 and `0x42`; host frame shows the effect | 074231 (host shot104) |
| T4 one power at a time | teammate's key refused, no energy spent | "local stop refused, a teammate's power runs", energy unchanged | 074231 |
| T5 energy per player | only the owner spends; server does not overwrite | owner drains, the other stays 1.0; client energy independent of the host's | 074231 |
| T6 end: key, energy out, death, owner leaves | world back on every machine | all four seen (death = `FailMission` on the client; leaving = client process killed: "owner left" after ~5 s) | 074742 |
| T7 world reset by a script on the non-owner | started again within 0.5 s | `StopTControl` on the client: "world lost ... started again" | 075251 |
| T8 both press in the same moment | one power survives | race forced by suspending one process: host+client both started -> the server took the client's (first at the server), the host's own power ended; other order: client's start refused at the gate | 080726 |
| Remote bodies | normal speed | the host's body on the client kept walking ~5 m/s during the client's stop; the host walks at normal speed during its own slow | 075715, 080726 |
| Reverse | does nothing in co-op | B: no reverse (not available without the recorder) | 074231 |
| Single player | unchanged | `TS_TIMESYNC=1`, level loaded by console: "co-op powers off", V slows as usual | sp-timesync-20261025-080119 |

Not seen: one extra re-start 0.3 s after a teammate's power began on the client (something resets the
client's world once; the re-start covers it). Not measured: the owner walking during its own stop (the probe
was spoiled by the key tool's "used" mark; engine behaviour as in single player).

## 9b. Story in co-op

`native/storysync.c`, **on by default** since 2026-10-25 (`coopsession.c` sets `TS_STORYSYNC=1` when it is not set;
`TS_STORYSYNC=0` switches it off, on the host for the whole session); it acts only in a co-op session whose host
runs the story (`ts_coop_feature(COOP_F_STORYSYNC)`; `TS_STORYSYNC=all` = every network map, for tests). Run live on the N100 pair 2026-10-25 (results at the end of this section). The classifier and the client rewrite were run offline over the scripts of all
24 campaign levels (6 135 object texts: 2 480 get statements wrapped, 10 746 statements in all; no statement
spans two lines). Addresses are those of patch 1.2. The rules come from `player-audit.md` (C1, C2, C4, C6-C8).

### The rule

- **The host runs the story.** Every level-script handler that changes the world (AI, creations, unlocks,
  saves, failure, level exits) runs on the host only. Clients see its results through the game's replication
  and through replays (below).
- **Presentation runs on every machine for its own player**: light zones, camera shake, hints, the automatic
  time-power choice (`$tc.SetMode`), sounds, holster and fall-height zones.
- **The first player to arrive starts the story**: a story domain counts as entered while any player is
  inside; its `OnLeave` fires when the last one has left.
- **The host's story events reach every client at the same moment**, and each client applies the `$player`
  calls of that handler (input and movement locks, teleports, time-power settings, loadouts, hints, FMVs) to
  its own player.
- **Level exits move the whole session**; a client never changes level by itself.
- **A player's death is a multiplayer respawn**, at the last checkpoint instead of the level start.
  **A mission failure restarts the level for everybody**, at the last checkpoint where the level has a
  restore object for it.

### What the engine does (static analysis)

| Part | Where | Notes |
|---|---|---|
| Script of a level object | `0x644e50`, compile call `0x644fc4` -> `0x8f9150` | each object with script text gets its own class; the compile call's hint argument is the object's name |
| Every script event | `0x903940` (thiscall: class, Status out, instance, member index, argc, argv; ret `0x14`) | called by index `0x8f82bf`, by name `0x8f97c6` (`0x8f9680`: instance, out, symbol, argc, argv) and for the per-frame mask events `0x905462`. **`argv` is an array of pointers to values** (`Variant **`: the handler match at `0x90e0c3` reads `[argv + i*4]`), and it must not be NULL. Handlers can match argument values (`override OnAnimEnd("COUNT")`) |
| Mask events | entity `+0x84`, fired by `0x6442f0` each frame | `OnLocked` 1, `OnUnlocked` 2, `OnInitLevel` 4, time events 8-`0x100`; class events `0x200`/`0x400`/`0x800` (domain `OnEnter`/`OnLeave`, door `OnOpen`/`OnClose`, button `OnPushed`/`OnPoped`, `OnComplete`) |
| Named events | by name with argument values | `OnBhvEnd(bhv)`, `OnActionFrame(anim, frame)`, `OnAnimBegin/End`, `OnEye(on)`, `OnAlert`, `OnDestroyed`, `OnPicked(user)`, ... |
| Domain test | `0x6c2dd0` (vtable `+0x134` of every domain class; thiscall, no arguments) | runs only when the watched object moves; tests the points of one watched object `+0xf8`, which is **not** the `$player` entity but one object shared by the level's domains (class vtable `0xdad650`); state `+0xe8` (1 inside, 2/4 fired once), lock count `+0x88`; the enter/leave virtuals `+0x124`/`+0x128` set the mask bits. **Only the local player sets off a domain**; `IsBelong` (`0x6c3d60`) tests any entity's position |
| Level exit | reload domain: per-point callback `0x8a9ec0` -> `Reload` `0x8a9f00` | needs the authority bit (never on a client); sends the single-player `LOAD_LEVEL` (`0x3ec`, `nameScene` at `+4`) to the world object (call `0x8aa0e3`): the host alone would leave |
| Server map change | `0x606960` (match rounds): in-process `0x3f6`, then `0x43e` to the level loader `[0x1033980]` | the loader (`0x6e3690`) tells every client (`0x43e`) and sends `0x3ec` with the level name; clients load the same level |
| Single-player level load | `0x675370` (stdcall, String) | used by `$ui.LoadLevel`, `$ui.RestartLevel` and the menus; on a server it ends the session |
| Checkpoint | `$saves_mng.AutoSave(id)` -> `0x6ce030` | |
| Respawn points | `mp_rules_sys` `[0xfcab18]` `+0x114`, count `+0x118`, stride `0x1c` (position, angles, team flags) | built from the level's `start_pos*` objects at the server's level start (`0x5ec4f0`) |
| Mission failure | `FailMission` `0x43b550` | on a server: a multiplayer death (observed, section 8a) |

### What the mod does

- **Classification** (host and clients, when a level object's script is compiled): each handler of the object
  is *local* when every statement is presentation (the calls listed under "The rule"), else *story*.
  `OnInitLevel` and `OnInit` are always local: every machine sets up its own copy of the level and its AI.
  Over the 24 levels: 900 story and 247 local `OnEnter` handlers, 902 story domains.
- **Client scripts** are compiled with the statements only the host may make wrapped in `if false ... end`:
  AI control (`SetBHV`, `SetAim`, `SetTarget`, `Die`, ...), creations (`Create`), deaths and failure
  (`Kill`, `FailMission`), saves and level loads, seats (`StartUse`). The rewritten text is compiled once into
  a throwaway class first; if it does not compile, the original is kept and that object's story is not
  replayed on the client.
- **Client**: a story event the client detects itself does not run. A story domain entry or exit is reported
  to the host (message 20), a look trigger too (22).
- **Host**: runs every handler as before. Before a story event runs, it is sent to every client (21: object
  name, event name, arguments). A story domain is entered while the host's player, a client's body (by
  `IsBelong`) or a client's report is inside it (the domain test's point count is replaced at `0x6c2e3d`).
  A look trigger fires once even when several players look.
- **Client replays**: the event runs on the client's own object of the same name, with the rewritten script,
  in the next script tick (in order; an object the host has just created is waited for up to 5 s). So the
  client's own player gets the handler's `$player` calls, local objects are shown, hidden, locked and
  started as on the host, and the world changes come from the host.
- **Level exits**: on the host the reload domain's `LOAD_LEVEL`, and `$ui.LoadLevel` / `$ui.RestartLevel` of a
  campaign level, become the server's map change (`0x3f6`, then `0x43e` to the level loader with the next
  level), done in the next script tick: every machine loads it. The profile's `ServerLocation` is set to that
  level, so a server started later continues there. On a client a campaign level load is refused.
- **Checkpoints**: `AutoSave(n)` on the host moves every respawn point to `Check_Point_<n>` if the level has
  that object, else to the player who set the checkpoint off (the one inside the domain whose handler saved),
  else to the host's player. Players who die respawn there.
- **Mission failure**: `$player.FailMission` on the host restarts the level for everybody 3 s later (server
  map change to the same level). When the level is up again and has `Check_Point_<n>` for the last checkpoint,
  the host unlocks it: its own restore script (teleport, world fixes) runs on the host and, replayed, moves
  each client's player too; the respawn points go there. Without such an object the level restarts from its
  start. `TS_STORYSYNC_FAIL=death` keeps the game's multiplayer death instead.

| Subtype | Direction | Body | Meaning |
|---|---|---|---|
| `20` | client -> server | `u8 1, u8 inside, name` | a story domain was entered / left by the client's player |
| `21` | server -> each client | `u8 1, u8 argc, u8 len, object, u8 len, event, arguments` | run this story event here |
| `22` | client -> server | `u8 1, u8 on, name` | a story look trigger (`OnEye`) fired on the client |

Arguments: `u8 type` (0 null, 1 int, 2 bool, 3 float, 4 string) and the value (4 bytes, 1 byte, or `u8 len` +
bytes). Object arguments (`OnPicked(user)`, `OnOccupied(user)`) travel as null.

| Environment | Effect |
|---|---|
| `TS_STORYSYNC=0` | no shared story (default: on in co-op sessions; single player and stock maps are unchanged either way) |
| `TS_STORYSYNC_FAIL=death` | mission failure stays a multiplayer death |
| `TS_STORYSYNC_LOG=1` | every held-back, sent and replayed event in the log |

Test console (`TS_CONSOLE`): `!ss state`, `!ss class <object>` (its handlers: story or local; rewritten),
`!ss enter <domain>` / `!ss leave <domain>` (client: report as if its player entered or left),
`!ss fire <object> <event> [0|1]` (run an event, with one bool argument when the value is given; on the host it is
also sent, on a client a story event is held back and reported like one the client detected),
`!ss find <event>` (the level objects with a handler of that event), `!ss goto <domain>` (this machine's player to
the middle of a domain's bounds, `+0xc8` min / `+0xd4` max), `!ss exit <level>` and `!ss restart` (host). The log
has every decision as `storysync: ...`.

Changes made in the live test (all in `storysync.c`): event arguments handled as `Variant **` on both sides
(the client crashed at `0xCCCCCCCC` on the first replay, and every argument had gone out as null); the
world-time events (`OnStartTStop`/`OnStopTStop`/...Slow/...Reverse) are local (each machine's world, kept in
step by timesync); sound actors and music (`$snd...`, `$music.`) are presentation; scripts are prepared at
compile time when the level is a co-op level (the session's settings arrive after the level is built, so the
first level had been left unprepared), replays are accepted before the settings arrive; the host re-runs the
domain test of story domains with a client inside, 4 times a second (a client's entry otherwise waited for the
host's player to move); the watched object is learnt as the one the story domains share instead of `$player`;
a level rebuilt under the same name (team restart) clears the class table on both sides (the host by its
"level running" flag, the client, which never sets that flag, by the collapse of the named script objects).

### Live test (N100 pair, 2026-10-25) **(observed)**

| Test | Expected | Observed | Run (`work\runs\`) |
|---|---|---|---|
| Gate: no crash, plain session | both join and play | after the fixes no crash in 8 runs up to 7 min (two restarts, two level changes each); before them the client crashed on the first replay | n100pair-20261025-090615 (before: 081404) |
| Client scripts prepared | host-only statements wrapped, compile | part02: 483 classes, 142 rewritten on the client; part03: 128/63; no "does not compile" | 083706 |
| T1 client's domain entry | reported, host runs it, replay, no local duplicate | `!ss enter dom_farm_first_room` on the client -> host fired `OnEnter` 4 ms later, created `ai_house_first_room_2`; client "ran ... (host's)", no duplicate; leave -> state 6 (fired once) | 090325 |
| T2 host's story events | sent before running, replayed on the client | 418 replays ran on the client in one session (cine, unlockers, domains, `OnBhvEnd`); errors only where the level itself errs in single player or names objects created at run time (they reach clients nameless) | 083706 |
| T3 client's own story events | held back, reported | 26 held back and reported in one session (domains the client walked into at part03) | 083706 |
| T4 checkpoint | respawn points move | `AutoSave(2)` -> respawn at `Check_Point_2` (-169.6 -10.9 -145.2) | 083706 |
| T5 death | multiplayer respawn at the checkpoint | client `FailMission` -> respawned at `Check_Point_2` | 084558 |
| T6 level exit | whole session changes level; client cannot | host `$ui.LoadLevel("ts2_city_part03.lg")` and `!ss exit` -> both on part03, client still joined; client `$ui.LoadLevel` refused | 083706, 083145 |
| T7 team restart | level restarts for everybody at the checkpoint | host `FailMission` -> both reload part03 and stand at `Check_Point_2`; on part02 (no restore object) restart from the level start | 083706, 084558, 090615 |
| Time powers with story on | unchanged | host slow -> client `0x41`; client stop -> host `0x42` | 084558, 090615 |
| Stock map `multiplayer1` with `TS_STORYSYNC=1`, `TS_TIMESYNC=1` | both modules idle | storysync role 0, no classes, nothing sent; timesync "co-op powers off"; V on the host spends energy on the stock chrono grenade, no world power; client joined and played (its first load of the map spent > 80 s building its shader cache, the next took 9 s) | n100pair-20261025-102207 (101905) |

Second round, 2026-10-25 afternoon **(observed)**:

| Test | Expected | Observed | Run (`work\runs\`) |
|---|---|---|---|
| T8 natural level exit | the host's player walks into the exit; everybody loads the next level | `$dom_reload.Unlock()` (in the level a rebel's `OnBhvEnd("RUN_TO_EXIT")` unlocks it), `!ss goto dom_reload` put the host's player into the domain (bounds 205.3 -7.3 -16.0 .. 209.6 -5.3 -11.7, a tunnel under the street): 1 s later "level change to ts2_city_part03 requested" from the reload domain's `LOAD_LEVEL`, then the server's map change; host and client on part03, the client still joined (`IsServer` No), both at the level start (frames `shots-20261025-124159`) | n100pair-20261025-124159, 124850 (113324: host only, the client had dropped) |
| T9 `OnEye` report | a client's look trigger is held back, reported (22), run once by the host | `!ss fire look_door_knock OnEye 1` on the client (its own detection path): the host ran the handler and created `ai_house_third_room_1`; on the client the soldier exists under its name and lives (the verbose story log was off in that run, so the held-back and reported lines are not in it) | n100pair-20261025-124159 |
| Default on, no `TS_STORYSYNC` anywhere | as with `TS_STORYSYNC=1` | log "TS_STORYSYNC=1 (default for co-op sessions)" on both; roles 2/3; T1 (client entry -> host created `ai_house_first_room_2`, named on the client, alive), T4 (`AutoSave(2)` -> "checkpoint 2"), T5 (client `FailMission`, respawn), time powers (host V: host `0x41`, client `0x4041`; client G: host `0x42`, client `0x4042`), T8 (natural exit, both on part03), T6 (client `$ui.LoadLevel` refused), T7 (host `FailMission` -> "restarted at checkpoint 2") | n100pair-20261025-124850 |
| T7 again (fix) | both at `Check_Point_2` after the restart | in 124850 both stood at the level start after the restart: the restore ran 3 s after the level start, before the players existed (they spawn ~6 s in on the N100). The restore now waits for the host's player (+2 s), a client's replays wait for its player (up to 30 s), 4 s after the restore a host's player more than 10 m away is put at the checkpoint, and a lower checkpoint id after a restore is ignored (the restarted level's own `AutoSave(1)` had moved the respawn points back). Host alone: at -169.62 -11.88 -145.22 = `Check_Point_2` (125817, 132139). With a client in the session (part02 -> natural exit -> part03 -> `AutoSave(2)` -> `FailMission`): the client lost the connection while reloading part03 (section 9d) and the host was then at the level start, not at the checkpoint (the 4 s check had not acted: the host was still at the checkpoint then; it was moved back to the start later, around the client's drop); the client side of T7 was therefore not confirmed then | n100pair-20261025-125817, 132139, 134709, 135403 |
| T7 with a client, after the join fixes of 9d | both reload part03, stay connected and stand at `Check_Point_2` (host -169.62 -11.88 -145.22, client -168.63 -11.88 -145.09) | as expected | n100pair-20261025-181337 |

### Not covered yet

- **Late joiners**: story events from before a client joined are not repeated; clients should join before
  the host leaves the start area (level changes through the server keep everybody in step afterwards).
- **"All players" gates** (lifts, trains, vehicle boarding) still follow the first player; level exits need
  the host's own player inside the exit domain (the reload runs from the host's point test).
- **Story handlers that test `IsBelong($player)`** (container traps, some event domains) check the host's
  player only (audit C3): a trapped client is not failed, a trapped host fails everybody.
- **Death scenes** (`RIP_cine` ... `$player.Kill()`): the host dies even when a client set the scene off.
- **Seats and vehicles** (audit C9): `StartUse` runs on the host only.
- **AI that names `"player"`** (audit C10) still means the host.
- Objects with duplicate names (unnamed instances) run their rewritten handlers locally on a client and are
  not replayed.
- A restart without a restore object loses all story progress of the level (no server-side saves are loaded).

## 9c. The co-op session (`native/coopsession.c`)

Run in every N100 pair session since 2026-10-25 (the lab's `AutoTest="join"` goes through the Co-op menu's
direct connection); results at the end of this section.

- **What a co-op session is**: a network game (game mode 2/3/4) on a campaign level (`ts2_` in the level
  file name). Only the mod's Co-op menu starts one (the stock map list does not offer campaign levels), so
  every machine knows it from the level alone, at once and without a message: `ts_coop_active()`.
- **Rules, co-op sessions only** (stock maps keep the stock rules):

  | Rule | Where | What the mod does |
  |---|---|---|
  | Friendly fire | MP damage factor `0x4e3fe0` (stdcall victim, attacker id; ret 8; float): same player -> `SelfDamageScale`, other player -> team game and same team -> `bFriendlyFire` | the "other player" branch (`jne 0x4e4142` at `0x4e40a9`) goes through a stub that returns 0.0 through the function's epilogue (`0x4e42fc`): players never hurt each other; self damage stays |
  | Warm-up | `bWarmUp` read at `0x5ee949` in the server's level start `0x5ee870` (modes 2/4; 0 = start the round at once, `0x5eea40`) | the call to the option store's int getter `0x4027f0` (thiscall store, index, int *out; ret 8) goes through a wrapper that returns 0 |
  | Ready system | `bReadySystem` read at `0x596cbb` (rules) and `0x5ff66a` (HUD hint "not ready") | same wrapper |
  | Minimum players, time and score limit | profile keys, read again at every check | set by the Co-op menu for its server, put back at the main menu (`building.md`) |

  `TS_COOP_FF=1` keeps the stock friendly fire rule, `TS_COOP_MPRULES=1` the warm-up and ready system,
  `TS_COOP=0` switches the whole file off.
- **Session settings** (`ts_coop_feature(COOP_F_*)`): the host's. Its co-op menu leaves them in UI strings
  (`CoopSession = host`, `CoopDifficulty`, `CoopMaxPlayers`), read by the DLL when the server's level starts;
  time powers (`TS_TIMESYNC`) and story (`TS_STORYSYNC`) from the environment, both `1` by default since
  2026-10-25 (`0` switches them off; timesync.c acts on campaign levels only, storysync.c only in co-op sessions
  with `COOP_F_STORYSYNC`). Both modules read these flags (`ts_coop_feature`).
- **Messages** (mod messages of section 9):

  | Subtype | Direction | Body | Meaning |
  |---|---|---|---|
  | `40` | server -> client | `u8 proto (1), u8 flags, u8 difficulty (0-2, 255 none), u8 max players, u8 len, version` | the session's settings and the host's mod version; sent when the client's slot is joined and ready, and as the answer to every `42` |
  | `41` | client -> server | `u8 proto, u8 result (0 same version, 1 other), u8 len, version` | the answer |
  | `42` | client -> server | `u8 proto, u8 0` | "send me `40`": a client that runs the level and has no session info asks every 3 s (20 times at most); after a level change in the session it asks once more (checked: n100pair-20261025-140028, "client 1 answers" on part03 too) |

  The first version repeated `40` every 3 s from the moment the client's slot was joined; such messages went to a
  client that was still loading the level (section 9d).

- **Client**: same version -> `$ui.SetStr("CoopSession", "client")` and the host's difficulty
  (`$ui.SetVariable("Multiplayer", "Difficulty", ...)`); other version -> `CoopError =
  "COOP_ERR_VERSION,<host>,<own>"` and `$ui.LoadLevel("menu.lg")` (as the in-game "quit"): the main menu
  shows "The host plays with co-op mod version X, you have version Y ...". Version strings from the network
  are reduced to `[0-9A-Za-z.+_-]` before they go into script text.
- **Open**: a client without the mod (or with a build older than this file) is not refused, only logged
  by the server ("never answered" after 90 s); the stock game's reaction to the mod's message id `0x7C01` is not known.

| Check **(observed)** | Result | Run (`work\runs\`) |
|---|---|---|
| Join through the Co-op menu (direct connection, production frames + lab recipe) | every pair run: host "client 1 joined", client "host version 0.0.1-dev (same); flags 0x7, difficulty 1, max players 4" 10-15 s after its level start, "joined the host's co-op session" | e.g. n100pair-20261025-121754, 124850 |
| Version check | client with `TS_COOP_VERSION=0.0.0-test` (lab: says it has another version): "host version 0.0.1-dev, here 0.0.0-test: leaving", back in the main menu with the Russian message "the server has co-op mod version 0.0.1-dev, you have 0.0.0-test; install the server's version" (client `shot108`) | n100pair-20261025-131715 |

## 9d. Joining a running level (fixed 2026-10-25, `netfix.c`, `coopnet.c`)

Before: on the N100 pair a client that joined a running co-op level was dropped while it loaded ("server closed",
`UI_ERROR_SERVER_CLOSED`): 9 of 9 joins into `ts2_city_part03`, about 1 in 8 into part02, also with the build
before the co-op work. Four causes, found one behind the other; all are the stock game's, the campaign levels'
object counts expose them.

**1. Too many unacknowledged packets (the drop).** The close came from the host: the transport's closed callback
(`0x77f6e0`, now logged with the reason and stack) said "local close", called from the channel send pump `0x77e3c0`,
which disconnects a peer whose channel holds more than 500 unacknowledged reliable packets (`cmp [channel+0xc],
0x1f4` at `0x77e4d2`). A loading client acknowledges nothing for 10-20 s, and reliable messages do go to loading
clients (`0x697f30` skips only unreliable ones for slots without flag 1). The bulk was message `0x43a`: the base
entity's frame handler (`0x6442f0`, message 1000) broadcasts it reliably every frame for each `isSyncMP`+`isSyncSML`
object whose state machine raised a signal in that frame (64-bit signal mask at entity `+0x3c`, 16-byte body). On
part03 that was ~650 a second, one more unacknowledged packet every frame: unacked 39 -> 526 in 5 s, then the close
(TS_NETDEBUG, `n100pair-20261025-152109`). On part02 the replicated AI tracers (`wpn_ai_tracer`, short-lived) send it.
Fix: the host holds `0x43a` for a slot that does not run the level yet, ORs the masks per object and sends one per
object once the client runs it ("client 1 runs the level: 4871 state-machine messages held during its load sent as
9"; objects that ended meanwhile are dropped from the table every second, at most 512 objects are held, more go
out as before). `TS_JOINHOLD=0` = stock.

**2. A crash on the client right after the join.** With the drop gone, 3 of 4 part03 joins crashed the client 2 s
into the level (`0x8672d3`, reading `[garbage + 4]`): the AI manager `[0x1095b6c]` keeps a list of vehicle-type
objects (`+0x1a0` count, `+0x1a4`, filled by `0x8671a0` from `0x8a1d70`), removal `0x867210` refuses in game mode 3
and without the manager's flag `+0xa8` bit 2. A joining client ends the objects the server no longer has, their
pointers stayed, and the next AI initialised there built its target list from freed memory (`0x867440` ->
`0x8672a0`). With `TS_AILIST=0` (stock) the TS_NETDEBUG check showed "list 7 entries, 1 not objects" just before the
crash (`n100pair-20261025-154126`). Fix: the removal runs in every mode and without the flag (two `je` nopped).

**3. Slow join state.** The transport's per-connection rate control (`0x77e890`) works between limits from `0x77ee10`:
1500 bytes/s minimum and `(players + ...) x 375 + 3625` maximum (4000 with one client) on the server, 650-4000 on a
client. The join state of part02 (descriptors of every object, ~60 KB) took 13 s at 1.5-4 KB/s after the client's
load; meanwhile the host's unacknowledged packets reached 300+, and in 2 of 13 part02 joins the client never got
through (`n100pair-20261025-170123`, `-171200`). Fix: in a co-op session every connection gets at least 16 KB/s minimum
and 96 KB/s maximum (`TS_NETRATE=<min>,<max>`, `0` = stock); measured: the client ran the level 0.4 s after its load
instead of 8-13 s, unacknowledged packets stayed below 50 (`n100pair-20261025-172759`).

**4. Acknowledgements.** A client acknowledges only in the packets it sends; until it runs the level it sent ~3 a
second. It now sends one empty unreliable mod message (sub 3) every frame in a co-op session (`TS_ACKPUMP=0` off);
after cause 3 this matters little, it is kept as a cheap margin.

Not the cause (measured before): the server's 5 s silence check in `0x769850` (raised to 30 s, joins still dropped),
the mod's own messages (already only to clients that run the level), AI replication (`TS_AISYNC=0` dropped too).

| Check **(observed)**, N100 pair, client 20-30 s after the host | Result | Runs (`work\runs\`) |
|---|---|---|
| Joins into `ts2_city_part03`, causes 1+2 fixed | 10 of 10 stay, no crash | n100pair-20261025-154348 .. -155920 |
| Joins into `ts2_city_part02`, causes 1+2 fixed | 9 of 10 (one stalled: cause 3) | n100pair-20261025-165100 .. -170634 |
| All four fixed, `TS_NETDEBUG=1`, AI fire on: part03 / part02 | **10 of 10 / 10 of 10** stay, no crash; the client runs the level 0-1 s after its load (before: 8-13 s); peak unacknowledged packets 12-88 (before: 300-526); 40 AI shots shown on the client in each | n100pair-20261025-172957 .. -174531 (part03), -174715 .. -180245 (part02) |
| Join after a level change (host part02 -> `!ss exit ts2_city_part03`, the client joins part03 22-29 s later) | stays, "joined the host's co-op session" 0.5 s after its load; 8737 held state-machine messages sent as 92 | n100pair-20261025-180455, -181044 |
| Team restart with a client (T7 client side): part02 -> exit -> part03 -> `AutoSave(2)` -> host `FailMission` | both reload part03 and stay connected; host at -169.62 -11.88 -145.22, client at -168.63 -11.88 -145.09 (= `Check_Point_2`); client frame `shot247` in the station | n100pair-20261025-181337 (frames `shots-20261025-181337`) |

Test tools: `TS_NETDEBUG=1` logs every second each connection's state, rate and limits, unacknowledged packets,
sequence numbers and the packets received, and the messages sent to loading slots by id; `TS_NETQUEUE=<n>` moves the
500 limit (lab).

## 10. Open questions

- On one test PC with two GPUs (game window on the integrated Intel GPU) the Direct3D device is lost at
  every map load (also a map loaded offline from the console, once already the main menu level) and
  never recovers (`TestCooperativeLevel` keeps returning `D3DERR_DEVICELOST`, no `Reset`): the game
  keeps playing and talking to the server but draws nothing. At the same seconds the Windows service
  "Intel Content Protection HECI Service" stops with an error. Not seen on the single-GPU PC.
- The LAN list reply format and whether the list fills when the broadcast reaches the server.
- What `ServerPort` and the dedicated-server strings do; whether two instances on one PC can share 7777.
