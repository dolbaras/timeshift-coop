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
and the co-op rule defaults below. Profile keys for a host that starts by itself:

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
| Saving | `$saves_mng.CanSave()` returns true on the server, but neither the level's autosave (single player shows "Saving game..." at the start) nor `$saves_mng.AutoSave(1)` from the console wrote a save file |

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

### Open on the host

- About 18 s after the level start (when the start camera hands over to the player) an empty menu opens on
  the host: the "Ver 1.02" menu background with a single highlighted bar and the mouse cursor, no text.
  `$ui.CloseMenu()` closes it and the game goes on; `$ui.BackFrame()` shows another empty menu. It does not
  open on multiplayer maps. Not identified yet (not the in-game menus `InGameMenu`/`InGameMulti`, which show
  their texts when opened from the console, and not the script autosave).
- Rebel soldiers of the level (friendly in the campaign) and players on a server: which faction a
  multiplayer player belongs to was not checked.

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
