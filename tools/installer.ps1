# TimeShift Co-op installer / uninstaller for players (shipped in the release zip as
# "Install Co-op.bat" + coop-installer.ps1, payload in the "files" folder next to it).
#   install: finds the game folder (this folder, install records in the registry, Steam
#            libraries, or the player picks it), checks the exe is patch 1.2, builds the mod's data
#            archives from the player's own game files (coop\python + coop\tools\build_pak.py),
#            keeps the files it replaces in <game>\coop-backup, copies the mod, points
#            bin\steamwars.ini at coop.s3darc and writes <game>\coop\install.txt.
#   update:  the same over an older version; originals already backed up are kept; the data
#            archives are kept when the game's archive, the mod version and the recipes are unchanged.
#   remove:  puts arcName back, deletes what install.txt lists and puts the backed-up files back.
#            Profiles and saves (UserConfigs) are not touched.
# A window shows the steps and a progress bar (also on the taskbar button).
# -Quiet: no window, the same steps written to the console (test scripts).
# -Game without -Uninstall starts installing into that folder right away; -Game -Uninstall asks
# first, unless -AutoClose is given too.
# The window's install also makes the "TimeShift Co-op" shortcut (desktop and Start menu, the game's
# icon with a co-op badge): it runs coop\coop-launcher.ps1, which installs a newer release from
# GitHub before starting the game. The launcher runs a new release's installer as
#   coop-installer.ps1 -Game <folder> -AutoClose -NoShortcut
# so every later installer has to take these: -AutoClose closes the window after a successful
# install, -NoShortcut keeps the shortcuts as they are (only those already made are refreshed).
# -ShortcutDir puts new shortcuts in that folder instead (tests; several folders split by ';').
# -GameProcess: the process name that counts as "the game runs" (tests only; default TimeShift).
# Usage: coop-installer.ps1 [-Game <folder>] [-Uninstall] [-Quiet] [-Lang en|ru]
#                           [-AutoClose] [-NoShortcut] [-ShortcutDir <folder>[;<folder>]]
param([string] $Game = "", [switch] $Uninstall, [switch] $Quiet, [string] $Lang = "",
      [switch] $AutoClose, [switch] $NoShortcut, [string] $ShortcutDir = "", [string] $GameProcess = "TimeShift")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$payload = Join-Path $here "files"
$script:TITLE = "TimeShift Co-op"
# The exe the mod is made for: TimeShift patch 1.2 (retail, as patched by the 1.2 update).
$script:EXE_SHA = "689C63DA36CD5902EDCAD2ABE1111D4697A92B12C4E17FE0659B856752641BE4"
# A log line of the mod's proxy DLL (native/proxy.c): a bin\dinput8.dll with it is ours.
$script:DLL_MARK = "TimeShift co-op proxy"
$script:PAKS = @("coop.s3darc", "coop_ati.s3darc", "coop_nv.s3darc")
$script:LOGNAME = "timeshift-coop-install.log"
# The window's texts by key ({0}, {1}... filled in from L's arguments). Not $S: variable names
# ignore case and a function sees its callers' ($s of an event handler).
$script:TEXTS = @{
    en = @{
        "not_installed" = "The co-op mod is not installed in`n{0}"
        "removed_files" = "Removed {0} file(s)."
        "put_back" = "Put back: "
        "no_files" = "The 'files' folder is missing next to the installer."
        "close_game" = "Close the game first."
        "incomplete" = "The downloaded mod is incomplete: nothing was changed. Start the game again to retry."
        "another_install" = "Another installation of the mod is running: wait until it ends."
        "no_admin" = "Windows did not allow writing to the game folder without administrator rights."
        "installed_now" = "Installed now: co-op "
        "original_kept" = "Original kept: "
        "copied" = "Copied {0} file(s)."
        "exe_wrong" = "This mod is made for TimeShift with patch 1.2 (bin\TimeShift.exe with SHA-256 689c63da...52641be4). The exe in this folder is another version (SHA-256 {0}).`nInstall the official patch 1.2 for your copy of the game, then run this installer again. Nothing was changed."
        "paks_missing" = "preload\paks\{0} is missing: this is not a complete TimeShift 1.2 folder. Nothing was changed."
        "paks_kept" = "The mod's data built by the last install is current: kept."
        "paks_built" = "The mod's data was built from your game files."
        "build_failed" = "The mod's data could not be built from your game files (the log says why). Nothing was changed."
        "ini_other" = "bin\steamwars.ini named another archive ({0}): it is put back on uninstall."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "start_msg" = "Start the game: the main menu has a 'Co-op' entry.`nTo remove: 'Uninstall Co-op.bat' in the game folder (or run this installer again)."
        "the_shortcut" = "The 'TimeShift Co-op' shortcut (desktop, Start menu) starts the game and installs a newer version of the mod from GitHub first, if there is one."
        "not_a_folder" = "Not a TimeShift folder (no bin\TimeShift.exe): {0}"
        "not_found" = "The game was not found: give its folder with -Game."
        "removed_done" = "The co-op mod was removed; the replaced files and the game's settings were put back."
        "is_installed" = "TimeShift Co-op {0} is installed in {1}"
        "error" = "Error: "
        "subtitle" = "Version {0}  -  co-op campaign for TimeShift, 2-4 players"
        "game_folder" = "Game folder"
        "browse" = "Browse..."
        "remove_co_op" = "Remove co-op"
        "cancel" = "Cancel"
        "install" = "Install"
        "copy_log" = "Copy log"
        "open_game_folder" = "Open game folder"
        "close" = "Close"
        "please_wait" = "Please wait until it finishes: stopping in the middle could leave the game half changed."
        "choose_folder" = "The game was not found automatically: choose its folder (the one with the bin folder)."
        "no_exe" = "There is no bin\TimeShift.exe in this folder."
        "found" = "TimeShift 1.2 found."
        "found_other" = "TimeShift found, but its exe is not patch 1.2: the mod cannot be installed (README-COOP.txt says more)."
        "already" = "Co-op {0} is already installed: it will be installed again."
        "reinstall" = "Reinstall"
        "will_update" = "Co-op {0} is installed: it will be updated to {1}."
        "update" = "Update"
        "what" = "The installer builds the mod's data from your own game files (a few seconds), copies the mod into the game folder and keeps every game file it replaces (coop-backup); 'Uninstall Co-op.bat' there puts everything back. Profiles and saves are not touched."
        "same_version" = "Every player needs the same version of the mod."
        "installing" = "Installing co-op {0}..."
        "st_check" = "Checking the game folder"
        "st_paks" = "Building the mod's data from your game files"
        "st_copy" = "Copying the mod (the game's replaced files are kept)"
        "st_finish" = "Shortcuts and the install record"
        "folder_log" = "Game folder: "
        "updated" = "Updated from {0} to {1}"
        "installed_title" = "Co-op {0} is installed"
        "install_failed" = "Installation failed"
        "log_in" = "The log is in %TEMP%\timeshift-coop-install.log. Nothing of the game is lost: replaced files are in coop-backup."
        "removing" = "Removing co-op..."
        "st_rm" = "Removing the mod's files and data"
        "st_restore" = "Putting back the game's original files"
        "st_clean" = "Removing the backups and the mod's folder"
        "removed_title" = "Co-op removed"
        "removed_text" = "The co-op mod was removed; the replaced files and bin\steamwars.ini were put back.`n`nProfiles and saves (UserConfigs) were not touched."
        "remove_failed" = "Removal failed"
        "select_folder" = "Select the TimeShift folder (the one with the bin folder)"
        "remove_q" = "Remove the co-op mod from`n{0} ?"
        "remove_page" = "Remove TimeShift Co-op {0} from this folder?`n`nThe game's original files and settings are put back; profiles and saves (UserConfigs) stay."
        "removed" = "Removed: "
        "shortcut" = "Shortcut: "
        "shortcut_desc" = "TimeShift with co-op: installs a newer version of the mod first, if there is one"
    }
    ru = @{
        "not_installed" = "Кооп-мод не установлен в`n{0}"
        "removed_files" = "Удалено файлов: {0}."
        "put_back" = "Возвращён: "
        "no_files" = "Рядом с установщиком нет папки 'files'."
        "close_game" = "Сначала закройте игру."
        "incomplete" = "Загруженный мод неполный: ничего не изменено. Запустите игру снова, чтобы повторить."
        "another_install" = "Уже идёт другая установка мода: дождитесь её окончания."
        "no_admin" = "Windows не разрешила запись в папку игры без прав администратора."
        "installed_now" = "Сейчас установлен кооп "
        "original_kept" = "Оригинал сохранён: "
        "copied" = "Скопировано файлов: {0}."
        "exe_wrong" = "Мод сделан для TimeShift с патчем 1.2 (bin\TimeShift.exe с SHA-256 689c63da...52641be4). В этой папке другая версия exe (SHA-256 {0}).`nУстановите официальный патч 1.2 для своей копии игры и запустите установщик снова. Ничего не изменено."
        "paks_missing" = "Нет preload\paks\{0}: это не полная папка TimeShift 1.2. Ничего не изменено."
        "paks_kept" = "Данные мода от прошлой установки актуальны: оставлены."
        "paks_built" = "Данные мода собраны из ваших файлов игры."
        "build_failed" = "Не удалось собрать данные мода из ваших файлов игры (причина в журнале). Ничего не изменено."
        "ini_other" = "В bin\steamwars.ini был указан другой архив ({0}): он вернётся при удалении."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "start_msg" = "Запустите игру: в главном меню появится пункт «Кооператив».`nУдалить: «Uninstall Co-op.bat» в папке игры (или снова этот установщик)."
        "the_shortcut" = "Ярлык «TimeShift Co-op» (рабочий стол, меню «Пуск») запускает игру, а перед этим ставит с GitHub новую версию мода, если она вышла."
        "not_a_folder" = "Это не папка TimeShift (нет bin\TimeShift.exe): {0}"
        "not_found" = "Игра не найдена: укажите папку через -Game."
        "removed_done" = "Кооп-мод удалён, заменённые файлы и настройки игры возвращены."
        "is_installed" = "TimeShift Co-op {0} установлен в {1}"
        "error" = "Ошибка: "
        "subtitle" = "Версия {0}  -  кооперативная кампания TimeShift, 2-4 игрока"
        "game_folder" = "Папка игры"
        "browse" = "Обзор..."
        "remove_co_op" = "Удалить кооп"
        "cancel" = "Отмена"
        "install" = "Установить"
        "copy_log" = "Копировать журнал"
        "open_game_folder" = "Открыть папку игры"
        "close" = "Закрыть"
        "please_wait" = "Подождите окончания: остановка на полпути может оставить игру изменённой наполовину."
        "choose_folder" = "Игра не найдена автоматически: выберите её папку (ту, где есть папка bin)."
        "no_exe" = "В этой папке нет bin\TimeShift.exe."
        "found" = "TimeShift 1.2 найдена."
        "found_other" = "TimeShift найдена, но её exe не от патча 1.2: мод нельзя установить (подробнее в README-COOP-ru.txt)."
        "already" = "Кооп {0} уже установлен: будет установлен заново."
        "reinstall" = "Переустановить"
        "will_update" = "Установлен кооп {0}: он будет обновлён до {1}."
        "update" = "Обновить"
        "what" = "Установщик соберёт данные мода из ваших файлов игры (несколько секунд), скопирует мод в папку игры и сохранит каждый заменяемый файл игры (coop-backup); «Uninstall Co-op.bat» там всё вернёт. Профили и сохранения не затрагиваются."
        "same_version" = "Версия мода должна быть одинаковой у всех игроков."
        "installing" = "Установка коопа {0}..."
        "st_check" = "Проверка папки игры"
        "st_paks" = "Сборка данных мода из ваших файлов игры"
        "st_copy" = "Копирование мода (заменяемые файлы игры сохраняются)"
        "st_finish" = "Ярлыки и сведения об установке"
        "folder_log" = "Папка игры: "
        "updated" = "Обновлено с {0} до {1}"
        "installed_title" = "Кооп {0} установлен"
        "install_failed" = "Установка не удалась"
        "log_in" = "Журнал: %TEMP%\timeshift-coop-install.log. Файлы игры не потеряны: заменённые лежат в coop-backup."
        "removing" = "Удаление коопа..."
        "st_rm" = "Удаление файлов и данных мода"
        "st_restore" = "Возврат оригинальных файлов игры"
        "st_clean" = "Удаление копий и папки мода"
        "removed_title" = "Кооп удалён"
        "removed_text" = "Кооп-мод удалён, заменённые файлы и bin\steamwars.ini возвращены.`n`nПрофили и сохранения (UserConfigs) не тронуты."
        "remove_failed" = "Удаление не удалось"
        "select_folder" = "Выберите папку TimeShift (в ней есть папка bin)"
        "remove_q" = "Удалить кооп-мод из`n{0} ?"
        "remove_page" = "Удалить TimeShift Co-op {0} из этой папки?`n`nОригинальные файлы и настройки игры будут возвращены; профили и сохранения (UserConfigs) останутся."
        "removed" = "Удалён: "
        "shortcut" = "Ярлык: "
        "shortcut_desc" = "TimeShift с кооперативом: сначала ставит новую версию мода, если она вышла"
    }
}
# Windows' own language when there is a translation (Russian also for Ukrainian, Belarusian and
# Kazakh Windows), else English. (Not $lang: that is the -Lang parameter.)
function Pick-Lang($code) {
    $code = "$code".ToLower()
    if ($code -in "uk", "be", "kk") { return "ru" }
    if ($script:TEXTS.ContainsKey($code)) { return $code }
    $null
}
$script:uiLang = if ($Lang) { Pick-Lang $Lang } else { Pick-Lang (Get-UICulture).TwoLetterISOLanguageName }
if (-not $script:uiLang) { $script:uiLang = "en" }
function L($key) {
    $text = $script:TEXTS[$script:uiLang][$key]
    if (-not $text) { $text = $script:TEXTS.en[$key] }
    if (-not $text) { return $key }
    if ($args.Count) { return ($text -f $args) }
    $text
}
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
# Started hidden ("Install Co-op.bat"): an error outside the install's own steps (before the
# window shows, in building it) would leave the player with nothing on screen. Said and logged.
trap {
    $why = "TimeShift Co-op installer: " + $_.Exception.Message
    try { Add-Content -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) $script:LOGNAME) -Value $why } catch { }
    if ($Quiet) { Write-Host $why } else {
        try { [System.Windows.Forms.MessageBox]::Show($why, $script:TITLE, "OK", "Error") | Out-Null } catch { }
    }
    exit 1
}
# This version: the payload's, or (the uninstaller in the game folder) the installed one's.
$version = ""
foreach ($v in (Join-Path $payload "coop\version.txt"), (Join-Path $here "coop\version.txt")) {
    if (-not $version -and (Test-Path -LiteralPath $v)) { $version = "$(Get-Content -LiteralPath $v -TotalCount 1)".Trim() }
}

# The taskbar button's progress, the console window, sharp text on scaled screens, icons.
try {
    Add-Type -ErrorAction Stop @"
using System;
using System.Runtime.InteropServices;
public static class TsCoopWin {
    [ComImport, Guid("ea1afb91-9e28-4b86-90e9-9e9f8a5eefaf"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface ITaskbarList3 {
        void HrInit(); void AddTab(IntPtr h); void DeleteTab(IntPtr h); void ActivateTab(IntPtr h); void SetActiveAlt(IntPtr h);
        void MarkFullscreenWindow(IntPtr h, [MarshalAs(UnmanagedType.Bool)] bool f);
        void SetProgressValue(IntPtr h, ulong done, ulong total);
        void SetProgressState(IntPtr h, int state);
    }
    [ComImport, Guid("56fdf344-fd6d-11d0-958a-006097c9a090"), ClassInterface(ClassInterfaceType.None)]
    class TaskbarList {}
    static ITaskbarList3 bar;
    // state: 0 none, 1 busy, 2 normal, 4 error, 8 paused
    public static void Progress(IntPtr h, int state, int pct) {
        try {
            if (bar == null) { bar = (ITaskbarList3)new TaskbarList(); bar.HrInit(); }
            bar.SetProgressState(h, state);
            if (state == 2 || state == 4 || state == 8) bar.SetProgressValue(h, (ulong)pct, 100);
        } catch { }
    }
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)] public static extern int SetWindowTheme(IntPtr h, string app, string id);
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int v, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern uint PrivateExtractIcons(string f, int i, int cx, int cy, IntPtr[] h, uint[] id, uint n, uint flags);
    [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr h);
    // A dark title bar (Windows 10 2004+ / 11; older ones keep theirs).
    public static void DarkTitle(IntPtr h) { int on = 1; try { DwmSetWindowAttribute(h, 20, ref on, 4); } catch { } }
}
"@
    $script:haveWin = $true
} catch { $script:haveWin = $false }

# ---------------------------------------------------------------------------------------------
# What the steps report: the window's controls, or the console (-Quiet).
$script:ui = $null
$script:logLines = New-Object System.Collections.Generic.List[string]

function Ui-Pump { if ($script:ui) { [System.Windows.Forms.Application]::DoEvents() } }

function Ui-Log($text) {
    $script:logLines.Add("$(Get-Date -Format 'HH:mm:ss') $text")
    if ($script:ui) { $script:ui.log.AppendText("$text`r`n"); Ui-Pump } else { Write-Host $text }
}

function Ui-Progress($pct, $text) {
    $pct = [Math]::Max(0, [Math]::Min(100, [int]$pct))
    if (-not $script:ui) { return }
    $u = $script:ui
    $u.fill.Width = [int]($u.bar.ClientSize.Width * $pct / 100)
    $u.pct.Text = "$pct %"
    if ($null -ne $text) { $u.action.Text = $text }
    if ($script:haveWin) { [TsCoopWin]::Progress($u.form.Handle, 2, $pct) }
    Ui-Pump
}

# state: run, done, warn, skip, fail
function Ui-Step($key, $state) {
    if (-not $script:ui) { Write-Host "[$state] $key"; return }
    $row = $script:ui.steps[$key]
    if (-not $row) { return }
    $c = $script:ui.colors
    switch ($state) {
        "run"  { $row.icon.Text = [char]0x25CF; $row.icon.ForeColor = $c.accent; $row.text.ForeColor = $c.text; $script:ui.running = $row }
        "done" { $row.icon.Text = [char]0x2714; $row.icon.ForeColor = $c.ok; $row.text.ForeColor = $c.text }
        "warn" { $row.icon.Text = "!"; $row.icon.ForeColor = $c.warn; $row.text.ForeColor = $c.text }
        "skip" { $row.icon.Text = [char]0x2013; $row.icon.ForeColor = $c.dim; $row.text.ForeColor = $c.dim }
        "fail" { $row.icon.Text = [char]0x2716; $row.icon.ForeColor = $c.bad; $row.text.ForeColor = $c.text }
    }
    if ($state -ne "run" -and $script:ui.running -eq $row) { $script:ui.running = $null }
    Ui-Pump
}

# ---------------------------------------------------------------------------------------------
# The game folder

# (Join-Path throws on a drive that does not exist: the D:\Games candidate on a PC without D:)
function Test-Game($dir) {
    if (-not $dir) { return $false }
    try { Test-Path -LiteralPath ([IO.Path]::Combine($dir, "bin\TimeShift.exe")) -PathType Leaf } catch { $false }
}

function Sha256-File($p) {
    $fs = [IO.File]::OpenRead($p)
    try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() }
}

# The exe's SHA-256 when it is not the one the mod is made for, else $null.
function Exe-Mismatch($g) {
    $sha = Sha256-File (Join-Path $g "bin\TimeShift.exe")
    if ($sha -eq $script:EXE_SHA) { return $null }
    $sha.ToLower()
}

# Candidate game folders, most likely first.
function Find-Games {
    $c = New-Object System.Collections.Generic.List[string]
    foreach ($d in @($here, (Split-Path $here -Parent))) { if ($d) { $c.Add($d) } }
    $keys = @("HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
              "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*",
              "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*")
    foreach ($k in $keys) {
        Get-ItemProperty $k -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like "*TimeShift*" } | ForEach-Object {
            foreach ($p in $_.InstallLocation, $_."Inno Setup: App Path") { if ($p) { $c.Add("$p".Trim('"')) } }
        }
    }
    $steam = (Get-ItemProperty "HKCU:\SOFTWARE\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libs = @($steam)
        $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) { $libs += $m.Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($l in $libs) { $c.Add((Join-Path $l "steamapps\common\TimeShift")) }
    }
    foreach ($p in @("${env:ProgramFiles(x86)}\Sierra\TimeShift", "$env:ProgramFiles\Sierra\TimeShift",
                     "${env:ProgramFiles(x86)}\TimeShift", "$env:ProgramFiles\TimeShift", "C:\Games\TimeShift", "D:\Games\TimeShift")) { $c.Add($p) }
    # (one form of each folder: "...\TimeShift\" is the same as "...\TimeShift")
    $c | Where-Object { Test-Game $_ } | ForEach-Object {
        $p = (Resolve-Path -LiteralPath $_).Path
        if ($p.Length -gt 3) { $p.TrimEnd('\') } else { $p }
    } | Select-Object -Unique
}

function Has-Text($p, [string[]] $words) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $false }
    try { $s = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($p)) } catch { return $false }
    foreach ($w in $words) { if ($s.Contains($w)) { return $true } }
    $false
}
function Is-CoopDll($p) { Has-Text $p @($script:DLL_MARK) }

# The install record: version=, then file=, backup=, built=, shortcut= lines and arcname= (the
# value of bin\steamwars.ini's [Preload] arcName before the install).
function Read-Manifest($g) {
    $m = @{ files = @(); backups = @(); built = @(); shortcuts = @(); version = ""; arcname = "" }
    $f = Join-Path $g "coop\install.txt"
    if (Test-Path -LiteralPath $f) {
        foreach ($line in Get-Content -LiteralPath $f) {
            if ($line -match '^version=(.*)$') { $m.version = $Matches[1] }
            elseif ($line -match '^file=(.+)$') { $m.files += $Matches[1] }
            elseif ($line -match '^backup=(.+)$') { $m.backups += $Matches[1] }
            elseif ($line -match '^built=(.+)$') { $m.built += $Matches[1] }
            elseif ($line -match '^shortcut=(.+)$') { $m.shortcuts += $Matches[1] }
            elseif ($line -match '^arcname=(.+)$') { $m.arcname = $Matches[1] }
        }
    }
    if ($m.files.Count -eq 0) { return (Find-Coop $g) }
    # (an "original" that is a co-op DLL is not the game's)
    $m.backups = @($m.backups | Where-Object { -not (Is-CoopDll (Join-Path $g "coop-backup\$_")) })
    $m
}

# A co-op install whose record is lost: what is the mod's, found by the files themselves.
function Find-Coop($g) {
    $m = @{ files = @(); backups = @(); built = @(); shortcuts = @(); version = ""; arcname = ""; found = $false }
    $root = [IO.Path]::GetFullPath($g).TrimEnd('\')
    $coop = Join-Path $root "coop"
    $dll = Is-CoopDll (Join-Path $root "bin\dinput8.dll")
    if (-not $dll -and -not (Test-Path -LiteralPath (Join-Path $coop "version.txt"))) { return $m }
    $m.found = $true
    if ($dll) { $m.files += "bin\dinput8.dll" }
    if (Test-Path -LiteralPath $coop) {
        foreach ($f in Get-ChildItem -LiteralPath $coop -Recurse -File -Force) { $m.files += $f.FullName.Substring($root.Length + 1) }
    }
    foreach ($f in "coop-installer.ps1", "Uninstall Co-op.bat") { if (Test-Path -LiteralPath (Join-Path $root $f)) { $m.files += $f } }
    foreach ($n in $script:PAKS) { if (Test-Path -LiteralPath (Join-Path $root "preload\paks\$n")) { $m.built += "preload\paks\$n" } }
    $bk = Join-Path $root "coop-backup"
    if (Test-Path -LiteralPath $bk) {
        foreach ($f in Get-ChildItem -LiteralPath $bk -Recurse -File -Force) {
            if (-not (Is-CoopDll $f.FullName)) { $m.backups += $f.FullName.Substring($bk.Length + 1) }
        }
    }
    $v = Get-Content -LiteralPath (Join-Path $coop "version.txt") -TotalCount 1 -ErrorAction SilentlyContinue
    $m.version = if ($v) { "$v".Trim() } else { "?" }
    $m
}

# The game running from this folder (a copy elsewhere does not lock these files).
function Game-Running($g) {
    $g = [IO.Path]::GetFullPath($g).TrimEnd('\') + "\"
    Get-Process -Name $GameProcess -ErrorAction SilentlyContinue |
        Where-Object { -not $_.Path -or $_.Path.StartsWith($g, [StringComparison]::OrdinalIgnoreCase) }
}

# The window's icon: the co-op one once made, else the game's.
function Window-Icon($g) {
    try {
        $ico = Join-Path $g "coop\coop.ico"
        if (Test-Path -LiteralPath $ico) { return New-Object Drawing.Icon($ico, 32, 32) }
        [Drawing.Icon]::ExtractAssociatedIcon((Join-Path $g "bin\TimeShift.exe"))
    } catch { $null }
}

# ---------------------------------------------------------------------------------------------
# bin\steamwars.ini: CRLF lines, and the game silently skips a line that ends in a bare LF. Only
# the value of [Preload] arcName is changed, as bytes (Latin-1 keeps every byte as it is).

function Ini-Path($g) { Join-Path $g "bin\steamwars.ini" }
$script:ARC_RX = New-Object regex '(?m)^([ \t]*arcName[ \t]*=[ \t]*)([^\r\n]*)'
function Get-ArcName($g) {
    $t = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes((Ini-Path $g)))
    $m = $script:ARC_RX.Match($t)
    if (-not $m.Success) { throw "bin\steamwars.ini has no arcName line" }
    $m.Groups[2].Value.Trim()
}
function Set-ArcName($g, $value) {
    $p = Ini-Path $g
    $enc = [Text.Encoding]::GetEncoding(28591)
    $t = $enc.GetString([IO.File]::ReadAllBytes($p))
    $m = $script:ARC_RX.Match($t)
    if (-not $m.Success) { throw "bin\steamwars.ini has no arcName line" }
    $g2 = $m.Groups[2]
    # (trailing blanks of the old value stay where they were)
    $old = $g2.Value.TrimEnd(" ", "`t")
    $t2 = $t.Substring(0, $g2.Index) + $value + $t.Substring($g2.Index + $old.Length)
    if ($t2 -ne $t) { [IO.File]::WriteAllBytes($p, $enc.GetBytes($t2)) }
}

# ---------------------------------------------------------------------------------------------
# Remove

function Do-Uninstall($g) {
    $m = Read-Manifest $g
    if ($m.files.Count -eq 0) { throw (L "not_installed" $g) }
    if (Game-Running $g) { throw (L "close_game") }
    Lock-Install
    Ui-Step "rm" "run"
    # The game's own archive first: whatever happens next, the game starts as it was.
    if ((Test-Path -LiteralPath (Ini-Path $g)) -and (Get-ArcName $g) -ieq "coop.s3darc") {
        $back = if ($m.arcname) { $m.arcname } else { "patch.s3darc" }
        Set-ArcName $g $back
        Ui-Log (L "ini_back" $back)
    }
    # (+ the log the mod's DLL writes next to the exe)
    $all = @($m.files + $m.built + "bin\ts_coop.log")
    $i = 0
    foreach ($rel in $all) {
        $i++
        Ui-Progress (5 + 50 * $i / [Math]::Max(1, $all.Count)) $rel
        Remove-Item -LiteralPath (Join-Path $g $rel) -Force -ErrorAction SilentlyContinue
    }
    Ui-Log (L "removed_files" $($all.Count))
    # (only "TimeShift Co-op.lnk" files the install made: the record names them)
    foreach ($lnk in $m.shortcuts) {
        if ($lnk -like "*.lnk" -and (Test-Path -LiteralPath $lnk)) { Remove-Item -LiteralPath $lnk -Force -ErrorAction SilentlyContinue; Ui-Log ((L "removed") + $lnk) }
    }
    Ui-Step "rm" "done"
    Ui-Step "restore" "run"
    $i = 0
    foreach ($rel in $m.backups) {
        $i++
        Ui-Progress (55 + 35 * $i / [Math]::Max(1, $m.backups.Count)) $rel
        $b = Join-Path $g "coop-backup\$rel"
        if (Test-Path -LiteralPath $b) { Copy-Item -LiteralPath $b (Join-Path $g $rel) -Force; Ui-Log ((L "put_back") + $rel) }
    }
    Ui-Step "restore" "done"
    Ui-Step "clean" "run"
    Ui-Progress 95 "coop, coop-backup"
    Remove-Item -LiteralPath (Join-Path $g "coop") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $g "coop-backup") -Recurse -Force -ErrorAction SilentlyContinue
    Ui-Step "clean" "done"
    Ui-Progress 100 ""
    @{ removed = $m.version }
}

# ---------------------------------------------------------------------------------------------
# Install

# A command line argument as the C runtime parses it back.
function Quote-Arg($a) {
    if ($a -and $a -notmatch '[\s"]') { return $a }
    '"' + (($a -replace '(\\*)"', '$1$1\"') -replace '(\\+)$', '$1$1') + '"'
}

# Runs a program, its output lines to $onLine as they come, the window kept alive meanwhile.
function Run-Live($exe, [string[]] $argList, [scriptblock] $onLine) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($argList | ForEach-Object { Quote-Arg $_ }) -join " "
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $p = [System.Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $next = $p.StandardOutput.ReadLineAsync()
    while ($true) {
        if ($next.Wait(40)) {
            $line = $next.Result
            if ($null -eq $line) { break }
            & $onLine $line
            $next = $p.StandardOutput.ReadLineAsync()
        } else { Ui-Pump }
    }
    while (-not $p.WaitForExit(40)) { Ui-Pump }
    foreach ($line in ($err.Result -split "`r?`n")) { if ($line) { Ui-Log $line } }
    $p.ExitCode
}

# One hash over the files that decide what the data build makes (the builder, the recipes, the
# version), by relative path and content.
function Hash-Tree($base, [string[]] $rels) {
    $sb = New-Object Text.StringBuilder
    $items = foreach ($r in $rels) {
        $p = Join-Path $base $r
        if (Test-Path -LiteralPath $p -PathType Leaf) { Get-Item -LiteralPath $p }
        elseif (Test-Path -LiteralPath $p) { Get-ChildItem -LiteralPath $p -Recurse -File | Where-Object { $_.FullName -notmatch '\\__pycache__\\' } }
    }
    foreach ($it in ($items | Sort-Object { $_.FullName.ToLower() })) {
        [void]$sb.Append($it.FullName.Substring($base.Length + 1).ToLower()).Append("=").Append((Sha256-File $it.FullName)).Append("`n")
    }
    [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($sb.ToString()))) -replace "-", ""
}

# The mod's data archives (coop.s3darc over the game's patch.s3darc, coop_ati / coop_nv), built
# by the payload's own Python from the player's archives into a temporary folder; nothing of the
# game is changed here. Kept instead when the last install built them from the same game archive
# with the same version and recipes and they are still in place unchanged.
function Build-Paks($g, $src) {
    $paks = Join-Path $g "preload\paks"
    foreach ($n in "patch.s3darc", "patch_ati.s3darc", "patch_nv.s3darc", "out.s3darc") {
        if (-not (Test-Path -LiteralPath (Join-Path $paks $n))) { throw (L "paks_missing" $n) }
    }
    $baseSha = (Sha256-File (Join-Path $paks "patch.s3darc")).ToLower()
    $recipes = Hash-Tree $src @("coop\tools", "coop\data", "coop\version.txt")
    $bt = Join-Path $g "coop\coop_build.txt"
    $pt = Join-Path $g "coop\paks.txt"
    if ((Test-Path -LiteralPath $bt) -and (Test-Path -LiteralPath $pt)) {
        $info = Get-Content -LiteralPath $bt -Raw
        $have = @{}
        foreach ($line in Get-Content -LiteralPath $pt) { if ($line -match '^([^=]+)=(.*)$') { $have[$Matches[1]] = $Matches[2] } }
        $same = ($info -match ('(?m)^version = ' + [regex]::Escape($version) + '\r?$')) -and
                ($info -match ('(?m)^base = patch\.s3darc \(sha256 ' + $baseSha + '\)')) -and ($have["recipes"] -eq $recipes) -and
                (Get-ArcName $g) -ieq "coop.s3darc"
        foreach ($n in $script:PAKS) {
            $p = Join-Path $paks $n
            if (-not $same) { break }
            $same = (Test-Path -LiteralPath $p) -and $have[$n] -and ((Sha256-File $p) -eq $have[$n])
        }
        if ($same) { return @{ state = "kept" } }
    }
    $out = Join-Path ([IO.Path]::GetTempPath()) ("timeshift-coop-build-" + $PID)
    Remove-Item -LiteralPath $out -Recurse -Force -ErrorAction SilentlyContinue
    $py = Join-Path $src "coop\python\python.exe"
    $argList = @("-I", "-B", "-u", "-X", "utf8", (Join-Path $src "coop\tools\build_pak.py"), $g, "-o", $out)
    $code = Run-Live $py $argList { param($line) Ui-Log $line }
    $ok = $code -eq 0 -and (Test-Path -LiteralPath (Join-Path $out "coop_build.txt"))
    foreach ($n in $script:PAKS) { if (-not (Test-Path -LiteralPath (Join-Path $out $n))) { $ok = $false } }
    if (-not $ok) {
        Ui-Log "build_pak.py exit code $code"
        Remove-Item -LiteralPath $out -Recurse -Force -ErrorAction SilentlyContinue
        throw (L "build_failed")
    }
    @{ state = "built"; dir = $out; recipes = $recipes }
}

# The shortcut's icon: the game's own (read from the player's exe at each size, nothing of the
# game is shipped) with a co-op badge, two figures on a blue disc, in the lower right corner.
function New-CoopIcon($exe, $ico) {
    $frames = @()
    foreach ($size in 16, 24, 32, 48, 64, 128, 256) {
        $h = New-Object IntPtr[] 1; $id = New-Object UInt32[] 1
        $n = [TsCoopWin]::PrivateExtractIcons($exe, 0, $size, $size, $h, $id, 1, 0)
        if ($n -lt 1 -or $h[0] -eq [IntPtr]::Zero) { throw "no icon in $exe" }
        $src = [Drawing.Icon]::FromHandle($h[0]).ToBitmap()
        [TsCoopWin]::DestroyIcon($h[0]) | Out-Null
        $bmp = New-Object Drawing.Bitmap($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $gr = [Drawing.Graphics]::FromImage($bmp)
        $gr.SmoothingMode = "AntiAlias"; $gr.InterpolationMode = "HighQualityBicubic"
        $gr.DrawImage($src, 0, 0, $size, $size)
        # (bigger on the small sizes, where it has to be seen at all)
        $d = [single]($size * $(if ($size -le 24) { 0.62 } else { 0.46 }))
        $x = [single]($size - $d); $y = [single]($size - $d)
        $rim = [single][Math]::Max(1, $size / 32)
        $gr.FillEllipse((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(16, 22, 32))), $x, $y, $d, $d)
        $gr.FillEllipse((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(34, 124, 206))), $x + $rim, $y + $rim, $d - 2 * $rim, $d - 2 * $rim)
        # head and shoulders twice, the one behind a little higher and paler
        $u = $d / 10
        foreach ($f in @(@(6.1, 3.0, [Drawing.Color]::FromArgb(196, 222, 248)), @(3.9, 3.6, [Drawing.Color]::White))) {
            $br = New-Object Drawing.SolidBrush($f[2])
            $cx = $x + $f[0] * $u; $top = $y + $f[1] * $u
            $gr.FillEllipse($br, [single]($cx - 1.25 * $u), [single]$top, [single](2.5 * $u), [single](2.5 * $u))
            $path = New-Object Drawing.Drawing2D.GraphicsPath
            $path.AddPie([single]($cx - 2.4 * $u), [single]($top + 2.7 * $u), [single](4.8 * $u), [single](4.2 * $u), [single]180, [single]180)
            $gr.FillPath($br, $path)
            $path.Dispose(); $br.Dispose()
        }
        $gr.Dispose(); $src.Dispose()
        $frames += , @($size, $bmp)
    }
    # ICO: 256 as PNG, the others as 32-bit DIBs (what both the shell and .NET's Icon read).
    $blobs = @()
    foreach ($fr in $frames) {
        $size, $bmp = $fr
        $p = New-Object IO.MemoryStream
        if ($size -ge 256) { $bmp.Save($p, [Drawing.Imaging.ImageFormat]::Png) } else {
            $bw = New-Object IO.BinaryWriter($p)
            $mask = [int]([Math]::Ceiling($size / 32.0) * 4)
            $bw.Write([UInt32]40); $bw.Write([Int32]$size); $bw.Write([Int32](2 * $size)); $bw.Write([UInt16]1); $bw.Write([UInt16]32)
            $bw.Write([UInt32]0); $bw.Write([UInt32]($size * $size * 4 + $mask * $size))
            $bw.Write([Int32]0); $bw.Write([Int32]0); $bw.Write([UInt32]0); $bw.Write([UInt32]0)
            $data = $bmp.LockBits((New-Object Drawing.Rectangle(0, 0, $size, $size)), [Drawing.Imaging.ImageLockMode]::ReadOnly, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $px = New-Object byte[] ($data.Stride * $size)
            [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $px, 0, $px.Length)
            $bmp.UnlockBits($data)
            for ($row = $size - 1; $row -ge 0; $row--) { $bw.Write($px, $row * $data.Stride, $size * 4) }
            $bw.Write((New-Object byte[] ($mask * $size)))
            $bw.Flush()
        }
        $blobs += , $p.ToArray()
    }
    $ms = New-Object IO.MemoryStream
    $w = New-Object IO.BinaryWriter($ms)
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$frames.Count)
    $off = 6 + 16 * $frames.Count
    for ($i = 0; $i -lt $frames.Count; $i++) {
        $size = $frames[$i][0]
        $w.Write([byte]($size % 256)); $w.Write([byte]($size % 256)); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$blobs[$i].Length); $w.Write([UInt32]$off)
        $off += $blobs[$i].Length
    }
    foreach ($b in $blobs) { $w.Write($b) }
    $w.Flush()
    [IO.File]::WriteAllBytes($ico, $ms.ToArray())
    foreach ($fr in $frames) { $fr[1].Dispose() }
}

# "TimeShift Co-op" shortcuts to coop\coop-launcher.ps1: on the desktop and in the Start menu (the
# window's install), those an earlier install made and the player kept (refreshed), or in
# -ShortcutDir. Returns their paths for the install record.
function Make-Shortcuts($g, $old) {
    $exe = Join-Path $g "bin\TimeShift.exe"
    $ico = Join-Path $g "coop\coop.ico"
    $iconAt = "$exe,0"
    if ($script:haveWin) {
        try { New-CoopIcon $exe $ico; $iconAt = "$ico,0" } catch { Ui-Log "coop.ico: $($_.Exception.Message)" }
    }
    $links = New-Object System.Collections.Generic.List[string]
    foreach ($lnk in $old.shortcuts) {
        if ((Test-Path -LiteralPath $lnk) -and -not $links.Contains($lnk)) { $links.Add($lnk) }
    }
    $dirs = @()
    if ($ShortcutDir) { $dirs = @($ShortcutDir -split ';' | Where-Object { $_ }) }
    elseif (-not $Quiet -and -not $NoShortcut) { $dirs = @([Environment]::GetFolderPath("Desktop"), [Environment]::GetFolderPath("Programs")) }
    foreach ($d in $dirs) {
        if (-not $d) { continue }
        $lnk = Join-Path $d "TimeShift Co-op.lnk"
        if (-not $links.Contains($lnk)) { $links.Add($lnk) }
    }
    $shell = New-Object -ComObject WScript.Shell
    foreach ($lnk in $links) {
        $sc = $shell.CreateShortcut($lnk)
        $sc.TargetPath = Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe"
        $sc.Arguments = '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + (Join-Path $g "coop\coop-launcher.ps1") + '"'
        $sc.WorkingDirectory = Join-Path $g "bin"
        $sc.IconLocation = $iconAt
        $sc.Description = L "shortcut_desc"
        $sc.WindowStyle = 7
        $sc.Save()
        Ui-Log ((L "shortcut") + $lnk)
    }
    $links.ToArray()
}

# One install or removal at a time (a double-clicked shortcut, a second installer window).
function Lock-Install {
    if (-not $script:installLock) { $script:installLock = New-Object Threading.Mutex($false, "Local\TimeShiftCoopInstall") }
    try { $got = $script:installLock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $got = $true }
    if (-not $got) { throw (L "another_install") }
}

# Program Files: writing there needs administrator rights.
function Need-Admin($g) {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    if ((New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { return $false }
    foreach ($d in @($g, (Join-Path $g "bin"), (Join-Path $g "preload\paks"))) {
        $probe = Join-Path $d ("coop-write-test-" + $PID + ".tmp")
        try { [IO.File]::WriteAllText($probe, "1"); Remove-Item -LiteralPath $probe -Force }
        catch [UnauthorizedAccessException] { return $true }
        catch { }
    }
    $false
}

# This installer again with administrator rights for the same folder (Windows asks the player);
# its exit code, -1 when the player said no.
function Run-Elevated($g, [switch] $Remove) {
    $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Quote-Arg $PSCommandPath), "-Game", (Quote-Arg $g))
    if ($Remove) { $a += "-Uninstall" }
    if ($AutoClose) { $a += "-AutoClose" }
    if ($NoShortcut) { $a += "-NoShortcut" }
    if ($ShortcutDir) { $a += @("-ShortcutDir", (Quote-Arg $ShortcutDir)) }
    $a += @("-Lang", $script:uiLang)
    try {
        $p = Start-Process powershell -Verb RunAs -ArgumentList $a -Wait -PassThru
        return $p.ExitCode
    } catch { return -1 }
}

function Do-Install($g) {
    Ui-Step "check" "run"
    Ui-Progress 1 $g
    if (-not (Test-Path -LiteralPath $payload)) { throw (L "no_files") }
    # The payload first into a folder of this installer's own: whatever happens to the folder it
    # came in (a launcher started twice cleans it up) cannot take files away mid-copy.
    $snap = Join-Path ([IO.Path]::GetTempPath()) ("timeshift-coop-payload-" + $PID)
    Remove-Item -LiteralPath $snap -Recurse -Force -ErrorAction SilentlyContinue
    Copy-Item -LiteralPath $payload $snap -Recurse -Force
    try { Install-From $g $snap }
    finally {
        foreach ($d in $snap, (Join-Path ([IO.Path]::GetTempPath()) ("timeshift-coop-build-" + $PID))) {
            Remove-Item -LiteralPath $d -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# The install itself, from the payload's snapshot.
function Install-From($g, $payload) {
    # (the package's list of its files: all of them here, or the game is not touched)
    $plist = Join-Path $payload "coop\payload.txt"
    if (-not (Test-Path -LiteralPath $plist)) { Ui-Log "missing: coop\payload.txt"; throw (L "incomplete") }
    $packageFiles = @(Get-Content -LiteralPath $plist | Where-Object { $_ })
    foreach ($rel in $packageFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $payload $rel))) { Ui-Log ("missing: " + $rel); throw (L "incomplete") }
    }
    if (Game-Running $g) { throw (L "close_game") }
    $bad = Exe-Mismatch $g
    if ($bad) { throw (L "exe_wrong" $bad) }
    Lock-Install
    $old = Read-Manifest $g
    if ($old.version) { Ui-Log ((L "installed_now") + $old.version) }
    Ui-Step "check" "done"

    # The data first: a build that fails (another mod changed the menus, another game version)
    # leaves the game as it was.
    Ui-Step "paks" "run"
    Ui-Progress 5 "preload\paks\patch.s3darc"
    $pk = Build-Paks $g $payload
    if ($pk.state -eq "kept") { Ui-Log (L "paks_kept") } else { Ui-Log (L "paks_built") }
    Ui-Step "paks" "done"

    Ui-Step "copy" "run"
    $backups = New-Object System.Collections.Generic.List[string]
    foreach ($b in $old.backups) { $backups.Add($b) }
    $files = New-Object System.Collections.Generic.List[string]
    $all = @(Get-ChildItem -LiteralPath $payload -Recurse -File)
    # The install record as far as it got: a failure later on can still be removed or retried.
    # coop\version.txt goes last, once everything is in: the launcher reads it, and an update
    # that broke off is tried again at the next start.
    $built = New-Object System.Collections.Generic.List[string]
    foreach ($b in $old.built) { $built.Add($b) }
    $links = @($old.shortcuts)
    $arcOrig = $old.arcname
    $writeRecord = {
        $lines = @("version=$version") + ($files | ForEach-Object { "file=$_" }) + ($backups | ForEach-Object { "backup=$_" }) +
            ($built | ForEach-Object { "built=$_" }) + ($links | ForEach-Object { "shortcut=$_" })
        if ($arcOrig) { $lines += "arcname=$arcOrig" }
        New-Item -ItemType Directory -Force (Join-Path $g "coop") | Out-Null
        Set-Content -LiteralPath (Join-Path $g "coop\install.txt") -Value $lines -Encoding UTF8
    }
    # A file of someone else's that we replace: keep the original once (a co-op DLL is not one).
    $keep = {
        param($rel)
        $dst = Join-Path $g $rel
        if ((Test-Path -LiteralPath $dst) -and ($old.files -notcontains $rel) -and ($old.built -notcontains $rel) -and
            ($backups -notcontains $rel) -and -not $rel.StartsWith("coop\") -and -not (Is-CoopDll $dst)) {
            $bk = Join-Path $g "coop-backup\$rel"
            # (a backup from an earlier, interrupted install is the real original: kept)
            if (-not (Test-Path -LiteralPath $bk)) {
                New-Item -ItemType Directory -Force (Split-Path $bk -Parent) | Out-Null
                Copy-Item -LiteralPath $dst $bk -Force
            }
            $backups.Add($rel)
            Ui-Log ((L "original_kept") + $rel)
        }
    }
    $versionFile = $null
    $i = 0
    foreach ($item in $all) {
        $i++
        $rel = $item.FullName.Substring($payload.Length + 1)
        if ($rel -eq "coop\version.txt") { $versionFile = $item; $files.Add($rel); continue }
        $dst = Join-Path $g $rel
        Ui-Progress (10 + 60 * $i / $all.Count) $rel
        & $keep $rel
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        Copy-Item -LiteralPath $item.FullName $dst -Force
        $files.Add($rel)
    }
    Ui-Log (L "copied" $($all.Count))
    # Files of an older version that this one no longer has (by the package's list: never a file it has).
    foreach ($rel in $old.files) {
        if ($packageFiles -notcontains $rel -and $files -notcontains $rel -and $rel -notin "coop-installer.ps1", "Uninstall Co-op.bat") {
            Remove-Item -LiteralPath (Join-Path $g $rel) -Force -ErrorAction SilentlyContinue
        }
    }
    # The uninstaller stays in the game folder.
    # (not onto itself when the archive was extracted into the game folder)
    $src, $dst = (Join-Path $here "coop-installer.ps1"), (Join-Path $g "coop-installer.ps1")
    if ([IO.Path]::GetFullPath($src) -ne [IO.Path]::GetFullPath($dst)) { Copy-Item -LiteralPath $src $dst -Force }
    if ($files -notcontains "coop-installer.ps1") { $files.Add("coop-installer.ps1") }
    $un = "Uninstall Co-op.bat"
    Set-Content -LiteralPath (Join-Path $g $un) -Encoding ASCII -Value '@start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0coop-installer.ps1" -Uninstall -Game "%~dp0."'
    if ($files -notcontains $un) { $files.Add($un) }
    & $writeRecord

    # The data archives next to the game's, then the game pointed at them.
    Ui-Progress 75 "preload\paks"
    if ($pk.state -eq "built") {
        $shas = @("recipes=" + $pk.recipes)
        foreach ($n in $script:PAKS) {
            $rel = "preload\paks\$n"
            & $keep $rel
            Copy-Item -LiteralPath (Join-Path $pk.dir $n) (Join-Path $g $rel) -Force
            if (-not $built.Contains($rel)) { $built.Add($rel) }
            $shas += ("$n=" + (Sha256-File (Join-Path $g $rel)))
        }
        Copy-Item -LiteralPath (Join-Path $pk.dir "coop_build.txt") (Join-Path $g "coop\coop_build.txt") -Force
        Set-Content -LiteralPath (Join-Path $g "coop\paks.txt") -Value $shas -Encoding ASCII
        Remove-Item -LiteralPath $pk.dir -Recurse -Force -ErrorAction SilentlyContinue
    }
    $cur = Get-ArcName $g
    if ($cur -ine "coop.s3darc") {
        if ($cur -ine "patch.s3darc") { Ui-Log (L "ini_other" $cur) }
        $arcOrig = $cur
        & $writeRecord
        Set-ArcName $g "coop.s3darc"
        Ui-Log (L "ini_set")
    } elseif (-not $arcOrig) { $arcOrig = "patch.s3darc" }
    & $writeRecord
    Ui-Step "copy" "done"

    Ui-Step "finish" "run"
    Ui-Progress 90 "coop\install.txt"
    $links = @()
    try { $links = @(Make-Shortcuts $g $old) } catch { Ui-Log "shortcut: $($_.Exception.Message)"; $links = @($old.shortcuts | Where-Object { Test-Path -LiteralPath $_ }) }
    & $writeRecord
    if ($versionFile) { Copy-Item -LiteralPath $versionFile.FullName (Join-Path $g "coop\version.txt") -Force }
    Ui-Step "finish" "done"
    Ui-Progress 100 ""
    @{ old = $old.version; paks = $pk.state; links = $links.Count }
}

function Install-Message($g, $r) {
    $msg = L "start_msg"
    if ($r.links -gt 0) { $msg += "`n`n" + (L "the_shortcut") }
    $msg
}

function Save-Log($g) {
    $text = $script:logLines -join "`r`n"
    try { Set-Content -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) $script:LOGNAME) -Value $text -Encoding UTF8 } catch { }
    if ($g -and (Test-Path -LiteralPath (Join-Path $g "coop"))) {
        try { Set-Content -LiteralPath (Join-Path $g "coop\install-log.txt") -Value $text -Encoding UTF8 } catch { }
    }
}

# ---------------------------------------------------------------------------------------------
# -Quiet: the console only

if ($Quiet) {
    $g = ""
    try {
        $g = $Game
        if ($g) { if (-not (Test-Game $g)) { throw (L "not_a_folder" $g) }; $g = (Resolve-Path -LiteralPath $g).Path.TrimEnd('\') }
        else { $g = @(Find-Games)[0]; if (-not $g) { throw (L "not_found") } }
        if ($Uninstall) {
            $null = Do-Uninstall $g
            Save-Log ""
            Write-Host (L "removed_done")
        } else {
            $r = Do-Install $g
            Save-Log $g
            Write-Host ((L "is_installed" $version $g) + "`n" + (Install-Message $g $r))
        }
        exit 0
    } catch {
        Ui-Log ((L "error") + $_.Exception.Message)
        Save-Log $g
        exit 1
    }
}

# ---------------------------------------------------------------------------------------------
# The window

if ($script:haveWin) {
    [TsCoopWin]::ShowWindow([TsCoopWin]::GetConsoleWindow(), 0) | Out-Null
    [TsCoopWin]::SetProcessDPIAware() | Out-Null
}
[System.Windows.Forms.Application]::EnableVisualStyles()

$C = @{
    bg = [Drawing.Color]::FromArgb(20, 24, 30); panel = [Drawing.Color]::FromArgb(30, 36, 46)
    text = [Drawing.Color]::FromArgb(230, 234, 240); dim = [Drawing.Color]::FromArgb(136, 146, 160)
    accent = [Drawing.Color]::FromArgb(34, 124, 206); accentHi = [Drawing.Color]::FromArgb(64, 152, 232)
    ok = [Drawing.Color]::FromArgb(84, 196, 122); warn = [Drawing.Color]::FromArgb(236, 176, 64); bad = [Drawing.Color]::FromArgb(236, 84, 84)
    hdr1 = [Drawing.Color]::FromArgb(12, 48, 92); hdr2 = [Drawing.Color]::FromArgb(14, 16, 22)
    btn = [Drawing.Color]::FromArgb(52, 60, 72); btnHi = [Drawing.Color]::FromArgb(70, 80, 94)
}
function Font($size, $style = "Regular", $name = "Segoe UI") { New-Object Drawing.Font($name, $size, [Drawing.FontStyle]$style) }

function New-Label($parent, $text, $x, $y, $w, $h, $font, $color) {
    $l = New-Object Windows.Forms.Label
    $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y); $l.Size = New-Object Drawing.Size($w, $h)
    $l.Font = $font; $l.ForeColor = $color; $l.BackColor = [Drawing.Color]::Transparent
    $l.UseMnemonic = $false
    $parent.Controls.Add($l)
    $l
}

function New-Button($parent, $text, $x, $y, $w, $primary) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x, $y); $b.Size = New-Object Drawing.Size($w, 36)
    $b.FlatStyle = "Flat"; $b.Font = Font 10 $(if ($primary) { "Bold" } else { "Regular" })
    $b.ForeColor = [Drawing.Color]::White; $b.Cursor = [Windows.Forms.Cursors]::Hand
    $b.BackColor = $(if ($primary) { $C.accent } else { $C.btn })
    $b.FlatAppearance.BorderSize = 0
    $b.FlatAppearance.MouseOverBackColor = $(if ($primary) { $C.accentHi } else { $C.btnHi })
    $b.UseMnemonic = $false
    $parent.Controls.Add($b)
    $b
}

$form = New-Object Windows.Forms.Form
$form.SuspendLayout()
$form.AutoScaleDimensions = New-Object Drawing.SizeF(96, 96)
$form.AutoScaleMode = "Dpi"
$form.Text = "$($script:TITLE) $version"
$form.ClientSize = New-Object Drawing.Size(660, 500)
$form.FormBorderStyle = "FixedSingle"; $form.MaximizeBox = $false
$form.StartPosition = "CenterScreen"
$form.BackColor = $C.bg; $form.ForeColor = $C.text
$form.Font = Font 10

# Header: a dark blue to black band, the title, a blue line under it.
$hdr = New-Object Windows.Forms.Panel
$hdr.Location = New-Object Drawing.Point(0, 0); $hdr.Size = New-Object Drawing.Size(660, 96)
$hdr.Add_Paint({
    param($s, $e)
    $r = $s.ClientRectangle
    if ($r.Width -le 0) { return }
    $br = New-Object Drawing.Drawing2D.LinearGradientBrush($r, $C.hdr1, $C.hdr2, [single]0)
    $e.Graphics.FillRectangle($br, $r); $br.Dispose()
    $pen = New-Object Drawing.Pen($C.accent, [single]3)
    $e.Graphics.DrawLine($pen, 0, $r.Height - 2, $r.Width, $r.Height - 2); $pen.Dispose()
})
$form.Controls.Add($hdr)
$null = New-Label $hdr "TIMESHIFT CO-OP" 26 16 600 40 (Font 22 "Bold") ([Drawing.Color]::White)
$lbSub = New-Label $hdr "" 28 58 600 24 (Font 10) ([Drawing.Color]::FromArgb(190, 206, 226))

# --- page 1: the folder and what will be done
$pSetup = New-Object Windows.Forms.Panel
$pSetup.Location = New-Object Drawing.Point(0, 96); $pSetup.Size = New-Object Drawing.Size(660, 404)
$form.Controls.Add($pSetup)
$lbFolder = New-Label $pSetup "" 28 22 400 22 (Font 10 "Bold") $C.text
$tbGame = New-Object Windows.Forms.TextBox
$tbGame.Location = New-Object Drawing.Point(28, 48); $tbGame.Size = New-Object Drawing.Size(474, 28)
$tbGame.BackColor = $C.panel; $tbGame.ForeColor = $C.text; $tbGame.BorderStyle = "FixedSingle"; $tbGame.Font = Font 10.5
$pSetup.Controls.Add($tbGame)
$btBrowse = New-Button $pSetup (L "browse") 512 45 120 $false
$lbInfo = New-Label $pSetup "" 28 96 604 220 (Font 10.5) $C.text
$btRemove = New-Button $pSetup (L "remove_co_op") 28 340 150 $false
$btCancel = New-Button $pSetup (L "cancel") 372 340 120 $false
$btGo = New-Button $pSetup (L "install") 502 340 130 $true

# --- page 2: the steps and the progress
$pWork = New-Object Windows.Forms.Panel
$pWork.Location = New-Object Drawing.Point(0, 96); $pWork.Size = New-Object Drawing.Size(660, 404); $pWork.Visible = $false
$form.Controls.Add($pWork)
$lbWorkTitle = New-Label $pWork "" 28 18 470 30 (Font 14 "Bold") $C.text
$lbTime = New-Label $pWork "0:00" 500 24 132 22 (Font 10) $C.dim
$lbTime.TextAlign = "MiddleRight"
$steps = @{}
function Add-Steps($list) {
    foreach ($k in @($steps.Keys)) { $pWork.Controls.Remove($steps[$k].icon); $pWork.Controls.Remove($steps[$k].text) }
    $steps.Clear()
    $y = 62
    foreach ($s in $list) {
        $ic = New-Label $pWork ([string][char]0x25CB) 30 $y 24 26 (Font 11 "Bold" "Segoe UI Symbol") $C.dim
        $tx = New-Label $pWork $s[1] 58 ($y + 2) 574 24 (Font 10.5) $C.dim
        $steps[$s[0]] = @{ icon = $ic; text = $tx }
        $y += 30
    }
}
$lbPct = New-Label $pWork "0 %" 540 212 92 22 (Font 10 "Bold") $C.text
$lbPct.TextAlign = "MiddleRight"
$lbAction = New-Label $pWork "" 28 212 510 22 (Font 9.5) $C.dim
$lbAction.AutoEllipsis = $true
$bar = New-Object Windows.Forms.Panel
$bar.Location = New-Object Drawing.Point(28, 238); $bar.Size = New-Object Drawing.Size(604, 14); $bar.BackColor = $C.panel
$pWork.Controls.Add($bar)
$fill = New-Object Windows.Forms.Panel
$fill.Location = New-Object Drawing.Point(0, 0); $fill.Size = New-Object Drawing.Size(0, 14); $fill.BackColor = $C.accent
$bar.Controls.Add($fill)
$tbLog = New-Object Windows.Forms.TextBox
$tbLog.Location = New-Object Drawing.Point(28, 264); $tbLog.Size = New-Object Drawing.Size(604, 70)
$tbLog.Multiline = $true; $tbLog.ReadOnly = $true; $tbLog.ScrollBars = "Vertical"; $tbLog.BorderStyle = "None"
$tbLog.BackColor = $C.panel; $tbLog.ForeColor = $C.dim; $tbLog.Font = Font 8.5 "Regular" "Consolas"
$pWork.Controls.Add($tbLog)
$btLogCopy = New-Button $pWork (L "copy_log") 28 346 170 $false
$btLogCopy.Visible = $false
$btFolder = New-Button $pWork (L "open_game_folder") 206 346 180 $false
$btFolder.Visible = $false
$btReadme = New-Button $pWork "README" 394 346 100 $false
$btReadme.Visible = $false
$btClose = New-Button $pWork (L "close") 502 346 130 $true
$btClose.Visible = $false

# --- page 3 (over page 2's steps): the result
$lbResIcon = New-Label $pWork "" 24 60 60 60 (Font 30 "Bold" "Segoe UI Symbol") $C.ok
$lbResIcon.Visible = $false
$lbResText = New-Label $pWork "" 92 66 540 140 (Font 10.5) $C.text
$lbResText.Visible = $false

$form.ResumeLayout($false)

$script:busy = $false
$script:exitCode = 1
$script:target = ""
$script:t0 = Get-Date
$script:removeVer = ""

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 500
$timer.Add_Tick({
    if (-not $script:busy) { return }
    $s = [int]((Get-Date) - $script:t0).TotalSeconds
    $lbTime.Text = "{0}:{1:00}" -f [int][Math]::Floor($s / 60), ($s % 60)
    # the running step's dot pulses: something is being done
    $r = $script:ui.running
    if ($r) { $r.icon.ForeColor = $(if ($r.icon.ForeColor.ToArgb() -eq $C.accent.ToArgb()) { $C.accentHi } else { $C.accent }) ; $r.icon.Text = $(if ($r.icon.Text -eq [string][char]0x25CF) { [string][char]0x25C9 } else { [string][char]0x25CF }) }
})

$form.Add_FormClosing({
    param($sender, $ev)
    if ($script:busy) {
        $ev.Cancel = $true
        [Windows.Forms.MessageBox]::Show($form, (L "please_wait"), $script:TITLE, "OK", "Information") | Out-Null
    }
})

# The README in the window's language.
function Readme-Path {
    $name = if ($script:uiLang -eq "ru") { "README-COOP-ru.txt" } else { "README-COOP.txt" }
    Join-Path $here $name
}

# What page 1 says about the chosen folder.
function Update-Setup {
    $g = $tbGame.Text.Trim().Trim('"')
    $ok = Test-Game $g
    $lines = @()
    $btRemove.Visible = $false
    if (-not $g) {
        $lines += (L "choose_folder")
    } elseif (-not $ok) {
        $lines += ([string][char]0x2716 + "  " + (L "no_exe"))
    } else {
        $m = Read-Manifest $g
        if ($m.files.Count -gt 0) { $btRemove.Visible = $true }
        if (Exe-Mismatch $g) {
            $ok = $false
            $lines += ([string][char]0x2716 + "  " + (L "found_other"))
        } else {
            $lines += ([string][char]0x2714 + "  " + (L "found"))
            if ($m.files.Count -gt 0) {
                if ($m.version -eq $version) {
                    $lines += ([string][char]0x2022 + "  " + (L "already" $version))
                    $btGo.Text = L "reinstall"
                } else {
                    $lines += ([string][char]0x2022 + "  " + (L "will_update" $($m.version) $version))
                    $btGo.Text = L "update"
                }
            } else { $btGo.Text = L "install" }
            $lines += ""
            $lines += (L "what")
            $lines += ""
            $lines += (L "same_version")
        }
    }
    $lbInfo.Text = $lines -join "`n"
    $lbInfo.ForeColor = $(if ($ok) { $C.text } else { $C.warn })
    $btGo.Enabled = $ok
    $btGo.BackColor = $(if ($ok) { $C.accent } else { $C.btn })
}

function Show-Work($title, $stepList) {
    $pSetup.Visible = $false
    $pWork.Visible = $true
    $lbWorkTitle.Text = $title
    Add-Steps $stepList
    $script:ui = @{ form = $form; bar = $bar; fill = $fill; pct = $lbPct; action = $lbAction; log = $tbLog; steps = $steps; colors = $C; running = $null }
    $script:t0 = Get-Date
    $script:busy = $true
    $timer.Start()
    Ui-Progress 0 ""
}

function Show-Result($ok, $title, $text) {
    $script:busy = $false
    $timer.Stop()
    foreach ($k in @($steps.Keys)) { $steps[$k].icon.Visible = $false; $steps[$k].text.Visible = $false }
    $lbWorkTitle.Text = $title
    $lbWorkTitle.ForeColor = $(if ($ok) { $C.ok } else { $C.bad })
    $lbResIcon.Text = $(if ($ok) { [string][char]0x2714 } else { [string][char]0x2716 })
    $lbResIcon.ForeColor = $(if ($ok) { $C.ok } else { $C.bad })
    $lbResText.Text = $text
    $lbResIcon.Visible = $true; $lbResText.Visible = $true
    $lbAction.Text = ""
    if ($ok) { $fill.BackColor = $C.ok } else { $fill.BackColor = $C.bad; $fill.Width = $bar.ClientSize.Width; $lbPct.Text = "" }
    if ($script:haveWin) { [TsCoopWin]::Progress($form.Handle, $(if ($ok) { 0 } else { 4 }), 100) }
    $btClose.Visible = $true
    $btLogCopy.Visible = -not $ok
    $btFolder.Visible = $ok -and $script:target -and (Test-Path -LiteralPath $script:target)
    $btReadme.Visible = $ok -and (Test-Path -LiteralPath (Readme-Path))
    $btClose.Focus()
    $form.Activate()
}

# Closes the window by itself after a successful run (-AutoClose: the launcher's update).
function Close-Soon {
    $script:closer = New-Object Windows.Forms.Timer
    $script:closer.Interval = 2000
    $script:closer.Add_Tick({ $script:closer.Stop(); $form.Close() })
    $script:closer.Start()
}

# A folder Windows protects (Program Files): the same work by an elevated copy of this installer,
# this window closes with its result.
function Elevated-Instead($g, [switch] $Remove) {
    if (-not (Need-Admin $g)) { return $false }
    $form.Hide()
    $code = Run-Elevated $g -Remove:$Remove
    if ($code -lt 0) {
        $form.Show()
        Show-Work $(if ($Remove) { L "removing" } else { L "installing" $version }) @()
        Ui-Log (L "no_admin")
        Show-Result $false $(if ($Remove) { L "remove_failed" } else { L "install_failed" }) (L "no_admin")
        return $true
    }
    $script:exitCode = $code
    $form.Close()
    $true
}

function Start-Install($g) {
    $g = (Resolve-Path -LiteralPath $g).Path.TrimEnd('\')
    if (Elevated-Instead $g) { return }
    $script:target = $g
    $ic = Window-Icon $g
    if ($ic) { $form.Icon = $ic }
    Show-Work (L "installing" $version) @(
        @("check", (L "st_check")),
        @("paks", (L "st_paks")),
        @("copy", (L "st_copy")),
        @("finish", (L "st_finish")))
    Ui-Log ((L "folder_log") + $g)
    try {
        $r = Do-Install $g
        Save-Log $g
        $title = $(if ($r.old -and $r.old -ne $version) { L "updated" $($r.old) $version } else { L "installed_title" $version })
        $script:exitCode = 0
        $ic = Window-Icon $g
        if ($ic) { $form.Icon = $ic }
        Show-Result $true $title (Install-Message $g $r)
        if ($AutoClose) { Close-Soon }
    } catch {
        Ui-Log ("ERROR: " + $_.Exception.Message)
        if ($script:ui.running) { $script:ui.running.icon.Text = [string][char]0x2716; $script:ui.running.icon.ForeColor = $C.bad }
        Save-Log $g
        Show-Result $false (L "install_failed") ($_.Exception.Message + "`n`n" + (L "log_in"))
    }
}

function Start-Uninstall($g) {
    $g = (Resolve-Path -LiteralPath $g).Path.TrimEnd('\')
    if (Elevated-Instead $g -Remove) { return }
    $script:target = $g
    if (Game-Running $g) {
        [Windows.Forms.MessageBox]::Show($form, (L "close_game"), $script:TITLE, "OK", "Warning") | Out-Null
        return
    }
    $ic = Window-Icon $g
    if ($ic) { $form.Icon = $ic }
    Show-Work (L "removing") @(
        @("rm", (L "st_rm")),
        @("restore", (L "st_restore")),
        @("clean", (L "st_clean")))
    try {
        $null = Do-Uninstall $g
        $script:exitCode = 0
        Save-Log ""
        Show-Result $true (L "removed_title") (L "removed_text")
        $btReadme.Visible = $false
        if ($AutoClose) { Close-Soon }
    } catch {
        Ui-Log ("ERROR: " + $_.Exception.Message)
        Save-Log ""
        Show-Result $false (L "remove_failed") ($_.Exception.Message + "`n`n" + (L "log_in"))
    }
}

$tbGame.Add_TextChanged({ Update-Setup })
$btBrowse.Add_Click({
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = L "select_folder"
    $cur = $tbGame.Text.Trim()
    if ($cur -and (Test-Path -LiteralPath $cur)) { $dlg.SelectedPath = $cur }
    if ($dlg.ShowDialog($form) -eq "OK") { $tbGame.Text = $dlg.SelectedPath }
})
$btCancel.Add_Click({ $form.Close() })
$btClose.Add_Click({ $form.Close() })
$btGo.Add_Click({
    $g = $tbGame.Text.Trim().Trim('"')
    if (-not (Test-Game $g)) { return }
    if (Game-Running $g) {
        [Windows.Forms.MessageBox]::Show($form, (L "close_game"), $script:TITLE, "OK", "Warning") | Out-Null
        return
    }
    Start-Install $g
})
$btRemove.Add_Click({
    $g = $tbGame.Text.Trim().Trim('"')
    # (the uninstaller's page is the question already)
    if (-not $Uninstall) {
        $q = [Windows.Forms.MessageBox]::Show($form, (L "remove_q" $g), $script:TITLE, "YesNo", "Question")
        if ($q -ne "Yes") { return }
    }
    Start-Uninstall $g
})
$btFolder.Add_Click({ if ($script:target) { Start-Process explorer.exe -ArgumentList ('"' + $script:target + '"') } })
$btReadme.Add_Click({ Start-Process notepad.exe -ArgumentList ('"' + (Readme-Path) + '"') })
$btLogCopy.Add_Click({ [Windows.Forms.Clipboard]::SetText(($script:logLines -join "`r`n") + "`r`n") })

$lbSub.Text = L "subtitle" $version
$lbFolder.Text = L "game_folder"

# Started for a folder: remove asks first (unless -AutoClose), install goes straight on.
# (the title bar's colour is taken as the window is created)
$form.Add_HandleCreated({ if ($script:haveWin) { [TsCoopWin]::DarkTitle($form.Handle) } })
$form.Add_Shown({
    if ($script:haveWin) { [TsCoopWin]::SetWindowTheme($tbLog.Handle, "DarkMode_Explorer", $null) | Out-Null }
    $form.Activate()
    if ($Game) {
        if (-not (Test-Game $Game)) {
            $tbGame.Text = $Game
            return
        }
        $tbGame.Text = (Resolve-Path -LiteralPath $Game).Path.TrimEnd('\')
        if ($Uninstall -and $AutoClose) {
            Start-Uninstall $tbGame.Text
        } elseif ($Uninstall) {
            $m = Read-Manifest $tbGame.Text
            $script:removeVer = "$($m.version)"
            $lbInfo.Text = (L "remove_page" $($m.version))
            $btGo.Visible = $false
            $btRemove.Visible = $true
            $btRemove.BackColor = $C.accent
            $btRemove.Location = New-Object Drawing.Point(($btGo.Left - ($btRemove.Width - $btGo.Width)), $btGo.Top)
            $btCancel.Left = $btRemove.Left - $btCancel.Width - 10
            $tbGame.ReadOnly = $true; $btBrowse.Enabled = $false
            $btRemove.Focus()
        } else {
            Start-Install $tbGame.Text
        }
    } else {
        $found = @(Find-Games)
        if ($found.Count -ge 1) { $tbGame.Text = $found[0] } else { Update-Setup }
        $btGo.Focus()
    }
})

[Windows.Forms.Application]::Run($form)
exit $script:exitCode
