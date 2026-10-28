# Co-op "Continue"

The Co-op menu has a **Continue** button: it hosts a co-op session on the chapter the host last reached a checkpoint
in, puts the host back at that checkpoint with its weapons and ammunition, and clients join as usual.

## Why the mod keeps its own record

The game's saves cannot be loaded on a server (`docs/multiplayer.md`, "Saves and checkpoints on the server"):
the level-start autosave and the checkpoint saves work on a host since `netfix.c` `saves_install` lets the level
loader save in server mode (`save_DDHHMMSS.tss` in the profile folder, ~1.1 MB each, a new one at every checkpoint), but
`$saves_mng.LoadGame` starts a **single-player** game, which ends the session. So the mod writes a small record of its
own and rebuilds the session from it with machinery that already exists (the team restart's checkpoint restore).

## How it works (`native/coopsave.c`)

| Step | Where | What |
|---|---|---|
| Record | `storysync.c` `checkpoint()` -> `coopsave_checkpoint` | on every checkpoint the host reaches: `coop\continue-<profile>.txt` in the game folder (see "Where the record lives"): `format=1`, `level=`, `checkpoint=`, `pos=` (the checkpoint's position), `slots=` (the host's three weapon slots, player +0x3a8), `ammo=` (total and magazine of its primary weapon), `health=` (+0x234), `energy=` (time energy, +0x2ac). A level's own early checkpoint below a continued one does not replace the record |
| Button | `data/ui/coopmenu.ui` | **Continue** (top of the Co-op frame), enabled while the UI string `CoopHasSave` is "1" (the DLL sets it once a second from the menu poll, with `CoopContLevel`). It sets the host frame's launch strings (`CoopLevel`, the player's last `CoopDifficulty`, `CoopMaxPlayers`, `CoopNetwork`, no password), `CoopContinue = "1"` and `CoopAuto = "host"`: the frame's own launch starts the server |
| Restore | `storysync.c` `level_changed` -> `coopsave_resume` | when the level comes up on the host and it is the recorded one, the restore of a team restart is armed with the recorded checkpoint: the level's own `Check_Point_<n>` object is unlocked 2 s after the host's player exists (its restore script puts the world and the respawn points there); a level without that object starts from its start |
| Loadout | `coopsave_tick` | 14 s into the level the host's weapon slots (`$player.SetupWeapon`), the primary weapon's ammunition, life (at least 1) and time energy are put back |
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
format, e.g. a file of an older or a newer mod, is ignored), `level=` is one of the 24 campaign chapters, `checkpoint`
1..99, weapon slots 0..40, ammunition, life and energy are numbers in range (not NaN), at most 40 lines. Otherwise it is
treated as no record: the Continue button stays greyed (`CoopHasSave = "0"`) and the frame says so in a hint line
(`COOP_HINT_CONTINUE_NONE`; with a record the line reads `COOP_HINT_CONTINUE`; both in all 9 languages). No error
dialog and no crash: Continue is simply unavailable. Tested by editing the file (below).

## Verified (main PC alone, host only)

| Run | Result |
|---|---|
| `20261028-035234` | a host on part02, `AutoSave(2)`: `coop\continue.txt` written (part02, checkpoint 2, position, weapons 1 0 0, ammunition 128/16) |
| `20261028-035508` | main menu -> Co-op frame -> **Continue** pressed with the pad: `continuing ts2_city_part02 at checkpoint 2`, `co-op session hosted on "ts2_city_part02.lg"`, `restarted ... from its start (no restore object for checkpoint 2)` (part02 has no `Check_Point_2`), weapons and ammunition put back |
| `20261028-040455` | the same on part03 (a recorded checkpoint 2 with `Check_Point_2`): `restarted at checkpoint 2 ($Check_Point_2.Unlock() -> Ok)`, the host stands on the Kronengarden platform with its pistol and 112 ammunition (`shot120.png`); the Continue button in the Co-op frame (`shot056.png`) |
| `20261028-041206` / `-041426` / `-041727` | the new record format (`format=1`, `health=56.0`, `energy=1.00`) in `continue-Default.txt`; Continue on part02: weapons 1 0 0, ammunition 128/16, life 56 and time energy 1.00 put back (`coopsave: ... put back (ok)`) |
| `-0419..` to `-0423..` | the record edited: no `format=` line, `format=2`, unknown chapter `ts2_nonexistent_part9`, `health=nan` + `checkpoint=abc`, 3 KB of random bytes: each time `CoopHasSave = 0`, no crash, the game runs on |
| `20261028-042417` | no record file: `CoopHasSave = 0`, Continue greyed, the hint says it appears after a checkpoint (`shot062.png`); with a record (`-042547`, `shot056.png`) the button is lit and the hint describes it |
| installer (`work\install_test`) | `continue-Default.txt` present, reinstall (`-Quiet`): file still there; `-Uninstall`: `coop\` gone with it |

## What is and is not restored

Restored: the chapter, the host's weapons, the primary weapon's ammunition, life and time energy; at a level that has
the checkpoint's `Check_Point_<n>` object (part03 has `Check_Point_2`) also the position and the story state of that
checkpoint (the restore script of a team restart).

Not restored: grenades and the other weapons' ammunition (the mod finds only the primary weapon's record; where the game
keeps grenade counts is not known), and, in a level without a restore object (part02 has none for checkpoint 2), the
checkpoint's place and story state: the chapter restarts from its beginning with the weapons, life and energy of the
checkpoint (no teleport into a story state that was not rebuilt).

## Not done

- A client joining a continued session (the respawn points are moved by the restore as in a team restart; not run).
- One record (the last checkpoint of the last session); no list of saves.
- The record is written only by the host of a co-op session (single-player saves are the game's own).
