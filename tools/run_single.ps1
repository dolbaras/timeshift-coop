# One TimeShift instance for a test: windowed on the test display (rendered by that display's GPU), muted,
# never taking focus; screenshots of the window (PrintWindow) at given seconds, then stop.
# The run folder (..\..\work\runs\<stamp>) gets the mod's log and the shots.
# (2026-11-15) The test display is the 1280x768 HDMI emulator (on the lab's main PC at 3840,0, driven by the Intel iGPU);
# the owner's own monitors never get a test window. The window is a FRAMELESS 1280x720 client at the display's top left
# (TS_WINSTYLE=popup), so the whole client lies on the display and no border pixel lands on the monitor to its left;
# 10 s after the start its rectangle is checked and the game is stopped if any part lies outside the test display.
param(
    [string] $Game = "",              # default: ..\..\work\game (the test copy)
    [int] $Seconds = 40,
    [int[]] $Shots = @(15, 30),       # seconds after start
    [string] $Adapter = "window",     # "window": the secondary monitor's GPU; "": the game's choice
    [switch] $NoMod,                  # no proxy DLL (the game as installed)
    [string] $Args = "",              # extra command line for TimeShift.exe
    [string] $Ini = "",               # more profile keys or overrides, "KEY=value;KEY2=value2"
    [string] $ProxyDll = "",          # another build of the proxy DLL (default: native\build; not $Dll: names ignore case)
    [string] $Size = "1280,720"       # the client (and the game's video size)
)
$ErrorActionPreference = "Stop"
$mod = Split-Path $PSScriptRoot
$work = Join-Path (Split-Path $mod) "work"
if (-not $Game) { $Game = Join-Path $work "game" }
$bin = Join-Path $Game "bin"
$run = Join-Path $work ("runs\" + (Get-Date -Format "yyyyMMdd-HHmmss"))
New-Item -ItemType Directory -Force $run | Out-Null

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
# (the old rule "the first secondary monitor" picked the owner's own second monitor once the emulator was added)
$screen = [System.Windows.Forms.Screen]::AllScreens | Where-Object {
    -not $_.Primary -and $_.Bounds.Width -eq 1280 -and $_.Bounds.Height -eq 768 } | Select-Object -First 1
if (-not $screen) { throw "no 1280x768 test display (HDMI emulator): test windows never go on the owner's monitors" }
$wa = $screen.WorkingArea
if ("$Size" -notmatch '^(\d+),(\d+)$') { throw "-Size w,h" }
$w = [int]$Matches[1]; $h = [int]$Matches[2]
if ($w -gt $wa.Width -or $h -gt $wa.Height) { throw "client ${w}x${h} does not fit the test display's work area $wa" }

# The profile the game reads (steamwars.ini: Profile Default in ..\UserConfigs): muted. VIDEO_FullScr stays "yes":
# the game's own windowed mode loses its Direct3D device at the first level load (2026-10-25, Intel UHD 630);
# the proxy's test mode makes the device windowed instead (TS_BACKGROUND).
$profileIni = Join-Path $Game "UserConfigs\Default\user.ini"   # (not $ini: names ignore case, -Ini is the parameter)
$text = [IO.File]::ReadAllText($profileIni)
$text = $text -replace "(?<!\r)\n", "`r`n"   # (the game skips lines ending in a bare LF)
$set = [ordered]@{ "VIDEO_SizeX" = "$w"; "VIDEO_SizeY" = "$h"; "VIDEO_FullScr" = "yes"; "Volume" = "0.000000";
    "Sfx_Volume" = "0.000000"; "Music_Volume" = "0.000000"; "Voice_Volume" = "0.000000" }
foreach ($kv in $Ini.Split(';')) { if ($kv -match '^([^=]+)=(.*)$') { $set[$Matches[1].Trim()] = $Matches[2].Trim() } }
foreach ($k in $set.Keys) {
    if ($text -match "(?m)^$k\s*=") { $text = $text -replace "(?m)^$k[ \t]*=[^\r\n]*", "$k = $($set[$k])" }
    else { $text = $text.TrimEnd() + "`r`n$k = $($set[$k])`r`n" }
}
[IO.File]::WriteAllText($profileIni, $text)

$dll = Join-Path $bin "dinput8.dll"
if ($NoMod) { Remove-Item $dll -ErrorAction SilentlyContinue }
else { Copy-Item $(if ($ProxyDll) { $ProxyDll } else { Join-Path $mod "native\build\dinput8.dll" }) $dll -Force }

$env:TS_LOG = Join-Path $run "ts_coop.log"
$env:TS_WINPOS = "$($wa.X),$($wa.Y)"
$env:TS_WINSIZE = "$w,$h"
$env:TS_WINSTYLE = "popup"
$env:TS_BACKGROUND = "1"
$env:TS_ADAPTER = $Adapter
if (-not $NoMod -and $Shots) {   # the proxy saves the rendered frames (PrintWindow often gets a blank or stale D3D window)
    $env:TS_SHOTS = ($Shots | Sort-Object) -join ","
    $env:TS_SHOTDIR = $run
}
"window ${w}x${h} at $env:TS_WINPOS (frameless) on $($screen.DeviceName) $($screen.Bounds), mod: $(-not $NoMod)" | Tee-Object (Join-Path $run "run.txt")

$start = @{ FilePath = (Join-Path $bin "TimeShift.Exe"); WorkingDirectory = $bin; PassThru = $true }
if ("$Args".Trim()) { $start.ArgumentList = "$Args" }   # (an empty -ArgumentList is refused when started with -Command)
$p = Start-Process @start
try { $p.PriorityClass = "BelowNormal" } catch {}

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class TsWin {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
}
"@
$t0 = Get-Date
foreach ($s in ($Shots | Sort-Object | Where-Object { $NoMod })) {
    while (((Get-Date) - $t0).TotalSeconds -lt $s -and -not $p.HasExited) { Start-Sleep -Milliseconds 250 }
    if ($p.HasExited) { break }
    $p.Refresh()
    $hw = $p.MainWindowHandle
    if ($hw -eq [IntPtr]::Zero) { "shot ${s}s: no window" | Tee-Object -Append (Join-Path $run "run.txt"); continue }
    $r = New-Object TsWin+RECT; [void][TsWin]::GetClientRect($hw, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R)), ([Math]::Max(1, $r.B))
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [void][TsWin]::PrintWindow($hw, $dc, 3)   # client area, full content
    $g.ReleaseHdc($dc); $g.Dispose()
    $bmp.Save((Join-Path $run ("shot{0:D3}.png" -f $s))); $bmp.Dispose()
}
$rectChecked = $false
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds -and -not $p.HasExited) {
    if (-not $rectChecked -and ((Get-Date) - $t0).TotalSeconds -ge 10) {   # (the whole window must lie on the test display)
        $rectChecked = $true
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
            $wr = New-Object TsWin+RECT; [void][TsWin]::GetWindowRect($p.MainWindowHandle, [ref]$wr)
            $b = $screen.Bounds
            $off = $wr.L -lt $b.Left -or $wr.T -lt $b.Top -or $wr.R -gt $b.Right -or $wr.B -gt $b.Bottom
            "window rect $($wr.L),$($wr.T)-$($wr.R),$($wr.B) on $($b.Left),$($b.Top)-$($b.Right),$($b.Bottom): $(if ($off) { 'OFF THE TEST DISPLAY: stopped' } else { 'inside' })" |
                Tee-Object -Append (Join-Path $run "run.txt")
            if ($off) { Stop-Process -Id $p.Id -Force; break }
        } else { "window rect: no window after 10 s" | Tee-Object -Append (Join-Path $run "run.txt") }
    }
    Start-Sleep -Milliseconds 1000
}
$exited = $p.HasExited
if (-not $exited) { Stop-Process -Id $p.Id -Force }
"game ran $([int]((Get-Date) - $t0).TotalSeconds) s, ended by itself: $exited$(if ($exited) { " (code $($p.ExitCode))" })" |
    Tee-Object -Append (Join-Path $run "run.txt")
foreach ($f in Get-ChildItem $run -Filter "shot*.bmp") {   # (the proxy's frames: png like the other shots)
    $img = [System.Drawing.Image]::FromFile($f.FullName)
    $img.Save([IO.Path]::ChangeExtension($f.FullName, ".png"), [System.Drawing.Imaging.ImageFormat]::Png); $img.Dispose()
    Remove-Item -LiteralPath $f.FullName
}
$dmp = Join-Path $bin "timeshift_minidump.dmp"
if (Test-Path $dmp) { Move-Item $dmp $run -Force; "CRASH: minidump moved to the run folder" | Tee-Object -Append (Join-Path $run "run.txt") }
"run=$run"
