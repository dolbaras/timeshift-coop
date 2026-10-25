# Local split screen (prototype)

Two players on one PC: player 1 on keyboard, mouse and pad 1, player 2 on pad 2. The screen is split side by
side on wide screens and top and bottom on others. This is a prototype, off unless the environment variable
`TS_SPLIT=1` is set. The code is `native/split.c`; the static research behind it is in the private notes
(`work/re/splitscreen.md`).

Status (2026-10-25, prototype, main PC, `ts2_city_part02`/`part03`, single player): both views render on alternate
frames into their halves with a HUD each; player 2 is spawned next to player 1 with its own first-person weapons,
moves, turns and fires with pad 2, has the script name `player2`, cannot hurt player 1, respawns next to player 1
instead of ending the game, stays through a level change, and each player sees the other's body. Not ready for players: see "Open problems".

## Why it has to be built

TimeShift on PC has no split-screen code left from the console versions. There is one local player, one camera,
one command context and one HUD, and each is a single global:

| What | Global | Notes |
|---|---|---|
| local player | `[0x1095b30]` and `[0xfc8180]` | the player constructor (`0x406750`) sets both, the destructor clears both |
| camera | `[0xfcabc4]` | a 0x360-byte object; the player's camera code writes it every frame |
| commands | input manager `[0xfcabb4]` + 0x40 | the active command context (`GAME` in play, `UI` in menus) |
| HUD | `[0x1095fc8]` | laid out on a 640x480-based virtual canvas (`[0xebfaac]` x `[0xebfab0]`) |
| sound listener | camera of `[0xfcabc4]` | FMOD listener 0, updated in the game update |

The mod therefore makes the second player out of the single-player pieces and swaps these globals at the right
moments of the frame.

## How it works

### The frame

With split screen on, the mod forces the engine's single-thread frame (what `[CONFIG] DisableMultithreading = 1`
does). Then one frame runs in this order on the main thread:

1. the game update (message 0x3e8: input evaluation, players, AI, scripts, sound listener);
2. the renderer's frame start (`R` vtable +0x1c, `0x67ee90`): it copies the camera into the view list and sets
   the visibility viewer;
3. the HUD update (`UI` vtable +0x114, `0x671da0`);
4. the render job (`0x649830`): Present of the previous frame, then the world, post-processing and HUD into an
   off-screen "pool" target, and one full-screen quad that copies the pool to the back buffer
   (call `0x987db0` at `0x770f42`);
5. the renderer's frame end (`R` vtable +0x24, `0x67f290`).

### Two views on alternate frames

Each frame renders one view, alternately player 1's and player 2's, through the engine's whole pipeline.
Visibility, shadows, post-processing and the HUD follow the view by themselves. From step 2 to step 5 the mod
sets:
- `[0xfcabc4]` = a copy of the view's camera with the field of view of a half screen. The vertical angle stays
  as the game set it for the whole screen and the horizontal angle follows the half's shape, so each half is a
  cut-out of a normal view, not a squeezed one;
- both player pointers = the view's player (HUD values, first-person weapon);
- the HUD canvas width = 480 x the half's aspect, so the HUD is laid out for a half screen. It is drawn over the
  whole picture and squeezed into the half together with the picture, which keeps its proportions.

The game update (step 1) always sees player 1's globals, so the sound listener follows player 1.

### Composite

The quad call at `0x770f42` is replaced. The current pool picture is copied into the view's own texture, and
both views' latest pictures are copied into their halves of the back buffer (`StretchRect`, linear filter). Each
half shows its view's newest frame, so nothing depends on the back buffer surviving Present. Each view runs at
half the frame rate.

Outside play (main menu, loading screens, pause menu, cutscenes with the scene camera) the screen is not split.

### Player 2

Player 2 is a second instance of the campaign class `player`, spawned next to player 1 after some seconds of
uninterrupted play (`TS_SPLIT_P2DELAY`) and placed with the player's own vtable +0x210 (position, direction).
The spawn is `0x655460(no transform, "player", "player", 0)`: the object name names a placed object to build
from, so player 1's own name serves as the template; a name of nothing placed gives no object. Its constructor
takes both player pointers, so they are put back to player 1 right after the spawn.

**Script name.** The spawn makes player 2's script instance (entity +0x80: +4 name, +0xc entity) with the name
`player`, which the script manager refuses ("object 'player' already exist, new one added as unnamed"). The mod
gives the instance the name `player2` and puts it into the manager's name table the way the engine's own insert
does (manager `[0xebedec]` +0x14: an array sorted by name, byte-wise compare `0x428b20`; insert `0x8fb000`).
Through the test console `return $player` gives `player` and `return $player2` gives `player2`, and natives work
on both (`$player2.SetupWeapon(4,0,0)`, `$player2.SelectHolster(0,1)`, `$player2.Kill()`).

**Own weapons.** The player's init spawns its first-person weapons (`wpn_plr_*`, at `0x447a2e`/`0x447a55`), the
hands (`wpn_plr_hands`, `0x442868`) and the third-person body (`player_ghost` "main_char", with its `wpn_mp_*`
weapons). For first-person weapon templates the world's node creation (`0x95dcf0(world, template, transform)`,
called at `0x6551be`) copies the template's shared geometry (template +0xb8, `0x961f30`) but refuses when that
geometry is marked in use (+0xbc, checked at `0x95dd3a`): player 1's weapons hold the mark, so a second player got
no weapons and the engine crashed writing through the NULL (`0x447a5c`) or skipped the rest of the init (`0x40b958`).
While player 2 is made, the mod lifts the mark for the call and puts it back, so player 2 gets its own
`wpn_plr_*` weapons and hands (log: `node for template "wpn_plr_pistol" geometry ... in use ... -> ...`). The
earlier stand-in (the network class `wpn_mp_*`) is kept as a fallback for a spawn that still fails.

**Everything of player 2 runs as player 2.** Its weapons check the local player while they animate and fire. The
mod hooks the task scheduler's message dispatch (stdcall `0x946e40(task, msg, a, b)`, entered by a jump at its
start; the trampoline holds the first 10 bytes): every update and message of player 2 and of the objects made at
its spawn (weapons, hands, holster) runs with the globals swapped to player 2 (nested calls swap once).

**Friendly fire.** A hit on player 1 is message `0x4e3`, delivered after the shot, outside the shooter's update; its
record holds the shooter's scheduler id at +8 (`0x947c10` finds the object). Hits by player 2 or its weapons go
nowhere. Player 2 is not "the" local player to the hit system: a kill of player 2 arrives as event -98 in the
player's event handler (vtable +0x22c, `0x40b120`), which calls the death directly, again with the shooter's id at
+8; one by player 1 or its weapons goes nowhere.

**Death and respawn.** Player 2's death (vtable +0x2e8, `0x410cc0`; also the end of the script's `Kill`) would run
the single-player death: the "you died" screen and the restart for both. Player 2 is put next to player 1 again
instead, with its damage value (+0x468) cleared.

**Level change.** Player 1 is one object for the whole game and survives level changes; player 2 does too (its
script name answers after the change). When the script tick resumes after a level load, player 2 is put next to
player 1 again once the delay has passed.

The swap (also around player 2's update, player vtable +0x13c, `0x4098c0`): both player pointers, player 2's own
camera object (the player's camera code writes `[0xfcabc4]`), its own command context, and the input manager's
look state (+0x28..+0x3b, +0x64..+0x73).

Player 2's command context is a copy of the `GAME` context with its own command entries, so it has the same
bindings, including the pad bindings of `gamepad.c`. The engine fills it with its own command evaluation
(`0x6595e0`). After the normal evaluation for player 1, the mod runs it a second time with:
- the raw input block of player 2 (current and previous codes, per-code state; `_inpINPUT` +0x10..+0x12a8),
  kept between frames, so press and release edges work;
- only the "keyboard" poll enabled, in which `gamepad.c` writes pad 2 into the joystick codes and leaves the
  keyboard and mouse codes idle (`gamepad_poll_player2`);
- player 2's context as the active one.

Player 1's raw input, context and look state are restored afterwards.

## Settings (environment, prototype)

| Variable | Meaning |
|---|---|
| `TS_SPLIT=1` | on |
| `TS_SPLIT_STAGE=1/2/3` | 1: the normal frame in one half; 2: a second camera (player 1's, offset) on alternate frames; 3 (default): player 2 |
| `TS_SPLIT_LAYOUT=v/h` | side by side / top and bottom (default: side by side when the screen is wider than 3:2) |
| `TS_SPLIT_MT=1` | keep the engine's worker threads (the split then stays off and says so in the log) |
| `TS_SPLIT_P2DELAY=s` | seconds after player 1 appears until player 2 is spawned (default 5) |
| `TS_SPLITLOG=1` | frame phases, threads, globals and the game state in the log (2: also the players' weapon sets) |
| `TS_SPLIT_MSGLOG=1` | tests: messages to the players and player 2's events in the log |
| `TS_SPLIT_P2FACE=1` | tests: player 2 is placed 2.5 m in front of player 1, facing it |

Pad assignment is `gamepad.c`'s: player 1 gets the first connected pad (or `[Gamepad] Pad=`), player 2 the next one.

## Test results (2026-10-25, main PC, `ts2_city_part02` loaded through the test console, 1768x992 picture)

Run folders are under `work/runs` (private).

| Milestone | Result | Evidence |
|---|---|---|
| 1. The frame in one half (composite at `0x770f42`) | works; the engine's quad and both halves by `StretchRect` | `20261025-065712` (stage 2, main menu split as well, before the in-play check) |
| 2. Second camera on alternate frames | works: left = player 1, right = player 1's camera moved 1.5 m right (inside a crate there); the HUD in each half keeps its shape (half-shaped canvas) | `20261025-070049` `shot125.png`, `shot175.png` |
| 3. Player 2 | spawned next to player 1 (log: `player 2 spawned`, `player 2 placed`), its own view on the right; pad 2 stick: it walks and turns (frames 110 -> 147 -> 157), RT: while it fires, player 1's half shows the hit screen (blood, red tint at 151; most likely player 2's shots hit player 1, nothing else attacks there); player 1's pad and keyboard still drive player 1 only (163); no crash in 200 s | `20261025-073118` `shot110/147/151/157/163/190.png` |
| 4. HUD per view | each half has its HUD, laid out for the half; the values come from the view's player (pointers swapped for the render span). The crosshair shows only in player 2's half (player 1 has no weapon out at that point) | same |
| Top and bottom (`TS_SPLIT_LAYOUT=h`) | works the same; player 2 (bottom) fires: a bullet hole on the wall in its view | `20261025-073623` `shot157.png` |
| 5. Sound listener | follows player 1 by construction (the listener update runs in the game update, outside the swap); not checked by ear (test runs are muted) | |

Frame phases (log, single-thread frame): the frame start, the composite and the frame end run on the main thread,
in this order, once per frame; the quad call at `0x770f42` is made once per frame. The views alternate 0/1.

Without `TS_SPLIT` the same level load runs as before (control run `20261025-071304`).

### Round 2 (2026-10-25, main PC; driver `lab/split_run.ps1`, console commands `lab/split_cmds*.txt`, pads `lab/split_p2moves*.json`)

| Item | Result | Evidence |
|---|---|---|
| Player 2's weapons | real first-person weapons and hands (template in-use mark lifted at the spawn); after `$player2.SetupWeapon(4,0,0)` the HUD of player 2's half shows the assault rifle and 210 rounds; RT fires (fire messages to player 2's weapon at the weapon's rate, bullet marks on the wall in its view) | `20261025-074222` log (node lines), `20261025-080547` `shot141.png` |
| First-person model of player 2 | **not drawn** in player 2's half (player 1's is drawn in player 1's half) | `20261025-074935` `shot124.png` |
| `$player`, `$player2` | `return $player` -> `player`, `return $player2` -> `player2`; natives on `$player2` work | `20261025-074935` |
| Friendly fire | before: player 1 got a hit message (`0x4e3`) with player 2 as the shooter for each of player 2's shots that hit it (`20261025-081949` log); now each one is dropped (`friendly hit on player 1 by <player 2> dropped`, 8 per burst). Enemies still hit player 1 (shooter ids of AI soldiers), and in some runs they killed it ("you died") | `20261025-082306`, `20261025-083424` |
| Player 2's death | before: `$player2.Kill()` ended the game for both ("you died", `20261025-080936` `shot170.png`); now: `player 2 died: respawned next to player 1`, also for kills by enemies | `20261025-081604`, `20261025-083424` |
| Level change | `$ui.LoadLevel("ts2_city_part03.lg")` with player 2 present: no crash, `return $player2` still answers, player 2 is placed again in the new level and has its own view there | `20261025-083954` `shot235.png`, `20261025-083424` |
| Pause menu | Start on pad 1 opens the pause menu over the whole screen, Start closes it, the split comes back | `20261025-083424` `shot155.png` |
| Bodies | each player's body is **not visible** to the other; clearing bit 0x800 of the body node (+4, set by the player's init on "player_3d") per view did not show it (fixed in round 3: bit 0x10000) | `20261025-081604` `shot136.png` |

### Round 3 (2026-10-25, main PC; stopped early at the session's usage limit)

| Item | Result | Evidence |
|---|---|---|
| Double placement at the spawn (fixed after round 2) | fixed: one `player 2 placed` line at the spawn | `20261025-091255` |
| Each player sees the other's body | works: in each half the other player's third-person model stands where it is (face to face test). The render prep skips nodes with +4 bit 0x10000 in the main view (view entry flags 0, `0x9a1389`); the local player's body has it, and both players are local here. The bit is cleared on the other player's body node from the frame start to the frame end of each view | `20261025-091649` `shot125.png` |

Round 3, continued after the limit reset (main PC, Direct3D 9Ex on, the default now):

| Item | Result | Evidence |
|---|---|---|
| Player 2's first-person weapon | partly: the shared weapon geometry's owner (+0xbc, a node) is set to player 2's node while player 2's objects update and for view 1, and each view hides the other player's first-person nodes (+4 bit 0). Player 1's weapon stays right; player 2's half now shows its hands at the lower right, but not the weapon. Without the hiding, player 2's half showed a weapon and player 1's lost its own (`20261025-101810`) | `20261025-102447`, `20261025-102821` `shot124.png` |
| Player 2's health | found: the player's life is +0x234 (100 at full; each rifle hit took 3, measured with the hit diff `TS_SPLIT_MSGLOG`) | `20261025-103150` log |
| Death and respawn of both | a death of either player becomes a respawn next to the partner with full life (+0x234 = 100), as long as the partner did not die in the last 5 s; when both are down within 5 s the game's own death runs ("you died", the restart). Checked with deaths by damage (enemies, and the other player with `TS_SPLIT_FF=1`) and by `$player.Kill()` / `$player2.Kill()` | `20261025-103525`, `20261025-103902` (log: `player 1 died: respawned next to player 2`, `player 2 died while player 1 was down: the game's death`) |
| Respawn spot | each player's positions are kept every half second in play; a respawn goes to the newest spot of the partner that is at least 2 s old and 1.5 m from it (ground the partner walked on), else beside the partner as before | `20261025-103525` (`player 2 placed on the partner's trail at ...`) |

### Round 4 (2026-10-25, main PC, Direct3D 9Ex)

| Item | Result | Evidence |
|---|---|---|
| Player 2's first-person weapon | works: drawn in its half (and player 1's in its own). Cause: a first-person template shares its geometry with every node made from it (node +0x1c0 = template +0xb8, set at `0x95dd6c`, node +8 0x1000), and the geometry is bound to one node (+0xbc); both players' rifle nodes had the same +0x1c0. For player 2's spawn the template takes the path of ordinary templates, which copy the geometry (`0x961f30`): its name's first letter is changed for the call (`0x95c480` tests "wpn_plr"), and the first-person flag is set on the node as the shared path does. Each view hides the other player's first-person nodes | `20261025-105425` `shot124/141.png`; `20261025-110134` `shot134.png` (player 2 firing, muzzle flash) |
| Death view after a respawn | cleared: the script's `Kill` starts the dying first (vtable +0x370, +0x3a4 (1): the death screen effect); the respawn ends it with the player's own counterparts, +0x3a4 (0, 0) and +0x36c, called as that player. No red or blur after either player's `Kill` | `20261025-105820` `shot148/158.png` |
| Per-view screen effects | works for the low-life tint and blur: player 1 at 10 life has the red, blurred picture in its half, player 2 (84) a clear one while it fires. The post-effect manager (`0x1096f18` = renderer +0x24) has 13 effect objects at +0x14f4 (vtable +4 on, +8 off, +0x14 strength; +8 bit 0 on). The players start and set them by index (`0x665f70`, `0x504b40`; off: `0x665fa0`, `0x504b80`): the death effect is 8, the low-life tint 10 (set every frame from the player's update, `0x446600`). These four entries are replaced: each call is done as before and kept per player (player 2's: made while its swap is on); for each view the effect objects get that view's player's state (on/off, strength) from the frame start to the frame end. `TS_SPLIT_FXSHARED=1` turns it off | `20261025-110134` `shot134.png`, `shot140.png` |
| Respawn spot when the partner has not moved | a respawn goes to the partner's trail (2 s old, 1.5 m away), else to the dying player's own trail near the partner (ground it stood on), never to a fixed offset; player 2 joins (and is placed again after a level change) only once player 1 has walked 1.5 m, on a spot player 1 stood on | `20261025-110431` (`placed on its own trail`), `20261025-111123` `shot135.png` (player 2 joins on player 1's start spot) |

Still open after round 4:
- **HUD values of player 2**: the widgets read the player through the swapped pointers, but whether the ammo counter
  follows player 2 is not shown yet (the reserve stayed 210 in both halves; the clip bar is too small to read in
  the frames). Life has no bar; the low-life tint is now per view.
- **Time-power effects**: they are effect objects too, so they now show in the half of the player who started them;
  whether that is wanted (the world's time is one for both) is to be decided.
- **Split screen with the network** (goal 6 of round 4): not started. What blocks it: the network co-op runs the
  multiplayer game mode (2) and the multi-thread frame, split screen needs the single-thread frame; player 2 is a
  campaign `player` spawned locally, which the server's entity replication (0x42a CREATE_ENT) would send to clients
  as a second campaign player (clients would make a local `player` of it); the network layer's per-player records
  (score, team, respawn) know one local player per machine. A test also needs two machines (the main PC may run
  one game; the N100 was busy). Plan: on the host, keep player 2 out of replication and send it to clients as a
  `player_mp` puppet (as remote players are), with its position from the host's player 2.

## Open problems

- **Player 2's first-person model is not drawn.** Its weapons exist, fire, and their scene nodes are set up
  like player 1's (round 3 log, `20261025-091649`: player 2's rifle and hands at its camera position, +4 bits
  0/1 clear, so not hidden; the only difference is +4 bit 0x4000000 on all of player 2's nodes). Lead for the
  next step: a mesh's geometry +0xbc is not just a flag but the node that owns the shared geometry (the "in use"
  value at the spawn was player 1's own weapon node, e.g. `7ce111c8` = player 1's pistol node), and the mesh
  pass (`0x9a1070`) reads it. Try making player 2's weapon node the owner of each weapon geometry for the span of
  view 1 (record geometry and node in the node-creation wrapper at its spawn, swap +0xbc in the frame start and
  back in the frame end, as for the bodies).
- **Per-view screen effects: not done.** Hit/blood overlays are HUD widgets and follow the view (blood only in
  player 1's half while it was hit, `20261025-081604` `shot136.png`), but the red tint and blur of the damage and
  death effects, the time-power effects and HDR adaptation are global and show in both halves. Their parameters
  still have to be found and kept per player.
- **Player 2's life and pick-ups**: the life is +0x234 (round 3); health pick-ups and time energy for player 2 are
  untested.
- **Respawn placement**: see round 3 (partner's trail; a checkpoint or respawn point would be better).
- **Player 2 is not a player to the game**: AI targets `[0x1095b30]` (player 1), scripts use `$player` (player 1):
  triggers, checkpoints, cutscenes, death and restart see player 1 only. The same audit as for network co-op
  (`player-audit.md`).
- Saving and loading with player 2 are untested.
- The pause menu and loading screens are full screen (UI command context); cutscenes with the scene camera are
  full screen. Player 2 has no menu control (only player 1's pad and keyboard drive the menus).
- Each view runs at half the frame rate; no frame cap yet.
- Multi-thread frame: off in split mode. With the engine's workers the render job overlaps the game update and
  the globals cannot simply be held for the render span; the swap would have to be limited to the frame start,
  the HUD update and the frame end (see the research notes), and live camera reads in the render job checked.
- In the test window the back buffer (1912x1000) is larger than the game's picture (1768x992); the strip outside
  the picture is not cleared (as without the mod).
- HUD: in the side-by-side layout the radar is missing from both halves (it shows in top and bottom); the
  narrow canvas may place it off screen. Not looked into.
- The player's double-tap state (input manager +0xa4/+0xa5) and the rumble of pad 2 are not handled.

## Combining with network co-op (notes)

- The network co-op already needs a second body that the level treats as a player; split screen makes player 2
  from the same `player` class. One "extra player" layer can serve both: a `player` instance whose command
  context is filled by pad 2 (split screen) or by the network (a remote player in co-op), with the same globals
  swap around its update.
- The per-player state (camera object, command context, look state, raw input) is kept in a small record per
  local player in `split.c`; a co-op client would add network-fed records. The render side (alternate frames,
  composite, half canvas) is independent of where player 2's input comes from.
- Spawning: the template issue of first-person weapons applies to any second `player` instance, so co-op's remote
  players should stay `player_mp` puppets on each machine, and only local players are `player` instances.
- Two local players on a co-op client means three or four players in a session: the `$player` audit and the AI
  target choice must take a list of players, not two fixed ones.
- Order of work: make the swaps also safe for the multi-thread frame first (co-op runs the normal frame), then
  per-player screen effects, then the player list for scripts and AI.
