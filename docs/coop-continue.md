# Co-op "Continue"

The Co-op menu has a **Continue** button: it hosts a co-op session on the chapter the host last reached a checkpoint
in, puts the host back at that checkpoint with its weapons, ammunition, grenades, life and time energy, and clients join as
usual (a client that was in the last session gets its own loadout back, see "Clients").

## Why the mod keeps its own record

The game's saves cannot be loaded on a server (`docs/multiplayer.md`, "Saves and checkpoints on the server"):
the level-start autosave and the checkpoint saves work on a host since `netfix.c` `saves_install` lets the level
loader save in server mode (`save_DDHHMMSS.tss` in the profile folder, ~1.1 MB each, a new one at every checkpoint), but
`$saves_mng.LoadGame` starts a **single-player** game, which ends the session. So the mod writes a small record of its
own and rebuilds the session from it with machinery that already exists (the team restart's checkpoint restore).

## How it works (`native/coopsave.c`)

| Step | Where | What |
|---|---|---|
| Record | `storysync.c` `checkpoint()` -> `coopsave_checkpoint` | on every checkpoint the host reaches: `coop\continue-<profile>.txt` in the game folder (see "Where the record lives"): `format=1`, `level=`, `checkpoint=`, `pos=` (the checkpoint's position), `slots=` (player +0x3a8: it never changes, kept only for the readers of older records), `ammo=` (total and magazine of the weapon in hand), `health=` (+0x234), `energy=` (time energy, +0x2ac) and the **inventory** (optional lines, see "The inventory"): `set=a b c`, `wpn<0..9>=t0 m0 t1 m1`, `gren=<selected> c0..c6`. A level's own early checkpoint below a continued one does not replace the record. The host also asks every client for its loadout (see "Clients") |
| Button | `data/ui/coopmenu.ui` | **Continue** (top of the Co-op frame), enabled while the UI string `CoopHasSave` is "1" (the DLL sets it once a second from the menu poll, with `CoopContLevel`). It sets the host frame's launch strings (`CoopLevel`, the player's last `CoopDifficulty`, `CoopMaxPlayers`, `CoopNetwork`, no password), `CoopContinue = "1"` and `CoopAuto = "host"`: the frame's own launch starts the server |
| Restore | `storysync.c` `level_changed` -> `coopsave_resume` | when the level comes up on the host and it is the recorded one, the restore of a team restart is armed with the recorded checkpoint: the level's own `Check_Point_<n>` object is unlocked 2 s after the host's player exists (its restore script puts the world and the respawn points there); a level without that object starts from its start |
| Loadout | `coopsave_tick` | 14 s into the level the chosen weapons (`$player.SetupWeapon(set)`), every weapon's two ammunition records, the grenade counts and the selected grenade, life (at least 1) and time energy are put back. A record without inventory lines (an older mod's) falls back to "the pistol slot numbers + the weapon in hand's ammunition" |
| Off | `TS_COOPSAVE=0` | no record, no Continue |

The main menu clears `CoopContinue`, so a stale flag never continues a later session.

## Where the record lives

- Per game folder (`<game>\coop\`) and per game profile: `continue-<profile>.txt`, the profile being `[Profile] Name` of
  `bin\steamwars.ini` (`Default` for most players; characters other than letters, digits, `_` and `-` become `_`). Two
  profiles, or two game copies, never share a record.
- A **reinstall or update keeps it** (the installer copies only the files of its own list; the record is not in it), so a
  session survives updating the mod. **The uninstall removes it** with `coop\` (the installer's manifest keeps the rest of
  the profile and the game's saves untouched).

## A record that cannot be used

Read once a second by the menu poll and again by the restore. A record is used only if: `format=1` (another or no
format, e.g. a file of an older or a newer mod, is not used), `level=` is one of the 24 campaign chapters, `checkpoint`
1..99, weapon slots 0..40, ammunition, life and energy are numbers in range (not NaN), at most 40 lines. The inventory
lines are checked on their own (weapon numbers 0..9, ammunition 0..99999 / magazine 0..9999, grenade counts 0..99,
selected grenade 0..7): a bad inventory line is dropped and the rest of the record is used with the older fallback
(slots and the weapon in hand's ammunition), it never invalidates the record.

What the player sees (UI string `CoopHasSave`, set by the DLL once a second; texts in all 9 languages):

| State | `CoopHasSave` | Continue button | Hint line |
|---|---|---|---|
| no file | `0` | greyed | `COOP_HINT_CONTINUE_NONE`: it becomes available once you reach a checkpoint as the host |
| a file that cannot be used (damaged, another mod version, wrong chapter, numbers out of range) | `2` | greyed | `COOP_HINT_CONTINUE_BAD`: the saved progress is damaged or from another version of the mod, reach a new checkpoint as the host to replace it |
| a usable record, the chapter has no restore object | `1`, `CoopContMarker = 0` | lit | `COOP_HINT_CONTINUE_START`: the chapter has no mid-chapter restore, it starts from its beginning with your weapons, ammunition, grenades and life |
| a usable record, the chapter has the restore object | `1`, `CoopContMarker = 1` | lit | `COOP_HINT_CONTINUE` |

No error dialog and no crash in any state; the game's own saves are not touched. The next checkpoint the host reaches
writes a good record over a bad one.

## Verified earlier (2026-10-28, main PC alone, host only; N100 host with the main PC as client)

| Run | Result |
|---|---|
| `20261028-035234` | a host on part02, `AutoSave(2)`: `coop\continue.txt` written (part02, checkpoint 2, position, weapons 1 0 0, ammunition 128/16) |
| `20261028-035508` | main menu -> Co-op frame -> **Continue** pressed with the pad: `continuing ts2_city_part02 at checkpoint 2`, `co-op session hosted on "ts2_city_part02.lg"`, `restarted ... from its start (no restore object for checkpoint 2)` (part02 has no `Check_Point_2`), weapons and ammunition put back |
| `20261028-040455` | the same on part03 (a recorded checkpoint 2 with `Check_Point_2`): `restarted at checkpoint 2 ($Check_Point_2.Unlock() -> Ok)`, the host stands on the Kronengarden platform with its pistol and 112 ammunition (`shot120.png`); the Continue button in the Co-op frame (`shot056.png`) |
| `20261028-041206` / `-041426` / `-041727` | the new record format (`format=1`, `health=56.0`, `energy=1.00`) in `continue-Default.txt`; Continue on part02: weapons 1 0 0, ammunition 128/16, life 56 and time energy 1.00 put back (`coopsave: ... put back (ok)`) |
| `-0419..` to `-0423..` | the record edited: no `format=` line, `format=2`, unknown chapter `ts2_nonexistent_part9`, `health=nan` + `checkpoint=abc`, 3 KB of random bytes: each time `CoopHasSave = 0`, no crash, the game runs on |
| `20261028-042417` | no record file: `CoopHasSave = 0`, Continue greyed, the hint says it appears after a checkpoint (`shot062.png`); with a record (`-042547`, `shot056.png`) the button is lit and the hint describes it |
| `20261028-044653` / `-045257` / `-045809` (N100 host, main PC client) | host on ts2_city_part03 through the Continue path (`AutoTestContinue = "1"`, a planted record with `format=1`, checkpoint 2, life 64, energy 0.50): `continuing ts2_city_part03 at checkpoint 2`, `restarted at checkpoint 2 ($Check_Point_2.Unlock() -> Ok)`, `put back (ok)`; the client joined 75 s later (`client 1 joined 109 s into the level`): no crash on either side for 200 s, the client stands on the Kronengarden platform 1 m from the host (its camera -168.63 -10.18 -145.09, the host's -169.62 -10.18 -145.22) and sees it ("Default" tag, its weapon at the edge of the view: `-045809` `shot088.png`, `shot092.png`); the host on the platform: `n100/shot160.png` |
| installer (`work\install_test`) | `continue-Default.txt` present, reinstall (`-Quiet`): file still there; `-Uninstall`: `coop\` gone with it |

## The inventory (what the game keeps where)

Found by dumping the player live (`!cs inv`, `!cs rec`) and by decompiling `SetupWeapon` (player vtable +0x38c) and the
pick-up paths; everything is in `native/coopsave.c`.

| What | Where | Notes |
|---|---|---|
| the weapons | an array of 10 weapon objects at `*(player+0x32c)` (count at +0x330), indexed by weapon number: 0 hands/holster (no ammunition), 1 the pistol (128/16 at the start, seen), 2-9 the other `wpn_plr_*` classes in the order the game spawns them (`crb`, `pmg`, `arifle`, `rlauncher`, `sniper`, `shotgun`, `uzi`, `swarmgun`; the names of 2-9 are an inference from that order and the default ammunition, the mod does not need them) | every weapon exists all the time; owning one is only "chosen" |
| the chosen weapons | pointers at `player+0x338/+0x33c/+0x340` into that array (`SetupWeapon(a, b, c)` sets them by weapon number, 0..9) | the old `+0x3a8` numbers never change (always `1 0 0`), that is why the first Continue restored "the pistol with the rifle's ammunition" |
| ammunition | each weapon answers vtable +0x4c(0 / 1) (of its virtual base) with a record: `+8` total (float), `+0xc` rounds in the magazine | record 1 is the secondary fire (the rifle's grenades 4/2, the uzi's 240/20); for single-fire weapons both calls give the same record. All 20 records are saved and put back |
| grenades | `*(player+0x36c)`: `+4` an int array of 7 counts (one per grenade type), `+8` the count 7, `+0x10` the selected type | a throw (pad LB) counts down by one (seen live: 1 -> 0). The engine keeps some counts at their type's maximum (a count of 3 written to type 4 reads 1) |
| life, time energy | `player+0x234` / `+0x2ac` | |

The restore runs `$player.SetupWeapon(set)` first (so the hands and the HUD follow), then writes the ammunition records and
grenade counts. It is applied once, 14 s after the level is up on the host (after the level's own start-up scripts).

## Chapters without a restore marker

The game's checkpoints are `AutoSave(n)` calls in the level scripts; the mod can rebuild a checkpoint on a server only
where the level also has a `Check_Point_<n>` object (the restore script a team restart uses). From the level files:

| Chapter | `Check_Point_` objects |
|---|---|
| `ts2_city_part03` | 1, 2, 3 |
| `ts2_cityfinal_part01` | 1 |
| `ts2_guardfactory_part05` | 1 |
| `ts2_zepride1_part01` | 1-8 |
| `ts2_zep_factory_part1` | 1, 2, 3 |
| `ts2_zep_factory_part2` | 1, 2 |
| `ts2_zep_factory_part4` | 1, 2, 3 |
| the other 17 chapters | none |

Defined behaviour: a record whose chapter has the object for its checkpoint number is restored there (position and story
state of the restore script, then the loadout). In every other case (17 chapters, or a checkpoint number above the
chapter's objects, e.g. part03's `AutoSave(4)`) **the chapter starts from its beginning with the loadout of the record**; the
Co-op frame says so before the player clicks (`COOP_HINT_CONTINUE_START`). A position-only resume (teleport to the
checkpoint without its story state) was left out on purpose: doors, triggers and AI would be in their first-minute state
around a player who stands in the middle of the chapter.

## Clients

A client's weapons live on its own machine, so they travel in mod messages (`coopsave.c`, subtypes 90-93 of `coopnet`):

| Message | From -> to | Content |
|---|---|---|
| 90 | host -> clients | "report your loadout now" (sent right after the host records a checkpoint) |
| 91 | client -> host | the client's id, its packed inventory (set, 20 records, grenades, life, energy: 142 bytes) |
| 92 | client -> host | "hello": sent from 8 s into each level, repeated every 4 s up to 8 times until answered |
| 93 | host -> client | that client's stored inventory, or an empty answer |

The host keeps the last report of every client (up to 8) in `coop\continue-<profile>-clients.txt` (one line per client:
the id and the packed inventory in hex; a damaged line is dropped). The id is a hash of the `PlayerGUID` of the client's
profile (each copy has its own). In a level the host continued (a Continue put it back there), a client's hello is answered
with its stored inventory and the client puts it back (`SetupWeapon`, records, grenades, life, energy), once per level. A
client that never was in the earlier session, or joins another level, gets nothing stored and keeps the level's own loadout.
The files of an uninstall go with `coop\`.

## Verified (2026-10-31, chunk D of 0.1.1)

Main PC, one copy, my own DLL build (`work\tmp_d`), the host's console (`!cs inv`, `!cs rec`: lab commands that dump/set the
inventory; `!cs ammo|gren|hp|en` set values):

| Run | Result |
|---|---|
| `20261031-203729`, `-204202`, `-204611` | the dumps that found the layout (table above); a grenade throw by pad LB lowered type 0 from 1 to 0 |
| `20261031-205218` | record: `SetupWeapon(1, 4, 8)`, weapon 4 set to 200/25 and 3/1, weapon 8 to 300/40 and 100/10, grenades `2 0 0 0 1 1 1`, then `AutoSave(2)`: `continue-Default.txt` has `set=1 4 8`, `wpn0..wpn9`, `gren=0 2 0 0 0 1 1 1` |
| `20261031-212826` | Continue through the menu's UI strings (`CoopContinue`, `CoopAuto`): `continuing ts2_city_part02 at checkpoint 2`, `the host's weapons 1 4 8 ... put back (ok; inventory: 20 records, 7 grenade types)`; read back 40 s later: `set 1 4 8; w4 200/25 3/1; w8 300/40 100/10; gren 2 0 0 0 1 1 1`, life 18 (the host was under fire, not in god mode). Later (life 18 at 115 s, 100 at 131 s) the host most likely died: at 131 s the set was `1 0 0` with default ammunition again, i.e. the stock respawn gave the level's start loadout (a death after a Continue does not bring the Continue loadout back) |
| `20261031-213932` | the four hint states with the record file edited during the run: good part02 record `CoopHasSave 1`, `CoopContMarker 0` and the "starts from its beginning" hint; a damaged file `2` and the damaged hint; no file `0`; a part03 record (checkpoint 2) `1 1` and the normal hint (shots `shot054/062/078.png`) |
| `n100pair-20261031-214102` | an OLD record (no inventory lines, written by the previous build) through Continue on the N100 pair: `put back (ok; inventory: 0 records, 0 grenade types)` (the fallback), the client joined and said hello (`nothing stored`), no crash |
| `n100pair-20261031-220414` | N100 pair (host + client, loopback), part02: the host set `SetupWeapon(1, 4, 8)` + ammunition + grenades, the client `SetupWeapon(1, 3, 7)` + ammunition + grenades; the host's `AutoSave(2)`: `checkpoint 2 ... recorded (inventory yes: chosen 1 4 8, grenades 2 0 0 0 1 1 1)` and, right after, `client 438571b5's loadout recorded (weapons 1 3 7, life 90, 142 bytes)` (message 90 -> 91) |
| `n100pair-20261031-222520` | N100 pair, Continue (`AutoTestContinue`) with the record and the clients file of the previous row PLANTED again (another agent's pair runs had overwritten the genuine ones with checkpoint 1 between the two runs): host `continuing ts2_city_part02 at checkpoint 2`, `put back (ok; inventory: 20 records, 7 grenade types)`, read back `set 1 4 8; w4 200/25 3/1; w8 300/40 100/10; gren 2 0 0 0 1 1 1`; 9 s later `client 438571b5 says hello ... its stored loadout sent` and on the client `this client's loadout put back (weapons 1 3 7, life 90, 20 records, 7 grenade types, ok)`, read back on the client `set 1 3 7; w3 90/15; w7 60/6; gren 3 0 0 0 1 1 1` (the stored last count 0 reads 1: the engine's own count). No crash; four handled `AccessViolation` lines at `0x75490196` on the host (the known harmless pattern of chunk 0's note) |

Not verified: a real mouse/pad click on the Continue button in this session (the vpad `A` press did not reach the Co-op frame in
two runs, the frame's transition looked stuck in the test window; the button's own script only sets the UI strings that the
runs above set from the console, and the click itself was verified on 2026-10-28, `20261028-035508`). The respawn after a death
(only the level start loadout, as in any co-op session) is not carried over by Continue.

## Not done

- One record (the last checkpoint of the last session); no list of saves. The clients file keeps one entry per client.
- A client's own weapons are carried only for clients that were connected at the checkpoint; a client that dies and respawns
  gets the level's start loadout (the loadout carry-over of `campaign.c` covers respawns within a session, not Continue).
- The record is written only by the host of a co-op session (single-player saves are the game's own).
- No position-only resume in the 17 chapters without a restore marker (above).
