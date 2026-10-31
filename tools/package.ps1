# Build the release archive dist\timeshift-coop-<version>.zip:
#   Install Co-op.bat + coop-installer.ps1   the installer (backs up what it replaces, uninstalls)
#   README-COOP.txt, README-COOP-ru.txt, LICENSE.txt
#   files\                                   what goes into the game folder:
#     bin\dinput8.dll                        the proxy DLL (native\build.sh)
#     coop\tools\build_pak.py, s3darc.py     the data builder the installer runs on the player's files
#     coop\data\...                          the recipes (no game files)
#     coop\python\...                        python.org's embeddable CPython 3.12.10 (amd64), trimmed
#                                            to what build_pak needs, PSF license in it
#     coop\version.txt                       TS_VERSION from native\ts.h (the one source of the version)
#     coop\coop-launcher.ps1                 behind the "TimeShift Co-op" shortcut (auto-update)
#     coop\README-COOP*.txt, coop\payload.txt
# The package holds no game files: the data archives are built on the player's PC.
# Run native\build.sh first. Usage: package.ps1 [-Version <TS_VERSION>] [-PythonZip <python-3.12.10-embed-amd64.zip>] [-Exe] [-Dist <folder>]
# -Exe also builds dist\timeshift-coop-setup-<version>.exe: a single-file installer that carries this zip (an ADDITIONAL release
# asset; the zip stays the one the shortcut's launcher updates from). Made by <workspace>\tools\mkinstaller (-Mkinstaller <folder>
# for a clone placed elsewhere); the exe is unsigned. -Dist: write to that folder instead of dist\ (tests: dist\ holds the published zips).
param([string] $Version = "", [string] $PythonZip = "", [switch] $Exe, [string] $Mkinstaller = "", [string] $Dist = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$tsh = Get-Content -LiteralPath (Join-Path $root "native\ts.h") -Raw
if ($tsh -notmatch '#define\s+TS_VERSION\s+"([^"]+)"') { throw "TS_VERSION not found in native\ts.h" }
$tsVersion = $Matches[1]
if (-not $Version) { $Version = $tsVersion }
if ($Version -ne $tsVersion) { throw "native\ts.h has TS_VERSION $tsVersion, not $Version" }
function Sha256($p) {
    $fs = [IO.File]::OpenRead($p)
    try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() }
}
# The DLL must be built from this ts.h: its log line "TimeShift co-op proxy %s loaded" and the version.
$dll = Join-Path $root "native\build\dinput8.dll"
if (-not (Test-Path -LiteralPath $dll)) { throw "build native\build\dinput8.dll first (sh native/build.sh)" }
$dllText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
if (-not $dllText.Contains("TimeShift co-op proxy")) { throw "native\build\dinput8.dll is not the co-op proxy" }
if (-not $dllText.Contains("`0$Version`0")) { throw "native\build\dinput8.dll was not built with TS_VERSION $Version (rebuild it)" }
# python.org's embeddable CPython, SHA-256 pinned; only what build_pak.py, s3darc.py and the
# recipes need (zlib, hashlib's sha256, tempfile are built into python312.dll / python312.zip).
# (default: the workspace's tools folder three levels above the repo; -PythonZip for a clone placed elsewhere)
$pyZip = if ($PythonZip) { $PythonZip } else { Join-Path $root "..\..\..\tools\vigem-dl\python-3.12.10-embed-amd64.zip" }
$pySha = "4ACBED6DD1C744B0376E3B1CF57CE906F9DC9E95E68824584C8099A63025A3C3"
if (-not (Test-Path -LiteralPath $pyZip)) { throw "tools\vigem-dl\python-3.12.10-embed-amd64.zip missing (python.org)" }
if ((Sha256 $pyZip) -ne $pySha) { throw "the Python zip is not the pinned one" }
$pyKeep = @("python.exe", "python312.dll", "python312.zip", "python312._pth", "vcruntime140.dll", "vcruntime140_1.dll", "LICENSE.txt")

$dist = if ($Dist) { $Dist } else { Join-Path $root "dist" }
$stage = Join-Path $dist "timeshift-coop-$Version"
$files = Join-Path $stage "files"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $files "bin"), (Join-Path $files "coop\python"), (Join-Path $files "coop\tools") | Out-Null
Copy-Item -LiteralPath $dll (Join-Path $files "bin")
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $pyZip).Path)
try {
    foreach ($n in $pyKeep) {
        $e = $z.GetEntry($n)
        if (-not $e) { throw "$n missing in the Python zip" }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $files "coop\python\$n"))
    }
} finally { $z.Dispose() }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "build_pak.py"), (Join-Path $PSScriptRoot "s3darc.py") (Join-Path $files "coop\tools")
# The recipes: every file of data\ except Python's caches.
$data = Join-Path $root "data"
Get-ChildItem -LiteralPath $data -Recurse -File | Where-Object { $_.FullName -notmatch '\\__pycache__\\' -and $_.Extension -ne ".pyc" } | ForEach-Object {
    $dst = Join-Path $files ("coop\data\" + $_.FullName.Substring($data.Length + 1))
    New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
    Copy-Item -LiteralPath $_.FullName $dst
}
[IO.File]::WriteAllText((Join-Path $files "coop\version.txt"), "$Version`r`n", (New-Object Text.ASCIIEncoding))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "launcher.ps1") (Join-Path $files "coop\coop-launcher.ps1")
# The bug-report collector (tools\collect-logs.ps1) and a double-clickable start for it: <game>\coop\Collect logs.bat
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "collect-logs.ps1") (Join-Path $files "coop\collect-logs.ps1")
[IO.File]::WriteAllText((Join-Path $files "coop\Collect logs.bat"),
    "@powershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0collect-logs.ps1`"`r`n", (New-Object Text.ASCIIEncoding))
Copy-Item -LiteralPath (Join-Path $root "docs\release-readme.txt") (Join-Path $files "coop\README-COOP.txt")
Copy-Item -LiteralPath (Join-Path $root "docs\release-readme-ru.txt") (Join-Path $files "coop\README-COOP-ru.txt")
# Every file of the payload: the installer checks the folder it got against it before touching the game.
$list = @(Get-ChildItem -LiteralPath $files -Recurse -File | ForEach-Object { $_.FullName.Substring($files.Length + 1) }) + "coop\payload.txt"
[IO.File]::WriteAllLines((Join-Path $files "coop\payload.txt"), [string[]]$list, (New-Object Text.ASCIIEncoding))
# The installer and the texts at the archive's root.
Copy-Item -LiteralPath (Join-Path $PSScriptRoot "installer.ps1") (Join-Path $stage "coop-installer.ps1")
# The installer starts hidden (its own window is the interface) and writes a flag file as its first act; when the flag does
# not appear within 25 s (Windows, Defender or a policy stopped PowerShell and the hidden window swallowed the message) the
# batch file says so on screen instead of leaving the player with nothing.
$bat = @'
@echo off
set "FLAG=%TEMP%\timeshift-coop-installer-started.flag"
del "%FLAG%" >nul 2>&1
start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0coop-installer.ps1" %*
for /l %%i in (1,1,25) do (
  if exist "%FLAG%" goto started
  ping -n 2 127.0.0.1 >nul
)
echo TimeShift Co-op: the installer window did not start (PowerShell blocked by Windows, a policy or an antivirus?).
echo See README-COOP.txt, "Troubleshooting". Log: %TEMP%\timeshift-coop-install.log
pause
exit /b 1
:started
del "%FLAG%" >nul 2>&1
'@
[IO.File]::WriteAllText((Join-Path $stage "Install Co-op.bat"), (($bat -split "`r?`n") -join "`r`n") + "`r`n", (New-Object Text.ASCIIEncoding))
Copy-Item -LiteralPath (Join-Path $root "docs\release-readme.txt") (Join-Path $stage "README-COOP.txt")
Copy-Item -LiteralPath (Join-Path $root "docs\release-readme-ru.txt") (Join-Path $stage "README-COOP-ru.txt")
Copy-Item -LiteralPath (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
# (ZipFile, not Compress-Archive: Windows PowerShell 5.1's writes "\" into entry names)
$zip = Join-Path $dist "timeshift-coop-$Version.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
$out = [IO.Compression.ZipFile]::Open($zip, "Create")
try {
    foreach ($f in Get-ChildItem -LiteralPath $stage -Recurse -File) {
        $name = $f.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($out, $f.FullName, $name, "Optimal")
    }
} finally { $out.Dispose() }
Remove-Item -LiteralPath $stage -Recurse -Force
"packaged $zip ($([int]((Get-Item -LiteralPath $zip).Length / 1KB)) KB)"
if ($Exe) {
    $mk = if ($Mkinstaller) { $Mkinstaller } else { Join-Path $root "..\..\..\tools\mkinstaller" }
    $mkScript = Join-Path $mk "mkinstaller.ps1"
    if (-not (Test-Path -LiteralPath $mkScript)) { throw "tools\mkinstaller\mkinstaller.ps1 missing (the workspace's tools folder; -Mkinstaller <folder>)" }
    $exeOut = Join-Path $dist "timeshift-coop-setup-$Version.exe"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $mkScript -Zip $zip -Out $exeOut -Product "TimeShift Co-op" -Version $Version `
        -Description "TimeShift Co-op installer" -TempPrefix "timeshift-coop-setup" -Accent "227cce"
    if ($LASTEXITCODE -ne 0) { throw "mkinstaller.ps1 failed" }
}
