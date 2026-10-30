# TimeShift Co-op: collects what a bug report needs into ONE zip on the desktop, without personal data.
#   powershell -NoProfile -ExecutionPolicy Bypass -File collect-logs.ps1 [-Game <game folder>] [-OutDir <folder>] [-NoPause]
# In the zip:
#   report.txt        mod version, install record summary, SHA-256 of TimeShift.exe and of the mod's DLL, Windows version and
#                     UI language, graphics adapter names and driver versions, memory
#   logs\*.log        bin\ts_coop.log and ts_coop.1..4.log (the mod's logs), cleaned (see below)
#   coop\install.txt, install-log.txt, version.txt, coop_build.txt, paks.txt   (cleaned)
#   files.txt         NAMES of the files in UserConfigs and coop (no contents, no profile files)
# Cleaned: the Windows user name, the computer name, C:\Users\<name> paths, the game folder path, the in-game player name
# and every public IP address (private/LAN/loopback addresses stay: they help to read a session), e-mail addresses, MAC addresses.
# Nothing is uploaded. The zip can be opened and checked before it is attached to a GitHub issue.
param(
    [string] $Game = "",
    [string] $OutDir = "",
    [switch] $NoPause
)
$ErrorActionPreference = "Stop"

function Test-Game($p) { $p -and (Test-Path -LiteralPath (Join-Path $p "bin\TimeShift.exe")) }
function Find-Game {
    $c = New-Object System.Collections.Generic.List[string]
    $d = $PSScriptRoot
    for ($i = 0; $i -lt 3 -and $d; $i++) { $c.Add($d); $d = Split-Path $d -Parent }   # coop\collect-logs.ps1 -> game folder
    foreach ($k in "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*", "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
                   "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*") {
        Get-ItemProperty $k -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like "*TimeShift*" } | ForEach-Object {
            foreach ($p in $_.InstallLocation, $_."Inno Setup: App Path") { if ($p) { $c.Add("$p".Trim('"')) } }
        }
    }
    $steam = (Get-ItemProperty "HKCU:\SOFTWARE\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) { $c.Add((Join-Path $steam "steamapps\common\TimeShift")) }
    foreach ($p in "${env:ProgramFiles(x86)}\Sierra\TimeShift", "$env:ProgramFiles\Sierra\TimeShift", "${env:ProgramFiles(x86)}\TimeShift",
                   "$env:ProgramFiles\TimeShift", "C:\Games\TimeShift", "D:\Games\TimeShift") { $c.Add($p) }
    foreach ($p in $c) { if (Test-Game $p) { return (Resolve-Path -LiteralPath $p).Path.TrimEnd('\') } }
    $null
}
function Done($code) {
    if (-not $NoPause -and [Environment]::UserInteractive) { Read-Host "Press Enter to close" | Out-Null }
    exit $code
}

if (-not $Game) { $Game = Find-Game }
if (-not (Test-Game $Game)) {
    Write-Host "The TimeShift folder was not found. Run this again with:  -Game ""<folder that contains bin\TimeShift.exe>"""
    Done 1
}
$Game = (Resolve-Path -LiteralPath $Game).Path.TrimEnd('\')
if (-not $OutDir) { $OutDir = [Environment]::GetFolderPath("Desktop") }
if (-not $OutDir -or -not (Test-Path -LiteralPath $OutDir)) { $OutDir = [IO.Path]::GetTempPath() }

# ---- the cleaner ----
$enc = try { [Text.Encoding]::GetEncoding([Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage) } catch { [Text.Encoding]::GetEncoding(28591) }
$literals = New-Object System.Collections.Generic.List[object]   # @(text, replacement)
function Add-Literal([string] $text, [string] $to) { if ($text -and $text.Trim().Length -ge 3) { $script:literals.Add(@($text.Trim(), $to)) } }
# longer first: a path before the user name inside it
Add-Literal $Game "<game>"
Add-Literal $env:USERPROFILE "<profile>"
Add-Literal $env:USERNAME "<user>"
Add-Literal (Split-Path $env:USERPROFILE -Leaf) "<user>"
Add-Literal $env:COMPUTERNAME "<computer>"
foreach ($ini in (Join-Path $Game "UserConfigs\*\user.ini")) {
    foreach ($f in Get-ChildItem -Path $ini -ErrorAction SilentlyContinue) {
        foreach ($line in Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue) {
            if ($line -match '^\s*PlayerName\s*=\s*"?([^"\r\n]+)"?\s*$' -and $Matches[1] -notin @("Default", "Player")) { Add-Literal $Matches[1] "<player>" }
        }
    }
}
$literals = @($literals | Sort-Object { - $_[0].Length })
$litRx = @($literals | ForEach-Object { , @([regex]::new([regex]::Escape($_[0]), 'IgnoreCase'), $_[1]) })
$rxUsers = [regex]::new('(?i)([A-Z]:\\Users\\)[^\\/\s"''<>|:*?]+', 'IgnoreCase')
$rxMail = [regex]::new('[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}')
$rxMac = [regex]::new('\b[0-9A-Fa-f]{2}([:\-][0-9A-Fa-f]{2}){5}\b')
$rxIp = [regex]::new('\b(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\b')
$ipEval = {
    param($m)
    $a = [int]$m.Groups[1].Value; $b = [int]$m.Groups[2].Value; $c2 = [int]$m.Groups[3].Value; $d = [int]$m.Groups[4].Value
    if ($a -gt 255 -or $b -gt 255 -or $c2 -gt 255 -or $d -gt 255) { return $m.Value }          # not an address (version numbers etc.)
    if ($a -eq 10 -or $a -eq 127 -or $a -eq 0 -or ($a -eq 192 -and $b -eq 168) -or ($a -eq 172 -and $b -ge 16 -and $b -le 31) -or
        ($a -eq 169 -and $b -eq 254) -or $a -ge 224) { return $m.Value }
    "<public-ip>"
}
function Clean-Line([string] $s) {
    foreach ($l in $script:litRx) { $s = $l[0].Replace($s, $l[1]) }
    $s = $rxUsers.Replace($s, '${1}<user>')
    $s = $rxMail.Replace($s, "<email>")
    $s = $rxMac.Replace($s, "<mac>")
    $rxIp.Replace($s, $ipEval)
}
function Clean-File([string] $src, [string] $dst) {
    $r = New-Object IO.StreamReader($src, $enc)
    $w = New-Object IO.StreamWriter($dst, $false, $enc)
    try { while ($null -ne ($line = $r.ReadLine())) { $w.WriteLine((Clean-Line $line)) } } finally { $r.Dispose(); $w.Dispose() }
}

# ---- stage ----
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stage = Join-Path ([IO.Path]::GetTempPath()) ("ts-report-" + $stamp)
New-Item -ItemType Directory -Force (Join-Path $stage "logs"), (Join-Path $stage "coop") | Out-Null
try {
    $bin = Join-Path $Game "bin"
    $coop = Join-Path $Game "coop"
    $nLogs = 0
    foreach ($f in Get-ChildItem -LiteralPath $bin -Filter "ts_coop*.log" -ErrorAction SilentlyContinue) {
        Clean-File $f.FullName (Join-Path $stage ("logs\" + $f.Name)); $nLogs++
    }
    foreach ($n in "install.txt", "install-log.txt", "version.txt", "coop_build.txt", "paks.txt") {
        $p = Join-Path $coop $n
        if (Test-Path -LiteralPath $p) { Clean-File $p (Join-Path $stage ("coop\" + $n)) }
    }

    # file names only: UserConfigs and coop (a profile folder name is the player's choice: cleaned like the logs)
    $names = New-Object System.Collections.Generic.List[string]
    foreach ($d in "UserConfigs", "coop") {
        $root = Join-Path $Game $d
        if (Test-Path -LiteralPath $root) {
            foreach ($f in Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue) {
                if ($f.FullName -like (Join-Path $root "python*")) { continue }
                $names.Add((Clean-Line ($d + $f.FullName.Substring($root.Length))) + "  " + $f.Length + " bytes")
            }
        }
    }
    Set-Content -LiteralPath (Join-Path $stage "files.txt") -Value $names -Encoding UTF8

    # report.txt
    $os = Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue
    $gpu = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { "{0}, driver {1}" -f $_.Name, $_.DriverVersion })
    $exe = Join-Path $bin "TimeShift.exe"
    $dll = Join-Path $bin "dinput8.dll"
    function Sha($p) {   # (.NET: Get-FileHash is missing on some Windows installs)
        if (-not (Test-Path -LiteralPath $p)) { return "(missing)" }
        try {
            $fs = [IO.File]::OpenRead($p)
            try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() }
        } catch { "(unreadable: the game is running?)" }
    }
    $ver = if (Test-Path -LiteralPath (Join-Path $coop "version.txt")) { "$(Get-Content -LiteralPath (Join-Path $coop 'version.txt') -TotalCount 1)".Trim() } else { "(no coop\version.txt: mod not installed?)" }
    $rec = @()
    $recPath = Join-Path $coop "install.txt"
    if (Test-Path -LiteralPath $recPath) {
        $all = @(Get-Content -LiteralPath $recPath)
        $rec = @("install record: {0} lines, {1} file= entries, {2} backup= entries" -f $all.Count, @($all | Where-Object { $_ -like "file=*" }).Count, @($all | Where-Object { $_ -like "backup=*" }).Count)
        $rec += @($all | Where-Object { $_ -like "version=*" -or $_ -like "exe=*" -or $_ -like "arcname=*" } | ForEach-Object { Clean-Line $_ })
    } else { $rec = @("install record: coop\install.txt not found") }
    $lines = @(
        "TimeShift Co-op report, made $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (local time)",
        "mod version (coop\version.txt): $ver",
        "TimeShift.exe SHA-256: $(Sha $exe)",
        "dinput8.dll (mod) SHA-256: $(Sha $dll)",
        "Windows: $($os.Caption) $($os.Version) build $($os.BuildNumber), $($os.OSArchitecture)",
        "Windows UI language: $([Globalization.CultureInfo]::CurrentUICulture.Name), culture $([Globalization.CultureInfo]::CurrentCulture.Name)",
        "memory: $([math]::Round($os.TotalVisibleMemorySize / 1MB, 1)) GB total, $([math]::Round($os.FreePhysicalMemory / 1MB, 1)) GB free",
        ("graphics: " + ($gpu -join "; ")),
        "logs included: $nLogs (bin\ts_coop*.log)"
    ) + $rec
    Set-Content -LiteralPath (Join-Path $stage "report.txt") -Value $lines -Encoding UTF8

    # ---- zip ----
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = Join-Path $OutDir ("timeshift-coop-report-" + $stamp + ".zip")
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
    Write-Host ""
    Write-Host "Report written to:"
    Write-Host "  $zip"
    Write-Host "It holds $nLogs log file(s), the version, the install record, hashes, Windows and graphics info and a list of file names."
    Write-Host "User name, computer name, folder paths, player name and public IP addresses were removed. Nothing was uploaded:"
    Write-Host "attach the zip to your GitHub issue (https://github.com/dolbaras/timeshift-coop/issues)."
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
Done 0
