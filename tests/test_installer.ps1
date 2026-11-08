# Offline tests of the players' installer with other mods' files in the game folder: an Ultimate ASI Loader (or another mod's
# DLL) as bin\dinput8.dll, the other proxy DLLs mods use (d3d9, ddraw, dsound, version, winmm), plugin folders; install,
# reinstall, update (what the shortcut's launcher runs: coop-installer.ps1 -Game <folder> -NoShortcut, here -Quiet), uninstall.
# Stand-in game folders are made next to -GameFrom (a lab COPY of TimeShift 1.2, default <workspace>\games\timeshift\work\game;
# never a Steam library or a player's install): its bin\TimeShift.exe (the .orig when there is one), bin\steamwars.ini (.orig) and
# the small preload\paks\patch*.s3darc are copied, the big out*.s3darc are hard links (only read by the data build; checked
# unchanged at the end). The other mods' DLLs are made-up PE files that are only read, never loaded. No game runs, no network.
# -Zip <release zip>: tests that package instead of one built from this tree (the published 0.1.3 fails the new cases).
# -OldZip <release zip>: also that release installed over an ASI loader and then updated by this tree's installer.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_installer.ps1 [-GameFrom <folder>] [-Zip <zip>] [-OldZip <zip>] [-Keep]
param([string] $GameFrom = "", [string] $Zip = "", [string] $OldZip = "", [switch] $Keep)
$ErrorActionPreference = "Stop"
$env:PSModulePath = (($env:PSModulePath -split ";") | Where-Object { $_ -notlike "*\PowerShell\*" }) -join ";"   # Windows PowerShell 5.1 modules only
foreach ($z in "Zip", "OldZip") {
    $v = Get-Variable $z -ValueOnly
    if ($v) { if (-not (Test-Path -LiteralPath $v -PathType Leaf)) { throw "-${z}: $v not found" }; Set-Variable $z (Resolve-Path -LiteralPath $v).Path }
}
$root = Split-Path $PSScriptRoot -Parent
if (-not $GameFrom) {
    foreach ($c in (Join-Path (Split-Path $root -Parent) "game"), (Join-Path (Split-Path $root -Parent) "work\game")) { if (Test-Path -LiteralPath (Join-Path $c "bin")) { $GameFrom = $c; break } }
}
if (-not $GameFrom -or -not (Test-Path -LiteralPath (Join-Path $GameFrom "preload\paks\out.s3darc"))) { throw "-GameFrom: a lab copy of TimeShift 1.2 is needed ($GameFrom)" }
$GameFrom = (Resolve-Path -LiteralPath $GameFrom).Path
if ($GameFrom -match '(?i)\\steamapps\\' -or $GameFrom -notmatch '(?i)\\work\\') { throw "-GameFrom must be a lab copy under a work folder, never a game install: $GameFrom" }
$base = Join-Path (Split-Path $GameFrom -Parent) ("installer-test-" + $PID)
Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $base | Out-Null
$script:fails = 0; $script:checks = 0
function Check($cond, $name) { $script:checks++; if ($cond) { Write-Host "ok   $name" } else { Write-Host "FAIL $name"; $script:fails++ } }
function Sha($p) { if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return "(missing)" }; $fs = [IO.File]::OpenRead($p); try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() } }
# (the hard-linked archives by size and time: hashing a gigabyte per snapshot is slow, and they are never written)
function Snap($dir) {
    $t = @{}
    foreach ($f in Get-ChildItem -LiteralPath $dir -Recurse -File -Force) {
        $rel = $f.FullName.Substring($dir.Length + 1)
        $t[$rel] = if ($rel -match '(?i)^preload\\paks\\out.*\.s3darc$') { "$($f.Length) $($f.LastWriteTimeUtc.Ticks)" } else { Sha $f.FullName }
    }
    $t
}
function Snap-Diff($a, $b) {   # (not "Diff": an alias of Compare-Object in Windows PowerShell)
    @($a.Keys | Where-Object { -not $b.ContainsKey($_) -or $a[$_] -ne $b[$_] }) + @($b.Keys | Where-Object { -not $a.ContainsKey($_) }) | Sort-Object -Unique
}
function Run-PS([string] $script, [string[]] $a) {
    $o = Join-Path $base "out.txt"; $e = Join-Path $base "err.txt"
    Remove-Item $o, $e -ErrorAction SilentlyContinue
    $p = Start-Process powershell -ArgumentList (@("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$script`"") + $a) -Wait -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $o -RedirectStandardError $e
    $script:exit = $p.ExitCode
    $script:out = (Get-Content -LiteralPath $o -Raw -Encoding UTF8 -ErrorAction SilentlyContinue) + (Get-Content -LiteralPath $e -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
}
function Rec($g) { @(Get-Content -LiteralPath (Join-Path $g "coop\install.txt") -ErrorAction SilentlyContinue) }
# the workspace's tools folder: the first one above the repository that has it
function Ws-Tool($rel) { for ($d = Split-Path $root -Parent; $d; $d = Split-Path $d -Parent) { $p = Join-Path $d "tools\$rel"; if (Test-Path -LiteralPath $p) { return $p } }; $null }

# 1. the package: built from this tree (tools\package.ps1 into the test folder) unless -Zip
if (-not $Zip) {
    $dist = Join-Path $base "dist"
    $pkgOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\package.ps1") -Dist $dist -PythonZip (Ws-Tool "vigem-dl\python-3.12.10-embed-amd64.zip") -LogCollector (Ws-Tool "log-collector") 2>&1
    if ($LASTEXITCODE) { throw "package.ps1 failed: $pkgOut" }
    $Zip = @(Get-ChildItem -LiteralPath $dist -Filter "timeshift-coop-*.zip")[0].FullName
}
Write-Host "package: $Zip"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$za = [IO.Compression.ZipFile]::OpenRead($Zip)
try { $en = @($za.Entries | ForEach-Object FullName) } finally { $za.Dispose() }
$bs = @($en | Where-Object { $_.Contains('\') })
Check ($en.Count -ge 20 -and $bs.Count -eq 0 -and ($en -contains "files/bin/dinput8.dll") -and ($en -contains "coop-installer.ps1")) "the zip's $($en.Count) entry names use '/' ($($bs.Count) with '\')"
$x1 = Join-Path $base "x1"; $x2 = Join-Path $base "x2"
[IO.Compression.ZipFile]::ExtractToDirectory($Zip, $x1)
# x2: the same package as a later version (what an update brings)
Copy-Item -LiteralPath $x1 $x2 -Recurse
[IO.File]::WriteAllText((Join-Path $x2 "files\coop\version.txt"), "9.9.9`r`n", (New-Object Text.ASCIIEncoding))
$ourDll = Sha (Join-Path $x1 "files\bin\dinput8.dll")

# 2. stand-in game folders. New-FakePe: a minimal PE with an export table of these names (never loaded; the installer reads it).
function New-FakePe($path, [string[]] $exports, [int] $machine = 0x14C, [string] $extra = "") {
    $b = New-Object byte[] 0xA00
    $w16 = { param($o, $v) $b[$o] = [byte]($v -band 0xFF); $b[$o + 1] = [byte](($v -shr 8) -band 0xFF) }
    $w32 = { param($o, [long] $v) & $w16 $o ($v -band 0xFFFF); & $w16 ($o + 2) (($v -shr 16) -band 0xFFFF) }
    $b[0] = 0x4D; $b[1] = 0x5A; & $w32 0x3C 0x80
    $b[0x80] = 0x50; $b[0x81] = 0x45
    $pe64 = $machine -eq 0x8664
    & $w16 0x84 $machine; & $w16 0x86 2; $optsize = $(if ($pe64) { 0xF0 } else { 0xE0 }); & $w16 0x94 $optsize; & $w16 0x96 0x2102
    $opt = 0x98; & $w16 $opt $(if ($pe64) { 0x20B } else { 0x10B })
    $dirs = $opt + $(if ($pe64) { 112 } else { 96 }); & $w32 ($dirs - 4) 16; & $w32 $dirs 0x2000; & $w32 ($dirs + 4) 0x100
    $sec = $opt + $optsize
    [Text.Encoding]::ASCII.GetBytes(".text").CopyTo($b, $sec); & $w32 ($sec + 8) 0x200; & $w32 ($sec + 12) 0x1000; & $w32 ($sec + 16) 0x200; & $w32 ($sec + 20) 0x400
    [Text.Encoding]::ASCII.GetBytes(".edata").CopyTo($b, $sec + 40); & $w32 ($sec + 48) 0x400; & $w32 ($sec + 52) 0x2000; & $w32 ($sec + 56) 0x400; & $w32 ($sec + 60) 0x600
    & $w32 (0x600 + 20) $exports.Count; & $w32 (0x600 + 24) $exports.Count; & $w32 (0x600 + 32) 0x2040
    $str = 0x100
    for ($i = 0; $i -lt $exports.Count; $i++) {
        & $w32 (0x640 + 4 * $i) (0x2000 + $str)
        [Text.Encoding]::ASCII.GetBytes($exports[$i]).CopyTo($b, 0x600 + $str); $str += $exports[$i].Length + 1
    }
    if ($extra) { [Text.Encoding]::ASCII.GetBytes($extra).CopyTo($b, 0x480) }
    New-Item -ItemType Directory -Force (Split-Path $path -Parent) | Out-Null
    [IO.File]::WriteAllBytes($path, $b)
}
$ualExports = @("DirectInput8Create", "DrawDibDraw", "IsUltimateASILoader", "timeBeginPeriod", "timeGetTime")
$srcExe = if (Test-Path -LiteralPath (Join-Path $GameFrom "bin\TimeShift.Exe.orig")) { Join-Path $GameFrom "bin\TimeShift.Exe.orig" } else { Join-Path $GameFrom "bin\TimeShift.exe" }
$srcIni = if (Test-Path -LiteralPath (Join-Path $GameFrom "bin\steamwars.ini.orig")) { Join-Path $GameFrom "bin\steamwars.ini.orig" } else { Join-Path $GameFrom "bin\steamwars.ini" }
if ((Sha $srcExe) -ne "689C63DA36CD5902EDCAD2ABE1111D4697A92B12C4E17FE0659B856752641BE4") { throw "$srcExe is not the patch 1.2 exe" }
$bigs = @(Get-ChildItem -LiteralPath (Join-Path $GameFrom "preload\paks") -Filter "out*.s3darc" -File)
$bigBefore = @($bigs | ForEach-Object { "$($_.Name) $($_.Length) $($_.LastWriteTimeUtc.Ticks)" }) -join ";"
$script:n = 0
# A game folder: the 1.2 exe, steamwars.ini (arcName = patch.s3darc), the archives; -Ual: an ASI loader as bin\dinput8.dll + ini +
# plugins in bin\scripts and bin\plugins; -Proxies: d3d9 / ddraw / dsound (+ini) / version of other mods; -Winmm: a winmm.dll.
function New-Game([switch] $Ual, [int] $Machine = 0x14C, [switch] $NoIni, [switch] $Proxies, [switch] $Winmm, [string] $Dinput8Text = "") {
    $script:n++
    $g = Join-Path $base ("game $($script:n) - TimeShift")
    New-Item -ItemType Directory -Force (Join-Path $g "bin"), (Join-Path $g "preload\paks"), (Join-Path $g "UserConfigs") | Out-Null
    Copy-Item -LiteralPath $srcExe (Join-Path $g "bin\TimeShift.exe")
    Copy-Item -LiteralPath $srcIni (Join-Path $g "bin\steamwars.ini")
    foreach ($f in Get-ChildItem -LiteralPath (Join-Path $GameFrom "preload\paks") -Filter "patch*.s3darc" -File) { Copy-Item -LiteralPath $f.FullName (Join-Path $g "preload\paks\$($f.Name)") }
    foreach ($f in $bigs) { New-Item -ItemType HardLink -Path (Join-Path $g "preload\paks\$($f.Name)") -Target $f.FullName | Out-Null }
    Set-Content -LiteralPath (Join-Path $g "UserConfigs\profile.txt") -Value "a player's profile" -Encoding ASCII
    if ($Ual) {
        New-FakePe (Join-Path $g "bin\dinput8.dll") $ualExports $Machine "Ultimate ASI Loader stand-in $($script:n)"
        if (-not $NoIni) { Set-Content -LiteralPath (Join-Path $g "bin\dinput8.ini") -Value "[GlobalSets]`r`nLoadPlugins=1" -Encoding ASCII }
        New-FakePe (Join-Path $g "bin\scripts\TimeShift.WidescreenFix.asi") @("InitializeASI") 0x14C "IsUltimateASILoader"
        Set-Content -LiteralPath (Join-Path $g "bin\scripts\TimeShift.WidescreenFix.ini") -Value "[MAIN]`r`nFOVFactor = 1.2" -Encoding ASCII
        New-FakePe (Join-Path $g "bin\plugins\FpsLimit.asi") @("InitializeASI") 0x14C ""
    }
    if ($Dinput8Text) { [IO.File]::WriteAllText((Join-Path $g "bin\dinput8.dll"), $Dinput8Text) }
    if ($Proxies) {
        New-FakePe (Join-Path $g "bin\d3d9.dll") @("Direct3DCreate9", "Direct3DCreate9Ex") 0x14C "DXVK stand-in"
        New-FakePe (Join-Path $g "bin\ddraw.dll") @("DirectDrawCreate", "DirectDrawCreateEx") 0x14C "dgVoodoo stand-in"
        New-FakePe (Join-Path $g "bin\dsound.dll") @("DirectSoundCreate", "DirectSoundCreate8") 0x14C "DSOAL stand-in"
        Set-Content -LiteralPath (Join-Path $g "bin\dsound.ini") -Value "[general]`r`nfrequency=48000" -Encoding ASCII
        New-FakePe (Join-Path $g "bin\version.dll") @("GetFileVersionInfoA", "VerQueryValueA") 0x14C "version proxy stand-in"
    }
    if ($Winmm) { New-FakePe (Join-Path $g "bin\winmm.dll") @("timeGetTime", "PlaySoundA") 0x14C "a winmm.dll of someone else's" }
    $g
}
function Install($x, $g, [string[]] $more = @()) { Run-PS (Join-Path $x "coop-installer.ps1") (@("-Quiet", "-Game", "`"$g`"", "-ShortcutDir", "`"$(Join-Path $base 'links')`"", "-Lang", "en") + $more) }
function Update($x, $g) { Install $x $g @("-NoShortcut") }
function Uninstall($g) { Run-PS (Join-Path $g "coop-installer.ps1") @("-Quiet", "-Uninstall", "-Game", "`"$g`"", "-Lang", "en") }
New-Item -ItemType Directory -Force (Join-Path $base "links") | Out-Null
$tok = $null; $perr = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $x1 "coop-installer.ps1"), [ref]$tok, [ref]$perr)
$asg = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$script:TEXTS' }, $true)
$TX = & ([ScriptBlock]::Create($asg.Right.Extent.Text))
function T($k) { $v = $TX["en"][$k]; if ($v) { $v } else { "<no text $k>" } }

# 3. an Ultimate ASI Loader as bin\dinput8.dll, next to the other proxies and plugin folders
$g = New-Game -Ual -Proxies
$clean = Snap $g
Install $x1 $g
$rec = Rec $g
Check ($script:exit -eq 0 -and (Sha (Join-Path $g "bin\dinput8.dll")) -eq $ourDll) "ASI loader: install exit 0, bin\dinput8.dll is the mod's ($($script:exit))"
if ($script:exit) { Write-Host $script:out }
Check ((Sha (Join-Path $g "coop-backup\bin\dinput8.dll")) -eq $clean["bin\dinput8.dll"]) "ASI loader: the original is kept in coop-backup"
Check ((Sha (Join-Path $g "bin\winmm.dll")) -eq $clean["bin\dinput8.dll"]) "ASI loader: it also runs on as bin\winmm.dll (the same bytes), so its plugins load"
Check ((Sha (Join-Path $g "bin\winmm.ini")) -eq $clean["bin\dinput8.ini"]) "ASI loader: its settings bin\dinput8.ini go along as bin\winmm.ini"
Check (($rec -contains "chain=bin\winmm.dll") -and ($rec -contains "chain=bin\winmm.ini") -and ($rec -contains "file=bin\winmm.dll") -and ($rec -contains "file=bin\winmm.ini") -and ($rec -contains "backup=bin\dinput8.dll")) "ASI loader: the record lists the copies (chain= and file=) and the backup"
Check ($script:out.Contains((T "asi_note"))) "ASI loader: the result says it runs on as bin\winmm.dll"
$after = Snap $g
$changed = @(Snap-Diff $clean $after | Where-Object { $clean.ContainsKey($_) -and $_ -notin "bin\dinput8.dll", "bin\TimeShift.exe", "bin\steamwars.ini" })
Check ($changed.Count -eq 0) "ASI loader: every other file unchanged: plugins (bin\scripts, bin\plugins), d3d9 / ddraw / dsound (+ini) / version, archives, profiles ($($changed -join ', '))"
Install $x1 $g
Update $x2 $g
$rec = Rec $g
Check ($script:exit -eq 0 -and (Get-Content -LiteralPath (Join-Path $g "coop\version.txt") -TotalCount 1) -eq "9.9.9" -and (Sha (Join-Path $g "bin\winmm.dll")) -eq $clean["bin\dinput8.dll"] -and
       (Sha (Join-Path $g "coop-backup\bin\dinput8.dll")) -eq $clean["bin\dinput8.dll"] -and (Sha (Join-Path $g "bin\dinput8.dll")) -eq $ourDll) "ASI loader: reinstall + update keep the backup and the copy"
Check (@($rec | Where-Object { $_ -eq "chain=bin\winmm.dll" }).Count -eq 1 -and @($rec | Where-Object { $_ -eq "file=bin\winmm.dll" }).Count -eq 1 -and
       @($rec | Where-Object { $_ -eq "backup=bin\dinput8.dll" }).Count -eq 1) "ASI loader: after reinstall + update each line once"
Uninstall $g
$d = @(Snap-Diff $clean (Snap $g))
Check ($script:exit -eq 0 -and $d.Count -eq 0) "ASI loader: uninstall puts the folder back byte for byte (the loader as bin\dinput8.dll, exe, steamwars.ini; $($d -join ', '))"

# 4. the player changed the copy while co-op was installed: the uninstall leaves it
$g = New-Game -Ual
$clean = Snap $g
Install $x1 $g
[IO.File]::WriteAllText((Join-Path $g "bin\winmm.dll"), "the player's own winmm.dll now")
Uninstall $g
Check ($script:exit -eq 0 -and (Get-Content -LiteralPath (Join-Path $g "bin\winmm.dll") -Raw) -eq "the player's own winmm.dll now" -and
       (Sha (Join-Path $g "bin\dinput8.dll")) -eq $clean["bin\dinput8.dll"] -and -not (Test-Path -LiteralPath (Join-Path $g "bin\winmm.ini")) -and
       -not (Test-Path -LiteralPath (Join-Path $g "coop-backup"))) "a winmm.dll the player changed stays; the loader comes back; winmm.ini goes"

# 5. no copy: a loader next to a foreign winmm.dll, a 64-bit loader, another mod's DLL that is no loader
foreach ($c in @(@{ name = "loader + a foreign winmm.dll"; g = (New-Game -Ual -Winmm) }, @{ name = "a 64-bit loader"; g = (New-Game -Ual -Machine 0x8664) },
                 @{ name = "a dinput8.dll that is no loader"; g = (New-Game -Dinput8Text "another mod's dinput8.dll") })) {
    $g = $c.g
    $cl = Snap $g
    $w0 = if ($cl.ContainsKey("bin\winmm.dll")) { $cl["bin\winmm.dll"] } else { "(missing)" }
    Install $x1 $g
    $rec = Rec $g
    Check ($script:exit -eq 0 -and (Sha (Join-Path $g "bin\winmm.dll")) -eq $w0 -and -not ($rec | Where-Object { $_ -like "chain=*" }) -and (Sha (Join-Path $g "coop-backup\bin\dinput8.dll")) -eq $cl["bin\dinput8.dll"]) "$($c.name): kept in coop-backup, no copy, no chain= line"
    Check ($script:out.Contains((T "off_note"))) "$($c.name): the result says it is kept and off"
    Uninstall $g
    $d = @(Snap-Diff $cl (Snap $g))
    Check ($script:exit -eq 0 -and $d.Count -eq 0) "$($c.name): uninstall leaves the folder byte for byte ($($d -join ', '))"
}

# 6. another mod's dinput8.dll put OVER the co-op one after the install
#    a) uninstall then: that DLL stays; everything of co-op goes
$g = New-Game -Proxies
$clean = Snap $g
Install $x1 $g
New-FakePe (Join-Path $g "bin\dinput8.dll") $ualExports 0x14C "a loader the player put over co-op"
$f1 = Sha (Join-Path $g "bin\dinput8.dll")
Uninstall $g
$d = @(Snap-Diff $clean (Snap $g))
Check ($script:exit -eq 0 -and (Sha (Join-Path $g "bin\dinput8.dll")) -eq $f1) "over co-op's DLL, then uninstall: the other mod's dinput8.dll stays in place"
Check (($d -join ",") -eq "bin\dinput8.dll") "over co-op's DLL, then uninstall: nothing else of co-op is left ($($d -join ', '))"
Check ($script:out.Contains((T "left_note").Trim())) "over co-op's DLL, then uninstall: the result says it stays"
#    b) an update then: kept as the original, its copy as winmm.dll made; the uninstall afterwards puts it back
$g = New-Game -Proxies
$clean = Snap $g
Install $x1 $g
New-FakePe (Join-Path $g "bin\dinput8.dll") $ualExports 0x14C "a loader the player put over co-op"
$f1 = Sha (Join-Path $g "bin\dinput8.dll")
Update $x2 $g
$rec = Rec $g
Check ($script:exit -eq 0 -and (Sha (Join-Path $g "coop-backup\bin\dinput8.dll")) -eq $f1 -and ($rec -contains "backup=bin\dinput8.dll")) "over co-op's DLL, then update: it is kept in coop-backup (not overwritten unseen)"
Check ((Sha (Join-Path $g "bin\dinput8.dll")) -eq $ourDll -and (Sha (Join-Path $g "bin\winmm.dll")) -eq $f1) "over co-op's DLL, then update: co-op's DLL back, the loader runs on as winmm.dll"
Uninstall $g
$d = @(Snap-Diff $clean (Snap $g))
Check ($script:exit -eq 0 -and (Sha (Join-Path $g "bin\dinput8.dll")) -eq $f1 -and -not (Test-Path -LiteralPath (Join-Path $g "bin\winmm.dll")) -and ($d -join ",") -eq "bin\dinput8.dll") "over co-op's DLL, update, uninstall: the player's loader is back as dinput8.dll, nothing of co-op left ($($d -join ', '))"

# 7. -OldZip: that release installed over an ASI loader, then this package's installer as the launcher runs it
if ($OldZip) {
    $xo = Join-Path $base "xold"
    [IO.Compression.ZipFile]::ExtractToDirectory($OldZip, $xo)
    $g = New-Game -Ual
    $clean = Snap $g
    Install $xo $g
    Check ($script:exit -eq 0 -and (Sha (Join-Path $g "coop-backup\bin\dinput8.dll")) -eq $clean["bin\dinput8.dll"]) "old release over an ASI loader: kept in coop-backup (winmm.dll made: $(Test-Path -LiteralPath (Join-Path $g 'bin\winmm.dll')))"
    Update $x2 $g
    $rec = Rec $g
    Check ($script:exit -eq 0 -and (Sha (Join-Path $g "bin\winmm.dll")) -eq $clean["bin\dinput8.dll"] -and ($rec -contains "chain=bin\winmm.dll")) "the update from the old release makes the kept loader run on as winmm.dll"
    Uninstall $g
    $d = @(Snap-Diff $clean (Snap $g))
    Check ($script:exit -eq 0 -and $d.Count -eq 0) "old release + update + uninstall: byte for byte ($($d -join ', '))"
}

# 8. texts: the new keys in all six languages, translated; the result texts fit the result page (measured, no window).
#    Fit-Result's rooms at 96 DPI (the label at y 66): 140 px, 190 without the progress bar, 270 without the log (success).
$langs = "en", "ru", "de", "fr", "es", "it"
$newKeys = "asi_copy", "asi_note", "off_note", "winmm_taken", "left_alone", "foreign_over_coop", "left_in_place", "left_note"
foreach ($lg in $langs) {
    $missing = @($newKeys | Where-Object { -not $TX[$lg][$_] })
    Check ($missing.Count -eq 0) "texts $lg : the dinput8.dll keys are there ($($missing -join ','))"
    if ($lg -ne "en") {
        $same = @($newKeys | Where-Object { $TX[$lg][$_] -and $TX[$lg][$_] -eq $TX["en"][$_] })
        Check ($same.Count -eq 0) "texts $lg : translated ($($same -join ','))"
    }
}
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$tall = @()
foreach ($lg in $langs) {
    $t = $TX[$lg]
    if (-not $t["asi_note"]) { $tall += "$lg (no note)"; continue }
    foreach ($txt in @(($t["start_msg"] + "`n`n" + $t["the_shortcut"] + "`n`n" + $t["asi_note"]), ($t["start_msg"] + "`n`n" + $t["the_shortcut"] + "`n`n" + $t["off_note"]),
                       ($t["removed_text"] + "`n`n" + $t["removed_steam"] + $t["left_note"]))) {
        $h = [Windows.Forms.TextRenderer]::MeasureText($txt, (New-Object Drawing.Font("Segoe UI", 10.5)), (New-Object Drawing.Size(540, 100000)), [Windows.Forms.TextFormatFlags]::WordBreak).Height
        if ($h -gt 190) { $tall += "$lg $h px" }
    }
}
Check ($tall.Count -eq 0) "the result texts with the dinput8.dll notes fit in 190 px at 10.5 pt ($($tall -join ', '))"
$inst = Get-Content -LiteralPath (Join-Path $x1 "coop-installer.ps1") -Raw -Encoding UTF8
Check ($inst.Contains("function Fit-Result") -and $inst.Contains("Fit-Result `$ok")) "the result page fits its text (Fit-Result)"
$bigAfter = @(Get-ChildItem -LiteralPath (Join-Path $GameFrom "preload\paks") -Filter "out*.s3darc" -File | ForEach-Object { "$($_.Name) $($_.Length) $($_.LastWriteTimeUtc.Ticks)" }) -join ";"
Check ($bigAfter -eq $bigBefore) "the lab copy's out*.s3darc (hard-linked) unchanged"

Write-Host ""
Write-Host "test_installer: $($script:checks - $script:fails)/$($script:checks) passed"
if (-not $Keep) { Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue } else { Write-Host "kept: $base" }
exit $(if ($script:fails) { 1 } else { 0 })
