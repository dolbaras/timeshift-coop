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
| `AutoTestMap = "<value>"` | (host, optional) value for the CreateGame `ServerLocation` select |

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
them was used in LAN or direct play. Ways to bring back Internet hosting: answer the three lookups with a
GameSpy replacement (OpenSpy) through a hook on `gethostbyname`, or keep servers in LAN mode and connect
by address (works through port forwarding of UDP 7777 on the host).

## 7. Test tools in the mod's DLL

| Environment | Effect |
|---|---|
| `TS_NETLOG=1` | socket calls in the log: socket, bind, connect, listen, accept, setsockopt, close, name lookups, and the first datagrams per socket and peer (then the 10th, 100th, ...) with their first bytes |
| `TS_SHOTDIR=<dir>`, `TS_SHOTS=s1,s2,...` | rendered frames saved by the DLL as `<dir>\shotNNN.bmp` at those seconds, and whenever a file `<dir>\shot.req` appears (window captures of the game return blank or stale pictures) |
| `TS_CONSOLE=<port>` | the script console: e.g. `$mp_rules_sys.Ready()`, `$mp_rules_sys.Spectate()` (the host then follows the other players' views), `return $ui.GetGameInfo("IsServer")` |

## 8. Open questions

- On one test PC with two GPUs (game window on the integrated Intel GPU) the Direct3D device is lost at
  every map load (also a map loaded offline from the console, once already the main menu level) and
  never recovers (`TestCooperativeLevel` keeps returning `D3DERR_DEVICELOST`, no `Reset`): the game
  keeps playing and talking to the server but draws nothing. At the same seconds the Windows service
  "Intel Content Protection HECI Service" stops with an error. Not seen on the single-GPU PC.
- The LAN list reply format and whether the list fills when the broadcast reaches the server.
- What `ServerPort` and the dedicated-server strings do; whether two instances on one PC can share 7777.
