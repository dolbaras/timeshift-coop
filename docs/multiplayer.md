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

## 4. Automation for tests (no keyboard or mouse)

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

With `VIDEO_FullScr = no` in the profile the Direct3D device was lost at the first level load (the main
menu level) and never reset: the game kept running and drew nothing (Intel UHD 630, 2026-10-25). With
`yes` and the proxy's test mode making the device windowed, everything renders. Players' windowed and
borderless modes are a job for the mod's display settings (as in the Jericho mod).

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
| AI movement and animation | **No** (with or without `isSyncMP` on `ai_brain`) | Same position and view direction on both machines: on the server the soldiers fight and fall, on the client the same soldiers stand frozen in their first pose for minutes; positions differ by 3-15 m (e.g. police3: server 196-207, client stays at 191-194). The shipped data and `--sync-ai` gave the same frozen picture. | Find why the AI regular update does not move client proxies (the AI classes have the update code, `netcode.md` section 4); else mirror AI transforms/animations over mod messages. |
| AI death | **Only with `isSyncMP`**, and it **crashes the client** | With `--sync-ai`, `Die()` on the server made `IsLive()` false on the client too (the figure kept standing); without it the client still said `true`. With `--sync-ai` and no fix the client crashed 1-4 minutes after joining, when AI died on the server: `AccessViolation at 0x8496D7 (read 0)` (2 of 3 unpatched sessions; the first, exploratory one ran 8 minutes without). | Native fix (below), then death animation/ragdoll on the client (the proxy stays standing). AIs that died before the client joined still count as alive there (`slo_*`): the join state must carry deaths. |
| Client shooting AI | **Yes** (damage reaches the server) | The client fired (input written in memory, ammo 15 -> 4 / 112 -> 96) at a soldier it saw (the frozen proxy); the soldier, alive on the server just before, was dead there right after, with no other shooter left (rebels removed by script, host far away with `SetGOD`). Twice, once with and once without `isSyncMP`. Without it the client did not learn of the death. | Make the client aim at where the AI really is (AI movement sync); show the death on the client. |
| AI shooting the client | **Not seen** | ~1 minute next to live hostile soldiers (server side 8 m away, host elsewhere): client health stayed full; one earlier session showed the red damage tint once while soldiers fought rebels. AI target natives take names and remote players have none (`SetTarget("player")` = the host, audit D7). | Check AI target selection over `player_mp` (C10 of the audit); probably native. |
| Pickups / dropped weapons | **Yes** | Weapons dropped by AI killed on the server appeared on the client's ground (`isSyncMP` items). | - |
| Time powers | **No** (each machine only) | Client `StartTControl("tslow", true)`: the client's flags `[0xfcace0]` = 0x41 and world scale 0.2 with the blue effect on its screen; the server stayed 0x40 / 1.0, no effect. Host `tslow`/`tstop`: server 0x41 / 0.2 and its grey screen; the client stayed 0x40 / 1.0, no effect. Host-side slow-down of AI cannot be seen on the client while AI do not move there. | Mod message: a client asks the host to run the power, the host broadcasts flags/effect to every client (as `netcode.md` section 5 says). |
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
  (`TS_CONSOLE`); otherwise it is logged and dropped.
- **Test console** (section 7, `TS_CONSOLE`): a line starting with `@all ` runs the rest here and on every
  other machine, `@peers ` only on the other machines, `@srv ` on the server. The reply is `sent to N`
  (plus the local result for `@all`), `not in a network game`, or `too long`.

| Environment | Effect |
|---|---|
| `TS_COOPNET=1` | run SSL received from the other machines (also on with `TS_CONSOLE`) |
| `TS_COOPNET=0` | no mod messages at all (the game's handlers stay as they are) |

The log shows the first 8 messages sent and received (`coopnet: sent sub ...`, `coopnet: got sub ...`).

## 10. Open questions

- On one test PC with two GPUs (game window on the integrated Intel GPU) the Direct3D device is lost at
  every map load (also a map loaded offline from the console, once already the main menu level) and
  never recovers (`TestCooperativeLevel` keeps returning `D3DERR_DEVICELOST`, no `Reset`): the game
  keeps playing and talking to the server but draws nothing. At the same seconds the Windows service
  "Intel Content Protection HECI Service" stops with an error. Not seen on the single-GPU PC.
- The LAN list reply format and whether the list fills when the broadcast reaches the server.
- What `ServerPort` and the dedicated-server strings do; whether two instances on one PC can share 7777.
