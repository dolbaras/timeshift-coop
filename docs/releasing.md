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
| `files\coop\version.txt`, `files\coop\payload.txt`, `files\coop\README-COOP*.txt` | version, the package's file list, the texts |

## What the installer does

1. Checks the folder: `bin\TimeShift.exe` must be patch 1.2 (SHA-256 `689c63da...52641be4`), else it stops
   and names the version needed; the game must not run from this folder; `patch.s3darc`, `patch_ati`,
   `patch_nv`, `out.s3darc` must be there; the payload must match `payload.txt`.
2. Builds `coop.s3darc`, `coop_ati.s3darc`, `coop_nv.s3darc` with the payload's own Python into
   `%TEMP%\timeshift-coop-build-<pid>`, before the game folder is changed. A reinstall keeps the installed
   archives when `coop\coop_build.txt` names the same version and the same `patch.s3darc` SHA-256,
   `coop\paks.txt` the same recipes hash (builder + recipes + version.txt) and each installed archive
   still has the SHA-256 recorded there, and arcName is `coop.s3darc`.
3. Copies `files\` (a file that is not the mod's is kept once in `coop-backup\`, e.g. another
   `bin\dinput8.dll`; a co-op DLL is recognised by its log line "TimeShift co-op proxy" and never kept
   as an original), the archives into `preload\paks\`, and sets `[Preload] arcName = coop.s3darc` in
   `bin\steamwars.ini` by replacing only the value's bytes (CRLF kept; the game skips bare-LF lines).
4. Shortcuts, then `coop\install.txt`, then `coop\version.txt` last.

Uninstall: arcName back to the recorded value first, then the listed files, the built archives,
`bin\ts_coop.log` (the DLL's log), the shortcuts, the backups put back, `coop\` and `coop-backup\`
removed. `UserConfigs\` (profiles, saves) is never touched.

## The launcher contract

Players start the game with the "TimeShift Co-op" shortcut, which runs `coop\coop-launcher.ps1`. Before
the game starts it reads `https://api.github.com/repos/dolbaras/timeshift-coop/releases/latest` and
installs that release when its tag is newer than `coop\version.txt`; then it starts `bin\TimeShift.Exe`
with the working directory `bin`. A 404 (no release yet), no network, a slow answer (6 s) or a version
that is not `x.y.z` (a `-dev` build) just start the game. Publishing a release therefore updates every
player who uses the shortcut, so a release must be finished and tested before it goes up.

What every later release has to keep:

- **Tag** `v<major>.<minor>.<patch>` (parsed as a .NET `[version]`), a full release: drafts and
  pre-releases are not "latest".
- **Asset** named `timeshift-coop-<version>.zip` (the first asset matching `timeshift-coop-*.zip`),
  under `https://github.com/dolbaras/timeshift-coop/releases/download/`. The launcher checks the
  download against the `sha256:` digest GitHub gives for the asset.
- **`coop-installer.ps1` at the root of the zip**, taking `-Game <folder> -AutoClose -NoShortcut`:
  install into that folder with the window, close it after a successful install, exit code 0 on
  success and non-zero on failure; `-NoShortcut` refreshes only the shortcuts the record lists.
- **`files\coop\version.txt`** holding the version without the `v`.
- **`<game>\coop\install.txt`**, the record the next installer reads: `version=`, then one line per
  `file=`, `backup=`, `built=` (the archives in `preload\paks`) and `shortcut=`, and `arcname=` (the
  arcName value before the install, put back on uninstall).

A change to any of these needs a launcher that copes with both the old and the new form, shipped one
release before the old form is dropped.

## Dry run from a clean clone (2026-10-28)

`git clone` the repo, `sh native/build.sh` (zig path in `ZIG`), `powershell.exe -File tools\package.ps1`. The clone must sit
three levels below the workspace root (so `..\..\..\tools\vigem-dl\python-3.12.10-embed-amd64.zip` resolves) or get
`-PythonZip <path>`. Done with HEAD 1d3a40a: 28 zip entries, no game files; no "Serj", "Ajbol", "@gmail" or a user profile
path in the DLL or in any text file (the only hits are the repository name in the launcher and Python's own strings);
`native\build\`, `dist\` and `__pycache__` are git-ignored and every file the package needs is tracked (data\coop_display.py,
native\coopsave.c...). The version is `0.0.1-dev` in ts.h, version.txt and the DLL (ts.h is 0.1.0 since 2026-10-28). A quiet
install of the zip into a copy of the clean game and the uninstall left 5426 files with the same SHA-256 listing; the
installed exe is the large-address-aware one (DE1A0AEB...), the original comes back; the log of an uninstall stays in `%TEMP%`.

## Tests before a release (Windows PowerShell 5.1: `powershell.exe`, never pwsh)

Work on a copy of a clean game (`robocopy <game> work\install_test /E`) and a SHA-256 listing of it
taken before; never on the owner's install, never beside a running game from that folder.

- Quiet install (`-Quiet -ShortcutDir "<dir1>;<dir2>"`: two folders, like a player's desktop + Start
  menu), window install (`-Game <copy> -AutoClose`, no input needed), reinstall (archives kept, both
  shortcuts refreshed), a damaged archive (rebuilt), uninstall (`-Uninstall`, also `-Uninstall -AutoClose`
  in the window): the copy must equal the clean listing byte for byte.
- The installed archives must be byte-identical to `python -I tools\build_pak.py <copy> -o <tmp>`.
- A foreign `bin\dinput8.dll`: kept in `coop-backup`, still the original after a reinstall, put back.
- A modified exe (a scratch folder with `bin\TimeShift.exe` + 1 byte): refused, nothing changed.
- Game running: a renamed `ping.exe` in the copy's `bin` with `-GameProcess <its name>`: install and
  uninstall refused; the same process in another folder does not count.
- No `%TEMP%\timeshift-coop-*` folders left after any of these.
- Launcher: serve a releases/latest-style JSON and the zip locally
  (`python -m http.server 8765 --bind 127.0.0.1`), install the previous version, then
  `coop-launcher.ps1 -Feed http://127.0.0.1:8765/latest.json -NoStart` (or `TIMESHIFT_COOP_FEED` for a
  start through the shortcut): the version, the record and both shortcuts after the update; an
  up-to-date feed, a 404 feed and the real GitHub URL start the game without a word.
