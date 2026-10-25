# Local split screen (prototype)

Two players on one PC: player 1 on keyboard, mouse and pad 1, player 2 on pad 2. The screen is split side by
side on wide screens and top and bottom on others. This is a prototype, off unless the environment variable
`TS_SPLIT=1` is set. The code is `native/split.c`; the static research behind it is in the private notes
(`work/re/splitscreen.md`).

Status (2026-10-25, prototype, main PC, `ts2_city_part02`, single player): both views render on alternate frames
into their halves with a HUD each; player 2 is spawned next to player 1 and moves, turns and fires with pad 2.
Not ready for players: see "Open problems".

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
takes both player pointers, so they are put back to player 1 right after the spawn, and its scene node gets the
name `player2` (the single-player init names it `player`; its script instance has no name in the log, so `$player` should stay
player 1; not checked through the console). Player 1 is one object for the whole game: it
survives level changes. Player 2 should be removed with the level and made again in the next one (not tested yet).

The player's init spawns its own objects, and a second player cannot get all of them: the first-person weapons
(`wpn_plr_pistol`, ...) and the hands (`wpn_plr_hands`) are made from a placed template (`nameTpl`) that
apparently only one object can use. The spawn returns no object, and the engine then writes through it
(`0x447a5c`) or skips the rest of the init (later a NULL at player +0x478, `0x40b958`). For player 2 the mod
replaces these spawns (calls at `0x447a2e`, `0x447a55`, `0x442868`): a failed `wpn_plr_X` becomes
`wpn_mp_X`, the network players' class of the same weapon (parent `wpn_plr_X`, the AI's third-person model as
template), and the hands become an empty holster. Player 2's body (`player_ghost`, template `main_char`) is
made normally.

Every frame, player 2's update (player vtable +0x13c, `0x4098c0`) runs with the globals swapped to player 2:
both player pointers, player 2's own camera object (the player's camera code writes `[0xfcabc4]`), its own command
context, and the input manager's look state (+0x28..+0x3b, +0x64..+0x73).

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
| `TS_SPLITLOG=1` | frame phases, threads, globals and the game state in the log |

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

## Open problems

- **Player 2's first-person weapon** is the network weapon (`wpn_mp_*`), whose third-person model hangs in front
  of player 2's camera as a large dark shape. The first-person templates allow one object. Needed: a
  first-person model per player (a copy of the template, or the weapon model of player 1 drawn for the other
  view), or hiding the model in player 2's view.
- **Global screen effects**: the damage/blood screen and the red tint appear in both halves (they are global
  render state); time powers' screen effects and HDR adaptation are global too. They need to be kept per player
  and swapped per frame (as in Jericho).
- **Friendly fire**: player 2's shots most likely hit player 1 (see test 3); to be checked and turned off.
- **Player 2 is not a player to the game**: AI targets `[0x1095b30]` (player 1), scripts use `$player` (player 1):
  triggers, checkpoints, cutscenes, death and restart see player 1 only. The same audit as for network co-op
  (`player-audit.md`).
- Player 2's death, health pick-ups, time-power energy, and saving are untested.
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
