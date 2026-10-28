# Local split screen (prototype)

> **Resume here (2026-10-28):** all code is committed. New: the Co-op menu's **Continue** (round 23, `docs/coop-continue.md`;
> host side proven on the main PC, with life/energy, a per-profile record `coop\continue-<profile>.txt` kept across a
> reinstall, damaged/old records ignored, hint texts in 9 languages; a client joining a continued session ran on the N100
> pair, part03: no crash, the client next to the host at the checkpoint). Split on a client, N100 run 2026-10-28
> (`lab/split_pair_t3.ps1`, `20261028-043741`): a 50 s walk of the copy stayed in the part02 courtyard (walls: ~8 m, no
> long open path tried yet), the host's slave's death/respawn ran (`player 2 died: respawned next to player 1`) without a
> crash; the pick-up test found the level's pistol pick-up already taken by player 1, so a pick-up by the copy is still not
> shown (use `TS_SPLIT_ITEMTEST=nearitems`). Second N100 slot (`lab/split_pair_t5.ps1`, `t6.ps1`; runs `-050409`, `-051811`,
> `-052504`): the copy's follow of the host's player 2 measured over a 2 min wall-following walk on part03: median 0.55 m
> from it (the 0.6 m design offset), at most 1.2 m while walking, no jitter or lag seen, camera and HUD in its half fine;
> but the walk stayed in the part03 start/stairwell area (path 24 m, about 12 m across), not an open 25 m. Pick-up: on the
> host the item update reaches player 2 (`item_grenade_pack ... in player 2's reach: not taken`), player 2 put on the
> level's ammo box (ammunition 30 -> 30, the copy's HUD stays 14) took nothing: **a pick-up by the copy is still not shown**
> (the split host's pick-up worked in round 8; the remote-host player 2 does not take). The split-client gaps stay open.
> Items for the host-side player 2, third N100 slot (2026-10-28): the plain split host takes items for player 2 (main PC,
> `lab/item_host_run.ps1`, run `-130617`: `item_grenade_pack ... in player 2's reach: taken by player 2`, the debug line shows
> the record handle found, `+0x208` check 1, filter `+0x248` 1). On a client's session the host's player 1, standing next to
> player 2, takes each drop first (`pick-up .. by player 1: taken`, then `player 2 ... not taken` because the item is gone,
> runs `-133030`, `-131420`), which explains most of the earlier "not taken" lines; the swap of player 1's record handle and
> the filter work for it too (`record handle` non-NULL, filter 1). Not proven: a pick-up by a client's player 2 with player 1
> out of reach (one run, `-131854`, a pack in reach and not taken; the later runs had no drop near it). `TS_SPLITLOG` now logs
> the stages (`item .. debug`, positions, flags) every 5 s for an item in player 2's reach on a server. Gap stays: *a pick-up
> by player 2 of a client is not shown*. Done earlier: the split-screen soak
> through the 24 levels (round 21), the level-22 crash = address space, fixed by the large-address-aware flag the installer
> sets (round 22; a LAA host + client join on the N100 was clean), split on a client (round 19, basics), the Co-op menu
> option on a host (round 20). Left, needs the N100 (host) and the main PC (client with virtual pads): items, pick-up
> messages, death/respawn of the client's copy, a long walk on a client (`lab/split_pair_t1.ps1`); a client joining a
> continued session. Open: level 20, player 2 keeps its weapon where the story takes player 1's away; the installer's exe
> step is tested on `work\install_test` only (not on a real install).


Two players on one PC: player 1 on keyboard, mouse and pad 1, player 2 on pad 2. The screen is split side by
side on wide screens and top and bottom on others. The player turns it on in the mod's Co-op menu: **Co-op ->
Split screen** shows the campaign's chapters (as the Co-op host frame) and the difficulty; **Start** starts the
chapter as a single-player game with split screen on. Without a second pad the frame says so (`COOP_ERR_PAD2`)
and starts nothing. Going back to the main menu ends split screen (player 2 is removed). The environment
variable `TS_SPLIT=1` still forces it on for tests (`TS_SPLIT=0`: never). The code is `native/split.c`; the
menu is `data/ui/coopsplit.ui` and `data/coop_menu.py`, its texts `data/coop_strings.py`; the static research
behind it is in the private notes (`work/re/splitscreen.md`).

Status (2026-10-25, prototype, main PC, `ts2_city_part02`/`part03`, single player): both views render on alternate
frames into their halves with a HUD each; player 2 is spawned next to player 1 with its own first-person weapons,
moves, turns and fires with pad 2, has the script name `player2`, cannot hurt player 1, respawns next to player 1
instead of ending the game, stays through a level change, and each player sees the other's body. Started from the Co-op menu (round 5); not finished: see "Open problems".

## For players (summary)

| | |
|---|---|
| What you need | two gamepads (XInput). Player 1 plays with keyboard and mouse or pad 1, player 2 with pad 2 |
| Two players on one PC | main menu -> **Co-op** -> **Split screen**, pick the chapter and the difficulty, **Start**. The screen splits side by side on wide screens, top and bottom otherwise. Back to the main menu ends it |
| Two players on one PC in a network co-op game | in the **Co-op** frame set **Second player on pad 2** to On (kept in the profile), then host or join a co-op game as usual. **Hosting**: your screen is split, player 2 plays on your PC and is seen by the other players (tested with `TS_SPLIT=1`, which this option now sets in a hosted game; the option itself on a host is not run yet). **Joining**: the host runs your second player, your screen is split (the host side is tested, the joining PC's second half is not tested yet, see round 18) |
| Each player has | an own camera, first-person weapon, crosshair, HUD (life, ammunition, grenades, time energy), screen effects (damage, death, low life), pick-up messages, pick-ups and weapons; prompts name the button of the player's own device |
| Time powers | either player's power works for everybody (as in co-op) |
| Death | a player who dies comes back beside the other one; both down together restarts as in the game (single player); in a network game player 1 follows the network rules |
| Not per player | the pause menu and loading screens (full screen, player 1 controls them), cutscenes (the scene camera, full screen), hint boxes (one for both). The scripts and the AI see player 1 as "the player" (e.g. which player an AI chases or a trigger waits for) |
| Performance | each half is drawn every second frame (the frame rate of each half is half the game's) |

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
| `TS_SPLIT=1` | on from the start (tests); `0`: never; unset: the player's choice in the Co-op menu (UI string `CoopSplit`) |
| `TS_SPLIT_STAGE=1/2/3` | 1: the normal frame in one half; 2: a second camera (player 1's, offset) on alternate frames; 3 (default): player 2 |
| `TS_SPLIT_LAYOUT=v/h` | side by side / top and bottom (default: side by side when the screen is wider than 3:2) |
| `TS_SPLIT_MT=1` | keep the engine's worker threads (the split then stays off and says so in the log) |
| `TS_SPLIT_P2DELAY=s` | seconds after player 1 appears until player 2 is spawned (default 5) |
| `TS_SPLITLOG=1` | frame phases, threads, globals and the game state in the log (2: also the players' weapon sets) |
| `TS_SPLIT_MSGLOG=1` | tests: messages to the players and player 2's events in the log |
| `TS_SPLIT_P2FACE=1` | tests: player 2 is placed 2.5 m in front of player 1, facing it |
| `TS_SPLIT_FXVIEW=8,10` | the screen effects (post-effect indices) kept per view; the others show in both halves |
| `TS_SPLIT_FXSHARED=1` | tests: all screen effects as the game has them (both halves) |
| `TS_SPLIT_ITEMTEST=<what>,<s>` | tests: <s> seconds after player 2 came in: `list`/`listall` log named objects (30 m / all), `nearitems` puts player 2 on the nearest recorded pick-up nodes one after another, `@name`, `name`, `#x/y/z` ("+"-separated, `!` = player 1) put a player on that object or spot; ammunition and life are logged before and 3 s after each |
| `TS_SPLIT_FPNOSCALE=1` | tests: the weapon pass keeps the half's horizontal angle (first-person weapons cut at the sides) |
| `TS_SPLITLOG=3` | tests: also every scene node made with its template, and the callers of a pick-up |
| `TS_SPLIT_HUDNEXT=0` | tests: the HUD weapon panel takes player 2 on the other frame parity (default 1 is right) |

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
| Respawn spot when the partner has not moved | a respawn goes to the partner's trail (2 s old, 1.5 m away), else to the dying player's own trail near the partner (ground it stood on), never to a fixed offset; player 2 joins (and is placed again after a level change) only once player 1 has walked 0.8 m, on a spot player 1 stood on | `20261025-110431` (`placed on its own trail`), `20261025-111123` `shot135.png` (player 2 joins on player 1's start spot) |

Still open after round 4 (all four taken up in round 5 below): ammunition looked shared, player 2 waited for
player 1 to move, time-power effects showed in one half only, split screen with the network.

### Round 5 (2026-10-25, main PC, Direct3D 9Ex; the Co-op menu entry; menu runs driven with virtual pad 1)

| Item | Result | Evidence |
|---|---|---|
| Co-op menu entry | **Co-op -> Split screen** (new frame `CoopSplit`, `data/ui/coopsplit.ui`): the 24 chapters with their pictures, a hint, the difficulty (kept with the host frame's), Back / Start. Start sets the UI string `CoopSplit` = "1", the difficulty, and loads the chapter (single player). The DLL's hooks are always in place and pass everything through while it is off; the script tick reads `CoopSplit` once a second and turns split screen on (single-thread frame) or off. Texts in the 9 string tables (`COOP_BTN_SPLIT`, `COOP_FRAME_SPLIT`, `COOP_HINT_SPLIT`, `COOP_ERR_PAD2`, `COOP_BTN_START`); `build_pak.py` passes | `20261025-114051` `shot083.png` (frame), `shot139/159.png` (split screen after Start, player 2 in); `20261025-113601` `shot129.png` (Co-op menu with the entry) |
| No second pad | the frame shows `COOP_ERR_PAD2` and starts nothing. The DLL tells the menu (UI string `CoopPad2`) after each input poll (`gamepad.c` `gamepad_after_poll`), which runs in the menus too (the script tick does not); pad 2 unplugged -> message, plugged in again -> Start works | `20261025-113601` `shot193.png` (message), log `split: on (chosen in the Co-op menu ...)` |
| Leaving split screen | the main menu clears `CoopSplit`; player 2 is removed then. A player's removal (message 2, vtable +0x144 = `0x4082f0`) stops the game on purpose unless bit 2 of the game flags `[0xfcace8]` is set (single player never sets it; the first try crashed there, `20261025-114051`); for player 2's own removal the bit is set for the call. Back in the menu: `player 2 removal message handled`, `player 2 destroyed`, no crash | `20261025-114805` log 11:55:43 |
| Time-power effects | in both halves (the world's time is one for both): only the player's own condition stays per view: damage tint and blur (effect 6), death (8), low life (10); `TS_SPLIT_FXVIEW` overrides the list | `20261025-112535` `shot129.png` (time freeze grey in both halves); `20261025-120358` `shot146.png` (player 2 at 23 life: red only in its half), `shot152.png` (both low: both red) |
| Joining without player 1 moving | player 2 comes in 3 s after the delay even if player 1 stands still: "ahead of the partner" (1.5 m along player 1's horizontal facing, the last-resort spot of `place_player`); with player 1 walking, on its trail as before | `20261025-112535` and `20261025-114805` logs (`player 2 placed ahead of the partner`, 8 s after the level start) |
| Ammunition | each player already has its own weapon set and ammunition: player vtable +0x248 -> weapon set, its virtual base's vtable +0x4c (0 primary, 1 secondary) -> record +8 total (float), +0xc magazine; player 2 firing took only its own (240 -> 210), player 1's stayed, and the other way round. Only the HUD was shared: its weapon panel (update `0x5bbcd0`, vtable slot `0xda1f84`) reads `[0xfc8180]` in the UI update, outside the view span. The panel is now updated for the view the next frame draws (views alternate per frame: `frame_no & 1`) with that view's player: each half shows its own counter and grenades | `20261025-115824` log (records per player), `20261025-120358` `shot136/138.png` (210 left, 180 right) |
| Pick-ups | by code the ammunition and health pick-ups are methods of the player that touched them (`0x441c90`, `0x4433d0`, called on the toucher), so they go to that player's own weapon set; not yet seen in a run (no pick-up reached with the test pads). The "ammo full" message goes to the shared HUD message line | `work/re/pickup.c` |
| Deaths by damage | no tint or blur stays after a respawn: player 2 died twice and player 1 three times by enemy fire, both halves clear after each respawn | `20261025-112535` `shot151/155.png` |

### Round 6 (2026-10-25, main PC; defects found in the round 5 frames, HUD, saves)

| Item | Result | Evidence |
|---|---|---|
| Repeated strip at the right edge | the test window's back buffer (1912x1000) is larger than the game's picture (1768x992; the composite quad and the frame texture are 1768x992, texture coordinates 0..1); the strip right of x 1768 kept an old picture, which looked like a repeated piece of the right view. The composite now clears everything outside the picture (and copies only the drawn part of the frame texture, from the quad's texture coordinates, in case they are below 1). A player's window has the picture's size, so the strip only exists in test windows | `20261025-121947` `shot131.png`: player 2's rifle and muzzle flash run into the picture's right edge and are cut there, black beyond, nothing repeated |
| Second crosshair in player 1's half | the crosshairs are scene nodes too (templates `crs_*`, made with each player's weapons and kept in front of the player's camera): player 2's floated at player 2's head in player 1's half. The crosshair nodes are recorded at their creation per player and each view hides the other player's, as with the first-person nodes. One crosshair per half, at its centre | `20261025-121947` `shot124/128/138.png`, `20261025-124717` `shot124/138.png` |
| Player 2's spot when nobody walked yet | spots 1.6 m behind, right, left, behind right, behind left of player 1, ahead only last. The placement (+0x210) puts the player on the floor under the spot and writes the spot back: a spot is taken when it comes back on player 1's floor level (within 0.45 m), not moved aside (0.5 m) and not inside player 1 (1 m). In `ts2_city_part02` the first one (behind) is taken: 1.6 m behind player 1 on the ground (the old spot ahead came back 1 m up, on the crate) | logs `player 2 spot behind the partner: asked .. 1.01 .., got .. 0.57 ..`; `20261025-124717` `shot100.png` (player 2 sees player 1's back; player 1 sees the courtyard) |
| Other HUD widgets | every HUD widget class whose update reads `[0xfc8180]` (14 classes, vtables of the HUD classes `0x59e000-0x5e2000`, slot +0x34; a thunk per class) is updated for the view the next frame draws with that view's player: weapon panel, life and armour, hit arrows, time-power widgets, ... | `20261025-124717` `shot100.png` (before the rifles: player 1 unarmed, empty panel; player 2's pistol 112 in its half), `shot138.png` (210 / 180) |
| Time-power bar | **still shared while a power runs**: player 2's time stop drains only player 2's energy (player +0x2ac: player 1 stays at 1.00 in the log), but the bar in both halves goes down and back up; the bar shows more than +0x2ac (not found yet). The power icons light up in both halves (the world's time is one) | `20261025-124717` log 12:49:20-12:49:38 and `shot128/130/132.png` |
| Pick-ups | **not shown in a run**. By code the pick-ups are methods of the player that touched them (`0x441c90`, `0x4433d0`). Tries: an `item_arifle` spawned on player 2's spot crashed in the node creation (wrong transform; the test was removed, `20261025-122418`); `$player2.MoveTo("pistol1")` (a placed `item_arifle`) did nothing; player 2 walking towards the dropped weapons was stopped after 2 m | `20261025-122418`, `20261025-122936`, `20261025-123537` |
| Saves | `$saves_mng.AutoSave(-1)` in split screen: Ok, `CanSave` true; `$saves_mng.LoadLastGame()`: the level reloads, no crash, split screen goes on and player 2 comes back next to player 1 (placed on its trail), **without weapons** (the save holds player 1's equipment only; player 2's weapon panel is empty, no first-person weapon, `Y` brings none). Removing and remaking player 2 after the load hung the game (`20261025-123537`), so player 2 stays unarmed; if a load gives it a new body, its recorded objects are forgotten | `20261025-122936` `shot175/195.png`, `20261025-124206` `shot183/193.png` and log (`player 2 weapon set .. total 0.0`) |

### Round 7 (2026-10-25, main PC; player 2's equipment after a load, the time-power bar, pick-ups)

| Item | Result | Evidence |
|---|---|---|
| Player 2's equipment after a load | after a level load player 2 without a weapon gets its weapons and hands made again with the player's own set-up (vtable +0x25c = `0x442840`: weapon set-up `0x4478e0` on player +0x328, then the hands), run as player 2 with the spawn hooks in their player 2 mode (own first-person geometry, recorded objects). At every level start player 2 gets player 1's three weapon slots (player +0x3a8/+0x3ac/+0x3b0, what the level script or the save gave player 1) through `SetupWeapon` when they differ (never an empty set). After `LoadLastGame`: `player 2's weapons made again after the load: ok, 11 objects`, `Y` brings the rifle (240), firing takes it to 220; after a level change (`ts2_city_part03`) player 2 keeps its weapons (rifle 210) | `20261025-130230` log 13:05:27-13:05:47, `shot197.png`; `20261025-141144` `shot197.png` (player 2 firing after the load), `shot272.png` (part03, rifle) |
| Time-power bar | the bar is the TimeGrenadeBar widget (class `0xda19f0`, update `0x5b8610`): it puts the player's energy (player +0x2ac) into its ProgressTime bar's target (+0x298 -> +0x108), and the bar moves its shown value (+0x10c) there over some frames in its own update. With one widget for both views the shown value ended between both players' energies. The shown value is now set to the view's player's energy at once (its motion stopped). The power icons stay shared (the world's time) | `20261025-141144` `shot128/132/138.png`: player 1's bar full, player 2's 25 % / empty / 20 % (log: player 2 0.97 -> 0 -> 0.36, player 1 1.00), the stop icon lit in both halves |
| Pick-ups | **not shown for player 2.** The way found: a pick-up item calls the toucher's vtable +0x17c (`0x420470`: item class check, +0x2a4 decides, +0x284 takes), from the item's own method `0x8ad570`; the mod logs each call with the player. One real pick-up by player 1 was logged (`pick-up .. by player 1: taken`, `20261025-134528`, 13:47:23, next to the level's ammo box). Player 2 was put on the level's pick-ups found in the script name table and the world's scene nodes (ammo box `s3d_refLocator55|ammobox`, the dropped pistol prop `wpn_drop_pistol`, a policeman's rifle dropped with the game's `DropWeapon()`, ground objects), with `X` (use) pressed: no +0x17c call for player 2 at all. What lets an item notice its toucher (possibly the local player only) is the next step | `20261025-131823`, `-132055`, `-132850`, `-133456`, `-134528`, `-134917`, `-135144` |
| Join spot at a level start | trail spots must be beside the partner (1-12 m away horizontally, within 1.2 m in height); spots are tried at 1.6 m and 0.9 m. `ts2_city_part03` starts player 1 about 100 m up on something moving (its height swings by half a metre) with the ground below every spot around it: player 2 waits on the ground below and is placed again every 2 s (putting it on player 1's own spot pushed player 1 off and both died) | `20261025-140722`, `20261025-141144` log 14:15:38 |

### Round 8 (2026-10-25, main PC; pick-ups for player 2, the ts2_city_part03 start, other levels)

| Item | Result | Evidence |
|---|---|---|
| Pick-ups for player 2 | found why none reached player 2: an item's update (`0x4f5aa0`, item vtable +0x13c) gives itself to the players of the game's player list (`[0xfcab34]` +0x28, 0xa4-byte records, +4 the player: the network players' list, which in single player holds player 1 only), through vtable +0x16c (in reach) and +0x20c (`0x8ad570`: check, the player's take +0x17c, done). The item update is hooked (its two vtable slots and the two calls of the subclasses): after it, the same is done for player 2, as player 2. Player 2 (pistol 102) on the level's pistol pick-up: `pick-up .. by player 2: taken`, its counter 96 -> 112 in its half, player 1's (unarmed) panel unchanged | `20261025-152204` log 15:23:50 and `shot105/107/109.png` |
| `ts2_city_part03` start | the cause of the 100 m start: a level start puts both players on the level's player start; two bodies in one spot threw both 105 m up, where they hung (player 1's half was red from that). At the first tick of a level player 2 is now moved beside player 1 when both stand on one spot. Both start on the ground, player 2 1.6 m beside player 1, no red. For a partner on something the floor ray does not see, player 2 is put 1 m beside it at its height without the ray and kept there if it stays (checked after 1.5 s, 3 tries), else on the ground below with a new try every 2 s | `20261025-152900` log 15:31:30 (`both players on one spot at the level start`), `shot160/190.png` |
| Opening scene of `ts2_city_part01` | the suit's start-up and the scripted arrival run with UI +0xc0 bit 0x10 (the game's camera unused at 0,0,0) and no cutscene flag: they were drawn into both halves (the same picture twice). That bit now counts as a cutscene: full screen | `20261025-153248` (before), `20261025-153629` `shot100-190.png` (after: full screen), `shot215.png` (split screen in play) |
| First-person pistol missing in a half | the weapon pass (`0x680730`) takes its vertical angle from WPN_FOV and the horizontal one from the picture's shape (+0x154, the half's): in a side-by-side half the pistol at the lower right fell outside (the rifle was cut). For a split view the weapon pass now keeps the whole screen's horizontal angle with the matching vertical one (at its projection rebuild, the call at `0x680917`): the weapon is drawn smaller, in its usual place. `TS_SPLIT_FPNOSCALE=1` turns it off | `20261025-154504` (no split: the pistol), `20261025-155515` (split, missing), `20261025-160120` `shot105.png` (both pistols) |
| `ts2_zepride1_part01` | split screen works on the zeppelin (player 1 at the gun turret, player 2 in the cabin, each with its HUD; the ship's status widget in both halves) | `20261025-160329` `shot100-195.png` |

### Round 9 (2026-10-25, main PC; the zeppelin's right half, HUD layout in a half)

| Item | Result | Evidence |
|---|---|---|
| Zeppelin right half "two pictures" | not a second render target: the dark column with the green screen is the cabin's own geometry (the same column stands at the right of the picture without split screen, `20261025-161607`, and moves with player 2's view when it turns, `20261025-161201`). The real defect was the HUD: the zeppelin status widget and the hints were laid out for the whole screen and landed at a half's right edge (cut, and looking like a second picture's HUD). | `20261025-161607` (no split), `20261025-161201` `shot102-120.png` |
| HUD laid out for the half | the HUD's elements are laid out once when the HUD is made (element vtable +0xbc = `0x6ffc30`, each element a share of its parent, the HUD frame takes the canvas). It is now laid out again for the half's canvas when split screen starts drawing (and back for the whole screen when it stops), and the half's canvas is kept between frames. In both halves now: the radar (missing before in side-by-side halves), the zeppelin status widget next to it, the hint ("Новое задание.") whole inside the half | `20261025-162434` `shot100/120/160/180.png`, `20261025-162851` `shot140.png`, `20261025-163139` `shot110.png`, `20261025-163337` `shot110.png` |
| Player 1's HUD on the zeppelin deck at the start | the same without split screen: the time/weapon panel is not shown there (only the zeppelin status and the radar) | `20261025-161607` `shot095-135.png` |

### Round 10 (2026-10-25, main PC)

| Item | Result | Evidence |
|---|---|---|
| Viewer's own body | for each view the viewer's own body gets the "local body" bit (node +4 0x10000, skipped in the main view) and the partner's body loses it, both put back at the frame end; the game marks only player 1's body, so in player 2's view player 2's own body could be drawn. Player 2 turned away from player 1: no body in its half | `20261025-165935` `shot110.png` (skyscraper), `20261025-170135` `shot125/140.png` (part02) |
| Message line per player | **not done.** The owner of each message is known (the message manager `[0x1095ec8]` vtable +0xc is hooked: inside player 2's swap, a pick-up by either player, or player 1's own objects; the log says `HUD message for player 2`), but hiding the text in the other half failed: the text is the HUD element `MessageText` (with the hint boxes `MessageBox`, `MessageBoxBig`), and setting its hidden bit (+0x18 bit 1) after its update, for the drawn view or the next one, did not remove it from the picture. The hiding is switched off; messages show in both halves as before | `20261025-164210` to `20261025-165705` |
| `ts2_city_part01` after the opening | the same without split screen: no HUD and no first-person weapon in the level's scripted wake-up | `20261025-170432` `shot215/240.png` (no split) |
| Frame rate | measured in split screen on the main PC (`ts2_city_part02`, test window 1912x1000): 67-73 frames/s, so each view 34-37 updates/s. Drawing both views every frame would cost the same two scene passes per shown frame (about 35 frames/s with both views each); the alternating views stay | `20261025-164210` log (`frames/s`) |

### Round 11 (2026-10-25, main PC)

| Item | Result | Evidence |
|---|---|---|
| Viewer's own body | checked with player 1 moved 18 m away (`TS_SPLIT_ITEMTEST=!#187.8/0.5/102.8`): player 2 looking ahead and down sees no body of its own. Log per view: own body node +4 with 0x10000 set (player 1 `422f2810` in view 0, player 2 `662f2810`/`64212810` in view 1), the partner's cleared (`662e2810`, `422e2810`) | `20261025-171010` `shot100/103.png` and log (`view 0: own body ..`) |
| Message line per player | done: the pick-up lines are drawn by the HUD's console element `ConsoleText` (class 0xd9f300, draw slot +0x28 = the text draw 0x706430; also `MessageText`). Its draw is skipped in the other player's view for 10 s after a player's own message (owner: player 2's swap, a pick-up by either player, player 1's own objects). Messages from elsewhere stay in both halves | `20261025-172638` `shot109/112.png` ("Получено: Пистолет КМ-33" only in player 2's half), `shot116.png` (gone) |

### Round 12 (2026-10-25, main PC): split screen on a co-op host (no client yet)

Host started by the lab recipe (`AutoTest="host"`, `AutoTestMap="ts2_city_part02"`, through the mod's Co-op menu),
`TS_SPLIT=1`, player 2 on pad 2.

| Item | Result | Evidence |
|---|---|---|
| Split screen in the multiplayer game mode | works: game mode 2, the single-thread frame forced by the split screen (as in single player), both views, HUDs, weapons, player 2 spawned 1.6 m behind player 1; 62-67 frames/s (31-34 per view) | `20261025-173616` `shot100-160.png`, log (`frames/s`) |
| Player 2 for clients (prepared, needs the client test) | the game already shows a `player` to clients as `player_mp`: the player's create descriptor (vtable +0x38 = `0x40b790`) names its class through vtable +0x334, which in a network game gives the multiplayer class; the join list (`0x6525e0`) and CREATE_ENT (`0x652c00`) take every isSyncMP entity with a net id, and `player` is isSyncMP (+0x98 bit 0; player 2 gets net id ~1560, role 0x1846). The mod adds the server's regular-update bit (role 0x10, which player 1 has and player 2 lacked) so its moves are sent. `TS_SPLIT_NETLOCAL=1` keeps player 2 out of the network game instead (isSyncMP cleared on player 2; its body and weapons are not isSyncMP) | `20261025-175539` log (`player 2 networked as a puppet for clients: net id 1572, sync flags 00000073, role 00001846 -> 00001856 (player 1 0000105e)`), `20261025-174755` (the hidden variant) |
| Deaths in a network game | player 1 dies by the multiplayer rules (death screen, respawn countdown, the session's respawn points moved by storysync's checkpoints); player 2, which those rules do not know, is always respawned next to player 1 (before: the "both down" rule called the game's death for player 2, which left it a dead object without HUD). The multiplayer texts (`mp*`: death and respawn countdown) are not drawn in player 2's half | `20261025-175539` `shot128-185.png` (player 1's red death screen and countdown only in its half, player 2 keeps its HUD and weapon), log (`player 1 died in a network game`, `player 2 died: respawned next to player 1`) |
| Needs the client | player 2 as a `player_mp` on the client (position, weapon, death), its shots and damage on the client, time powers of player 2 seen by the client, story sync gates with three players | - |

### Round 13 (2026-10-25, main PC, co-op host)

| Item | Result | Evidence |
|---|---|---|
| "Ожидание сервера." in both halves | its source was player 2: the player's update (`0x40c020`) shows GAME_WAIT_FOR_SERVER while its +0x39c bit 1 is clear, the server's "you may play" mark, which the game sets on player 1 only; player 2, updated as the local player, showed it all the time. Player 2 now gets player 1's mark in a network game. Gone from both halves | `20261025-180342` `shot125-170.png`, log (`player 2's server mark 0 set to player 1's 2`) |
| Death screen per half | player 1's death screen (red, "Ожидание возрождения", countdown, prompt) only in its half; player 2 respawns next to player 1 at once (no countdown of its own) | `20261025-180342` `shot137/141/145.png` (player 1 dead), `shot161/163.png` (after player 2's death) |
| Host without split screen | no "Ожидание сервера." during the host's death; the multiplayer death screen and prompt as the game has them | `20261025-180700` `shot137-145.png` |
| Respawn prompt with a pad | the prompt is GAME_PLAYER_IS_DEAD_MP_FORCE_PC ("Нажмите {PARAM}", {PARAM} the first binding of the fire command: the mouse button); the game has pad variants (GAME_PLAYER_IS_DEAD_MP_FORCE "Отклоните {PARAM}") used on consoles. Not changed yet (the pad's RT does respawn, fire is bound to it) | strings tables |

### Round 14 (2026-10-25, main PC): prompts name the pad button

| Item | Result | Evidence |
|---|---|---|
| Respawn prompt and other "press {button}" prompts | the game picks the _PC strings when it loads the tables (`0x6901d0`: a key ending in _PC replaces the plain one), so the text stays; the button comes from the command's first binding (`0x707e30` copies slot 0 of the command's entry in the GAME context, a HUD element's naming method vtable +0x110 = `0x709bf0` names it as CTRL_<key name>). When the player whose half is drawn last used a pad (`gamepad_pad_active`, gamepad.c: pad input newer than keyboard/mouse input; player 2 always), the naming gets the command's pad binding (slot 3/4) instead. The pad codes are named PAD_* (gamepad.c), and CTRL_PAD_* strings in all 9 tables give "RT", "RB", "X", "D-pad up" ... | `20261025-182729` `shot141.png` ("Нажмите RT, чтобы возродиться сейчас."), `20261025-183014` `shot100/104.png` (time-power hint "нажмите RB"), log (`prompt in view 0 names pad code 0x10d instead of 0x21`: use) |

### Round 15 (2026-10-25, two PCs): split-screen host on the main PC, client on the N100

Driver `lab/split_net.ps1` (split host via the Co-op menu, `TS_SPLIT=1`, virtual pad 2; the N100 joins with its
own profile identity once player 2 is in; profiles cleaned at the end).

| Item | Result | Evidence |
|---|---|---|
| Player 2 on the client | a `player_mp` (net id ~1575, owner -2) by the game's own replication: position follows the host's player 2 (moved 206.83 -> 205.80 on the host, the same on the client a few seconds later; back to its respawn spot after its death), life (76 -> 100 after its respawn), the white suit with its rifle seen from the client (`TS_SPLIT_WATCH=1` puts the client's player 4 m from it). Without a client identity of its own the N100 was refused (UI_NET_ERR_DUPLICATE_PROFILE) | `20261025-190909` and `20261025-191606` client logs (`remote player .. net id 1575 owner -2 at ..`), host logs (`player 2 camera at ..`), `20261025-192307` `client/shot157.png` |
| Player 2's damage to AI | its hits reach the server's AI like player 1's (`player 2 hit .. (net id 1113)`); AI deaths go to the clients by aisync. A death caused by player 2 was not caught on the client (the soldiers near the start were dead before) | `20261025-192307` log |
| Player 2's time power on the client | **not synced**: timesync takes the host's power from `$player` (player 1) only, so player 2's freeze runs on the host and is not sent. Needs timesync to read player 2's power controller too (the network agent's file) | `20261025-191606` |
| Story events | still replayed to the client (sub 21 sent 3, received 3) | `20261025-192307` logs |
| Split screen on a client | **not in the first release**: the server knows one player per connection (0x429 creates one `player_mp` per client slot); a second player from one client would need a second connection from the process or a mod-message player driven by the client and made on the host | - |
| Plain join over the LAN (host N100 on part03, client main PC) | first try: the host closed the connection 24 s after the join while the client still loaded the level (`local close`, "client 1 left before it ran the level"); second try: joined, in the level (the client was shot dead at the start and saw the respawn countdown) | `20261025-193624` (dropped), `20261025-193931` `shot100-155.png` |

## Split screen on a client (round 16, code; not tested yet)

Design: a client keeps one player of its own (player 1, as every client). Its **player 2 is a campaign `player`
that the host runs**, the same way the host runs its own split-screen player 2, so the host keeps the authority
over movement, hits, damage, deaths, items and time powers, and the game replicates player 2 to every machine as a
`player_mp` (proven for the split host in round 15).

| Step | Who | How |
|---|---|---|
| Pad 2 to the host | client | mod message 60 `{u8 1, u8 on, XINPUT_GAMEPAD}` at up to 30 Hz and on every change, also in menus and loading (an idle pad) so the host keeps player 2 |
| Player 2 on the host | host | the first client asking gets a player 2 (`remote_host`): spawned as the split host's, placed next to that client's own player (its `player_mp`, from the game's player list), respawned next to it; the received pad goes into player 2's command context in place of a local pad 2 (`gamepad_set_player2_pad`). No split screen on the host (its screen stays whole); player 2's own screen effects (damage, death, low life) are kept off the host's screen. A host with split screen of its own refuses (one player 2 per host). No pad message for 3 s, an "off" message, or the end of the network game removes player 2 |
| Player 2's camera to the client | host | mod message 61 `{u8 1, u8 flags (1 alive), i32 net id, float right/up/forward[9], float position[3], float life}` at 30 Hz, to that client only |
| The client's second half | client | split screen as on a single PC, the second view's camera from message 61 (position smoothed, the client's own field of view); in that half the echoed `player_mp` of its player 2 is hidden, its player 1's body is shown, and its player 1's first-person weapon, crosshair and HUD are hidden |
| Time powers | host | timesync takes player 2's power as the host's when player 1 has none (`split_player2()`), so a power of either player 2 (split host or client) runs for everybody |

Switches: the client turns it on with `TS_SPLIT=1` or the UI string `CoopSplitJoin = "1"` (the Co-op frame's
option, round 17) and needs a second pad; `TS_SPLIT_CLIENT=0` turns the whole feature off on either side.

Known limits of the first version (round 16): player 2's half on a client has no HUD (life, ammunition, powers)
and no first-person weapon (the host has them; only the camera and life travel); its camera follows the host's at
30 Hz (aim latency = the network round trip); player 2's own screen effects are not shown on the client; its
pick-up messages show on the host. Round 17 below addresses these.

### Round 17 (code; not tested yet): the menu option, player 2's HUD, weapon, effects and messages on the client

| Part | How |
|---|---|
| Menu | Co-op frame: option "Second player on pad 2" (`COOP_OPT_SPLITJOIN`, all 9 string tables; values the game's own `UI_SOFF`/`UI_SON`). It sets the UI string `CoopSplitJoin` and the profile key `CoopSplitJoin`; the main menu sets the UI string from the profile at every start, so the choice holds until changed. With it on and a second pad, joining any co-op game brings player 2 in |
| Player 2's copy on the client (the "shadow") | once message 61 arrives in play, the client spawns its own local `player` as player 2, the same way as the single-PC split screen (so the second half has the game's own first-person weapon, crosshair, HUD widgets and screen effects per view, and movement and aim answer the local pad 2 at once). It is kept out of the network game (`isSyncMP` cleared, role bit 0x4000, also its body and weapons) and placed beside the host's player 2 (its `player_mp`). Its hits go nowhere (message 0x4e3 from its objects dropped) and its time-power and grenade buttons (RB, LB, d-pad up/left/right) are masked: the host's player 2, driven by the same pad, fires the real shots, throws the grenades and uses the powers. Items are never taken on a client |
| State from the host | message 61 is now version 2: + the host's player 2's body position, time energy, ammunition of the current weapon (total and magazine), its three weapon slots (`+0x3a8`) and its screen effects 6 (damage), 8 (death), 10 (low life) (on and strength). The client writes life, energy and ammunition into its copy every message, gives it the host's weapon slots (`$player2.SetupWeapon`) when they differ, and puts the effects into player 2's per-view effect records (shown in its half only). When the copy is more than 2.5 m from the host's player 2 (the two bodies push each other apart; after a respawn or a level change) it is put back beside it, at most once a second |
| Views on the client | view 0 (player 1): the copy's body is hidden, the host's player 2 is seen as its `player_mp`; view 1 (player 2): the `player_mp` of its own player 2 is hidden, player 1's body is shown, player 1's first-person weapon and crosshair hidden (the single-PC rules) |
| Messages | host: a HUD message of a client's player 2 (pick-ups etc., `split_msg_owner() == 2`) is not shown on the host but sent as mod message 62 `{u8 2, u8 n, u16 text[n]}` (UTF-16 from the game's counted wide string) to that client; the client adds it to its message line (`"console"` channel, as the game's pick-up message 0x440720) as player 2's, so it shows in player 2's half only. Messages of the client's copy itself are dropped (the host's are the real ones). The text is in the host's language |
| Items on a host | player 2 (of a split host or of a client) now also takes items on a server: the item loop's network branch (items the server has authority over, kinds other than 1-10, which are taken with the use key), the same check and take as in single player |

Switches: `TS_SPLIT_SHADOW=0` (client): no local copy, the round 16 camera-only half; `TS_SPLIT_HOSTITEMS=0`
(host): player 2 takes no items in a network game; `TS_SPLIT_CLIENT=0` everything off.

Risks to check first in the tests: whether a client can spawn a local `player` at all (the spawn path is the
single-PC one; never run in client mode before), whether the copy and the `player_mp` of the host's player 2 jitter
when they touch, whether the copy's local shots leave pictures only (impact effects without damage on the
client), and that the menu option changes the UI string (the select is a stock `select` without a profile
section).

### Round 18 (2026-10-25, tests: host N100 + client main PC, then the main PC alone)

Drivers: `lab/split_pair.ps1` (`-Mode client`: host on the N100, client on this PC with two virtual pads and the
profile key `CoopSplitJoin=1`; `-Mode host`: split host here, client on the N100; steps `Here`/`There` console,
`Pad`, `ShotHere`/`ShotThere`, `WaitLog`), `split_pair_t1.ps1` (split on a client), `split_pair_t2.ps1` (split
host: time power, AI kill, item), `split_run.ps1 -Dll`.

| Item | Result | Evidence |
|---|---|---|
| Main menu with a profile without `CoopSplitJoin` | **was broken by round 17**: the comparison of a missing key (null) with a string stopped the main menu's `OnInit` (and the AutoTest host after it). Fixed (key read into a string first), committed aa022f4 | N100 host log `20261025-212425`: `can't perform math operation '==' 'null' 'string' in '$.OnInit'` |
| Menu option | works: Co-op frame, pad 1 down x5 to "Second player on pad 2", right: UI string and profile key 0 -> 1 -> 0, the row shows Вкл./Откл. The frame no longer writes the key by itself (it did on its own `OnInit`) | `20261025-225133` (`shot058.png`, console answers) |
| Client joins with the option on | the client turns split screen on and streams pad 2; the host makes the client's player 2 (`player 2 networked as a puppet`, net id 2201) | `20261025-213310` both logs |
| A player 2 made while a client is connected | **crashed the client** (0x447a5c, its first-person weapons): the server's entity init announces a new entity (CREATE_ENT 0x42a, sent at 0x654bee) with its node's name/class/template, "player" for player 2, so the client made a campaign `player` of it. A client joining later is fine (join list: the create descriptor's multiplayer class, round 15). Fix: the announcement's class and template say `player_mp` while player 2 is made. On a host the hook fires (`player 2 announced to the clients as "player_mp" (name "Default", was "player" "player")`); **the client side is not re-tested yet** | client logs `20261025-213310`, `-214537`, `-215305`, `-222038`, `-222711`; host `20261025-224245` |
| Removing player 2 (client left, split off, tests) | **hung or crashed the game** in single player and on a server: its sniper crosshair object (vtable 0xd9b9f8) and other crosshair objects went on updating (`__purecall`), then its weapons (`0x5460d0` -> crash 0x81f09e). Fix: the body and the crosshair objects (found at their update by their node) are removed first; the weapons, holster and hands (recorded at player 2's spawn sites) are not updated any more and hidden until the level ends; a light-pass guard for instances without node (0x98b840). Single player and a split host now go on after the removal (`TS_SPLIT_DROPAT=25/40`; player 2 is made again 8 s later) | `20261025-215305` (host hang, watchdog), `-220418`, `-220747`, `-221623`, `-223352`, `-223839` (failing), `-224245`, `-225308` (fixed) |
| Client's copy of player 2 | its weapon set-up runs later on a client (the player's network start, 0x41ec5f), outside the spawn: the set-up now always runs as player 2's. **Not reached in a run yet** (the client crashed before) | - |

| Items for player 2 on a split host | **refused before**: the network item filter (item vtable +0x248, `0x4f5240`) looks the taker up in the game's player table (`[0xfcab18]` +0xfc, 0x84-byte records, +0x40 a handle of the player; `0x5fb6f0`) and refuses a player without a record (`0x428f30`). Player 2 has none. During player 2's take the handle of player 1's record names player 2 (alive, grenade count and energy are then read from player 2) and is put back. Now: `pick-up .. by player 2: taken`, its grenades 1 -> 2, "Получено: граната-липучка (1)" in player 2's half only | `20261025-232815` `shot128.png` (log `HUD message for player 2`); before: `-231749`, `-230850` |
| Checkpoint, team restart, level change on a split host | `$saves_mng.AutoSave(2)`, `$player.FailMission()` (team restart: the session reloads part02), `!ss exit ts2_city_part03`: player 2 is kept, placed beside player 1 after each load, both halves with HUD and weapons | `20261025-233250` `shot150.png` (after the restart), `shot260.png` (part03) |
| Test-window strip | in these test windows (1912x1000, larger than the game's 1768x992 picture) the split picture covers 1768x992 and the rest is black (cleared on purpose, see the composite); not seen at a real resolution | all main-PC frames |

### Round 19 (2026-10-27, split screen on a client: the copy of player 2)

| Item | Result | Evidence |
|---|---|---|
| Crash of the copy's shot | the copy fired with pad 2's triggers; its hit found no shooter (`0x576180`, esi 0). The copy no longer fires (triggers and right stick click masked; the host's player 2 fires) | `20261026-221127` (crash), `20261027-005811` (fixed) |
| Weapon and ammunition in player 2's half | the host's slots are given to the copy once (`SetupWeapon` + `SelectHolster(0, 1)`), its ammunition only once its weapon exists: pistol and "112 / 1" show | `20261027-010554` `shot098.png`, `shot111.png` |
| The copy does not walk with pad 2 | the command pass for the copy runs about 70 times a second and pad 2's stick values reach it; the copy still moved about 1.3 m. Running its update in game mode 1 (`TS_SPLIT_COPYMODE=1`, off) did not change that | `20261027-012413` (`command pass`, `player 2 pass` log lines) |
| The copy as a slave | `client_slave` moves the copy to the host's player 2's position (message 61) every frame, half way each time, a jump over 6 m at once, **0.6 m to the right of it**: on the same spot the host's player 2's replicated shots hit the copy and the game stopped in the hit handler without a shooter (`0x576180`, run `20261027-050946`). Hits on the copy are dropped. Its look direction stays its own (pad 2's right stick applied locally); its own walking still runs and is overwritten. `TS_SPLIT_COPYFREE=1` switches it off. The host's puppet of player 2 is moved 1000 m up for player 2's view (the flags alone did not hide it). Result: no crash for 330 s, player 2's half has its own camera, the first-person pistol, the crosshair, HUD and ammunition ("112 / 1"), the other players' bodies are seen as in local split, a red damage tint, the pick-up message in player 1's half | `20261027-052442` `shot113.png`, `shot117.png`; before: `-051718` (puppet body filling the half) |

### Round 20 (2026-10-27, main PC alone: the Co-op menu option on a host)

Driver `lab/host_option_run.ps1`: the profile gets `AutoTest = "host"` and `CoopSplitJoin = "1"` (no `TS_SPLIT`), two virtual
pads, short walks of each player, a death of each; the keys are taken out again.

| Item | Result | Evidence |
|---|---|---|
| "Second player on pad 2" on a co-op host | works: `split: on (a co-op host with "Second player on pad 2"; player 2 on XInput pad 2)`, player 2 spawned 8 s after the level started, both halves with a HUD each, the first-person weapon, the crosshair, the radar; no `TS_SPLIT` | `20261027-053204` `shot100.png`, `-053701` frames 096-113 |
| Walks | player 2 walked forward and back (stick +/-, 2.5 s each), then player 1: the other player's body moves in each half as expected, nothing stuck; with a longer straight walk player 2 ended against a wall and stayed there (the test's direction, not a fault) | `-053701` (frames 096-113), `-053204` `shot134.png` |
| Death and respawn | `$player2.Kill()`: `player 2 died: respawned next to player 1`; player 1's death screen (red, "До возрождения осталось: 3", the button named by the pad: RT) in its own half only, player 2's half stays in play; player 1 respawns | `-053204` `shot148.png`, `-053701` frames 126-156 |
| Test windows | the strip right of the picture (1768x992 in 1912x1000) stays black | all frames |

### Round 21 (2026-10-27, main PC alone: the soak through the campaign)

`lab/split_soak.ps1` (pwsh 7): one TimeShift with `TS_SPLIT=1` and two virtual pads; for each level in order it waits until both
players are placed (log), skips an intro cutscene with pad 1's Start, waits until both halves show their radar, takes a frame at
full size, idles 20 s, takes a second frame, then the level's exit (reload domain unlocked and the player put in; if the level
does not change in 20 s `$ui.LoadLevel(next)`). One line per level goes to `workuns\split-soak-<stamp>.txt`. Run
`20261027-060114` (levels 1-21), `-062443` (23-24), `-161845` (22 alone, fresh).

| Levels | Result |
|---|---|
| 1-18 | both players in and alive (health 100/100), 1.0-2.9 m apart, both halves with their radar, no crash, no stall. Level 1: the intro runs ~40 s before the HUD shows (waited for). Natural exit worked in 14 of 18, the others by the console |
| 19 `zep_factory_part1` | a full-screen computer-screen sequence (the level's own cutscene): no split, no HUD (`shot1102.png`), as for any cutscene |
| 20 `zep_factory_part2` | red corridor lighting (the level's own); **player 2 holds its weapon and ammunition, player 1 has none** (`shot1161.png`): the story takes player 1's weapons, player 2 keeps its own |
| 21 `zep_factory_part3` | ok (HUD after ~20 s) |
| 22 `zep_factory_part4` | **crashed loading it in the long session** (access violation `0xa63c1a`, write to 0, in a resource load; 21 level loads in one process); loaded alone from a fresh start it is fine (`-161845`, both halves, 1.6 m apart, natural exit) |
| 23 `zepride2_part01` | the airship ride: player 1 in the gun turret, **player 2 stands in the cabin with its own view, weapon and ammunition** (`-062443` `shot119.png`); 21 m apart (turret vs cabin); player 2 cannot use the turret |
| 24 `cityfinal_part01` | ok, 5.1 m apart (final level, no exit) |

Gaps: player 2 does not take part in the story's weapon removals (20); only player 1 can use turrets and vehicles (23);
cutscenes and computer-screen sequences are full screen; the crash after 21 loads in one process is not yet compared with a
game without split screen (it may be the engine's own leak). A red-screen check found no leaks (red share <= 0.05, from the
levels' own red lights).

### Round 22 (2026-10-27, main PC alone: the crash loading level 22 after 21 level loads)

`TS_MEMLOG=1` (split.c) logs the process's address space every 5 s (committed, reserved, free, largest free block); the soak
(`lab/split_soak.ps1`, `-NoSplit` for a game without split screen) gave, per level of one process:

| Run | Level 2-3 | Level 12-14 | Level 22 | Result |
|---|---|---|---|---|
| no split screen, 1-24 (`20261027-162311`) | committed 1.2 GB, reserved 0.21 GB, free 0.62 GB, largest free block 0.27 GB | free 0.52 GB, largest 0.19 GB | committed 1.25 GB, reserved 0.30 GB, **free 0.50 GB, largest 0.17 GB** | no crash through 24 |
| split screen, 2-22 (`-163849`, fresh from level 2) | committed 1.2 GB, reserved 0.28 GB, free 0.57 GB, largest 0.22 GB | reserved 0.50 GB, free 0.33 GB, largest 0.05 GB | committed 1.24 GB, reserved 0.52 GB, **free 0.29 GB, largest 0.05 GB** | no crash (close to the limit) |
| split screen, 1-21 + 22 (`-060114`) | - | - | - | **crashed loading level 22** (access violation `0xa63c1a`, write to 0 in a resource load: an allocation that failed) |
| split screen, **large address aware**, 1-24 (`-221243`) | address space 4 GB; free 2.6 GB, largest 1.9 GB | free 2.2-2.4 GB, largest 1.9 GB | committed 1.26 GB, reserved 0.63 GB, **free 2.2 GB, largest 1.9 GB** | no crash through 24, one process |

So it is the 2 GB address space of the 32-bit exe: the committed memory stays flat at about 1.2 GB, but the reserved
address space grows with split screen (the second view's pictures and player 2's objects: 0.3 -> 0.5-0.6 GB over the
campaign against 0.2 -> 0.3 GB without) and fragments it (largest free block 0.05 GB at level 22 against 0.17 GB). The
crash is the failed allocation when the load needs a block bigger than that; the run after a restart or without split
screen passes by a margin of a few tens of MB.

Fix: the installer (`tools/installer.ps1`) makes `bin\TimeShift.exe` large address aware (PE header Characteristics 0x20; 4 GB
on 64-bit Windows). The original exe (SHA-256 `689c63da...52641be4`, the gate is unchanged) is first kept in
`coop-backup`, the patched exe's hash (`de1a0aeb...a24fb01c`) is recorded in `coop\install.txt` (`exe=`), a reinstall recognises
the patched exe and leaves it, the uninstall puts the original back. Tested on `work\install_test` (install, reinstall,
uninstall: the original hash back, nothing left). The soak above used the patched exe in the development copy.

Round 22b (LAA exe, main PC): the installer was run with the LAA exe into `work\install_test` and the game started from it (main
menu and Co-op frame as before, `mem: address space 4096 MB`, `20261027-223916`); in the development copy with the patched exe
the Co-op menu option on a host (walks, deaths, a level change part02 -> part03) ran as without it: no crash, both halves with
HUD and weapon on part03 (`20261027-224520`, `shot185.png`). Not run: a LAA host + client join on two PCs.

Still to run (needs the N100): split on a client end to end (copy, halves, HUD, weapon, hits, items, messages,
death, jitter), and the split host + client checks (player 2's time power, its AI kill, its pick-up) with
`split_pair_t2.ps1`.

## Open problems

- **Player 2 is not a player to the game**: AI targets `[0x1095b30]` (player 1), scripts use `$player` (player 1):
  triggers, checkpoints, cutscenes, death and restart see player 1 only. The same audit as for network co-op
  (`player-audit.md`).
- **Split screen with the network**: player 2 of a split host is seen on clients (round 15); its time powers run
  for everybody since round 16. Split screen on a client: code of rounds 16 and 17, not tested yet.
- Hint boxes are one for both halves: a hint naming a button names player 1's device when both see it.
- Old notes on the network (round 5 goal 6): What blocks it: the
  network co-op runs the multiplayer game mode (2) and the multi-thread frame, split screen needs the
  single-thread frame; player 2 is a campaign `player` spawned locally, which the server's entity replication
  (0x42a CREATE_ENT) would send to clients as a second campaign player; the network layer's per-player records
  (score, team, respawn) know one local player per machine. Plan: on the host, keep player 2 out of replication
  and send it to clients as a `player_mp` puppet (as remote players are), with its position from the host's
  player 2. The menu's split screen starts a single-player game, so it never meets a network session.
- The line of sight of a joining spot is not checked (only the floor level).
- The pause menu and loading screens are full screen (UI command context); cutscenes with the scene camera are
  full screen. Player 2 has no menu control (only player 1's pad and keyboard drive the menus).
- Each view runs at half the frame rate; no frame cap yet.
- Multi-thread frame: off while split screen is on. With the engine's workers the render job overlaps the game
  update and the globals cannot simply be held for the render span; the swap would have to be limited to the
  frame start, the HUD update and the frame end (see the research notes), and live camera reads in the render job
  checked.
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
