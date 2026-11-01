# Releasing

## Building the release archive

```
sh native/build.sh                      # native/build/dinput8.dll
powershell -File tools\package.ps1      # dist\timeshift-coop-<TS_VERSION>.zip (dist\ is git-ignored)
```

The version has one source: `TS_VERSION` in `native/ts.h`. `package.ps1` refuses a DLL that does not
carry it, and writes it to `files\coop\version.txt`; the installed `build_pak.py` reads it from there
(no `native\` in an installed copy), so the DLL, the data archives and the install record agree.
`-Version` may be given but must equal `TS_VERSION`. The package is made from the working tree
(`data\` as it is on disk, untracked recipes included): build a release from a clean checkout.

The archive holds no game files:

| Path in the zip | What |
|---|---|
| `Install Co-op.bat`, `coop-installer.ps1` | the installer (`tools/installer.ps1`), also the uninstaller |
| `README-COOP.txt`, `README-COOP-ru.txt`, `LICENSE.txt` | players' texts (`docs/release-readme*.txt`), MIT license |
| `files\bin\dinput8.dll` | the proxy DLL |
| `files\coop\tools\build_pak.py`, `s3darc.py`, `files\coop\data\...` | the data builder and the recipes |
| `files\coop\python\...` | python.org's embeddable CPython 3.12.10 amd64 (SHA-256 pinned in `package.ps1`), trimmed to `python.exe`, `python312.dll`, `python312.zip`, `python312._pth`, `vcruntime140*.dll`, `LICENSE.txt` (PSF) |
| `files\coop\coop-launcher.ps1` | the shortcut's launcher (`tools/launcher.ps1`) |
| `files\coop\collect-logs.ps1`, `files\coop\Collect logs.bat` | the bug-report collector (`tools/collect-logs.ps1`) and its double-click start |
| `files\coop\version.txt`, `files\coop\payload.txt`, `files\coop\README-COOP*.txt` | version, the package's file list, the texts |

`Install Co-op.bat` is the player's entry point. It starts the installer with a hidden PowerShell and waits up to 25 s for the
installer's start flag (`%TEMP%\timeshift-coop-installer-started.flag`, the first thing `installer.ps1` writes); when it does not
appear (Windows, Defender or a policy stopped PowerShell, and the hidden window swallowed the message) the .bat says so on screen
and `pause`s, exit code 1. When the flag appears the .bat deletes it and returns at once (the installer's own window is the
interface). `Uninstall Co-op.bat` (written into the game folder by the installer) does the same for `-Uninstall`. The flag is also
removed by a run started by the launcher (`-AutoClose`: no .bat is there to clear it).

## The setup exe (additional release asset)

`timeshift-coop-setup-<version>.exe` is a single-file installer for players who do not want to unzip and run `Install Co-op.bat`.
It is an EXTRA asset: the zip stays the asset the launcher updates from (the launcher takes the first asset matching
`timeshift-coop-*.zip`, and the exe's name does not match that pattern; keep it so), the zip's layout and the installer's
parameters are unchanged, and the exe's payload is the release zip byte for byte.

```
powershell -File tools\package.ps1 -Exe        # dist\timeshift-coop-<version>.zip + dist\timeshift-coop-setup-<version>.exe
```

`-Exe` calls `<workspace>\tools\mkinstaller\mkinstaller.ps1` (game independent: `-Zip -Out -Product -Version [-Description
-Icon | -Accent -TempPrefix -Installer]`; `-Mkinstaller <folder>` for a clone placed elsewhere; `-Dist <folder>` writes
somewhere other than `dist\`, handy for tests because `dist\` holds the published zips; give a full, not an 8.3 short, path).
It builds `tools\mkinstaller\stub.c` with the workspace's zig (`zig cc -target x86-windows-gnu -Os`, windows subsystem, no
console, system bcrypt/shell32 only), embeds the zip as RCDATA 1 and a small config as RCDATA 2 (product, temp prefix, the
installer's name, the zip's SHA-256), our own icon (`make_icon.ps1`: two players and an install arrow in the game's accent
colour, no game art), VERSIONINFO (product, description, versions, no company) and a manifest (asInvoker, DPI aware, supported
OS list, long paths). Right after the build it runs the exe with `--extract` and compares every unpacked file with the zip
(names, sizes, SHA-256).

What the exe does when a player runs it:

1. Checks its payload's SHA-256 (a mismatch: message box "damaged, download again").
2. Unpacks the zip (own inflate, CRC-32 of every file checked, entry names that could leave the folder refused) into a NEW
   folder `%TEMP%\timeshift-coop-setup-<8 hex>` (never the current folder; a folder of an earlier killed run older than a day and
   holding the stub's marker file is removed first).
3. Starts `powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File <that folder>\coop-installer.ps1 <the
   command line the player gave, unchanged>` (no console window; the 64-bit PowerShell also from the 32-bit stub, through
   Sysnative), with `COOP_SETUP_DIR` = the folder the exe lies in and `COOP_SETUP_FLAG` = a file the installer writes as its
   first act (not in `-Quiet`). The installer reads them: the game (and a Steam update package) is also looked
   for next to the exe, and a failed run whose flag never appeared makes the exe say "the installer window did not start
   (PowerShell blocked?)" with a pointer to the zip.
4. Waits, returns the installer's exit code (0 ok, 1 failed; a window the player closes is 0, as the launcher relies on), and
   deletes the folder (ten tries, an antivirus may hold a file for a moment; junctions are unlinked, never followed).

`-Quiet` keeps its console output: the exe joins the starter's console or hands the redirected std handles on. Every installer
option works as before: `-Game`, `-Uninstall`, `-Quiet`, `-Lang`, `-AutoClose`, `-NoShortcut`, `-ShortcutDir`, `-SteamUpdate <zip|folder>` and `-AcceptSteamUpdate` (the Steam update flow with its consent works from the exe: tested with the real community package on a simulated Steam copy).
Only the exe's own `--extract <folder>` is extra (unpack only, run nothing; for audits and tests). The installer still copies
`coop-installer.ps1` and writes `Uninstall Co-op.bat` into the game folder, so removal is the same with or without the exe.
Error boxes are in English, Russian, German, French, Spanish and Italian (Windows' UI language).

Honest limits: the exe is NOT code-signed (no certificate), so SmartScreen will probably warn "Windows protected your PC" and
an antivirus may flag a self-extracting program that starts PowerShell; the README texts say so, offer the zip as the
alternative and `--extract` for a look inside. Nothing is done to avoid detection (no packer, no obfuscation, no
signature tricks): signing it later is the only real fix. The unpacked files do not carry the "downloaded from the Internet"
mark, which the zip's would after Explorer's Extract all; the installer is started with `-ExecutionPolicy Bypass` either way,
as `Install Co-op.bat` does. A machine whose policy forbids running scripts stops both ways. The stub's source is in the
workspace (`tools\mkinstaller`), not in this repository yet: publish it with the release if the exe is shipped.

## What the installer does

1. Checks the folder (`Exe-Info`, by SHA-256 of `bin\TimeShift.exe`):
   - accepted: the retail patch 1.2 exe (`689c63da...52641be4`), the same exe with the large-address-aware flag set, the
     community "pre-patched" 1.2 exe (the CPU fix; `49bcca23...`, accepted with a notice), the exe inside the community
     Steam update package (`b337d72e...`), and any other build of the 1.2 code (the file with the bytes zeroed that those
     builds and our own LAA step change must hash to the normalised 1.2 value; a changed header entry point or a changed code
     byte outside the mask = refused, nothing is changed);
   - a Steam copy (patch 1.0 code, other exe): refused with the words of `exe_steam` unless the player supplies the
     community 1.2 update package (see "A Steam copy" below);
   - anything else (a protected exe, another version) is refused and the version needed is named.
   Also: the game must not run from this folder; `patch.s3darc`, `patch_ati`, `patch_nv`, `out.s3darc` must be there; the
   payload must match `payload.txt`; the folder must be writable (a read-only flag on the exe, the ini or an archive is cleared;
   an ACL that denies writing is refused with the write text, nothing changed; `Program Files (x86)` is done by an elevated copy
   of the installer after one UAC click, see Tests).
2. Builds `coop.s3darc`, `coop_ati.s3darc`, `coop_nv.s3darc` with the payload's own Python into
   `%TEMP%\timeshift-coop-build-<pid>`, before the game folder is changed. A reinstall keeps the installed
   archives when `coop\coop_build.txt` names the same version and the same `patch.s3darc` SHA-256,
   `coop\paks.txt` the same recipes hash (builder + recipes + version.txt) and each installed archive
   still has the SHA-256 recorded there, and arcName is `coop.s3darc`.
3. Copies `files\` (a file that is not the mod's is kept once in `coop-backup\`, e.g. another
   `bin\dinput8.dll`; a co-op DLL is recognised by its log line "TimeShift co-op proxy" and never kept
   as an original), the archives into `preload\paks\`, and sets `[Preload] arcName = coop.s3darc` in
   `bin\steamwars.ini` by replacing only the value's bytes (CRLF kept; the game skips bare-LF lines).
   The exe gets the large-address-aware flag (original kept in `coop-backup\bin\TimeShift.Exe`, hash of the new one in the
   record's `exe=` line).
4. Shortcuts, then `coop\install.txt`, then `coop\version.txt` last.

Installer parameters: `-Game <folder>` (default: found by `Find-Games`: the installer's own folder and the one above it, the
uninstall registry (Inno Setup installs), Steam libraries, the common install folders), `-Uninstall`, `-Quiet` (no window; messages on the console), `-Lang en|ru|de|fr|es|it` (default: the
UI language), `-AutoClose` (the window closes by itself after success), `-NoShortcut` (refresh only the shortcuts the record
lists), `-ShortcutDir "<dir1>;<dir2>"`, `-GameProcess <name>` (tests), `-SteamUpdate <zip|folder>`, `-AcceptSteamUpdate`.

**Exit codes.** 0 = success, and also 0 for a window the player simply closes (a closed window is not a failure); 1 = a failed
install or uninstall (the window's own error page, `coop\install-log.txt` or `%TEMP%\timeshift-coop-install.log` has the log).
`-Quiet` exits 0 or 1 the same way and prints the message; an elevated child's exit code is passed on. The launcher relies on this.

### A Steam copy (patch 1.0)

Steam's TimeShift is patch 1.0 (code and data), which Steam never updated; every hook of the mod is bound to the 1.2 code. The
installer therefore only works on a Steam copy together with the community "TimeShift Update 1.2 (Steam)" package that the
player downloads himself (nothing is shipped or fetched by us; the installer's texts and the readme name it as a community
package, never as official, and give no links). Choices and evidence: `notes\plan-0.1.1-steam.md` (private).

- The installer's window shows a package row (it looks in `Downloads` and next to the installer for a zip with
  `bin\TimeShift.exe`; the player may pick a zip or an extracted folder) and a consent tick naming the files that will be
  replaced and the package exe's SHA-256. In `-Quiet` mode `-SteamUpdate <zip|folder>` supplies the package and
  `-AcceptSteamUpdate` is the consent; without it the install stops with the same words and changes nothing.
- Before anything is touched the package is checked: path safety (no `..`, no absolute path), `bin\TimeShift.exe` of the 1.2 code
  class, `patch.s3darc`, `patch_ati.s3darc`, `patch_nv.s3darc` and `out.s3darc` present. A package's `userconfigs`, `coop`,
  `coop-backup` and every program or script file (`.exe` other than the game's own, `.bat`, `.ps1`, ...) are never taken.
- `coop\steam.txt` is written BEFORE the first file is touched: `steam=<path>` for a file the package replaced (its original is
  in `coop-backup\steam10\<path>`, kept once so that an interrupted run's copy is the real original) and `steamnew=<path>` for a
  file it added. A failure in the middle (including a failed data build, e.g. the recipes do not apply to the package's
  archives) rolls everything back from `steam10`. Then the normal install continues on the now-1.2 copy.
- Uninstall puts the `steam=` files back from `steam10`, removes the `steamnew=` files and says so (`removed_steam`), so a
  Steam copy is byte-identical to before (tested with a simulated Steam copy and with the real package).
- A later release updating such a copy needs no package: the exe is 1.2 already, `steam.txt` only carries over.

### Uninstall

arcName back to the recorded value first, then the Steam files (if `steam.txt` exists), then the listed files, the built
archives, `bin\ts_coop.log` and the rotated `bin\ts_coop.1.log` ... `.9.log`, the shortcuts, the backups put back,
`coop\` (including `collect-logs.ps1`, `Collect logs.bat`, `launcher-log.txt`, `install-log.txt`) and `coop-backup\` removed.
`UserConfigs\` (profiles, saves, the Continue records `coop\continue-*` live in `coop\` and go with it) is never touched.
A bug-report zip on the desktop is the player's file and stays.

### Records in the game folder

| File | Written by | Content |
|---|---|---|
| `coop\install.txt` | installer, last | `version=`, then `file=` (every installed file, relative), `backup=` (originals kept in `coop-backup\`), `built=` (archives in `preload\paks`), `shortcut=`, `arcname=` (arcName value before the install), `exe=` (SHA-256 of the exe the installer made) |
| `coop\version.txt` | installer, after `install.txt` | the version without `v`; the launcher compares with it |
| `coop\steam.txt` | installer, Steam copies only | `steam=` / `steamnew=` lines (above) |
| `coop-backup\...` | installer | originals: `bin\TimeShift.Exe`, a foreign `bin\dinput8.dll`, `steam10\...` |
| `coop\coop_build.txt`, `coop\paks.txt` | installer | what the archives were built from (reinstall decision) |
| `coop\install-log.txt` | installer | the last install's log |
| `coop\launcher-log.txt` | launcher | one line per update check and start; rate limit, 404, 5xx, no network are named |

### Logs and bug reports

The DLL's log is `bin\ts_coop.log` (the current run). At the start of the next run the old logs rotate:
`ts_coop.log` -> `ts_coop.1.log` -> ... -> `ts_coop.4.log` (five kept, the newest first); a previous log under 4096 bytes (a start
that never left the menu) is deleted instead of rotated; `TS_LOG` set = no rotation (test runs). The players' readmes and the
issue form (`.github\ISSUE_TEMPLATE\bug_report.yml`) tell players to run `<game>\coop\Collect logs.bat`: it runs
`coop\collect-logs.ps1` and puts a zip on the desktop (`report.txt` with mod version, exe/DLL hashes, Windows, UI language,
GPU and driver, memory, install record summary; the cleaned logs; the install files; `files.txt` with names and sizes of
`UserConfigs` and `coop`, no contents). Cleaned: user name, computer name, `C:\Users\<x>`, game path, in-game player name,
public IPs (LAN and loopback kept), e-mails, MACs. The script also takes `-Game <folder>` and `-NoPause`.

## The launcher contract

Players start the game with the "TimeShift Co-op" shortcut, which runs `coop\coop-launcher.ps1`. Before
the game starts it reads `https://api.github.com/repos/dolbaras/timeshift-coop/releases/latest` and
installs that release when its tag is newer than `coop\version.txt`; then it starts `bin\TimeShift.Exe`
with the working directory `bin`. A 404 (no release yet), no network, a slow answer (6 s), GitHub's rate limit (403/429
with `X-RateLimit-Remaining: 0`, 60 requests an hour without a login) or a version that is not `x.y.z` (a `-dev` build) just
start the game (one line in `coop\launcher-log.txt`). A game of this folder that already runs: the launcher does nothing.
A failed game start shows a message box (the shortcut hides the window). Publishing a release therefore updates every
player who uses the shortcut, so a release must be finished and tested before it goes up.

What every later release has to keep (this is what lets an installed 0.1.0 update itself to 0.1.1, and the same later):

- **Tag** `v<major>.<minor>.<patch>` (parsed as a .NET `[version]`), a full release: drafts and
  pre-releases are not "latest".
- **Asset** named `timeshift-coop-<version>.zip` (the first asset matching `timeshift-coop-*.zip`),
  under `https://github.com/dolbaras/timeshift-coop/releases/download/` (any other download URL is ignored). The launcher
  checks the download against the `sha256:` digest GitHub gives for the asset (made by uploading with `gh release
  create`/`upload`); a release whose asset has no digest is installed without a check (logged: "the release gives no
  checksum"), a digest that does not match stops the update with a message box and the game starts with the old version.
- **`coop-installer.ps1` at the root of the zip**, taking `-Game <folder> -AutoClose -NoShortcut`:
  install into that folder with the window, close it after a successful install, exit code 0 on
  success and non-zero on failure; `-NoShortcut` refreshes only the shortcuts the record lists. The launcher runs it with a
  hidden PowerShell 5.1, waits for it, clears the start flag and goes on only when it exited 0 (otherwise: the message box
  "The update could not be installed" and the old version starts).
- **`files\coop\version.txt`** holding the version without the `v`.
- **`<game>\coop\install.txt`**, the record the next installer reads: `version=`, then one line per
  `file=`, `backup=`, `built=` (the archives in `preload\paks`) and `shortcut=`, and `arcname=` (the
  arcName value before the install, put back on uninstall) and `exe=`. A new installer must read the record of every earlier
  release (0.1.0 has no `exe=` line and no `steam.txt`: both optional).

A change to any of these needs a launcher that copes with both the old and the new form, shipped one
release before the old form is dropped.

Self-update 0.1.0 -> 0.1.1 was run end to end on 2026-10-31 on a scratch copy (see Tests): the published 0.1.0 zip installed,
the 0.1.0 launcher given a feed offering `v0.1.1`, a zip built from HEAD with `TS_VERSION 0.1.1` -> download, checksum ok,
installer exit 0 in 9 s, version, record, DLL, `coop\collect-logs.ps1` and `Collect logs.bat` updated, the shortcut kept, no temp
folders, start flag cleared; uninstall by the new installer left the listing equal to the clean copy.

## Dry run from a clean clone (2026-10-28)

`git clone` the repo, `sh native/build.sh` (zig path in `ZIG`), `powershell.exe -File tools\package.ps1`. The clone must sit
three levels below the workspace root (so `..\..\..\tools\vigem-dl\python-3.12.10-embed-amd64.zip` resolves) or get
`-PythonZip <path>`. Done with HEAD 1d3a40a: 28 zip entries, no game files; no "Serj", "Ajbol", "@gmail" or a user profile
path in the DLL or in any text file (the only hits are the repository name in the launcher and Python's own strings);
`native\build\`, `dist\` and `__pycache__` are git-ignored and every file the package needs is tracked (data\coop_display.py,
native\coopsave.c...). The version was `0.0.1-dev` in ts.h, version.txt and the DLL at that time (ts.h is 0.1.0 since 2026-10-28). A quiet
install of the zip into a copy of the clean game and the uninstall left 5426 files with the same SHA-256 listing; the
installed exe is the large-address-aware one (DE1A0AEB...), the original comes back; the log of an uninstall stays in `%TEMP%`.
Repeat it for 0.1.1 (the package now has more files: `collect-logs.ps1`, `Collect logs.bat`).

## Tests before a release (Windows PowerShell 5.1: `powershell.exe`, never pwsh)

Work on a copy of a clean game (`robocopy <game> work\install_test /E`) and a SHA-256 listing of it
taken before; never on the owner's install, never beside a running game from that folder.

- Quiet install (`-Quiet -ShortcutDir "<dir1>;<dir2>"`: two folders, like a player's desktop + Start
  menu), window install (`-Game <copy> -AutoClose`, no input needed), reinstall (archives kept, both
  shortcuts refreshed), a damaged archive (rebuilt), uninstall (`-Uninstall`, also `-Uninstall -AutoClose`
  in the window): the copy must equal the clean listing byte for byte.
- The installed archives must be byte-identical to `python -I tools\build_pak.py <copy> -o <tmp>`.
- A foreign `bin\dinput8.dll`: kept in `coop-backup`, still the original after a reinstall, put back.
- Exe classes: retail, large-address-aware, community pre-patched, Steam package exe, a variant, one code byte changed, a changed
  entry point, padding changed, a truncated file: the first five are accepted, the rest refused with nothing changed.
- A modified exe (a scratch folder with `bin\TimeShift.exe` + 1 byte): refused, nothing changed.
- Steam: a simulated Steam 1.0 folder (the 1.2 copy with a changed exe and bytes appended to `patch*.s3darc`) without a package
  (refusal text, en/ru), with bad packages (bad exe, `..` path, no patch archive, no exe, missing file: refused, nothing changed),
  with a good zip, without the zip's top folder, with an extracted folder, with `-AcceptSteamUpdate` missing (refused) and
  given; `user.ini`, `UserConfigs` and an `evil.exe` in the package are not taken; a rollback when `patch_nv.s3darc` is missing
  from the package; reinstall; uninstall = byte-identical listing; and once with the REAL community package.
- Edge matrix: a game path with Cyrillic and spaces and a TEMP folder with the same; read-only flags on the exe, ini and an
  archive; a folder or `bin` that denies writing (refused with the write text, nothing changed); a missing folder. Not run (needs a
  UAC click on a player's PC): the elevated copy for `Program Files (x86)`.
- `Install Co-op.bat` / `Uninstall Co-op.bat`: a positive start (returns in about 1 s) and a negative one (a flag that never
  appears: the message after 25 s, exit 1).
- Texts: every key exists in all six languages with the same placeholders (the installer and the launcher), and a picture of the
  installer window in each language (Steam page and plain page) has no clipped text.
- Game running: a renamed `ping.exe` in the copy's `bin` with `-GameProcess <its name>`: install and
  uninstall refused; the same process in another folder does not count.
- No `%TEMP%\timeshift-coop-*` folders left after any of these.
- The setup exe (`package.ps1 -Exe`, then `lab\setup_exe_test.ps1 -Exe <exe>` and `lab\setup_exe_steam_test.ps1 -Exe <exe>`
  in the workspace): `--extract` into a Cyrillic path, quiet install / reinstall / uninstall through the exe on a fake game
  folder under `work\setup_exe_test` (listing equal to the clean game afterwards, installer messages and exit codes through the
  redirected handles, shortcuts made and removed), a failed run's exit code, a game path with spaces and a trailing backslash, a
  TEMP with Cyrillic and spaces, no `timeshift-coop-setup-*` folder left; the Steam flow (refused without `-AcceptSteamUpdate`,
  installed with it, uninstall = the Steam 1.0 stand-in); once the window (`-AutoClose`) and the error boxes (see
  `notes\installer-exe.md`).
- Launcher: serve a releases/latest-style JSON and the zip locally
  (`python -I -m http.server 8765 --bind 127.0.0.1 --directory <feed>`), install the PREVIOUS PUBLISHED zip, then
  `coop-launcher.ps1 -Feed http://127.0.0.1:8765/latest.json -NoStart` (or `TIMESHIFT_COOP_FEED` for a
  start through the shortcut): the version, the record and both shortcuts after the update; an
  up-to-date feed, a 404 feed (also 403 with the rate-limit headers, 429, 500, an old tag) and the real GitHub URL start the
  game without a word; a feed with a wrong digest stops the update. The previous release's launcher is the one that runs the
  update, so use its zip, not the new one.
- The bug-report collector on a game folder with strings that must be cleaned (user name, computer name, public IP, e-mail,
  MAC, the profile's player name): the zip holds none of them.

## Published releases

| Tag | Date | Asset SHA-256 | Notes |
|---|---|---|---|
| v0.1.0 | 2026-10-28 | `6b4f37a60c92ab3c1a7b3be206a8a6e99adf8619129bbc03f5b54b5e45fd9100` | first public build, commit 9a8b81e; the release page lists what is not finished |

### 0.1.1: what was run before it (2026-11-01, from a clean clone of the release commit; zip and setup exe built with `package.ps1 -Exe -Dist <temp>`)

- Package audit: 30 zip entries with `/` separators, `payload.txt` = the 25 files under `files/`, `version.txt` 0.1.1, the DLL carries `0.1.1`, the exe's
  `--extract` equals the zip file by file (SHA-256) and the zip is inside the exe byte for byte. The DLL built in the clone differs from the working-tree DLL
  that was tested only in the PE timestamp and debug-directory id bytes (20 bytes; the compiler stamps them from the build path): the code is identical.
- Installer on the N100 (Windows 10, a clean copy of the game with a Cyrillic path): from the zip (quiet install, reinstall, uninstall = clean listing by SHA-256;
  the built archives equal those built on the main PC; the exe gets the LAA flag), from `Install Co-op.bat` (no -Quiet, `-AutoClose`: returns in 5 s, installs,
  flag cleared), from the setup exe (`lab\setup_exe_test.ps1`, 20 checks) and the Steam branch through the exe (`lab\setup_exe_steam_test.ps1`, 7 checks: refused without
  `-AcceptSteamUpdate`, installed with it and the real community package, uninstall = the Steam 1.0 stand-in). A quiet install through the .bat is not a supported
  path (the start flag is not written in -Quiet mode, the .bat waits 25 s and pauses).
- Self-update 0.1.0 -> 0.1.1 through the real shortcut (its own target and arguments, started in the N100 user's session, local feed with the assets in the
  order [setup exe, zip]): the launcher took the zip (checksum ok, installer exit 0), the game started with the 0.1.1 DLL, the shortcut stayed, nothing was left in
  `%TEMP%`, uninstall left a clean game.
- Game runs with the release DLL and data: 24-level bot (host + client), split screen smoke (levels 2, 12, turret levels 8 and 23 with firing), pad-only address entry,
  Regroup and the gate hint, Screen mode row, Continue with a client on the real N100 + main PC pair, netem 5 % and 15 % loss.

The release is made with `gh release create v<version> dist\timeshift-coop-<version>.zip dist\timeshift-coop-setup-<version>.exe
--repo dolbaras/timeshift-coop --target main --title "TimeShift Co-op <version>" --notes-file <notes>` after pushing `main`; then compare the asset's
`digest` in `gh release view --json assets` with the SHA-256 of the local zip and check that `releases/latest` answers
the new tag. Before the release the version in `native/ts.h` is bumped, the DLL and the zip are rebuilt from a clean clone,
the readmes' "Not finished" list and claims are checked against the roadmap, and the notes (`notes\releases\relnotes-<ver>.md`,
private draft) are finished by the lead.
