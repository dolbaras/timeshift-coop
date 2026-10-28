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
| Record | `storysync.c` `checkpoint()` -> `coopsave_checkpoint` | on every checkpoint the host reaches: `coop\continue.txt` in the game folder: `level=`, `checkpoint=`, `pos=` (the checkpoint's position), `slots=` (the host's three weapon slots, player +0x3a8), `ammo=` (total and magazine of its primary weapon). A level's own early checkpoint below a continued one does not replace the record |
| Button | `data/ui/coopmenu.ui` | **Continue** (top of the Co-op frame), enabled while the UI string `CoopHasSave` is "1" (the DLL sets it once a second from the menu poll, with `CoopContLevel`). It sets the host frame's launch strings (`CoopLevel`, the player's last `CoopDifficulty`, `CoopMaxPlayers`, `CoopNetwork`, no password), `CoopContinue = "1"` and `CoopAuto = "host"`: the frame's own launch starts the server |
| Restore | `storysync.c` `level_changed` -> `coopsave_resume` | when the level comes up on the host and it is the recorded one, the restore of a team restart is armed with the recorded checkpoint: the level's own `Check_Point_<n>` object is unlocked 2 s after the host's player exists (its restore script puts the world and the respawn points there); a level without that object starts from its start |
| Loadout | `coopsave_tick` | 14 s into the level the host's weapon slots (`$player.SetupWeapon`) and the primary weapon's ammunition are put back |
| Off | `TS_COOPSAVE=0` | no record, no Continue |

The main menu clears `CoopContinue`, so a stale flag never continues a later session. The uninstaller removes `coop\` with the record.

## Verified (main PC alone, host only)

| Run | Result |
|---|---|
| `20261028-035234` | a host on part02, `AutoSave(2)`: `coop\continue.txt` written (part02, checkpoint 2, position, weapons 1 0 0, ammunition 128/16) |
| `20261028-035508` | main menu -> Co-op frame -> **Continue** pressed with the pad: `continuing ts2_city_part02 at checkpoint 2`, `co-op session hosted on "ts2_city_part02.lg"`, `restarted ... from its start (no restore object for checkpoint 2)` (part02 has no `Check_Point_2`), weapons and ammunition put back |
| `20261028-040455` | the same on part03 (a recorded checkpoint 2 with `Check_Point_2`): `restarted at checkpoint 2 ($Check_Point_2.Unlock() -> Ok)`, the host stands on the Kronengarden platform with its pistol and 112 ammunition (`shot120.png`); the Continue button in the Co-op frame (`shot056.png`) |

## Not done

- A client joining a continued session (the respawn points are moved by the restore as in a team restart; not run).
- Health, grenades, time energy and the story state of levels without a restore object are not restored.
- One record (the last checkpoint of the last session); no list of saves.
- The record is written only by the host of a co-op session (single-player saves are the game's own).
