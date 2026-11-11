# TimeShift Co-op installer / uninstaller for players (shipped in the release zip as
# "Install Co-op.bat" + coop-installer.ps1, payload in the "files" folder next to it).
#   install: finds the game folder (this folder, install records in the registry, Steam
#            libraries, or the player picks it), checks the exe is patch 1.2, builds the mod's data
#            archives from the player's own game files (coop\python + coop\tools\build_pak.py),
#            keeps the files it replaces in <game>\coop-backup, copies the mod, points
#            bin\steamwars.ini at coop.s3darc and writes <game>\coop\install.txt. When the bin\dinput8.dll
#            it replaces is an Ultimate ASI Loader (the loader of most ASI mods), a copy of it runs on as
#            bin\winmm.dll, so its plugins stay on (Chain-Plan).
#   update:  the same over an older version; originals already backed up are kept; the data
#            archives are kept when the game's archive, the mod version and the recipes are unchanged.
#            A dinput8.dll another mod put over the co-op one meanwhile becomes the original.
#   remove:  puts arcName back, deletes what install.txt lists and puts the backed-up files back (a
#            dinput8.dll another mod put over the co-op one, or a loader copy the player changed, stays).
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
# -SteamUpdate: the 1.2 update package the player supplies for a Steam copy (zip or folder); used only when the
# game's exe is not the patch 1.2 code. The files it replaces (the exe, the ini and the patch archives) are named first; the window asks
# for a tick, the quiet mode needs -AcceptSteamUpdate, else it refuses with the same words (see notes\plan-0.1.1-steam.md).
# Usage: coop-installer.ps1 [-Game <folder>] [-Uninstall] [-Quiet] [-Lang en|ru|de|fr|es|it|pl|cs|hu]
#                           [-AutoClose] [-NoShortcut] [-ShortcutDir <folder>[;<folder>]] [-SteamUpdate <zip|folder> [-AcceptSteamUpdate]]
param([string] $Game = "", [switch] $Uninstall, [switch] $Quiet, [string] $Lang = "",
      [switch] $AutoClose, [switch] $NoShortcut, [string] $ShortcutDir = "", [string] $GameProcess = "TimeShift",
      [string] $SteamUpdate = "", [switch] $AcceptSteamUpdate)
$ErrorActionPreference = "Stop"
# The first act: "Install Co-op.bat" waits for this file to know that PowerShell really started the installer.
if (-not $Quiet -and -not $env:COOP_SETUP_FLAG) { try { [IO.File]::WriteAllText((Join-Path ([IO.Path]::GetTempPath()) "timeshift-coop-installer-started.flag"), "1") } catch { } }
# The setup exe (timeshift-coop-setup-<version>.exe, tools\mkinstaller) unpacks this installer into a temp folder and says where it
# lies (COOP_SETUP_DIR: the game and a Steam update package are looked for there too) and which file to write as the first act
# (COOP_SETUP_FLAG: a failed run without it = PowerShell never ran the script, the exe says so; it replaces the %TEMP% flag of the .bat).
if ($env:COOP_SETUP_FLAG) { try { [IO.File]::WriteAllText($env:COOP_SETUP_FLAG, "1") } catch { } }
$script:setupDir = if ($env:COOP_SETUP_DIR -and (Test-Path -LiteralPath $env:COOP_SETUP_DIR -PathType Container)) { $env:COOP_SETUP_DIR } else { $null }
$here = $PSScriptRoot
$payload = Join-Path $here "files"
$script:TITLE = "TimeShift Co-op"
# The exe the mod is made for: TimeShift patch 1.2 (the build the mod was developed on).
$script:EXE_SHA = "689C63DA36CD5902EDCAD2ABE1111D4697A92B12C4E17FE0659B856752641BE4"
# The same exe with the large-address-aware flag set (PE header Characteristics 0x20: a 32-bit game gets up to 4 GB of
# address space on 64-bit Windows instead of 2 GB; the mod's own memory and a long session add up). The installer makes
# it from the original (kept in coop-backup, put back on uninstall) and records this hash in coop\install.txt (exe=).
$script:EXE_SHA_LAA = "DE1A0AEB4A52350ABCF8D0717E9BD715BCF57713CD0A65B16B6269A9A24FB01C"
# A community-made "pre-patched" 1.2 exe (the "CPU fix" build): the 1.2 code with the CPU fix in code caves. It differs from the exe above in 162 bytes only
# (header fields, 13 call sites, the driver check's jump, the zero padding at the end of .text), so every byte check of the
# mod's hooks passes on it. Accepted with a notice.
$script:EXE_SHA_COMMUNITY = "49BCCA23D3D7872FBB578183B77472B64ECD8E3F8D68BFCCCB1AF6F04D4D8ED6"
# The exe inside the community-made 1.2 update package for the Steam edition: the same 1.2 code, 8 bytes of header (the last
# section's name) apart.
$script:EXE_SHA_STEAMPKG = "B337D72E2BAA5B049C128CDD3377A49B4BCA41917D3138D99CC40757B66FA9D0"
# Any other exe is accepted when its code is the 1.2 code: SHA-256 of the file with the bytes zeroed that those community
# builds and our own large-address-aware step change (checksum, characteristics, .text size, the last section's name, the
# call sites, the two start-up check jumps, .text's padding). A protected (SecuROM, SteamStub) or other-version exe cannot match.
# The known package's files (SHA-256 by path): recognised and said so; another package is accepted when its exe is the
# 1.2 code and the data build (the recipes check the archives) goes through.
$script:PKG_KNOWN = @{
    "bin\timeshift.exe" = "B337D72E2BAA5B049C128CDD3377A49B4BCA41917D3138D99CC40757B66FA9D0"
    "bin\steamwars.ini" = "8C93DCC1174A3CE6D16B38E051968C29904ED91BD3A6D255DFF3350F391EF9A5"
    "preload\paks\patch.s3darc" = "3E18E7B0F345B7BA2EB41B4BBAD6469D8A3AB0186BA1F0878B2AAE11D4D1B74F"
    "preload\paks\patch_ati.s3darc" = "5B59CED11547DA406E0769F4C395A4B137A946EBF464C25377CEE47C9453DF01"
    "preload\paks\patch_en_us.s3darc" = "6AF65BCDF294AAA394BC617BDEDB3819C56B5A8584653B49409F2C5FF0EF39F3"
    "preload\paks\patch_nv.s3darc" = "8F8199281FE7B075785E5AA0376EDBB9B4E9E423AC6369083BA66B9966E206EB"
}
$script:EXE_SIZE = 11431936
$script:EXE_NORM = "A08D90489E83387ADD4FAA40471B6FD46AEF7523756F3AB76F0A2E8B28AE22EB"
$script:EXE_MASK = @(@(0xcdd23, 6), @(0x2491a9, 6), @(0x249219, 6), @(0x24945c, 6), @(0x2499a3, 6), @(0x39b74e, 6), @(0x3e49f7, 6),
    @(0x3e4c63, 6), @(0x3e4cb7, 6), @(0x3e5010, 6), @(0x3e99c7, 6), @(0x3f25e6, 6), @(0x3f5dee, 6), @(0x3738e6, 1), @(0x3734b6, 1),
    @(0x91026b, (0x911000 - 0x91026b)))
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
        "asi_copy" = "Your ASI loader also runs as: "
        "asi_note" = "Your ASI loader (another mod's dinput8.dll) now also runs as bin\winmm.dll, so its plugins stay on."
        "off_note" = "Another mod's dinput8.dll is kept in coop-backup and is off while co-op is installed; removing co-op puts it back."
        "winmm_taken" = "bin\winmm.dll of another mod is there already: the ASI loader stays off."
        "left_alone" = "Not the copy co-op made any more, left in place: "
        "foreign_over_coop" = "Another mod's file was put over co-op's; kept as the original now: "
        "left_in_place" = "Another mod's file over co-op's, left in place: "
        "left_note" = "`n`nAnother mod's bin\dinput8.dll had been put over co-op's: it stays in place."
        "copied" = "Copied {0} file(s)."
        "large_address_aware" = "Large address aware flag set in bin\TimeShift.exe (the original is kept in coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe is already large address aware"
        "exe_wrong" = "This mod is made for TimeShift with patch 1.2 (bin\TimeShift.exe with SHA-256 689c63da...52641be4, or the community's pre-patched build of it). The exe in this folder is another version (SHA-256 {0}).`nInstall the official patch 1.2 for your copy of the game, then run this installer again. If the main menu of your game already shows 'Ver 1.02', please report this SHA-256 on the mod's GitHub page (Issues). Nothing was changed."
        "exe_steam" = "This is a Steam copy at patch 1.0 (exe SHA-256 {0}); the mod needs patch 1.2, which Steam never received.`nSupply the 1.2 update package for the Steam edition (community-made) with -SteamUpdate <zip or folder> and agree to the replacement with -AcceptSteamUpdate (the window asks by itself). Nothing was changed."
        "steam_need" = "Your Steam copy is patch 1.0; the mod needs patch 1.2. Installing will REPLACE {0} with the files of the 1.2 update package for the Steam edition (community-made) that YOU supply below (a zip or a folder). Your originals are backed up in coop-backup\steam10 and Uninstall puts them back exactly."
        "found_steam_old" = "TimeShift found (Steam edition), but it is version 1.0, not patch 1.2."
        "steam_pick" = "Update 1.2 package for Steam (zip or folder)"
        "steam_pick_dir" = "Select the folder the package was extracted to"
        "browse_zip" = "Zip..."
        "browse_dir" = "Folder..."
        "steam_pkg_none" = "No package yet: choose the zip or the extracted folder."
        "steam_pkg_ok" = "The package is checked: it brings the game to 1.2 ({0} files)."
        "steam_pkg_found" = "(found in your Downloads)"
        "steam_pkg_known" = "(the known community package: every file checked)"
        "steam_pkg_unknown" = "(not the known package: its exe is checked as the 1.2 code, the data build checks the archives)"
        "pkg_bad" = "The package cannot be used: {0}"
        "pkg_nothing" = "nothing is there."
        "pkg_read" = "it could not be read ({0})."
        "pkg_noexe" = "it has no bin\TimeShift.exe."
        "pkg_unsafe" = "it holds an unsafe path ({0})."
        "pkg_exe_other" = "its bin\TimeShift.exe is not the patch 1.2 build (SHA-256 {0})."
        "pkg_nopaks" = "it has no preload\paks\{0}."
        "pkg_skipped" = "Left out {0} file(s) of the package that are not game data (programs, settings, saves)."
        "steam_applying" = "Updating the Steam copy to 1.2 from {0}"
        "steam_done" = "The game is at 1.2 now ({0} files replaced or added; the originals are in coop-backup\steam10)."
        "steam_rollback" = "The update was undone: the Steam files are back as they were."
        "steam_restored" = "Steam files put back: {0}"
        "st_steam" = "Bringing the Steam copy to 1.2 (originals are kept)"
        "notice_community" = "The exe is the community's pre-patched 1.2 build (its CPU fix is in already). The mod's own fix does the same: nothing else changes."
        "notice_variant" = "The exe has the patch 1.2 code with small changes (a community build). It is accepted; if the game does not start, bin\ts_coop.log says which of the mod's fixes did not fit."
        "install_steam" = "Update and install"
        "write_denied" = "The game folder or one of its files is read-only or protected: {0}`nClose programs that use the game folder, remove the read-only flag, or run the installer as administrator."
        "paks_missing" = "preload\paks\{0} is missing: this is not a complete TimeShift 1.2 folder. Nothing was changed."
        "paks_kept" = "The mod's data built by the last install is current: kept."
        "paks_built" = "The mod's data was built from your game files."
        "build_failed" = "The mod's data could not be built from your game files (the log says why). Nothing was changed."
        "ini_other" = "bin\steamwars.ini named another archive ({0}): it is put back on uninstall."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "bin\steamwars.ini has no arcName line"
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
        "other_games" = "Other copies of the game were found: {0}`nUse Browse to choose another one."
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
        "steam_accept" = "I understand: TimeShift.exe and the data archives listed above are replaced by the package's files (originals are kept, Uninstall restores them)"
        "steam_need_accept" = "This Steam copy (patch 1.0) would get {0} REPLACED by the files of the package {1} (TimeShift.exe SHA-256 {2}). Originals are backed up in coop-backup\steam10 and Uninstall restores them exactly. Run again with -AcceptSteamUpdate to agree. Nothing was changed."
        "steam_pkg_exe" = "The package's TimeShift.exe: SHA-256 {0}"
        "removed_steam" = "Your Steam copy is back as it was (patch 1.0): TimeShift.exe, steamwars.ini and the data archives were put back exactly from coop-backup\steam10."
    }
    ru = @{
        "not_installed" = "Кооп-мод не установлен в`n{0}"
        "removed_files" = "Удалено файлов: {0}."
        "put_back" = "Возвращён: "
        "no_files" = "Рядом с установщиком нет папки «files»."
        "close_game" = "Сначала закройте игру."
        "incomplete" = "Загруженный мод неполный: ничего не изменено. Запустите игру снова, чтобы повторить."
        "another_install" = "Уже идёт другая установка мода: дождитесь её окончания."
        "no_admin" = "Windows не разрешила запись в папку игры без прав администратора."
        "installed_now" = "Сейчас установлен кооп-мод "
        "original_kept" = "Оригинал сохранён: "
        "asi_copy" = "Ваш загрузчик ASI работает и как: "
        "asi_note" = "Ваш загрузчик ASI (dinput8.dll другого мода) теперь работает и как bin\winmm.dll, поэтому его плагины остаются включены."
        "off_note" = "dinput8.dll другого мода сохранён в coop-backup и выключен, пока установлен кооп-мод; удаление кооп-мода вернёт его."
        "winmm_taken" = "bin\winmm.dll другого мода уже есть: загрузчик ASI остаётся выключенным."
        "left_alone" = "Уже не копия, сделанная кооп-модом, оставлен на месте: "
        "foreign_over_coop" = "Поверх файла кооп-мода положен файл другого мода; теперь он сохранён как оригинал: "
        "left_in_place" = "Файл другого мода поверх файла кооп-мода, оставлен на месте: "
        "left_note" = "`n`nПоверх файла кооп-мода был положен bin\dinput8.dll другого мода: он остаётся на месте."
        "copied" = "Скопировано файлов: {0}."
        "large_address_aware" = "В bin\TimeShift.exe включён флаг large address aware (оригинал сохранён в coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe уже large address aware"
        "exe_wrong" = "Мод сделан для TimeShift с патчем 1.2 (bin\TimeShift.exe с SHA-256 689c63da...52641be4 или готовая сборка сообщества на его основе). В этой папке другая версия exe (SHA-256 {0}).`nУстановите официальный патч 1.2 для своей копии игры и запустите установщик снова. Если в главном меню игры уже написано «Ver 1.02», сообщите этот SHA-256 на странице мода на GitHub (Issues). Ничего не изменено."
        "exe_steam" = "Это копия из Steam с патчем 1.0 (SHA-256 exe {0}); моду нужен патч 1.2, которого в Steam не было.`nУкажите пакет обновления 1.2 для издания Steam (сделан сообществом) через -SteamUpdate <zip или папка> и согласитесь с заменой через -AcceptSteamUpdate (окно спросит само). Ничего не изменено."
        "steam_need" = "У вашей копии из Steam патч 1.0, а моду нужен патч 1.2. Установка ЗАМЕНИТ {0} файлами пакета обновления 1.2 для издания Steam (сделан сообществом), который ВЫ укажете ниже (zip или папка). Оригиналы сохраняются в coop-backup\steam10, а удаление возвращает их в точности."
        "found_steam_old" = "TimeShift найдена (издание Steam), но это версия 1.0, а не патч 1.2."
        "steam_pick" = "Пакет обновления 1.2 для Steam (zip или папка)"
        "steam_pick_dir" = "Выберите папку, куда распакован пакет"
        "browse_zip" = "Zip..."
        "browse_dir" = "Папка..."
        "steam_pkg_none" = "Пакета пока нет: выберите zip или распакованную папку."
        "steam_pkg_ok" = "Пакет проверен: он приводит игру к 1.2 (файлов: {0})."
        "steam_pkg_found" = "(найден в «Загрузках»)"
        "steam_pkg_known" = "(известный пакет сообщества: все файлы проверены)"
        "steam_pkg_unknown" = "(неизвестный пакет: его exe проверен как код 1.2, архивы проверит сборка данных)"
        "pkg_bad" = "Пакет не подходит: {0}"
        "pkg_nothing" = "там ничего нет."
        "pkg_read" = "его не удалось прочитать ({0})."
        "pkg_noexe" = "в нём нет bin\TimeShift.exe."
        "pkg_unsafe" = "в нём небезопасный путь ({0})."
        "pkg_exe_other" = "его bin\TimeShift.exe — не сборка патча 1.2 (SHA-256 {0})."
        "pkg_nopaks" = "в нём нет preload\paks\{0}."
        "pkg_skipped" = "Пропущено файлов пакета, которые не являются данными игры (программы, настройки, сохранения): {0}."
        "steam_applying" = "Копия из Steam обновляется до 1.2 из {0}"
        "steam_done" = "Игра теперь версии 1.2 (файлов заменено или добавлено: {0}; оригиналы в coop-backup\steam10)."
        "steam_rollback" = "Обновление отменено: файлы Steam возвращены как были."
        "steam_restored" = "Возвращено файлов Steam: {0}"
        "st_steam" = "Приведение копии из Steam к 1.2 (оригиналы сохраняются)"
        "notice_community" = "Это готовая сборка сообщества с патчем 1.2 (исправление для процессоров уже внутри). Собственное исправление мода делает то же самое: больше ничего не меняется."
        "notice_variant" = "В exe код патча 1.2 с небольшими изменениями (сборка сообщества). Он принят; если игра не запустится, bin\ts_coop.log покажет, какие исправления мода не подошли."
        "install_steam" = "Обновить и установить"
        "write_denied" = "Папка игры или один из её файлов только для чтения или защищён: {0}`nЗакройте программы, использующие папку игры, снимите флаг «только чтение» или запустите установщик от имени администратора."
        "paks_missing" = "Нет preload\paks\{0}: это не полная папка TimeShift 1.2. Ничего не изменено."
        "paks_kept" = "Данные мода от прошлой установки актуальны: оставлены."
        "paks_built" = "Данные мода собраны из ваших файлов игры."
        "build_failed" = "Не удалось собрать данные мода из ваших файлов игры (причина в журнале). Ничего не изменено."
        "ini_other" = "В bin\steamwars.ini был указан другой архив ({0}): он вернётся при удалении."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "В bin\steamwars.ini нет строки arcName"
        "start_msg" = "Запустите игру: в главном меню появится пункт «Кооператив».`nУдаление: «Uninstall Co-op.bat» в папке игры (или повторный запуск этого установщика)."
        "the_shortcut" = "Ярлык «TimeShift Co-op» (рабочий стол, меню «Пуск») запускает игру, а перед этим ставит с GitHub новую версию мода, если она вышла."
        "not_a_folder" = "Это не папка TimeShift (нет bin\TimeShift.exe): {0}"
        "not_found" = "Игра не найдена: укажите папку через -Game."
        "removed_done" = "Кооп-мод удалён, заменённые файлы и настройки игры возвращены."
        "is_installed" = "TimeShift Co-op {0} установлен в {1}"
        "error" = "Ошибка: "
        "subtitle" = "Версия {0}  -  кооперативная кампания TimeShift, 2-4 игрока"
        "game_folder" = "Папка игры"
        "browse" = "Обзор..."
        "remove_co_op" = "Удалить кооп-мод"
        "cancel" = "Отмена"
        "install" = "Установить"
        "copy_log" = "Копировать журнал"
        "open_game_folder" = "Открыть папку игры"
        "close" = "Закрыть"
        "please_wait" = "Дождитесь окончания: если прервать работу на полпути, игра может остаться изменённой наполовину."
        "choose_folder" = "Игра не найдена автоматически: выберите её папку (ту, где есть папка bin)."
        "no_exe" = "В этой папке нет bin\TimeShift.exe."
        "found" = "TimeShift 1.2 найдена."
        "other_games" = "Найдены другие копии игры: {0}`nДругую можно выбрать кнопкой «Обзор»."
        "found_other" = "TimeShift найдена, но её exe не от патча 1.2: мод нельзя установить (подробнее в README-COOP-ru.txt)."
        "already" = "Кооп-мод {0} уже установлен: он будет установлен заново."
        "reinstall" = "Переустановить"
        "will_update" = "Установлен кооп-мод {0}: он будет обновлён до {1}."
        "update" = "Обновить"
        "what" = "Установщик соберёт данные мода из ваших файлов игры (несколько секунд), скопирует мод в папку игры и сохранит каждый заменяемый файл игры (coop-backup); «Uninstall Co-op.bat» там всё вернёт. Профили и сохранения не затрагиваются."
        "same_version" = "Версия мода должна быть одинаковой у всех игроков."
        "installing" = "Установка кооп-мода {0}..."
        "st_check" = "Проверка папки игры"
        "st_paks" = "Сборка данных мода из ваших файлов игры"
        "st_copy" = "Копирование мода (заменяемые файлы игры сохраняются)"
        "st_finish" = "Ярлыки и сведения об установке"
        "folder_log" = "Папка игры: "
        "updated" = "Обновлено с {0} до {1}"
        "installed_title" = "Кооп-мод {0} установлен"
        "install_failed" = "Установка не удалась"
        "log_in" = "Журнал: %TEMP%\timeshift-coop-install.log. Файлы игры не потеряны: заменённые лежат в coop-backup."
        "removing" = "Удаление кооп-мода..."
        "st_rm" = "Удаление файлов и данных мода"
        "st_restore" = "Возврат оригинальных файлов игры"
        "st_clean" = "Удаление резервных копий и папки мода"
        "removed_title" = "Кооп-мод удалён"
        "removed_text" = "Кооп-мод удалён, заменённые файлы и bin\steamwars.ini возвращены.`n`nПрофили и сохранения (UserConfigs) не тронуты."
        "remove_failed" = "Удаление не удалось"
        "select_folder" = "Выберите папку TimeShift (в ней есть папка bin)"
        "remove_q" = "Удалить кооп-мод из`n{0} ?"
        "remove_page" = "Удалить TimeShift Co-op {0} из этой папки?`n`nОригинальные файлы и настройки игры будут возвращены; профили и сохранения (UserConfigs) останутся."
        "removed" = "Удалён: "
        "shortcut" = "Ярлык: "
        "shortcut_desc" = "TimeShift с кооперативом: сначала ставит новую версию мода, если она вышла"
        "steam_accept" = "Я понимаю: TimeShift.exe и перечисленные выше архивы данных заменяются файлами пакета (оригиналы сохраняются, удаление вернёт их)"
        "steam_need_accept" = "В этой копии из Steam (патч 1.0) {0} будут ЗАМЕНЕНЫ файлами пакета {1} (SHA-256 TimeShift.exe: {2}). Оригиналы сохраняются в coop-backup\steam10, удаление возвращает их в точности. Чтобы согласиться, запустите снова с -AcceptSteamUpdate. Ничего не изменено."
        "steam_pkg_exe" = "TimeShift.exe пакета: SHA-256 {0}"
        "removed_steam" = "Ваша копия из Steam снова такая, как была (патч 1.0): TimeShift.exe, steamwars.ini и архивы данных возвращены точно из coop-backup\steam10."
    }
    de = @{
        "not_installed" = "Der Koop-Mod ist nicht installiert in`n{0}"
        "removed_files" = "{0} Datei(en) entfernt."
        "put_back" = "Wiederhergestellt: "
        "no_files" = "Neben dem Installationsprogramm fehlt der Ordner `„files`“."
        "close_game" = "Beenden Sie zuerst das Spiel."
        "incomplete" = "Der heruntergeladene Mod ist unvollständig: Nichts wurde geändert. Starten Sie das Spiel erneut, um es noch einmal zu versuchen."
        "another_install" = "Eine andere Installation des Mods läuft: Warten Sie, bis sie fertig ist."
        "no_admin" = "Windows hat das Schreiben in den Spielordner ohne Administratorrechte nicht erlaubt."
        "installed_now" = "Jetzt installiert: Koop-Mod "
        "original_kept" = "Original gesichert: "
        "asi_copy" = "Ihr ASI-Loader läuft zusätzlich als: "
        "asi_note" = "Ihr ASI-Loader (die dinput8.dll eines anderen Mods) läuft jetzt zusätzlich als bin\winmm.dll, seine Plugins bleiben also aktiv."
        "off_note" = "Die dinput8.dll eines anderen Mods ist in coop-backup gesichert und aus, solange der Koop-Mod installiert ist; das Entfernen des Koop-Mods stellt sie wieder her."
        "winmm_taken" = "Eine bin\winmm.dll eines anderen Mods ist schon vorhanden: Der ASI-Loader bleibt aus."
        "left_alone" = "Nicht mehr die Kopie des Koop-Mods, bleibt an ihrem Platz: "
        "foreign_over_coop" = "Eine Datei eines anderen Mods wurde über die des Koop-Mods gelegt; sie gilt jetzt als Original: "
        "left_in_place" = "Datei eines anderen Mods über der des Koop-Mods, bleibt an ihrem Platz: "
        "left_note" = "`n`nÜber die Datei des Koop-Mods war die bin\dinput8.dll eines anderen Mods gelegt worden: Sie bleibt an ihrem Platz."
        "copied" = "{0} Datei(en) kopiert."
        "large_address_aware" = "Das Flag Large Address Aware wurde in bin\TimeShift.exe gesetzt (das Original liegt in coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe ist bereits Large Address Aware"
        "exe_wrong" = "Dieser Mod ist für TimeShift mit Patch 1.2 gemacht (bin\TimeShift.exe mit SHA-256 689c63da...52641be4 oder die vorgepatchte Version der Community). Die exe in diesem Ordner ist eine andere Version (SHA-256 {0}).`nInstallieren Sie den offiziellen Patch 1.2 für Ihr Spiel und starten Sie dieses Installationsprogramm erneut. Zeigt das Hauptmenü Ihres Spiels schon `„Ver 1.02`“, melden Sie diesen SHA-256 bitte auf der GitHub-Seite des Mods (Issues). Nichts wurde geändert."
        "exe_steam" = "Das ist eine Steam-Kopie mit Patch 1.0 (exe SHA-256 {0}); der Mod braucht Patch 1.2, den Steam nie bekommen hat.`nGeben Sie das 1.2-Update-Paket für die Steam-Ausgabe (von der Community gemacht) mit -SteamUpdate <Zip oder Ordner> an und stimmen Sie dem Ersetzen mit -AcceptSteamUpdate zu (das Fenster fragt selbst). Nichts wurde geändert."
        "steam_need" = "Ihre Steam-Kopie hat Patch 1.0; der Mod braucht Patch 1.2. Die Installation ERSETZT {0} durch die Dateien des 1.2-Update-Pakets für die Steam-Ausgabe (von der Community gemacht), das SIE unten angeben (Zip oder Ordner). Die Originale werden in coop-backup\steam10 gesichert, und die Deinstallation stellt sie genau wieder her."
        "found_steam_old" = "TimeShift gefunden (Steam-Ausgabe), aber in Version 1.0, nicht Patch 1.2."
        "steam_pick" = "Update-1.2-Paket für Steam (Zip oder Ordner)"
        "steam_pick_dir" = "Wählen Sie den Ordner, in den das Paket entpackt wurde"
        "browse_zip" = "Zip..."
        "browse_dir" = "Ordner..."
        "steam_pkg_none" = "Noch kein Paket: Wählen Sie die Zip-Datei oder den entpackten Ordner."
        "steam_pkg_ok" = "Das Paket ist geprüft: Es bringt das Spiel auf 1.2 ({0} Dateien)."
        "steam_pkg_found" = "(in Ihren Downloads gefunden)"
        "steam_pkg_known" = "(das bekannte Community-Paket: alle Dateien geprüft)"
        "steam_pkg_unknown" = "(nicht das bekannte Paket: seine exe wird als 1.2-Code geprüft, die Archive prüft der Datenbau)"
        "pkg_bad" = "Das Paket ist nicht verwendbar: {0}"
        "pkg_nothing" = "dort ist nichts."
        "pkg_read" = "es konnte nicht gelesen werden ({0})."
        "pkg_noexe" = "es enthält keine bin\TimeShift.exe."
        "pkg_unsafe" = "es enthält einen unsicheren Pfad ({0})."
        "pkg_exe_other" = "seine bin\TimeShift.exe ist nicht der Build von Patch 1.2 (SHA-256 {0})."
        "pkg_nopaks" = "es enthält kein preload\paks\{0}."
        "pkg_skipped" = "{0} Datei(en) des Pakets ausgelassen, die keine Spieldaten sind (Programme, Einstellungen, Spielstände)."
        "steam_applying" = "Die Steam-Kopie wird aus {0} auf 1.2 gebracht"
        "steam_done" = "Das Spiel ist jetzt auf 1.2 ({0} Dateien ersetzt oder hinzugefügt; die Originale liegen in coop-backup\steam10)."
        "steam_rollback" = "Das Update wurde rückgängig gemacht: Die Steam-Dateien sind wieder wie vorher."
        "steam_restored" = "Steam-Dateien wiederhergestellt: {0}"
        "st_steam" = "Steam-Kopie auf 1.2 bringen (Originale bleiben erhalten)"
        "notice_community" = "Die exe ist der vorgepatchte 1.2-Build der Community (sein CPU-Fix ist schon drin). Der Fix des Mods tut dasselbe: Sonst ändert sich nichts."
        "notice_variant" = "Die exe enthält den Code von Patch 1.2 mit kleinen Änderungen (ein Community-Build). Sie wird akzeptiert; startet das Spiel nicht, nennt bin\ts_coop.log, welche Fixes des Mods nicht passten."
        "install_steam" = "Aktualisieren und installieren"
        "write_denied" = "Der Spielordner oder eine seiner Dateien ist schreibgeschützt oder geschützt: {0}`nSchließen Sie Programme, die den Spielordner benutzen, entfernen Sie den Schreibschutz oder starten Sie das Installationsprogramm als Administrator."
        "paks_missing" = "preload\paks\{0} fehlt: Das ist kein vollständiger TimeShift-1.2-Ordner. Nichts wurde geändert."
        "paks_kept" = "Die bei der letzten Installation gebauten Moddaten sind aktuell: behalten."
        "paks_built" = "Die Moddaten wurden aus Ihren Spieldateien gebaut."
        "build_failed" = "Die Moddaten konnten nicht aus Ihren Spieldateien gebaut werden (das Protokoll nennt den Grund). Nichts wurde geändert."
        "ini_other" = "bin\steamwars.ini nannte ein anderes Archiv ({0}): Es wird bei der Deinstallation zurückgesetzt."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "bin\steamwars.ini hat keine Zeile arcName"
        "start_msg" = "Starten Sie das Spiel: Das Hauptmenü hat einen Eintrag `„Kooperativ`“.`nZum Entfernen: `„Uninstall Co-op.bat`“ im Spielordner (oder dieses Installationsprogramm erneut starten)."
        "the_shortcut" = "Die Verknüpfung `„TimeShift Co-op`“ (Desktop, Startmenü) startet das Spiel und installiert vorher eine neuere Version des Mods von GitHub, falls es eine gibt."
        "not_a_folder" = "Kein TimeShift-Ordner (keine bin\TimeShift.exe): {0}"
        "not_found" = "Das Spiel wurde nicht gefunden: Geben Sie den Ordner mit -Game an."
        "removed_done" = "Der Koop-Mod wurde entfernt; die ersetzten Dateien und die Einstellungen des Spiels wurden wiederhergestellt."
        "is_installed" = "TimeShift Co-op {0} ist installiert in {1}"
        "error" = "Fehler: "
        "subtitle" = "Version {0}  -  Koop-Kampagne für TimeShift, 2-4 Spieler"
        "game_folder" = "Spielordner"
        "browse" = "Durchsuchen..."
        "remove_co_op" = "Koop-Mod entfernen"
        "cancel" = "Abbrechen"
        "install" = "Installieren"
        "copy_log" = "Protokoll kopieren"
        "open_game_folder" = "Spielordner öffnen"
        "close" = "Schließen"
        "please_wait" = "Bitte warten Sie, bis es fertig ist: Ein Abbruch mittendrin könnte das Spiel halb verändert zurücklassen."
        "choose_folder" = "Das Spiel wurde nicht automatisch gefunden: Wählen Sie seinen Ordner (den mit dem Ordner bin)."
        "no_exe" = "In diesem Ordner gibt es keine bin\TimeShift.exe."
        "found" = "TimeShift 1.2 gefunden."
        "other_games" = "Weitere Kopien des Spiels gefunden: {0}`nWählen Sie mit `„Durchsuchen`“ eine andere."
        "found_other" = "TimeShift gefunden, aber die exe ist nicht Patch 1.2: Der Mod kann nicht installiert werden (mehr in README-COOP.txt)."
        "already" = "Koop-Mod {0} ist bereits installiert: Er wird neu installiert."
        "reinstall" = "Neu installieren"
        "will_update" = "Koop-Mod {0} ist installiert: Er wird auf {1} aktualisiert."
        "update" = "Aktualisieren"
        "what" = "Das Installationsprogramm baut die Moddaten aus Ihren eigenen Spieldateien (ein paar Sekunden), kopiert den Mod in den Spielordner und behält jede ersetzte Spieldatei (coop-backup); `„Uninstall Co-op.bat`“ dort stellt alles wieder her. Profile und Spielstände bleiben unberührt."
        "same_version" = "Alle Spieler brauchen dieselbe Version des Mods."
        "installing" = "Koop-Mod {0} wird installiert..."
        "st_check" = "Spielordner prüfen"
        "st_paks" = "Moddaten aus Ihren Spieldateien bauen"
        "st_copy" = "Mod kopieren (ersetzte Spieldateien bleiben erhalten)"
        "st_finish" = "Verknüpfungen und Installationsprotokoll"
        "folder_log" = "Spielordner: "
        "updated" = "Aktualisiert von {0} auf {1}"
        "installed_title" = "Koop-Mod {0} ist installiert"
        "install_failed" = "Installation fehlgeschlagen"
        "log_in" = "Das Protokoll liegt in %TEMP%\timeshift-coop-install.log. Vom Spiel geht nichts verloren: Ersetzte Dateien liegen in coop-backup."
        "removing" = "Koop-Mod wird entfernt..."
        "st_rm" = "Dateien und Daten des Mods entfernen"
        "st_restore" = "Originaldateien des Spiels wiederherstellen"
        "st_clean" = "Sicherungen und Ordner des Mods entfernen"
        "removed_title" = "Koop-Mod entfernt"
        "removed_text" = "Der Koop-Mod wurde entfernt; die ersetzten Dateien und bin\steamwars.ini wurden wiederhergestellt.`n`nProfile und Spielstände (UserConfigs) wurden nicht berührt."
        "remove_failed" = "Entfernen fehlgeschlagen"
        "select_folder" = "Wählen Sie den TimeShift-Ordner (den mit dem Ordner bin)"
        "remove_q" = "Den Koop-Mod entfernen aus`n{0} ?"
        "remove_page" = "TimeShift Co-op {0} aus diesem Ordner entfernen?`n`nOriginaldateien und Einstellungen des Spiels werden wiederhergestellt; Profile und Spielstände (UserConfigs) bleiben."
        "removed" = "Entfernt: "
        "shortcut" = "Verknüpfung: "
        "shortcut_desc" = "TimeShift mit Koop: installiert vorher eine neuere Version des Mods, falls es eine gibt"
        "steam_accept" = "Ich verstehe: TimeShift.exe und die oben genannten Datenarchive werden durch die Dateien des Pakets ersetzt (Originale bleiben erhalten, die Deinstallation stellt sie wieder her)"
        "steam_need_accept" = "In dieser Steam-Kopie (Patch 1.0) würde {0} durch die Dateien des Pakets {1} ERSETZT (SHA-256 der TimeShift.exe: {2}). Die Originale werden in coop-backup\steam10 gesichert, die Deinstallation stellt sie genau wieder her. Zum Zustimmen mit -AcceptSteamUpdate erneut starten. Nichts wurde geändert."
        "steam_pkg_exe" = "TimeShift.exe des Pakets: SHA-256 {0}"
        "removed_steam" = "Ihre Steam-Kopie ist wieder wie vorher (Patch 1.0): TimeShift.exe, steamwars.ini und die Datenarchive wurden genau aus coop-backup\steam10 wiederhergestellt."
    }
    fr = @{
        "not_installed" = "Le mod coop n'est pas installé dans`n{0}"
        "removed_files" = "{0} fichier(s) supprimé(s)."
        "put_back" = "Remis en place : "
        "no_files" = "Le dossier « files » manque à côté de l'installateur."
        "close_game" = "Fermez d'abord le jeu."
        "incomplete" = "Le mod téléchargé est incomplet : rien n'a été modifié. Relancez le jeu pour réessayer."
        "another_install" = "Une autre installation du mod est en cours : attendez qu'elle se termine."
        "no_admin" = "Windows n'a pas autorisé l'écriture dans le dossier du jeu sans droits d'administrateur."
        "installed_now" = "Installé actuellement : mod coop "
        "original_kept" = "Original conservé : "
        "asi_copy" = "Votre chargeur ASI tourne aussi sous le nom : "
        "asi_note" = "Votre chargeur ASI (le dinput8.dll d'un autre mod) tourne désormais aussi sous le nom bin\winmm.dll : ses plugins restent actifs."
        "off_note" = "Le dinput8.dll d'un autre mod est conservé dans coop-backup et désactivé tant que le mod coop est installé ; retirer le mod coop le remet en place."
        "winmm_taken" = "Un bin\winmm.dll d'un autre mod est déjà là : le chargeur ASI reste désactivé."
        "left_alone" = "N'est plus la copie faite par le mod coop, laissé en place : "
        "foreign_over_coop" = "Un fichier d'un autre mod a remplacé celui du mod coop ; il est désormais conservé comme original : "
        "left_in_place" = "Fichier d'un autre mod à la place de celui du mod coop, laissé en place : "
        "left_note" = "`n`nLe bin\dinput8.dll d'un autre mod avait remplacé celui du mod coop : il reste en place."
        "copied" = "{0} fichier(s) copié(s)."
        "large_address_aware" = "Indicateur Large Address Aware activé dans bin\TimeShift.exe (l'original est conservé dans coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe est déjà Large Address Aware"
        "exe_wrong" = "Ce mod est fait pour TimeShift avec le patch 1.2 (bin\TimeShift.exe de SHA-256 689c63da...52641be4, ou la version pré-patchée de la communauté). L'exe de ce dossier est une autre version (SHA-256 {0}).`nInstallez le patch officiel 1.2 pour votre jeu, puis relancez cet installateur. Si le menu principal de votre jeu affiche déjà « Ver 1.02 », signalez ce SHA-256 sur la page GitHub du mod (Issues). Rien n'a été modifié."
        "exe_steam" = "C'est une copie Steam au patch 1.0 (exe SHA-256 {0}) ; le mod exige le patch 1.2, que Steam n'a jamais reçu.`nIndiquez le paquet de mise à jour 1.2 pour l'édition Steam (fait par la communauté) avec -SteamUpdate <zip ou dossier> et acceptez le remplacement avec -AcceptSteamUpdate (la fenêtre le demande elle-même). Rien n'a été modifié."
        "steam_need" = "Votre copie Steam est au patch 1.0 ; le mod exige le patch 1.2. L'installation REMPLACERA {0} par les fichiers du paquet de mise à jour 1.2 pour l'édition Steam (fait par la communauté) que VOUS indiquez ci-dessous (zip ou dossier). Vos originaux sont sauvegardés dans coop-backup\steam10 et la désinstallation les remet exactement en place."
        "found_steam_old" = "TimeShift trouvé (édition Steam), mais en version 1.0, pas en patch 1.2."
        "steam_pick" = "Paquet de mise à jour 1.2 pour Steam (zip ou dossier)"
        "steam_pick_dir" = "Choisissez le dossier où le paquet a été extrait"
        "browse_zip" = "Zip..."
        "browse_dir" = "Dossier..."
        "steam_pkg_none" = "Pas encore de paquet : choisissez le zip ou le dossier extrait."
        "steam_pkg_ok" = "Le paquet est vérifié : il amène le jeu en 1.2 ({0} fichiers)."
        "steam_pkg_found" = "(trouvé dans vos Téléchargements)"
        "steam_pkg_known" = "(le paquet connu de la communauté : tous les fichiers vérifiés)"
        "steam_pkg_unknown" = "(pas le paquet connu : son exe est vérifié comme code 1.2, la construction des données vérifie les archives)"
        "pkg_bad" = "Le paquet est inutilisable : {0}"
        "pkg_nothing" = "il n'y a rien là."
        "pkg_read" = "il n'a pas pu être lu ({0})."
        "pkg_noexe" = "il ne contient pas bin\TimeShift.exe."
        "pkg_unsafe" = "il contient un chemin dangereux ({0})."
        "pkg_exe_other" = "son bin\TimeShift.exe n'est pas la version du patch 1.2 (SHA-256 {0})."
        "pkg_nopaks" = "il ne contient pas preload\paks\{0}."
        "pkg_skipped" = "{0} fichier(s) du paquet ignoré(s) car ce ne sont pas des données du jeu (programmes, réglages, sauvegardes)."
        "steam_applying" = "Mise à jour de la copie Steam en 1.2 depuis {0}"
        "steam_done" = "Le jeu est maintenant en 1.2 ({0} fichiers remplacés ou ajoutés ; les originaux sont dans coop-backup\steam10)."
        "steam_rollback" = "La mise à jour a été annulée : les fichiers Steam sont revenus comme avant."
        "steam_restored" = "Fichiers Steam remis en place : {0}"
        "st_steam" = "Passage de la copie Steam en 1.2 (les originaux sont conservés)"
        "notice_community" = "L'exe est la version 1.2 pré-patchée de la communauté (son correctif CPU est déjà dedans). Le correctif du mod fait la même chose : rien d'autre ne change."
        "notice_variant" = "L'exe contient le code du patch 1.2 avec de petites modifications (une version de la communauté). Il est accepté ; si le jeu ne démarre pas, bin\ts_coop.log indique quels correctifs du mod ne conviennent pas."
        "install_steam" = "Mettre à jour et installer"
        "write_denied" = "Le dossier du jeu ou l'un de ses fichiers est en lecture seule ou protégé : {0}`nFermez les programmes qui utilisent le dossier du jeu, retirez l'attribut lecture seule ou lancez l'installateur en administrateur."
        "paks_missing" = "preload\paks\{0} est absent : ce n'est pas un dossier TimeShift 1.2 complet. Rien n'a été modifié."
        "paks_kept" = "Les données du mod construites à la dernière installation sont à jour : conservées."
        "paks_built" = "Les données du mod ont été construites à partir de vos fichiers de jeu."
        "build_failed" = "Les données du mod n'ont pas pu être construites à partir de vos fichiers de jeu (le journal en donne la raison). Rien n'a été modifié."
        "ini_other" = "bin\steamwars.ini nommait une autre archive ({0}) : elle est remise à la désinstallation."
        "ini_set" = "bin\steamwars.ini : arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini : arcName = {0}"
        "ini_noarc" = "bin\steamwars.ini n'a pas de ligne arcName"
        "start_msg" = "Lancez le jeu : le menu principal a une entrée « Coopération ».`nPour désinstaller : « Uninstall Co-op.bat » dans le dossier du jeu (ou relancez cet installateur)."
        "the_shortcut" = "Le raccourci « TimeShift Co-op » (bureau, menu Démarrer) lance le jeu et installe d'abord depuis GitHub une version plus récente du mod, s'il y en a une."
        "not_a_folder" = "Pas un dossier TimeShift (pas de bin\TimeShift.exe) : {0}"
        "not_found" = "Le jeu est introuvable : indiquez son dossier avec -Game."
        "removed_done" = "Le mod coop a été supprimé ; les fichiers remplacés et les réglages du jeu ont été remis en place."
        "is_installed" = "TimeShift Co-op {0} est installé dans {1}"
        "error" = "Erreur : "
        "subtitle" = "Version {0}  -  campagne coopérative pour TimeShift, 2 à 4 joueurs"
        "game_folder" = "Dossier du jeu"
        "browse" = "Parcourir..."
        "remove_co_op" = "Supprimer le mod coop"
        "cancel" = "Annuler"
        "install" = "Installer"
        "copy_log" = "Copier le journal"
        "open_game_folder" = "Ouvrir le dossier du jeu"
        "close" = "Fermer"
        "please_wait" = "Patientez jusqu'à la fin : s'arrêter en plein milieu pourrait laisser le jeu à moitié modifié."
        "choose_folder" = "Le jeu n'a pas été trouvé automatiquement : choisissez son dossier (celui qui contient le dossier bin)."
        "no_exe" = "Il n'y a pas de bin\TimeShift.exe dans ce dossier."
        "found" = "TimeShift 1.2 trouvé."
        "other_games" = "D'autres copies du jeu ont été trouvées : {0}`nUtilisez « Parcourir » pour en choisir une autre."
        "found_other" = "TimeShift trouvé, mais son exe n'est pas le patch 1.2 : le mod ne peut pas être installé (plus de détails dans README-COOP.txt)."
        "already" = "Le mod coop {0} est déjà installé : il sera réinstallé."
        "reinstall" = "Réinstaller"
        "will_update" = "Le mod coop {0} est installé : il sera mis à jour vers la version {1}."
        "update" = "Mettre à jour"
        "what" = "L'installateur construit les données du mod à partir de vos propres fichiers de jeu (quelques secondes), copie le mod dans le dossier du jeu et conserve chaque fichier de jeu remplacé (coop-backup) ; « Uninstall Co-op.bat » dans ce dossier remet tout en place. Les profils et sauvegardes ne sont pas touchés."
        "same_version" = "Tous les joueurs ont besoin de la même version du mod."
        "installing" = "Installation du mod coop {0}..."
        "st_check" = "Vérification du dossier du jeu"
        "st_paks" = "Construction des données du mod à partir de vos fichiers de jeu"
        "st_copy" = "Copie du mod (les fichiers de jeu remplacés sont conservés)"
        "st_finish" = "Raccourcis et fiche d'installation"
        "folder_log" = "Dossier du jeu : "
        "updated" = "Mis à jour de {0} à {1}"
        "installed_title" = "Le mod coop {0} est installé"
        "install_failed" = "L'installation a échoué"
        "log_in" = "Le journal est dans %TEMP%\timeshift-coop-install.log. Rien du jeu n'est perdu : les fichiers remplacés sont dans coop-backup."
        "removing" = "Suppression du mod coop..."
        "st_rm" = "Suppression des fichiers et des données du mod"
        "st_restore" = "Remise en place des fichiers d'origine du jeu"
        "st_clean" = "Suppression des sauvegardes et du dossier du mod"
        "removed_title" = "Mod coop supprimé"
        "removed_text" = "Le mod coop a été supprimé ; les fichiers remplacés et bin\steamwars.ini ont été remis en place.`n`nLes profils et sauvegardes (UserConfigs) n'ont pas été touchés."
        "remove_failed" = "La suppression a échoué"
        "select_folder" = "Choisissez le dossier TimeShift (celui qui contient le dossier bin)"
        "remove_q" = "Supprimer le mod coop de`n{0} ?"
        "remove_page" = "Supprimer TimeShift Co-op {0} de ce dossier ?`n`nLes fichiers d'origine et les réglages du jeu sont remis en place ; les profils et sauvegardes (UserConfigs) restent."
        "removed" = "Supprimé : "
        "shortcut" = "Raccourci : "
        "shortcut_desc" = "TimeShift en coop : installe d'abord une version plus récente du mod, s'il y en a une"
        "steam_accept" = "Je comprends : TimeShift.exe et les archives de données listées ci-dessus sont remplacés par les fichiers du paquet (originaux conservés, la désinstallation les remet)"
        "steam_need_accept" = "Dans cette copie Steam (patch 1.0), {0} seraient REMPLACÉS par les fichiers du paquet {1} (SHA-256 de TimeShift.exe : {2}). Les originaux sont sauvegardés dans coop-backup\steam10 et la désinstallation les remet exactement en place. Pour accepter, relancez avec -AcceptSteamUpdate. Rien n'a été modifié."
        "steam_pkg_exe" = "TimeShift.exe du paquet : SHA-256 {0}"
        "removed_steam" = "Votre copie Steam est revenue comme avant (patch 1.0) : TimeShift.exe, steamwars.ini et les archives de données ont été remis exactement depuis coop-backup\steam10."
    }
    es = @{
        "not_installed" = "El mod cooperativo no está instalado en`n{0}"
        "removed_files" = "{0} archivo(s) eliminado(s)."
        "put_back" = "Devuelto: "
        "no_files" = "Falta la carpeta «files» junto al instalador."
        "close_game" = "Cierra primero el juego."
        "incomplete" = "El mod descargado está incompleto: no se cambió nada. Inicia el juego otra vez para reintentarlo."
        "another_install" = "Hay otra instalación del mod en marcha: espera a que termine."
        "no_admin" = "Windows no permitió escribir en la carpeta del juego sin permisos de administrador."
        "installed_now" = "Instalado ahora: mod cooperativo "
        "original_kept" = "Original conservado: "
        "asi_copy" = "Tu cargador ASI también funciona como: "
        "asi_note" = "Tu cargador ASI (el dinput8.dll de otro mod) ahora también funciona como bin\winmm.dll, así que sus plugins siguen activos."
        "off_note" = "El dinput8.dll de otro mod se guarda en coop-backup y queda desactivado mientras el mod cooperativo esté instalado; al quitar el mod cooperativo, se devuelve."
        "winmm_taken" = "Ya hay un bin\winmm.dll de otro mod: el cargador ASI queda desactivado."
        "left_alone" = "Ya no es la copia que hizo el mod cooperativo, se deja en su sitio: "
        "foreign_over_coop" = "Un archivo de otro mod sustituyó al del mod cooperativo; ahora se guarda como original: "
        "left_in_place" = "Archivo de otro mod en lugar del del mod cooperativo, se deja en su sitio: "
        "left_note" = "`n`nUn bin\dinput8.dll de otro mod había sustituido al del mod cooperativo: se queda en su sitio."
        "copied" = "{0} archivo(s) copiado(s)."
        "large_address_aware" = "Se activó el indicador Large Address Aware en bin\TimeShift.exe (el original se conserva en coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe ya es Large Address Aware"
        "exe_wrong" = "Este mod está hecho para TimeShift con el parche 1.2 (bin\TimeShift.exe con SHA-256 689c63da...52641be4, o la versión ya parcheada de la comunidad). El exe de esta carpeta es otra versión (SHA-256 {0}).`nInstala el parche oficial 1.2 para tu juego y vuelve a ejecutar este instalador. Si el menú principal de tu juego ya muestra «Ver 1.02», informa de este SHA-256 en la página de GitHub del mod (Issues). No se cambió nada."
        "exe_steam" = "Es una copia de Steam con el parche 1.0 (exe SHA-256 {0}); el mod necesita el parche 1.2, que Steam nunca recibió.`nIndica el paquete de actualización 1.2 para la edición de Steam (hecho por la comunidad) con -SteamUpdate <zip o carpeta> y acepta el reemplazo con -AcceptSteamUpdate (la ventana lo pregunta sola). No se cambió nada."
        "steam_need" = "Tu copia de Steam es el parche 1.0; el mod necesita el parche 1.2. La instalación REEMPLAZARÁ {0} por los archivos del paquete de actualización 1.2 para la edición de Steam (hecho por la comunidad) que TÚ indiques abajo (zip o carpeta). Los originales se guardan en coop-backup\steam10 y la desinstalación los devuelve exactamente."
        "found_steam_old" = "TimeShift encontrado (edición Steam), pero es la versión 1.0, no el parche 1.2."
        "steam_pick" = "Paquete de actualización 1.2 para Steam (zip o carpeta)"
        "steam_pick_dir" = "Elige la carpeta donde se extrajo el paquete"
        "browse_zip" = "Zip..."
        "browse_dir" = "Carpeta..."
        "steam_pkg_none" = "Aún no hay paquete: elige el zip o la carpeta extraída."
        "steam_pkg_ok" = "El paquete está comprobado: lleva el juego a 1.2 ({0} archivos)."
        "steam_pkg_found" = "(encontrado en tus Descargas)"
        "steam_pkg_known" = "(el paquete conocido de la comunidad: todos los archivos comprobados)"
        "steam_pkg_unknown" = "(no es el paquete conocido: su exe se comprueba como código 1.2, la construcción de datos comprueba los archivos)"
        "pkg_bad" = "El paquete no sirve: {0}"
        "pkg_nothing" = "no hay nada ahí."
        "pkg_read" = "no se pudo leer ({0})."
        "pkg_noexe" = "no contiene bin\TimeShift.exe."
        "pkg_unsafe" = "contiene una ruta insegura ({0})."
        "pkg_exe_other" = "su bin\TimeShift.exe no es la versión del parche 1.2 (SHA-256 {0})."
        "pkg_nopaks" = "no contiene preload\paks\{0}."
        "pkg_skipped" = "Se omitieron {0} archivo(s) del paquete que no son datos del juego (programas, ajustes, partidas guardadas)."
        "steam_applying" = "Actualizando la copia de Steam a 1.2 desde {0}"
        "steam_done" = "El juego está ahora en 1.2 ({0} archivos reemplazados o añadidos; los originales están en coop-backup\steam10)."
        "steam_rollback" = "Se deshizo la actualización: los archivos de Steam han vuelto a como estaban."
        "steam_restored" = "Archivos de Steam devueltos: {0}"
        "st_steam" = "Llevando la copia de Steam a 1.2 (se conservan los originales)"
        "notice_community" = "El exe es la versión 1.2 ya parcheada de la comunidad (su arreglo de CPU ya está dentro). El arreglo propio del mod hace lo mismo: no cambia nada más."
        "notice_variant" = "El exe tiene el código del parche 1.2 con pequeños cambios (una versión de la comunidad). Se acepta; si el juego no arranca, bin\ts_coop.log dice qué arreglos del mod no encajaron."
        "install_steam" = "Actualizar e instalar"
        "write_denied" = "La carpeta del juego o uno de sus archivos es de solo lectura o está protegido: {0}`nCierra los programas que usen la carpeta del juego, quita el atributo de solo lectura o ejecuta el instalador como administrador."
        "paks_missing" = "Falta preload\paks\{0}: no es una carpeta completa de TimeShift 1.2. No se cambió nada."
        "paks_kept" = "Los datos del mod de la última instalación están al día: se conservan."
        "paks_built" = "Los datos del mod se construyeron a partir de tus archivos del juego."
        "build_failed" = "No se pudieron construir los datos del mod a partir de tus archivos del juego (el registro dice por qué). No se cambió nada."
        "ini_other" = "bin\steamwars.ini nombraba otro archivo ({0}): se devuelve al desinstalar."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "bin\steamwars.ini no tiene la línea arcName"
        "start_msg" = "Inicia el juego: el menú principal tiene una entrada «Cooperativo».`nPara quitarlo: «Uninstall Co-op.bat» en la carpeta del juego (o vuelve a ejecutar este instalador)."
        "the_shortcut" = "El acceso directo «TimeShift Co-op» (escritorio, menú Inicio) inicia el juego e instala antes desde GitHub una versión más nueva del mod, si la hay."
        "not_a_folder" = "No es una carpeta de TimeShift (no hay bin\TimeShift.exe): {0}"
        "not_found" = "No se encontró el juego: indica su carpeta con -Game."
        "removed_done" = "El mod cooperativo se eliminó; los archivos reemplazados y los ajustes del juego se devolvieron."
        "is_installed" = "TimeShift Co-op {0} está instalado en {1}"
        "error" = "Error: "
        "subtitle" = "Versión {0}  -  campaña cooperativa para TimeShift, 2-4 jugadores"
        "game_folder" = "Carpeta del juego"
        "browse" = "Examinar..."
        "remove_co_op" = "Desinstalar"
        "cancel" = "Cancelar"
        "install" = "Instalar"
        "copy_log" = "Copiar el registro"
        "open_game_folder" = "Abrir la carpeta del juego"
        "close" = "Cerrar"
        "please_wait" = "Espera a que termine: parar a medias podría dejar el juego cambiado a medias."
        "choose_folder" = "El juego no se encontró automáticamente: elige su carpeta (la que contiene la carpeta bin)."
        "no_exe" = "No hay bin\TimeShift.exe en esta carpeta."
        "found" = "TimeShift 1.2 encontrado."
        "other_games" = "Se encontraron otras copias del juego: {0}`nUsa Examinar para elegir otra."
        "found_other" = "TimeShift encontrado, pero su exe no es el parche 1.2: el mod no se puede instalar (más en README-COOP.txt)."
        "already" = "El mod cooperativo {0} ya está instalado: se instalará de nuevo."
        "reinstall" = "Reinstalar"
        "will_update" = "El mod cooperativo {0} está instalado: se actualizará a {1}."
        "update" = "Actualizar"
        "what" = "El instalador construye los datos del mod a partir de tus propios archivos del juego (unos segundos), copia el mod en la carpeta del juego y conserva cada archivo del juego que reemplaza (coop-backup); «Uninstall Co-op.bat» allí lo devuelve todo. Los perfiles y las partidas guardadas no se tocan."
        "same_version" = "Todos los jugadores necesitan la misma versión del mod."
        "installing" = "Instalando el mod cooperativo {0}..."
        "st_check" = "Comprobando la carpeta del juego"
        "st_paks" = "Construyendo los datos del mod a partir de tus archivos del juego"
        "st_copy" = "Copiando el mod (se conservan los archivos del juego reemplazados)"
        "st_finish" = "Accesos directos y registro de la instalación"
        "folder_log" = "Carpeta del juego: "
        "updated" = "Actualizado de {0} a {1}"
        "installed_title" = "El mod cooperativo {0} está instalado"
        "install_failed" = "La instalación falló"
        "log_in" = "El registro está en %TEMP%\timeshift-coop-install.log. No se pierde nada del juego: los archivos reemplazados están en coop-backup."
        "removing" = "Quitando el mod cooperativo..."
        "st_rm" = "Eliminando los archivos y datos del mod"
        "st_restore" = "Devolviendo los archivos originales del juego"
        "st_clean" = "Eliminando las copias y la carpeta del mod"
        "removed_title" = "Mod cooperativo eliminado"
        "removed_text" = "El mod cooperativo se eliminó; los archivos reemplazados y bin\steamwars.ini se devolvieron.`n`nLos perfiles y las partidas guardadas (UserConfigs) no se tocaron."
        "remove_failed" = "No se pudo eliminar"
        "select_folder" = "Elige la carpeta de TimeShift (la que contiene la carpeta bin)"
        "remove_q" = "¿Quitar el mod cooperativo de`n{0} ?"
        "remove_page" = "¿Quitar TimeShift Co-op {0} de esta carpeta?`n`nSe devuelven los archivos originales y los ajustes del juego; los perfiles y las partidas guardadas (UserConfigs) se quedan."
        "removed" = "Eliminado: "
        "shortcut" = "Acceso directo: "
        "shortcut_desc" = "TimeShift con el mod cooperativo: instala antes una versión más nueva del mod, si la hay"
        "steam_accept" = "Entiendo: TimeShift.exe y los archivos de datos indicados arriba se reemplazan por los del paquete (los originales se conservan, la desinstalación los devuelve)"
        "steam_need_accept" = "En esta copia de Steam (parche 1.0), {0} serían REEMPLAZADOS por los archivos del paquete {1} (SHA-256 de TimeShift.exe: {2}). Los originales se guardan en coop-backup\steam10 y la desinstalación los devuelve exactamente. Para aceptar, ejecuta de nuevo con -AcceptSteamUpdate. No se cambió nada."
        "steam_pkg_exe" = "TimeShift.exe del paquete: SHA-256 {0}"
        "removed_steam" = "Tu copia de Steam vuelve a estar como antes (parche 1.0): TimeShift.exe, steamwars.ini y los archivos de datos se devolvieron exactamente desde coop-backup\steam10."
    }
    it = @{
        "not_installed" = "La mod co-op non è installata in`n{0}"
        "removed_files" = "{0} file rimossi."
        "put_back" = "Rimesso a posto: "
        "no_files" = "Accanto al programma di installazione manca la cartella «files»."
        "close_game" = "Chiudi prima il gioco."
        "incomplete" = "La mod scaricata è incompleta: non è stato modificato nulla. Avvia di nuovo il gioco per riprovare."
        "another_install" = "È in corso un'altra installazione della mod: attendi che finisca."
        "no_admin" = "Windows non ha permesso di scrivere nella cartella del gioco senza diritti di amministratore."
        "installed_now" = "Installata ora: mod co-op "
        "original_kept" = "Originale conservato: "
        "asi_copy" = "Il tuo caricatore ASI gira anche come: "
        "asi_note" = "Il tuo caricatore ASI (il dinput8.dll di un'altra mod) ora gira anche come bin\winmm.dll, quindi i suoi plugin restano attivi."
        "off_note" = "Il dinput8.dll di un'altra mod è conservato in coop-backup e resta disattivato finché la mod co-op è installata; rimuovendo la mod co-op torna al suo posto."
        "winmm_taken" = "C'è già un bin\winmm.dll di un'altra mod: il caricatore ASI resta disattivato."
        "left_alone" = "Non è più la copia fatta dalla mod co-op, lasciato al suo posto: "
        "foreign_over_coop" = "Un file di un'altra mod ha sostituito quello della mod co-op; ora è conservato come originale: "
        "left_in_place" = "File di un'altra mod al posto di quello della mod co-op, lasciato al suo posto: "
        "left_note" = "`n`nUn bin\dinput8.dll di un'altra mod aveva sostituito quello della mod co-op: resta al suo posto."
        "copied" = "{0} file copiati."
        "large_address_aware" = "Flag Large Address Aware attivato in bin\TimeShift.exe (l'originale è conservato in coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe è già Large Address Aware"
        "exe_wrong" = "Questa mod è fatta per TimeShift con la patch 1.2 (bin\TimeShift.exe con SHA-256 689c63da...52641be4, o la versione già patchata della comunità). L'exe in questa cartella è un'altra versione (SHA-256 {0}).`nInstalla la patch ufficiale 1.2 per il tuo gioco e riavvia questo programma di installazione. Se il menu principale del gioco mostra già «Ver 1.02», segnala questo SHA-256 sulla pagina GitHub della mod (Issues). Non è stato modificato nulla."
        "exe_steam" = "Questa è una copia Steam alla patch 1.0 (exe SHA-256 {0}); la mod richiede la patch 1.2, che Steam non ha mai ricevuto.`nIndica il pacchetto di aggiornamento 1.2 per l'edizione Steam (fatto dalla comunità) con -SteamUpdate <zip o cartella> e accetta la sostituzione con -AcceptSteamUpdate (la finestra lo chiede da sola). Non è stato modificato nulla."
        "steam_need" = "La tua copia Steam è alla patch 1.0; la mod richiede la patch 1.2. L'installazione SOSTITUIRÀ {0} con i file del pacchetto di aggiornamento 1.2 per l'edizione Steam (fatto dalla comunità) che TU indichi qui sotto (zip o cartella). Gli originali vengono salvati in coop-backup\steam10 e la disinstallazione li rimette esattamente a posto."
        "found_steam_old" = "TimeShift trovato (edizione Steam), ma è la versione 1.0, non la patch 1.2."
        "steam_pick" = "Pacchetto di aggiornamento 1.2 per Steam (zip o cartella)"
        "steam_pick_dir" = "Scegli la cartella in cui è stato estratto il pacchetto"
        "browse_zip" = "Zip..."
        "browse_dir" = "Cartella..."
        "steam_pkg_none" = "Ancora nessun pacchetto: scegli lo zip o la cartella estratta."
        "steam_pkg_ok" = "Il pacchetto è verificato: porta il gioco alla 1.2 ({0} file)."
        "steam_pkg_found" = "(trovato nei tuoi Download)"
        "steam_pkg_known" = "(il pacchetto noto della comunità: tutti i file verificati)"
        "steam_pkg_unknown" = "(non è il pacchetto noto: il suo exe è verificato come codice 1.2, la costruzione dei dati verifica gli archivi)"
        "pkg_bad" = "Il pacchetto non è utilizzabile: {0}"
        "pkg_nothing" = "lì non c'è nulla."
        "pkg_read" = "non è stato possibile leggerlo ({0})."
        "pkg_noexe" = "non contiene bin\TimeShift.exe."
        "pkg_unsafe" = "contiene un percorso non sicuro ({0})."
        "pkg_exe_other" = "il suo bin\TimeShift.exe non è la versione della patch 1.2 (SHA-256 {0})."
        "pkg_nopaks" = "non contiene preload\paks\{0}."
        "pkg_skipped" = "Esclusi {0} file del pacchetto che non sono dati del gioco (programmi, impostazioni, salvataggi)."
        "steam_applying" = "Aggiornamento della copia Steam alla 1.2 da {0}"
        "steam_done" = "Il gioco è ora alla 1.2 ({0} file sostituiti o aggiunti; gli originali sono in coop-backup\steam10)."
        "steam_rollback" = "L'aggiornamento è stato annullato: i file di Steam sono tornati com'erano."
        "steam_restored" = "File di Steam rimessi a posto: {0}"
        "st_steam" = "Porto la copia Steam alla 1.2 (gli originali vengono conservati)"
        "notice_community" = "L'exe è la versione 1.2 già patchata della comunità (la sua correzione per la CPU è già dentro). La correzione propria della mod fa lo stesso: nient'altro cambia."
        "notice_variant" = "L'exe ha il codice della patch 1.2 con piccole modifiche (una versione della comunità). È accettato; se il gioco non parte, bin\ts_coop.log dice quali correzioni della mod non hanno trovato il loro posto."
        "install_steam" = "Aggiorna e installa"
        "write_denied" = "La cartella del gioco o uno dei suoi file è di sola lettura o protetto: {0}`nChiudi i programmi che usano la cartella del gioco, togli l'attributo di sola lettura o avvia il programma come amministratore."
        "paks_missing" = "Manca preload\paks\{0}: non è una cartella TimeShift 1.2 completa. Non è stato modificato nulla."
        "paks_kept" = "I dati della mod dell'ultima installazione sono aggiornati: conservati."
        "paks_built" = "I dati della mod sono stati costruiti dai tuoi file di gioco."
        "build_failed" = "Non è stato possibile costruire i dati della mod dai tuoi file di gioco (il registro ne dice il motivo). Non è stato modificato nulla."
        "ini_other" = "bin\steamwars.ini indicava un altro archivio ({0}): viene rimesso alla disinstallazione."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "bin\steamwars.ini non ha la riga arcName"
        "start_msg" = "Avvia il gioco: il menu principale ha una voce «Cooperativa».`nPer rimuoverla: «Uninstall Co-op.bat» nella cartella del gioco (o riavvia questo programma)."
        "the_shortcut" = "Il collegamento «TimeShift Co-op» (desktop, menu Start) avvia il gioco e prima installa da GitHub una versione più recente della mod, se c'è."
        "not_a_folder" = "Non è una cartella TimeShift (manca bin\TimeShift.exe): {0}"
        "not_found" = "Gioco non trovato: indica la sua cartella con -Game."
        "removed_done" = "La mod co-op è stata rimossa; i file sostituiti e le impostazioni del gioco sono stati rimessi a posto."
        "is_installed" = "TimeShift Co-op {0} è installato in {1}"
        "error" = "Errore: "
        "subtitle" = "Versione {0}  -  campagna cooperativa per TimeShift, 2-4 giocatori"
        "game_folder" = "Cartella del gioco"
        "browse" = "Sfoglia..."
        "remove_co_op" = "Rimuovi la mod"
        "cancel" = "Annulla"
        "install" = "Installa"
        "copy_log" = "Copia il registro"
        "open_game_folder" = "Apri la cartella del gioco"
        "close" = "Chiudi"
        "please_wait" = "Attendi la fine: fermarsi a metà potrebbe lasciare il gioco modificato a metà."
        "choose_folder" = "Il gioco non è stato trovato automaticamente: scegli la sua cartella (quella che contiene la cartella bin)."
        "no_exe" = "In questa cartella non c'è bin\TimeShift.exe."
        "found" = "TimeShift 1.2 trovato."
        "other_games" = "Sono state trovate altre copie del gioco: {0}`nUsa Sfoglia per sceglierne un'altra."
        "found_other" = "TimeShift trovato, ma il suo exe non è la patch 1.2: la mod non può essere installata (altro in README-COOP.txt)."
        "already" = "La mod co-op {0} è già installata: verrà installata di nuovo."
        "reinstall" = "Reinstalla"
        "will_update" = "La mod co-op {0} è installata: verrà aggiornata alla {1}."
        "update" = "Aggiorna"
        "what" = "Il programma costruisce i dati della mod dai tuoi file di gioco (pochi secondi), copia la mod nella cartella del gioco e conserva ogni file di gioco che sostituisce (coop-backup); «Uninstall Co-op.bat» lì rimette tutto a posto. Profili e salvataggi non vengono toccati."
        "same_version" = "Tutti i giocatori devono avere la stessa versione della mod."
        "installing" = "Installazione della mod co-op {0}..."
        "st_check" = "Controllo della cartella del gioco"
        "st_paks" = "Costruzione dei dati della mod dai tuoi file di gioco"
        "st_copy" = "Copia della mod (i file di gioco sostituiti vengono conservati)"
        "st_finish" = "Collegamenti e registro dell'installazione"
        "folder_log" = "Cartella del gioco: "
        "updated" = "Aggiornato dalla {0} alla {1}"
        "installed_title" = "La mod co-op {0} è installata"
        "install_failed" = "Installazione non riuscita"
        "log_in" = "Il registro è in %TEMP%\timeshift-coop-install.log. Del gioco non si perde nulla: i file sostituiti sono in coop-backup."
        "removing" = "Rimozione della mod co-op..."
        "st_rm" = "Rimozione dei file e dei dati della mod"
        "st_restore" = "Ripristino dei file originali del gioco"
        "st_clean" = "Rimozione delle copie e della cartella della mod"
        "removed_title" = "Mod co-op rimossa"
        "removed_text" = "La mod co-op è stata rimossa; i file sostituiti e bin\steamwars.ini sono stati rimessi a posto.`n`nProfili e salvataggi (UserConfigs) non sono stati toccati."
        "remove_failed" = "Rimozione non riuscita"
        "select_folder" = "Scegli la cartella di TimeShift (quella che contiene la cartella bin)"
        "remove_q" = "Rimuovere la mod co-op da`n{0} ?"
        "remove_page" = "Rimuovere TimeShift Co-op {0} da questa cartella?`n`nI file originali e le impostazioni del gioco vengono rimessi a posto; profili e salvataggi (UserConfigs) restano."
        "removed" = "Rimosso: "
        "shortcut" = "Collegamento: "
        "shortcut_desc" = "TimeShift in co-op: prima installa una versione più recente della mod, se c'è"
        "steam_accept" = "Capisco: TimeShift.exe e gli archivi di dati elencati sopra vengono sostituiti dai file del pacchetto (gli originali restano, la disinstallazione li ripristina)"
        "steam_need_accept" = "In questa copia Steam (patch 1.0), {0} verrebbero SOSTITUITI dai file del pacchetto {1} (SHA-256 di TimeShift.exe: {2}). Gli originali vengono salvati in coop-backup\steam10 e la disinstallazione li rimette esattamente a posto. Per accettare, riavvia con -AcceptSteamUpdate. Non è stato modificato nulla."
        "steam_pkg_exe" = "TimeShift.exe del pacchetto: SHA-256 {0}"
        "removed_steam" = "La tua copia Steam è tornata com'era (patch 1.0): TimeShift.exe, steamwars.ini e gli archivi di dati sono stati rimessi esattamente da coop-backup\steam10."
    }
    pl = @{
        "not_installed" = "Mod kooperacji nie jest zainstalowany w`n{0}"
        "removed_files" = "Usunięto plików: {0}."
        "put_back" = "Przywrócono: "
        "no_files" = "Obok instalatora brakuje folderu `„files`”."
        "close_game" = "Najpierw zamknij grę."
        "incomplete" = "Pobrany mod jest niekompletny: nic nie zostało zmienione. Uruchom grę ponownie, aby spróbować jeszcze raz."
        "another_install" = "Trwa inna instalacja moda: poczekaj, aż się zakończy."
        "no_admin" = "Windows nie pozwolił na zapis w folderze gry bez uprawnień administratora."
        "installed_now" = "Teraz zainstalowany: mod kooperacji "
        "original_kept" = "Zachowano oryginał: "
        "asi_copy" = "Twój loader ASI działa też jako: "
        "asi_note" = "Twój loader ASI (dinput8.dll innego moda) działa teraz także jako bin\winmm.dll, więc jego wtyczki pozostają włączone."
        "off_note" = "dinput8.dll innego moda jest zachowany w coop-backup i wyłączony, dopóki mod kooperacji jest zainstalowany; usunięcie moda kooperacji go przywróci."
        "winmm_taken" = "Jest już bin\winmm.dll innego moda: loader ASI pozostaje wyłączony."
        "left_alone" = "To już nie jest kopia zrobiona przez mod kooperacji, pozostawiono na miejscu: "
        "foreign_over_coop" = "Plik innego moda zastąpił plik moda kooperacji; teraz jest zachowany jako oryginał: "
        "left_in_place" = "Plik innego moda w miejscu pliku moda kooperacji, pozostawiono na miejscu: "
        "left_note" = "`n`nbin\dinput8.dll innego moda zastąpił plik moda kooperacji: pozostaje na miejscu."
        "copied" = "Skopiowano plików: {0}."
        "large_address_aware" = "Ustawiono flagę Large Address Aware w bin\TimeShift.exe (oryginał jest zachowany w coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe ma już flagę Large Address Aware"
        "exe_wrong" = "Ten mod jest przeznaczony dla TimeShift z patchem 1.2 (bin\TimeShift.exe z SHA-256 689c63da...52641be4 lub gotowa wersja społeczności oparta na nim). Plik exe w tym folderze to inna wersja (SHA-256 {0}).`nZainstaluj oficjalny patch 1.2 dla swojej kopii gry i uruchom ten instalator ponownie. Jeśli menu główne gry pokazuje już `„Ver 1.02`”, zgłoś ten SHA-256 na stronie moda na GitHubie (Issues). Nic nie zostało zmienione."
        "exe_steam" = "To kopia ze Steam z patchem 1.0 (SHA-256 exe {0}); mod wymaga patcha 1.2, którego Steam nigdy nie otrzymał.`nPodaj pakiet aktualizacji 1.2 dla wersji Steam (zrobiony przez społeczność) przez -SteamUpdate <zip lub folder> i zgódź się na zastąpienie przez -AcceptSteamUpdate (okno zapyta samo). Nic nie zostało zmienione."
        "steam_need" = "Twoja kopia ze Steam to patch 1.0; mod wymaga patcha 1.2. Instalacja ZASTĄPI {0} plikami pakietu aktualizacji 1.2 dla wersji Steam (zrobionego przez społeczność), który TY podasz poniżej (zip lub folder). Oryginały są zachowywane w coop-backup\steam10, a odinstalowanie przywraca je dokładnie."
        "found_steam_old" = "Znaleziono TimeShift (wersja Steam), ale to wersja 1.0, a nie patch 1.2."
        "steam_pick" = "Pakiet aktualizacji 1.2 dla Steam (zip lub folder)"
        "steam_pick_dir" = "Wybierz folder, do którego rozpakowano pakiet"
        "browse_zip" = "Zip..."
        "browse_dir" = "Folder..."
        "steam_pkg_none" = "Brak pakietu: wybierz plik zip lub rozpakowany folder."
        "steam_pkg_ok" = "Pakiet sprawdzony: aktualizuje grę do 1.2 (plików: {0})."
        "steam_pkg_found" = "(znaleziony w folderze Pobrane)"
        "steam_pkg_known" = "(znany pakiet społeczności: sprawdzono każdy plik)"
        "steam_pkg_unknown" = "(nie jest to znany pakiet: jego exe sprawdzono jako kod 1.2, archiwa sprawdzi budowanie danych)"
        "pkg_bad" = "Tego pakietu nie można użyć: {0}"
        "pkg_nothing" = "nic tam nie ma."
        "pkg_read" = "nie udało się go odczytać ({0})."
        "pkg_noexe" = "nie zawiera bin\TimeShift.exe."
        "pkg_unsafe" = "zawiera niebezpieczną ścieżkę ({0})."
        "pkg_exe_other" = "jego bin\TimeShift.exe nie jest wersją z patcha 1.2 (SHA-256 {0})."
        "pkg_nopaks" = "nie zawiera preload\paks\{0}."
        "pkg_skipped" = "Pominięto plików pakietu, które nie są danymi gry (programy, ustawienia, zapisy): {0}."
        "steam_applying" = "Aktualizacja kopii ze Steam do 1.2 z {0}"
        "steam_done" = "Gra ma teraz wersję 1.2 (zastąpiono lub dodano plików: {0}; oryginały są w coop-backup\steam10)."
        "steam_rollback" = "Aktualizacja została cofnięta: pliki Steam są takie jak wcześniej."
        "steam_restored" = "Przywrócono pliki Steam: {0}"
        "st_steam" = "Aktualizacja kopii ze Steam do 1.2 (oryginały są zachowywane)"
        "notice_community" = "Ten exe to gotowa wersja 1.2 od społeczności (jej poprawka dla procesorów jest już w środku). Poprawka moda robi to samo: nic więcej się nie zmienia."
        "notice_variant" = "Ten exe zawiera kod patcha 1.2 z drobnymi zmianami (wersja społeczności). Został przyjęty; jeśli gra się nie uruchomi, bin\ts_coop.log pokaże, które poprawki moda nie pasowały."
        "install_steam" = "Aktualizuj i zainstaluj"
        "write_denied" = "Folder gry lub jeden z jego plików jest tylko do odczytu lub chroniony: {0}`nZamknij programy używające folderu gry, usuń atrybut tylko do odczytu albo uruchom instalator jako administrator."
        "paks_missing" = "Brak preload\paks\{0}: to nie jest kompletny folder TimeShift 1.2. Nic nie zostało zmienione."
        "paks_kept" = "Dane moda z ostatniej instalacji są aktualne: pozostawiono je."
        "paks_built" = "Dane moda zostały zbudowane z twoich plików gry."
        "build_failed" = "Nie udało się zbudować danych moda z twoich plików gry (przyczyna jest w dzienniku). Nic nie zostało zmienione."
        "ini_other" = "bin\steamwars.ini wskazywał inne archiwum ({0}): zostanie przywrócone przy odinstalowaniu."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "bin\steamwars.ini nie zawiera wiersza arcName"
        "start_msg" = "Uruchom grę: w menu głównym jest pozycja `„Kooperacja`”.`nAby usunąć mod: `„Uninstall Co-op.bat`” w folderze gry (lub ponownie ten instalator)."
        "the_shortcut" = "Skrót `„TimeShift Co-op`” (pulpit, menu Start) uruchamia grę, a wcześniej instaluje z GitHuba nowszą wersję moda, jeśli jest dostępna."
        "not_a_folder" = "To nie jest folder TimeShift (brak bin\TimeShift.exe): {0}"
        "not_found" = "Nie znaleziono gry: podaj jej folder przez -Game."
        "removed_done" = "Mod kooperacji został usunięty; zastąpione pliki i ustawienia gry zostały przywrócone."
        "is_installed" = "TimeShift Co-op {0} jest zainstalowany w {1}"
        "error" = "Błąd: "
        "subtitle" = "Wersja {0}  -  kampania w kooperacji dla TimeShift, 2-4 graczy"
        "game_folder" = "Folder gry"
        "browse" = "Przeglądaj..."
        "remove_co_op" = "Usuń mod kooperacji"
        "cancel" = "Anuluj"
        "install" = "Zainstaluj"
        "copy_log" = "Kopiuj dziennik"
        "open_game_folder" = "Otwórz folder gry"
        "close" = "Zamknij"
        "please_wait" = "Poczekaj do końca: przerwanie w połowie mogłoby zostawić grę zmienioną tylko częściowo."
        "choose_folder" = "Nie znaleziono gry automatycznie: wybierz jej folder (ten z folderem bin)."
        "no_exe" = "W tym folderze nie ma bin\TimeShift.exe."
        "found" = "Znaleziono TimeShift 1.2."
        "other_games" = "Znaleziono inne kopie gry: {0}`nInną możesz wybrać przyciskiem `„Przeglądaj`”."
        "found_other" = "Znaleziono TimeShift, ale jego exe nie pochodzi z patcha 1.2: nie można zainstalować moda (więcej w README-COOP.txt)."
        "already" = "Mod kooperacji {0} jest już zainstalowany: zostanie zainstalowany ponownie."
        "reinstall" = "Przeinstaluj"
        "will_update" = "Zainstalowany jest mod kooperacji {0}: zostanie zaktualizowany do {1}."
        "update" = "Aktualizuj"
        "what" = "Instalator zbuduje dane moda z twoich własnych plików gry (kilka sekund), skopiuje mod do folderu gry i zachowa każdy zastępowany plik gry (coop-backup); `„Uninstall Co-op.bat`” w tym folderze wszystko przywróci. Profile i zapisane gry pozostają nietknięte."
        "same_version" = "Wszyscy gracze muszą mieć tę samą wersję moda."
        "installing" = "Instalowanie moda kooperacji {0}..."
        "st_check" = "Sprawdzanie folderu gry"
        "st_paks" = "Budowanie danych moda z twoich plików gry"
        "st_copy" = "Kopiowanie moda (zastępowane pliki gry są zachowywane)"
        "st_finish" = "Skróty i zapis o instalacji"
        "folder_log" = "Folder gry: "
        "updated" = "Zaktualizowano z {0} do {1}"
        "installed_title" = "Mod kooperacji {0} jest zainstalowany"
        "install_failed" = "Instalacja nie powiodła się"
        "log_in" = "Dziennik jest w %TEMP%\timeshift-coop-install.log. Z gry nic nie przepadło: zastąpione pliki są w coop-backup."
        "removing" = "Usuwanie moda kooperacji..."
        "st_rm" = "Usuwanie plików i danych moda"
        "st_restore" = "Przywracanie oryginalnych plików gry"
        "st_clean" = "Usuwanie kopii zapasowych i folderu moda"
        "removed_title" = "Mod kooperacji usunięty"
        "removed_text" = "Mod kooperacji został usunięty; zastąpione pliki i bin\steamwars.ini zostały przywrócone.`n`nProfile i zapisane gry (UserConfigs) pozostały nietknięte."
        "remove_failed" = "Usuwanie nie powiodło się"
        "select_folder" = "Wybierz folder TimeShift (ten z folderem bin)"
        "remove_q" = "Usunąć mod kooperacji z`n{0} ?"
        "remove_page" = "Usunąć TimeShift Co-op {0} z tego folderu?`n`nOryginalne pliki i ustawienia gry zostaną przywrócone; profile i zapisane gry (UserConfigs) pozostaną."
        "removed" = "Usunięto: "
        "shortcut" = "Skrót: "
        "shortcut_desc" = "TimeShift z kooperacją: najpierw instaluje nowszą wersję moda, jeśli jest dostępna"
        "steam_accept" = "Rozumiem: TimeShift.exe i wymienione wyżej archiwa danych zostaną zastąpione plikami pakietu (oryginały są zachowywane, odinstalowanie je przywraca)"
        "steam_need_accept" = "W tej kopii ze Steam (patch 1.0) {0} zostałyby ZASTĄPIONE plikami pakietu {1} (SHA-256 TimeShift.exe: {2}). Oryginały są zachowywane w coop-backup\steam10, a odinstalowanie przywraca je dokładnie. Aby się zgodzić, uruchom ponownie z -AcceptSteamUpdate. Nic nie zostało zmienione."
        "steam_pkg_exe" = "TimeShift.exe pakietu: SHA-256 {0}"
        "removed_steam" = "Twoja kopia ze Steam jest znowu taka jak wcześniej (patch 1.0): TimeShift.exe, steamwars.ini i archiwa danych zostały dokładnie przywrócone z coop-backup\steam10."
    }
    cs = @{
        "not_installed" = "Kooperativní mod není nainstalován v`n{0}"
        "removed_files" = "Odstraněno souborů: {0}."
        "put_back" = "Vráceno: "
        "no_files" = "Vedle instalátoru chybí složka `„files`“."
        "close_game" = "Nejdřív ukonči hru."
        "incomplete" = "Stažený mod je neúplný: nic nebylo změněno. Spusť hru znovu a zkus to ještě jednou."
        "another_install" = "Už běží jiná instalace modu: počkej, až skončí."
        "no_admin" = "Windows nepovolil zápis do složky hry bez práv správce."
        "installed_now" = "Nyní nainstalován: kooperativní mod "
        "original_kept" = "Originál zachován: "
        "asi_copy" = "Tvůj ASI loader běží také jako: "
        "asi_note" = "Tvůj ASI loader (dinput8.dll jiného modu) teď běží také jako bin\winmm.dll, takže jeho pluginy zůstávají zapnuté."
        "off_note" = "dinput8.dll jiného modu je uložen v coop-backup a je vypnutý, dokud je nainstalován kooperativní mod; odebrání kooperativního modu ho vrátí."
        "winmm_taken" = "bin\winmm.dll jiného modu už tam je: ASI loader zůstává vypnutý."
        "left_alone" = "Už to není kopie vytvořená kooperativním modem, ponecháno na místě: "
        "foreign_over_coop" = "Soubor jiného modu nahradil soubor kooperativního modu; nyní je uložen jako originál: "
        "left_in_place" = "Soubor jiného modu místo souboru kooperativního modu, ponechán na místě: "
        "left_note" = "`n`nbin\dinput8.dll jiného modu nahradil soubor kooperativního modu: zůstává na místě."
        "copied" = "Zkopírováno souborů: {0}."
        "large_address_aware" = "V bin\TimeShift.exe nastaven příznak Large Address Aware (originál je uložen v coop-backup)"
        "already_large_address_aware" = "bin\TimeShift.exe už má příznak Large Address Aware"
        "exe_wrong" = "Tento mod je určen pro TimeShift s patchem 1.2 (bin\TimeShift.exe se SHA-256 689c63da...52641be4 nebo hotová verze komunity na jeho základě). Exe v této složce je jiná verze (SHA-256 {0}).`nNainstaluj oficiální patch 1.2 pro svou kopii hry a spusť tento instalátor znovu. Pokud hlavní menu tvé hry už ukazuje `„Ver 1.02`“, nahlas prosím tento SHA-256 na stránce modu na GitHubu (Issues). Nic nebylo změněno."
        "exe_steam" = "Toto je kopie ze Steamu s patchem 1.0 (SHA-256 exe {0}); mod potřebuje patch 1.2, který Steam nikdy nedostal.`nZadej balíček aktualizace 1.2 pro verzi ze Steamu (vytvořený komunitou) pomocí -SteamUpdate <zip nebo složka> a souhlas s nahrazením potvrď pomocí -AcceptSteamUpdate (okno se zeptá samo). Nic nebylo změněno."
        "steam_need" = "Tvoje kopie ze Steamu je patch 1.0; mod potřebuje patch 1.2. Instalace NAHRADÍ {0} soubory balíčku aktualizace 1.2 pro verzi ze Steamu (vytvořeného komunitou), který TY zadáš níže (zip nebo složka). Originály se ukládají do coop-backup\steam10 a odinstalace je přesně vrátí."
        "found_steam_old" = "TimeShift nalezen (verze ze Steamu), ale je to verze 1.0, ne patch 1.2."
        "steam_pick" = "Balíček aktualizace 1.2 pro Steam (zip nebo složka)"
        "steam_pick_dir" = "Vyber složku, do které byl balíček rozbalen"
        "browse_zip" = "Zip..."
        "browse_dir" = "Složka..."
        "steam_pkg_none" = "Zatím žádný balíček: vyber zip nebo rozbalenou složku."
        "steam_pkg_ok" = "Balíček je zkontrolován: aktualizuje hru na 1.2 (souborů: {0})."
        "steam_pkg_found" = "(nalezen ve složce Stažené soubory)"
        "steam_pkg_known" = "(známý balíček komunity: zkontrolován každý soubor)"
        "steam_pkg_unknown" = "(není to známý balíček: jeho exe je zkontrolován jako kód 1.2, archivy zkontroluje sestavení dat)"
        "pkg_bad" = "Balíček nelze použít: {0}"
        "pkg_nothing" = "nic tam není."
        "pkg_read" = "nepodařilo se ho přečíst ({0})."
        "pkg_noexe" = "neobsahuje bin\TimeShift.exe."
        "pkg_unsafe" = "obsahuje nebezpečnou cestu ({0})."
        "pkg_exe_other" = "jeho bin\TimeShift.exe není sestavení patche 1.2 (SHA-256 {0})."
        "pkg_nopaks" = "neobsahuje preload\paks\{0}."
        "pkg_skipped" = "Vynecháno souborů balíčku, které nejsou daty hry (programy, nastavení, uložené hry): {0}."
        "steam_applying" = "Kopie ze Steamu se aktualizuje na 1.2 z {0}"
        "steam_done" = "Hra je nyní ve verzi 1.2 (nahrazeno nebo přidáno souborů: {0}; originály jsou v coop-backup\steam10)."
        "steam_rollback" = "Aktualizace byla vrácena zpět: soubory Steamu jsou jako předtím."
        "steam_restored" = "Vráceny soubory Steamu: {0}"
        "st_steam" = "Aktualizace kopie ze Steamu na 1.2 (originály se zachovají)"
        "notice_community" = "Toto exe je hotové sestavení 1.2 od komunity (jeho oprava pro procesory už je uvnitř). Oprava modu dělá totéž: nic jiného se nemění."
        "notice_variant" = "Toto exe obsahuje kód patche 1.2 s malými změnami (sestavení komunity). Je přijato; pokud se hra nespustí, bin\ts_coop.log ukáže, které opravy modu nepasovaly."
        "install_steam" = "Aktualizovat a nainstalovat"
        "write_denied" = "Složka hry nebo některý z jejích souborů je jen pro čtení nebo chráněný: {0}`nZavři programy, které složku hry používají, zruš atribut jen pro čtení nebo spusť instalátor jako správce."
        "paks_missing" = "Chybí preload\paks\{0}: toto není úplná složka TimeShift 1.2. Nic nebylo změněno."
        "paks_kept" = "Data modu z poslední instalace jsou aktuální: ponechána."
        "paks_built" = "Data modu byla sestavena z tvých souborů hry."
        "build_failed" = "Data modu se nepodařilo sestavit z tvých souborů hry (důvod je v protokolu). Nic nebylo změněno."
        "ini_other" = "bin\steamwars.ini uváděl jiný archiv ({0}): při odinstalaci se vrátí."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "bin\steamwars.ini neobsahuje řádek arcName"
        "start_msg" = "Spusť hru: v hlavním menu je položka `„Kooperace`“.`nOdebrání: `„Uninstall Co-op.bat`“ ve složce hry (nebo znovu tento instalátor)."
        "the_shortcut" = "Zástupce `„TimeShift Co-op`“ (plocha, nabídka Start) spouští hru a předtím nainstaluje z GitHubu novější verzi modu, pokud vyšla."
        "not_a_folder" = "Toto není složka TimeShift (chybí bin\TimeShift.exe): {0}"
        "not_found" = "Hra nebyla nalezena: zadej její složku pomocí -Game."
        "removed_done" = "Kooperativní mod byl odebrán; nahrazené soubory a nastavení hry byly vráceny."
        "is_installed" = "TimeShift Co-op {0} je nainstalován v {1}"
        "error" = "Chyba: "
        "subtitle" = "Verze {0}  -  kooperativní kampaň pro TimeShift, 2-4 hráči"
        "game_folder" = "Složka hry"
        "browse" = "Procházet..."
        "remove_co_op" = "Odebrat mod"
        "cancel" = "Zrušit"
        "install" = "Nainstalovat"
        "copy_log" = "Kopírovat protokol"
        "open_game_folder" = "Otevřít složku hry"
        "close" = "Zavřít"
        "please_wait" = "Počkej, až to skončí: zastavení uprostřed by mohlo nechat hru změněnou jen napůl."
        "choose_folder" = "Hra nebyla nalezena automaticky: vyber její složku (tu se složkou bin)."
        "no_exe" = "V této složce není bin\TimeShift.exe."
        "found" = "TimeShift 1.2 nalezen."
        "other_games" = "Nalezeny další kopie hry: {0}`nJinou můžeš vybrat tlačítkem `„Procházet`“."
        "found_other" = "TimeShift nalezen, ale jeho exe není z patche 1.2: mod nelze nainstalovat (více v README-COOP.txt)."
        "already" = "Kooperativní mod {0} už je nainstalován: nainstaluje se znovu."
        "reinstall" = "Přeinstalovat"
        "will_update" = "Je nainstalován kooperativní mod {0}: bude aktualizován na {1}."
        "update" = "Aktualizovat"
        "what" = "Instalátor sestaví data modu z tvých vlastních souborů hry (pár sekund), zkopíruje mod do složky hry a zachová každý nahrazovaný soubor hry (coop-backup); `„Uninstall Co-op.bat`“ tam vše vrátí. Profily a uložené hry zůstanou nedotčené."
        "same_version" = "Všichni hráči potřebují stejnou verzi modu."
        "installing" = "Instalace kooperativního modu {0}..."
        "st_check" = "Kontrola složky hry"
        "st_paks" = "Sestavení dat modu z tvých souborů hry"
        "st_copy" = "Kopírování modu (nahrazované soubory hry se zachovají)"
        "st_finish" = "Zástupci a záznam o instalaci"
        "folder_log" = "Složka hry: "
        "updated" = "Aktualizováno z {0} na {1}"
        "installed_title" = "Kooperativní mod {0} je nainstalován"
        "install_failed" = "Instalace se nezdařila"
        "log_in" = "Protokol je v %TEMP%\timeshift-coop-install.log. Ze hry se nic neztratilo: nahrazené soubory jsou v coop-backup."
        "removing" = "Odebírání kooperativního modu..."
        "st_rm" = "Odebrání souborů a dat modu"
        "st_restore" = "Vrácení původních souborů hry"
        "st_clean" = "Odebrání záloh a složky modu"
        "removed_title" = "Kooperativní mod odebrán"
        "removed_text" = "Kooperativní mod byl odebrán; nahrazené soubory a bin\steamwars.ini byly vráceny.`n`nProfily a uložené hry (UserConfigs) zůstaly nedotčené."
        "remove_failed" = "Odebrání se nezdařilo"
        "select_folder" = "Vyber složku TimeShift (tu se složkou bin)"
        "remove_q" = "Odebrat kooperativní mod z`n{0} ?"
        "remove_page" = "Odebrat TimeShift Co-op {0} z této složky?`n`nPůvodní soubory a nastavení hry se vrátí; profily a uložené hry (UserConfigs) zůstanou."
        "removed" = "Odebráno: "
        "shortcut" = "Zástupce: "
        "shortcut_desc" = "TimeShift s kooperací: nejdřív nainstaluje novější verzi modu, pokud vyšla"
        "steam_accept" = "Rozumím: TimeShift.exe a výše uvedené datové archivy budou nahrazeny soubory balíčku (originály se zachovají, odinstalace je vrátí)"
        "steam_need_accept" = "V této kopii ze Steamu (patch 1.0) by {0} byly NAHRAZENY soubory balíčku {1} (SHA-256 TimeShift.exe: {2}). Originály se ukládají do coop-backup\steam10 a odinstalace je přesně vrátí. Pro souhlas spusť instalátor znovu s -AcceptSteamUpdate. Nic nebylo změněno."
        "steam_pkg_exe" = "TimeShift.exe balíčku: SHA-256 {0}"
        "removed_steam" = "Tvoje kopie ze Steamu je zase taková, jaká byla (patch 1.0): TimeShift.exe, steamwars.ini a datové archivy byly přesně vráceny z coop-backup\steam10."
    }
    hu = @{
        "not_installed" = "A kooperatív mod nincs telepítve ide:`n{0}"
        "removed_files" = "{0} fájl eltávolítva."
        "put_back" = "Visszaállítva: "
        "no_files" = "A telepítő mellől hiányzik a `„files`” mappa."
        "close_game" = "Előbb zárd be a játékot."
        "incomplete" = "A letöltött mod hiányos: semmi sem változott. Indítsd el újra a játékot, hogy újra megpróbáld."
        "another_install" = "Már fut a mod egy másik telepítése: várd meg, amíg befejeződik."
        "no_admin" = "A Windows nem engedélyezte az írást a játék mappájába rendszergazdai jogok nélkül."
        "installed_now" = "Most telepítve: kooperatív mod "
        "original_kept" = "Eredeti megőrizve: "
        "asi_copy" = "Az ASI-betöltőd így is fut: "
        "asi_note" = "Az ASI-betöltőd (egy másik mod dinput8.dll-je) mostantól bin\winmm.dll néven is fut, így a bővítményei bekapcsolva maradnak."
        "off_note" = "Egy másik mod dinput8.dll-je a coop-backup mappában van megőrizve, és ki van kapcsolva, amíg a kooperatív mod telepítve van; a kooperatív mod eltávolítása visszateszi."
        "winmm_taken" = "Már van egy másik modhoz tartozó bin\winmm.dll: az ASI-betöltő kikapcsolva marad."
        "left_alone" = "Már nem a kooperatív mod által készített másolat, a helyén marad: "
        "foreign_over_coop" = "Egy másik mod fájlja felülírta a kooperatív modét; mostantól eredetiként van megőrizve: "
        "left_in_place" = "Egy másik mod fájlja a kooperatív modé helyén, a helyén marad: "
        "left_note" = "`n`nEgy másik mod bin\dinput8.dll-je felülírta a kooperatív modét: a helyén marad."
        "copied" = "{0} fájl másolva."
        "large_address_aware" = "A Large Address Aware jelző beállítva a bin\TimeShift.exe-ben (az eredeti a coop-backup mappában van)"
        "already_large_address_aware" = "A bin\TimeShift.exe már Large Address Aware"
        "exe_wrong" = "Ez a mod az 1.2-es patchelt TimeShifthez készült (bin\TimeShift.exe, SHA-256 689c63da...52641be4, vagy a közösség erre épülő, előre patchelt változata). Ebben a mappában az exe más verzió (SHA-256 {0}).`nTelepítsd a hivatalos 1.2-es patchet a játékodhoz, majd futtasd újra ezt a telepítőt. Ha a játék főmenüje már a `„Ver 1.02`” feliratot mutatja, jelezd ezt a SHA-256-ot a mod GitHub-oldalán (Issues). Semmi sem változott."
        "exe_steam" = "Ez egy 1.0-s patchelt Steam-példány (exe SHA-256 {0}); a modhoz az 1.2-es patch kell, amelyet a Steam sosem kapott meg.`nAdd meg a Steam-kiadáshoz készült 1.2-es frissítőcsomagot (a közösség készítette) a -SteamUpdate <zip vagy mappa> kapcsolóval, és fogadd el a cserét a -AcceptSteamUpdate kapcsolóval (az ablak magától rákérdez). Semmi sem változott."
        "steam_need" = "A Steam-példányod 1.0-s patch; a modhoz 1.2-es patch kell. A telepítés LECSERÉLI ezeket: {0}, a Steam-kiadáshoz készült 1.2-es frissítőcsomag (a közösség készítette) fájljaira, amelyet TE adsz meg lent (zip vagy mappa). Az eredetiket a coop-backup\steam10 mappa őrzi, és az eltávolítás pontosan visszateszi őket."
        "found_steam_old" = "TimeShift megtalálva (Steam-kiadás), de ez az 1.0-s verzió, nem az 1.2-es patch."
        "steam_pick" = "1.2-es frissítőcsomag a Steamhez (zip vagy mappa)"
        "steam_pick_dir" = "Válaszd ki a mappát, ahová a csomagot kicsomagoltad"
        "browse_zip" = "Zip..."
        "browse_dir" = "Mappa..."
        "steam_pkg_none" = "Még nincs csomag: válaszd ki a zipet vagy a kicsomagolt mappát."
        "steam_pkg_ok" = "A csomag ellenőrizve: 1.2-re frissíti a játékot ({0} fájl)."
        "steam_pkg_found" = "(a Letöltések mappádban található)"
        "steam_pkg_known" = "(az ismert közösségi csomag: minden fájl ellenőrizve)"
        "steam_pkg_unknown" = "(nem az ismert csomag: az exe-je 1.2-es kódként van ellenőrizve, az archívumokat az adatépítés ellenőrzi)"
        "pkg_bad" = "A csomag nem használható: {0}"
        "pkg_nothing" = "nincs benne semmi."
        "pkg_read" = "nem sikerült beolvasni ({0})."
        "pkg_noexe" = "nincs benne bin\TimeShift.exe."
        "pkg_unsafe" = "nem biztonságos útvonal van benne ({0})."
        "pkg_exe_other" = "a bin\TimeShift.exe-je nem az 1.2-es patch változata (SHA-256 {0})."
        "pkg_nopaks" = "nincs benne preload\paks\{0}."
        "pkg_skipped" = "A csomag {0} fájlja kimaradt, mert nem játékadat (programok, beállítások, mentések)."
        "steam_applying" = "A Steam-példány frissítése 1.2-re innen: {0}"
        "steam_done" = "A játék most 1.2-es ({0} fájl lecserélve vagy hozzáadva; az eredetik a coop-backup\steam10 mappában vannak)."
        "steam_rollback" = "A frissítés visszavonva: a Steam fájljai olyanok, mint előtte."
        "steam_restored" = "Visszaállított Steam-fájlok: {0}"
        "st_steam" = "A Steam-példány frissítése 1.2-re (az eredetik megmaradnak)"
        "notice_community" = "Az exe a közösség előre patchelt 1.2-es változata (a processzorjavítás már benne van). A mod saját javítása ugyanezt teszi: más nem változik."
        "notice_variant" = "Az exe az 1.2-es patch kódját tartalmazza kis változtatásokkal (közösségi változat). Elfogadva; ha a játék nem indul, a bin\ts_coop.log megmutatja, a mod mely javításai nem illeszkedtek."
        "install_steam" = "Frissítés és telepítés"
        "write_denied" = "A játék mappája vagy annak egyik fájlja írásvédett vagy védett: {0}`nZárd be a játék mappáját használó programokat, vedd le az írásvédettséget, vagy futtasd a telepítőt rendszergazdaként."
        "paks_missing" = "Hiányzik a preload\paks\{0}: ez nem teljes TimeShift 1.2 mappa. Semmi sem változott."
        "paks_kept" = "A legutóbbi telepítéskor készült moddadatok naprakészek: megtartva."
        "paks_built" = "A moddadatok a játékfájljaidból készültek el."
        "build_failed" = "A moddadatokat nem sikerült a játékfájljaidból elkészíteni (a napló megmondja, miért). Semmi sem változott."
        "ini_other" = "A bin\steamwars.ini egy másik archívumot nevezett meg ({0}): eltávolításkor visszakerül."
        "ini_set" = "bin\steamwars.ini: arcName = coop.s3darc"
        "ini_back" = "bin\steamwars.ini: arcName = {0}"
        "ini_noarc" = "A bin\steamwars.ini-ben nincs arcName sor"
        "start_msg" = "Indítsd el a játékot: a főmenüben van egy `„Kooperatív`” pont.`nEltávolítás: `„Uninstall Co-op.bat`” a játék mappájában (vagy futtasd újra ezt a telepítőt)."
        "the_shortcut" = "A `„TimeShift Co-op`” parancsikon (asztal, Start menü) elindítja a játékot, előtte pedig telepíti a GitHubról a mod újabb verzióját, ha van."
        "not_a_folder" = "Nem TimeShift mappa (nincs bin\TimeShift.exe): {0}"
        "not_found" = "A játék nem található: add meg a mappáját a -Game kapcsolóval."
        "removed_done" = "A kooperatív mod eltávolítva; a lecserélt fájlok és a játék beállításai visszaállítva."
        "is_installed" = "A TimeShift Co-op {0} telepítve van ide: {1}"
        "error" = "Hiba: "
        "subtitle" = "{0} verzió  -  kooperatív kampány a TimeShifthez, 2-4 játékos"
        "game_folder" = "A játék mappája"
        "browse" = "Tallózás..."
        "remove_co_op" = "Mod eltávolítása"
        "cancel" = "Mégse"
        "install" = "Telepítés"
        "copy_log" = "Napló másolása"
        "open_game_folder" = "Játékmappa megnyitása"
        "close" = "Bezárás"
        "please_wait" = "Várj, amíg befejeződik: ha közben leállítod, a játék félig módosítva maradhat."
        "choose_folder" = "A játék nem található automatikusan: válaszd ki a mappáját (azt, amelyikben a bin mappa van)."
        "no_exe" = "Ebben a mappában nincs bin\TimeShift.exe."
        "found" = "TimeShift 1.2 megtalálva."
        "other_games" = "A játék más példányai is megvannak: {0}`nA `„Tallózás`” gombbal másikat választhatsz."
        "found_other" = "TimeShift megtalálva, de az exe-je nem az 1.2-es patch: a mod nem telepíthető (bővebben a README-COOP.txt-ben)."
        "already" = "A kooperatív mod {0} már telepítve van: újra lesz telepítve."
        "reinstall" = "Újratelepítés"
        "will_update" = "A kooperatív mod {0} van telepítve: {1} verzióra frissül."
        "update" = "Frissítés"
        "what" = "A telepítő a saját játékfájljaidból elkészíti a moddadatokat (néhány másodperc), bemásolja a modot a játék mappájába, és megőriz minden lecserélt játékfájlt (coop-backup); az ottani `„Uninstall Co-op.bat`” mindent visszaállít. A profilok és a mentések érintetlenek maradnak."
        "same_version" = "Minden játékosnak ugyanaz a modverzió kell."
        "installing" = "A kooperatív mod {0} telepítése..."
        "st_check" = "A játék mappájának ellenőrzése"
        "st_paks" = "A moddadatok elkészítése a játékfájljaidból"
        "st_copy" = "A mod másolása (a lecserélt játékfájlok megmaradnak)"
        "st_finish" = "Parancsikonok és a telepítési adatok"
        "folder_log" = "A játék mappája: "
        "updated" = "Frissítve erről: {0}, erre: {1}"
        "installed_title" = "A kooperatív mod {0} telepítve"
        "install_failed" = "A telepítés nem sikerült"
        "log_in" = "A napló helye: %TEMP%\timeshift-coop-install.log. A játékból semmi sem veszett el: a lecserélt fájlok a coop-backup mappában vannak."
        "removing" = "A kooperatív mod eltávolítása..."
        "st_rm" = "A mod fájljainak és adatainak eltávolítása"
        "st_restore" = "A játék eredeti fájljainak visszaállítása"
        "st_clean" = "A biztonsági másolatok és a mod mappájának eltávolítása"
        "removed_title" = "A kooperatív mod eltávolítva"
        "removed_text" = "A kooperatív mod eltávolítva; a lecserélt fájlok és a bin\steamwars.ini visszaállítva.`n`nA profilokhoz és a mentésekhez (UserConfigs) nem nyúlt."
        "remove_failed" = "Az eltávolítás nem sikerült"
        "select_folder" = "Válaszd ki a TimeShift mappáját (azt, amelyikben a bin mappa van)"
        "remove_q" = "Eltávolítod a kooperatív modot innen:`n{0} ?"
        "remove_page" = "Eltávolítod a TimeShift Co-op {0} verzióját ebből a mappából?`n`nA játék eredeti fájljai és beállításai visszaállnak; a profilok és a mentések (UserConfigs) megmaradnak."
        "removed" = "Eltávolítva: "
        "shortcut" = "Parancsikon: "
        "shortcut_desc" = "TimeShift kooperatív móddal: előbb telepíti a mod újabb verzióját, ha van"
        "steam_accept" = "Értem: a TimeShift.exe és a fent felsorolt adatarchívumok a csomag fájljaira cserélődnek (az eredetik megmaradnak, az eltávolítás visszaállítja őket)"
        "steam_need_accept" = "Ebben a Steam-példányban (1.0-s patch) ezeket LECSERÉLNÉ a(z) {1} csomag fájljaira: {0} (a TimeShift.exe SHA-256-ja: {2}). Az eredetiket a coop-backup\steam10 mappa őrzi, és az eltávolítás pontosan visszateszi őket. Az elfogadáshoz futtasd újra a -AcceptSteamUpdate kapcsolóval. Semmi sem változott."
        "steam_pkg_exe" = "A csomag TimeShift.exe-je: SHA-256 {0}"
        "removed_steam" = "A Steam-példányod ismét olyan, mint volt (1.0-s patch): a TimeShift.exe, a steamwars.ini és az adatarchívumok pontosan visszaállítva a coop-backup\steam10 mappából."
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

function Sha256-Bytes([byte[]] $b) {
    [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($b)) -replace "-", ""
}

# What an exe is: ok (the patch 1.2 code), kind base / community / variant, whether the large-address-aware flag is set,
# and its SHA-256. Only the file's bytes are looked at; an exe that is not 1.2 code (the Steam edition's 1.0 build, a
# protected exe, another region) is not ok.
function Exe-Info-File($path) {
    $r = @{ ok = $false; kind = ""; laa = $false; sha = ""; size = 0 }
    $len = (Get-Item -LiteralPath $path).Length
    $r.size = $len
    if ($len -ne $script:EXE_SIZE) { $r.sha = Sha256-File $path; return $r }
    $b = [IO.File]::ReadAllBytes($path)
    $r.sha = Sha256-Bytes $b
    $pe = [BitConverter]::ToInt32($b, 0x3c)
    if ($pe -lt 64 -or $pe + 0x100 -gt $b.Length) { return $r }
    $r.laa = ([BitConverter]::ToUInt16($b, $pe + 22) -band 0x20) -ne 0
    # the same file with the flag cleared: the known exes (they are recognised by their own hashes, flag set or not)
    $plain = $r.sha
    if ($r.laa) { $b[$pe + 22] = [byte]($b[$pe + 22] -band 0xDF); $plain = Sha256-Bytes $b; $b[$pe + 22] = [byte]($b[$pe + 22] -bor 0x20) }
    if ($plain -eq $script:EXE_SHA) { $r.ok = $true; $r.kind = "base"; return $r }
    if ($plain -eq $script:EXE_SHA_COMMUNITY) { $r.ok = $true; $r.kind = "community"; return $r }
    if ($plain -eq $script:EXE_SHA_STEAMPKG) { $r.ok = $true; $r.kind = "update"; return $r }
    # other builds of the same code: the changeable bytes zeroed
    $opt = [BitConverter]::ToUInt16($b, $pe + 20)
    $nsec = [BitConverter]::ToUInt16($b, $pe + 6)
    $sec = $pe + 24 + $opt
    if ($nsec -lt 1 -or $sec + 40 * $nsec -gt $b.Length) { return $r }
    $masks = @(@(($pe + 0x58), 4), @(($pe + 22), 2), @(($sec + 8), 4), @(($sec + 40 * ($nsec - 1)), 8)) + $script:EXE_MASK
    foreach ($m in $masks) { [Array]::Clear($b, $m[0], $m[1]) }
    if ((Sha256-Bytes $b) -eq $script:EXE_NORM) { $r.ok = $true; $r.kind = "variant" }
    $r
}
function Exe-Info($g) { Exe-Info-File (Join-Path $g "bin\TimeShift.exe") }

# The exe's SHA-256 when it is not the 1.2 code the mod is made for, else $null.
function Exe-Mismatch($g) {
    $i = Exe-Info $g
    if ($i.ok) { return $null }
    $i.sha.ToLower()
}

# A Steam copy: by its place (steamapps\common, the app manifest, the Steam API files) - the exe's hash of the Steam
# build is not published, so "an exe that is not 1.2 code in a Steam folder" is what is looked for.
function Is-Steam($g) {
    try {
        $full = [IO.Path]::GetFullPath($g).TrimEnd('\')
        if ($full -match '(?i)\\steamapps\\common\\[^\\]+$') { return $true }
        if (Test-Path -LiteralPath (Join-Path (Split-Path (Split-Path $full -Parent) -Parent) "appmanifest_10130.acf")) { return $true }
        foreach ($f in "bin\steam_api.dll", "bin\steam_appid.txt", "steam_appid.txt", "bin\steam.dll") {
            if (Test-Path -LiteralPath (Join-Path $full $f)) { return $true }
        }
    } catch { }
    $false
}

# Candidate game folders, most likely first.
function Find-Games {
    $c = New-Object System.Collections.Generic.List[string]
    foreach ($d in @($here, (Split-Path $here -Parent), $script:setupDir)) { if ($d) { $c.Add($d) } }
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

function Same-File($a, $b) {
    (Test-Path -LiteralPath $a -PathType Leaf) -and (Test-Path -LiteralPath $b -PathType Leaf) -and
        ((Get-Item -LiteralPath $a).Length -eq (Get-Item -LiteralPath $b).Length) -and ((Sha256-File $a) -eq (Sha256-File $b))
}

# The machine and the exported names of a PE file, read from its bytes (nothing of it runs); $null for anything that is not one.
function Pe-Exports($p) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return $null }
    try { $b = [IO.File]::ReadAllBytes($p) } catch { return $null }
    $n = $b.Length
    if ($n -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return $null }
    $pe = [long][BitConverter]::ToUInt32($b, 0x3C)
    if ($pe + 26 -gt $n -or [Text.Encoding]::ASCII.GetString($b, [int]$pe, 4) -ne "PE`0`0") { return $null }
    $machine = [BitConverter]::ToUInt16($b, $pe + 4); $nsec = [BitConverter]::ToUInt16($b, $pe + 6); $optsize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24; $sec = $opt + $optsize
    if ($sec + 40 * $nsec -gt $n) { return $null }
    $magic = [BitConverter]::ToUInt16($b, $opt)
    $dirs = if ($magic -eq 0x10B) { $opt + 96 } elseif ($magic -eq 0x20B) { $opt + 112 } else { return $null }
    if ($dirs + 8 -gt $opt + $optsize) { return $null }
    # (an RVA's offset in the file by the section table, -1 outside the file)
    $secs = @(for ($i = 0; $i -lt $nsec; $i++) { $s = $sec + 40 * $i
        @{ va = [long][BitConverter]::ToUInt32($b, $s + 12); vs = [long][BitConverter]::ToUInt32($b, $s + 8); raw = [long][BitConverter]::ToUInt32($b, $s + 16); ptr = [long][BitConverter]::ToUInt32($b, $s + 20) } })
    $off = { param([long] $rva)
        foreach ($s in $secs) { $span = [Math]::Max($s.vs, $s.raw)
            if ($rva -ge $s.va -and $rva -lt $s.va + $span) { $o = $rva - $s.va; if ($o -ge $s.raw -or $s.ptr + $o -ge $n) { return -1 }; return $s.ptr + $o } }
        -1 }
    $names = New-Object System.Collections.Generic.HashSet[string]
    $erva = [long][BitConverter]::ToUInt32($b, $dirs)
    $ed = if ($erva) { & $off $erva } else { -1 }
    if ($ed -ge 0 -and $ed + 40 -le $n) {
        $cnt = [long][BitConverter]::ToUInt32($b, $ed + 24)
        $np = & $off ([long][BitConverter]::ToUInt32($b, $ed + 32))
        if ($np -ge 0 -and $cnt -le 65536) {
            for ($i = 0; $i -lt $cnt -and $np + 4 * $i + 4 -le $n; $i++) {
                $so = & $off ([long][BitConverter]::ToUInt32($b, $np + 4 * $i))
                if ($so -lt 0) { continue }
                $e = $so; while ($e -lt $n -and $b[$e] -ne 0 -and $e - $so -lt 512) { $e++ }
                [void]$names.Add([Text.Encoding]::ASCII.GetString($b, [int]$so, [int]($e - $so)))
            }
        }
    }
    @{ machine = $machine; names = $names }
}

# An Ultimate ASI Loader (ThirteenAG; the usual dinput8.dll of ASI mods) that can stand in for winmm.dll: 32-bit (the game is),
# exports IsUltimateASILoader (the name plugins and other loaders know it by) and winmm's timeGetTime (it serves winmm.dll).
# Both builds checked for Revelations 2 do: v10.0.2 and v7.8.0 (2026-11-07).
function Is-AsiLoader($p) {
    $x = Pe-Exports $p
    [bool]($x -and $x.machine -eq 0x14C -and $x.names.Contains("IsUltimateASILoader") -and $x.names.Contains("timeGetTime"))
}

# The player's ASI loader keeps running next to the mod (whose DLL is bin\dinput8.dll too). The loader knows itself by its file
# name only (any other name: "This library isn't supported." and the game closes), so the copy is named bin\winmm.dll: a name the
# loader serves (it forwards winmm's functions to the system's winmm.dll) and TimeShift.exe imports, so Windows loads it from bin at
# the start and it loads its plugins (bin\scripts, bin\plugins) as it did as dinput8.dll. The loader patches the exe's start-up
# imports and writes them back at its first call: the mod's DLL keeps its own hooks on that (native/iat_guard.c). The original stays
# in coop-backup\bin\dinput8.dll; the loader's settings file bin\dinput8.ini goes along as bin\winmm.ini (it reads <its name>.ini).
# Copy -> where it comes from (relative to the game folder). Same as Resident Evil Revelations 2 Co-op 0.2.0.
$script:CHAIN = [ordered]@{ "bin\winmm.dll" = "coop-backup\bin\dinput8.dll"; "bin\winmm.ini" = "bin\dinput8.ini" }
$script:PROXY = "bin\dinput8.dll"

# What the install does for a foreign bin\dinput8.dll kept in coop-backup: state "asi" (the loader runs as bin\winmm.dll), "off" (it
# stays off: not such a loader, or a winmm.dll of someone else's is in bin already), "" (none); keep = copies of an earlier install
# that are still the same as their source; make = copies to make. A copy that is no longer the same as its source is the player's
# now: it is left alone and no longer listed.
function Chain-Plan($g, $old, $backups) {
    $plan = @{ state = ""; keep = @(); make = @() }
    if ($backups -notcontains $script:PROXY) { return $plan }
    $plan.state = "off"
    if (-not (Is-AsiLoader (Join-Path $g "coop-backup\$($script:PROXY)"))) { return $plan }
    foreach ($name in $script:CHAIN.Keys) {
        $from = Join-Path $g $script:CHAIN[$name]; $dst = Join-Path $g $name
        if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { continue }
        if (Test-Path -LiteralPath $dst) {
            if (($old.chains -contains $name) -and (Same-File $dst $from)) { $plan.keep += $name }
            elseif ($name -eq "bin\winmm.dll") { Ui-Log (L "winmm_taken"); $plan.keep = @(); $plan.make = @(); return $plan }
        } else { $plan.make += $name }
        # (the settings file only goes along with the loader)
        if ($name -eq "bin\winmm.dll" -and $plan.keep -notcontains $name -and $plan.make -notcontains $name) { return $plan }
    }
    $plan.state = "asi"
    $plan
}

# The install record: version=, then file=, backup=, built=, shortcut= lines and arcname= (the
# value of bin\steamwars.ini's [Preload] arcName before the install).
function Read-Manifest($g) {
    $m = @{ files = @(); backups = @(); built = @(); shortcuts = @(); chains = @(); version = ""; arcname = ""; exe = "" }
    $f = Join-Path $g "coop\install.txt"
    if (Test-Path -LiteralPath $f) {
        foreach ($line in Get-Content -LiteralPath $f) {
            if ($line -match '^version=(.*)$') { $m.version = $Matches[1] }
            elseif ($line -match '^file=(.+)$') { $m.files += $Matches[1] }
            elseif ($line -match '^backup=(.+)$') { $m.backups += $Matches[1] }
            # (after 0.1.3: a copy the install made for the player's ASI loader, see Chain-Plan; also listed as file=)
            elseif ($line -match '^chain=(.+)$') { if ($script:CHAIN.Contains($Matches[1])) { $m.chains += $Matches[1] } }
            elseif ($line -match '^built=(.+)$') { $m.built += $Matches[1] }
            elseif ($line -match '^shortcut=(.+)$') { $m.shortcuts += $Matches[1] }
            elseif ($line -match '^arcname=(.+)$') { $m.arcname = $Matches[1] }
            elseif ($line -match '^exe=(.+)$') { $m.exe = $Matches[1] }
        }
    }
    if ($m.files.Count -eq 0) { return (Find-Coop $g) }
    # (an "original" that is a co-op DLL is not the game's)
    $m.backups = @($m.backups | Where-Object { -not (Is-CoopDll (Join-Path $g "coop-backup\$_")) })
    $m
}

# A co-op install whose record is lost: what is the mod's, found by the files themselves.
function Find-Coop($g) {
    $m = @{ files = @(); backups = @(); built = @(); shortcuts = @(); chains = @(); version = ""; arcname = ""; found = $false }
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
            $rel = $f.FullName.Substring($bk.Length + 1)
            # (steam10\ is the Steam update's own backup: coop\steam.txt names it)
            if (-not (Is-CoopDll $f.FullName) -and $rel -notmatch '(?i)^steam10\\') { $m.backups += $rel }
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
    if (-not $m.Success) { throw (L "ini_noarc") }
    $m.Groups[2].Value.Trim()
}
function Set-ArcName($g, $value) {
    $p = Ini-Path $g
    $enc = [Text.Encoding]::GetEncoding(28591)
    $t = $enc.GetString([IO.File]::ReadAllBytes($p))
    $m = $script:ARC_RX.Match($t)
    if (-not $m.Success) { throw (L "ini_noarc") }
    $g2 = $m.Groups[2]
    # (trailing blanks of the old value stay where they were)
    $old = $g2.Value.TrimEnd(" ", "`t")
    $t2 = $t.Substring(0, $g2.Index) + $value + $t.Substring($g2.Index + $old.Length)
    if ($t2 -ne $t) { Make-Writable $p; [IO.File]::WriteAllBytes($p, $enc.GetBytes($t2)) }
}

# ---------------------------------------------------------------------------------------------
# A Steam copy (patch 1.0) brought to 1.2 with the 1.2 update the player downloaded himself (the community's
# "TimeShift Update 1.2 (Steam)": a zip, or its extracted folder). Nothing is downloaded or shipped. The files the package
# replaces are kept in coop-backup\steam10 (once: an interrupted run's copy is the real original), the files it adds are
# recorded, both in coop\steam.txt (written BEFORE the first file is touched) and undone by the uninstall.

$script:STEAM_BACKUP = "coop-backup\steam10"
# not game data: programs, settings and saves are never taken from the package
$script:PKG_SKIP_DIRS = @("userconfigs", "coop", "coop-backup")
$script:PKG_SKIP_EXT = @(".exe", ".bat", ".cmd", ".ps1", ".vbs", ".js", ".msi", ".scr", ".lnk", ".com", ".url")

function Steam-Record($g) { Join-Path $g "coop\steam.txt" }

# The Steam update record: steam= (replaced, original in coop-backup\steam10), steamnew= (added).
function Read-Steam($g) {
    $s = @{ replaced = @(); added = @() }
    $f = Steam-Record $g
    if (Test-Path -LiteralPath $f) {
        foreach ($line in Get-Content -LiteralPath $f) {
            if ($line -match '^steam=(.+)$') { $s.replaced += $Matches[1] }
            elseif ($line -match '^steamnew=(.+)$') { $s.added += $Matches[1] }
        }
    }
    $s
}
function Write-Steam($g, $s) {
    New-Item -ItemType Directory -Force (Join-Path $g "coop") | Out-Null
    $lines = @($s.replaced | ForEach-Object { "steam=$_" }) + @($s.added | ForEach-Object { "steamnew=$_" })
    Set-Content -LiteralPath (Steam-Record $g) -Value $lines -Encoding UTF8
}

# Opens a package (zip file or folder): its game files by path relative to the game folder, or an error.
# The game folder is found by bin\TimeShift.exe: whatever folders lie above it in the package are dropped.
function Open-Package($path) {
    $r = @{ ok = $false; why = ""; gfiles = @(); skipped = 0; zip = $null; dir = $null; exe = $null; known = $false; exesha = "" }
    $path = "$path".Trim().Trim('"')
    if (-not $path -or -not (Test-Path -LiteralPath $path)) { $r.why = L "pkg_nothing"; return $r }
    $all = New-Object System.Collections.Generic.List[object]
    try {
        if (Test-Path -LiteralPath $path -PathType Container) {
            $r.dir = (Resolve-Path -LiteralPath $path).Path.TrimEnd('\')
            foreach ($f in Get-ChildItem -LiteralPath $r.dir -Recurse -File -Force) {
                $all.Add(@{ name = $f.FullName.Substring($r.dir.Length + 1); entry = $f.FullName; size = $f.Length })
            }
        } else {
            Add-Type -AssemblyName System.IO.Compression.FileSystem
            $r.zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $path).Path)
            foreach ($e in $r.zip.Entries) {
                if ($e.FullName.EndsWith("/") -or $e.FullName.EndsWith("\") -or -not $e.Name) { continue }
                $all.Add(@{ name = $e.FullName.Replace('/', '\'); entry = $e; size = $e.Length })
            }
        }
    } catch { Close-Package $r; $r.why = (L "pkg_read" $_.Exception.Message); return $r }
    # the prefix above bin\TimeShift.exe (the shortest one if there are several)
    $exeItem = $null
    foreach ($it in $all) {
        if ($it.name -match '(?i)^(.*\\)?bin\\timeshift\.exe$') {
            if (-not $exeItem -or $it.name.Length -lt $exeItem.name.Length) { $exeItem = $it }
        }
    }
    if (-not $exeItem) { Close-Package $r; $r.why = L "pkg_noexe"; return $r }
    $prefix = $exeItem.name.Substring(0, $exeItem.name.Length - "bin\TimeShift.exe".Length)
    $items = New-Object System.Collections.Generic.List[object]
    foreach ($it in $all) {
        if (-not $it.name.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { continue }
        $rel = $it.name.Substring($prefix.Length)
        $segs = $rel -split '\\'
        foreach ($sg in $segs) {
            if ($sg -eq ".." -or $sg -eq "." -or $sg -eq "" -or $sg.Contains(":") -or $sg -match '[\x00-\x1f<>"|?*]') {
                Close-Package $r; $r.why = (L "pkg_unsafe" $rel); return $r
            }
        }
        $ext = [IO.Path]::GetExtension($rel).ToLower()
        $isExe = $rel -ieq "bin\TimeShift.exe"
        if (-not $isExe -and ($script:PKG_SKIP_DIRS -contains $segs[0].ToLower() -or $script:PKG_SKIP_EXT -contains $ext -or $rel -ieq "bin\dinput8.dll")) {
            $r.skipped++
            continue
        }
        $items.Add(@{ rel = $rel; entry = $it.entry; size = $it.size })
    }
    $r.gfiles = $items.ToArray()
    # the 1.2 exe: looked at in a temporary copy before anything of the game is changed
    $exeEntry = ($r.gfiles | Where-Object { $_.rel -ieq "bin\TimeShift.exe" } | Select-Object -First 1)
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("timeshift-coop-pkgexe-" + $PID + ".tmp")
    try {
        if ($r.zip) { [IO.Compression.ZipFileExtensions]::ExtractToFile($exeEntry.entry, $tmp, $true) }
        else { Copy-Item -LiteralPath $exeEntry.entry $tmp -Force }
        $info = Exe-Info-File $tmp
    } finally { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    if (-not $info.ok) { Close-Package $r; $r.why = (L "pkg_exe_other" $($info.sha.ToLower())); return $r }
    $r.exesha = $info.sha.ToLower()
    if (-not ($r.gfiles | Where-Object { $_.rel -ieq "preload\paks\patch.s3darc" })) { Close-Package $r; $r.why = (L "pkg_nopaks" "patch.s3darc"); return $r }
    # the known package: every one of its files by SHA-256 (path, then content)
    $hit = 0
    foreach ($it in $r.gfiles) {
        $k = $it.rel.ToLower()
        if (-not $script:PKG_KNOWN.ContainsKey($k)) { continue }
        $tmp2 = Join-Path ([IO.Path]::GetTempPath()) ("timeshift-coop-pkgchk-" + $PID + ".tmp")
        try {
            if ($r.zip) { [IO.Compression.ZipFileExtensions]::ExtractToFile($it.entry, $tmp2, $true); $h = Sha256-File $tmp2 } else { $h = Sha256-File $it.entry }
        } finally { Remove-Item -LiteralPath $tmp2 -Force -ErrorAction SilentlyContinue }
        if ($h -eq $script:PKG_KNOWN[$k]) { $hit++ }
    }
    $r.known = ($hit -eq $script:PKG_KNOWN.Count)
    $r.ok = $true
    $r
}
# The files a package brings, for the consent text ("a, b, c").
$script:STEAM_DEFAULT_NAMES = "bin\TimeShift.exe, bin\steamwars.ini, preload\paks\patch.s3darc, patch_ati.s3darc, patch_nv.s3darc, patch_en_us.s3darc"
function Package-Names($r) { (@($r.gfiles | ForEach-Object { $_.rel }) -join ", ") }
function Close-Package($r) { if ($r.zip) { try { $r.zip.Dispose() } catch { }; $r.zip = $null } }

# Finds a package by itself: a zip in the Downloads folder or next to the installer that holds bin\TimeShift.exe and the
# 1.2 archive and is named like the update. Only a suggestion: the window shows it, the player confirms by installing.
function Find-Package {
    $dirs = @($here, (Split-Path $here -Parent), $script:setupDir, (Join-Path $env:USERPROFILE "Downloads")) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    foreach ($d in $dirs) {
        foreach ($z in @(Get-ChildItem -LiteralPath $d -Filter "*.zip" -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '(?i)time.?shift' -and $_.Name -notmatch '(?i)coop' -and $_.Length -lt 200MB })) {
            $p = Open-Package $z.FullName
            Close-Package $p
            if ($p.ok) { return $z.FullName }
        }
    }
    $null
}

# Puts the package's files into the game folder. $ran (replaced / added lists) is filled in as it goes, for the undo.
function Apply-Package($g, $pkg, $ran) {
    $st = Read-Steam $g
    $bk = Join-Path $g $script:STEAM_BACKUP
    $replaced = New-Object System.Collections.Generic.List[string]; foreach ($x in $st.replaced) { $replaced.Add($x) }
    $added = New-Object System.Collections.Generic.List[string]; foreach ($x in $st.added) { $added.Add($x) }
    # 1. the plan and the backups, recorded before the first file is replaced
    foreach ($it in $pkg.gfiles) {
        $dst = Join-Path $g $it.rel
        if (Test-Path -LiteralPath $dst -PathType Leaf) {
            if ($replaced -notcontains $it.rel -and $added -notcontains $it.rel) {
                $b = Join-Path $bk $it.rel
                if (-not (Test-Path -LiteralPath $b)) {
                    New-Item -ItemType Directory -Force (Split-Path $b -Parent) | Out-Null
                    Copy-Item -LiteralPath $dst $b -Force
                }
                $replaced.Add($it.rel); $ran.replaced += $it.rel
            }
        } elseif ($added -notcontains $it.rel) { $added.Add($it.rel); $ran.added += $it.rel }
    }
    Write-Steam $g @{ replaced = @($replaced); added = @($added) }
    # 2. the files
    $i = 0
    foreach ($it in $pkg.gfiles) {
        $i++
        Ui-Progress (3 + 12 * $i / [Math]::Max(1, $pkg.gfiles.Count)) $it.rel
        $dst = Join-Path $g $it.rel
        New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
        if (Test-Path -LiteralPath $dst -PathType Leaf) { (Get-Item -LiteralPath $dst).Attributes = "Normal" }
        if ($pkg.zip) { [IO.Compression.ZipFileExtensions]::ExtractToFile($it.entry, $dst, $true) }
        else { Copy-Item -LiteralPath $it.entry $dst -Force }
    }
}

# Puts back what Apply-Package changed (all of the record, or only $only's files of one run).
function Undo-Steam($g, $only, $st = $null) {
    if (-not $st) { $st = Read-Steam $g }
    $bk = Join-Path $g $script:STEAM_BACKUP
    $n = 0
    foreach ($rel in $st.replaced) {
        if ($only -and $only.replaced -notcontains $rel) { continue }
        $b = Join-Path $bk $rel
        if (Test-Path -LiteralPath $b) { Copy-Item -LiteralPath $b (Join-Path $g $rel) -Force; $n++ }
    }
    foreach ($rel in $st.added) {
        if ($only -and $only.added -notcontains $rel) { continue }
        Remove-Item -LiteralPath (Join-Path $g $rel) -Force -ErrorAction SilentlyContinue; $n++
    }
    if ($only) {
        $keepR = @($st.replaced | Where-Object { $only.replaced -notcontains $_ }); $keepA = @($st.added | Where-Object { $only.added -notcontains $_ })
        if ($keepR.Count -or $keepA.Count) { Write-Steam $g @{ replaced = $keepR; added = $keepA } }
        else {
            # nothing of the update is left: its backup copies and the folders made for it go too
            Remove-Item -LiteralPath (Steam-Record $g) -Force -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $bk -Recurse -Force -ErrorAction SilentlyContinue
            foreach ($d in (Join-Path $g "coop-backup"), (Join-Path $g "coop")) {
                if ((Test-Path -LiteralPath $d) -and -not (Get-ChildItem -LiteralPath $d -Force)) { Remove-Item -LiteralPath $d -Force -ErrorAction SilentlyContinue }
            }
        }
    }
    $n
}

# ---------------------------------------------------------------------------------------------
# Remove

function Do-Uninstall($g) {
    $m = Read-Manifest $g
    $steamRec = Read-Steam $g
    if ($m.files.Count -eq 0 -and $steamRec.replaced.Count -eq 0 -and $steamRec.added.Count -eq 0) { throw (L "not_installed" $g) }
    if (Game-Running $g) { throw (L "close_game") }
    Lock-Install
    $wp = Probe-Write $g
    if ($wp) { throw (L "write_denied" $wp) }
    Ui-Step "rm" "run"
    # The game's own archive first: whatever happens next, the game starts as it was.
    if ((Test-Path -LiteralPath (Ini-Path $g)) -and (Get-ArcName $g) -ieq "coop.s3darc") {
        $back = if ($m.arcname) { $m.arcname } else { "patch.s3darc" }
        Set-ArcName $g $back
        Ui-Log (L "ini_back" $back)
    }
    # (+ the log the mod's DLL writes next to the exe)
    $all = @($m.files + $m.built + "bin\ts_coop.log" + (1..9 | ForEach-Object { "bin\ts_coop.$_.log" }))
    $leftIn = New-Object System.Collections.Generic.List[string]
    $i = 0
    foreach ($rel in $all) {
        $i++
        Ui-Progress (5 + 50 * $i / [Math]::Max(1, $all.Count)) $rel
        $p = Join-Path $g $rel
        # (a copy made for the player's ASI loader goes only while it is still the same as its source: the loader in coop-backup,
        # the player's dinput8.ini)
        if ($m.chains -contains $rel -and (Test-Path -LiteralPath $p) -and -not (Same-File $p (Join-Path $g $script:CHAIN[$rel]))) {
            Ui-Log ((L "left_alone") + $rel); continue
        }
        # (a dinput8.dll another mod put over the co-op one after the install is the player's now: it stays, and the older original
        # in coop-backup is not put back over it)
        if ($rel -eq $script:PROXY -and (Test-Path -LiteralPath $p) -and -not (Is-CoopDll $p)) {
            Ui-Log ((L "left_in_place") + $rel); $leftIn.Add($rel); continue
        }
        Remove-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue
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
        if ($leftIn -contains $rel) { continue }
        $b = Join-Path $g "coop-backup\$rel"
        if (Test-Path -LiteralPath $b) { Copy-Item -LiteralPath $b (Join-Path $g $rel) -Force; Ui-Log ((L "put_back") + $rel) }
    }
    # a Steam copy brought to 1.2 by the install: the Steam files as they were (after the exe's own backup above)
    if ($steamRec.replaced.Count -or $steamRec.added.Count) {
        $n = Undo-Steam $g $null $steamRec
        Ui-Log (L "steam_restored" $n)
    }
    Ui-Step "restore" "done"
    Ui-Step "clean" "run"
    Ui-Progress 95 "coop, coop-backup"
    Remove-Item -LiteralPath (Join-Path $g "coop") -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $g "coop-backup") -Recurse -Force -ErrorAction SilentlyContinue
    Ui-Step "clean" "done"
    Ui-Progress 100 ""
    @{ removed = $m.version; steam = $(if ($steamRec.replaced.Count -or $steamRec.added.Count) { $n } else { 0 }); left = $leftIn.Count }
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

# Can the folders the install writes to be written (a read-only drive or folder, an ACL, a full disk)? The first problem as
# text, else $null. (Administrator rights are asked for before this: Need-Admin.)
function Probe-Write($g) {
    foreach ($d in @($g, (Join-Path $g "bin"), (Join-Path $g "preload\paks"))) {
        $probe = Join-Path $d ("coop-write-test-" + $PID + ".tmp")
        try { [IO.File]::WriteAllText($probe, "1"); Remove-Item -LiteralPath $probe -Force }
        catch { return ("{0} ({1})" -f $d, $_.Exception.Message) }
    }
    $null
}

# A file with the read-only flag (a game folder made read-only): the flag off before the installer rewrites it.
function Make-Writable($p) {
    if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { return }
    $f = Get-Item -LiteralPath $p -Force
    if ($f.Attributes -band [IO.FileAttributes]::ReadOnly) { $f.Attributes = $f.Attributes -bxor [IO.FileAttributes]::ReadOnly }
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
    $pk = if ($script:pkgPath) { $script:pkgPath } else { $SteamUpdate }
    if ($pk) { $a += @("-SteamUpdate", (Quote-Arg $pk)) }
    if ($AcceptSteamUpdate -or $script:steamAccepted) { $a += "-AcceptSteamUpdate" }
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
    $info = Exe-Info $g
    $pkgPath = if ($script:pkgPath) { $script:pkgPath } else { $SteamUpdate }
    if (-not $info.ok -and -not $pkgPath) {
        if (Is-Steam $g) { throw (L "exe_steam" $($info.sha.ToLower())) }
        throw (L "exe_wrong" $($info.sha.ToLower()))
    }
    Lock-Install
    $wp = Probe-Write $g
    if ($wp) { throw (L "write_denied" $wp) }
    $old = Read-Manifest $g
    if ($old.version) { Ui-Log ((L "installed_now") + $old.version) }
    if ($info.ok -and $info.kind -eq "community") { Ui-Log (L "notice_community") }
    if ($info.ok -and $info.kind -eq "variant") { Ui-Log (L "notice_variant") }
    Ui-Step "check" "done"

    # A Steam copy (patch 1.0): the player's own 1.2 update first, every replaced file kept.
    $ranSteam = $null
    if (-not $info.ok) {
        Ui-Step "steam" "run"
        Ui-Progress 2 $pkgPath
        $pkg = Open-Package $pkgPath
        if (-not $pkg.ok) { throw (L "pkg_bad" $pkg.why) }
        # the player's consent first: the window's tick, or -AcceptSteamUpdate; the same words either way
        $names = Package-Names $pkg
        if (-not ($AcceptSteamUpdate -or $script:steamAccepted)) {
            $why = L "steam_need_accept" $names $pkgPath $pkg.exesha
            Close-Package $pkg
            throw $why
        }
        Ui-Log (L "steam_need" $names)
        Ui-Log (L "steam_pkg_exe" $pkg.exesha)
        $ranSteam = @{ replaced = @(); added = @() }
        try {
            if ($pkg.skipped) { Ui-Log (L "pkg_skipped" $pkg.skipped) }
            Ui-Log (L "steam_applying" $pkgPath)
            Ui-Log $(if ($pkg.known) { L "steam_pkg_known" } else { L "steam_pkg_unknown" })
            Apply-Package $g $pkg $ranSteam
            $n = Exe-Info $g
            if (-not $n.ok) { throw (L "pkg_exe_other" $($n.sha.ToLower())) }
            foreach ($a in "patch.s3darc", "patch_ati.s3darc", "patch_nv.s3darc", "out.s3darc") {
                if (-not (Test-Path -LiteralPath (Join-Path $g "preload\paks\$a"))) { throw (L "paks_missing" $a) }
            }
        } catch {
            $why = $_.Exception.Message
            $null = Undo-Steam $g $ranSteam
            Ui-Log (L "steam_rollback")
            throw $why
        } finally { Close-Package $pkg }
        Ui-Log (L "steam_done" ($ranSteam.replaced.Count + $ranSteam.added.Count))
        $info = Exe-Info $g
        Ui-Step "steam" "done"
    }

    # The data first: a build that fails (another mod changed the menus, another game version)
    # leaves the game as it was.
    Ui-Step "paks" "run"
    Ui-Progress 15 "preload\paks\patch.s3darc"
    try { $pk = Build-Paks $g $payload }
    catch {
        $why = $_.Exception.Message
        if ($ranSteam) { $null = Undo-Steam $g $ranSteam; Ui-Log (L "steam_rollback") }
        throw $why
    }
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
    $exeRecord = $old.exe
    $chains = New-Object System.Collections.Generic.List[string]
    $writeRecord = {
        $lines = @("version=$version") + ($files | ForEach-Object { "file=$_" }) + ($backups | ForEach-Object { "backup=$_" }) +
            ($chains | ForEach-Object { "chain=$_" }) + ($built | ForEach-Object { "built=$_" }) + ($links | ForEach-Object { "shortcut=$_" })
        if ($arcOrig) { $lines += "arcname=$arcOrig" }
        if ($exeRecord) { $lines += "exe=$exeRecord" }
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
    # A dinput8.dll another mod put over the co-op one after an earlier install (an ASI loader, a fix): the player's now, kept like
    # any other file of someone else's before the co-op DLL goes back in. It replaces an older original in coop-backup (the file
    # the player put there last is the one the removal puts back); copies made from that older one (Chain-Plan) go with it and are
    # made again from this one.
    $proxyPath = Join-Path $g $script:PROXY
    if (($old.files -contains $script:PROXY) -and (Test-Path -LiteralPath $proxyPath) -and -not (Is-CoopDll $proxyPath)) {
        foreach ($c in $old.chains) {
            $cp = Join-Path $g $c
            if (Same-File $cp (Join-Path $g $script:CHAIN[$c])) { Remove-Item -LiteralPath $cp -Force -ErrorAction SilentlyContinue }
        }
        $bk = Join-Path $g "coop-backup\$($script:PROXY)"
        New-Item -ItemType Directory -Force (Split-Path $bk -Parent) | Out-Null
        Copy-Item -LiteralPath $proxyPath $bk -Force
        if ($backups -notcontains $script:PROXY) {
            $backups.Add($script:PROXY)
            # (in the record at once, the earlier install's files with it: a run that breaks off still puts it back)
            foreach ($f in $old.files) { $files.Add($f) }
            foreach ($c in $old.chains) { $chains.Add($c) }
            & $writeRecord
            $files.Clear(); $chains.Clear()
        }
        Ui-Log ((L "foreign_over_coop") + $script:PROXY)
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
    # The player's ASI loader (kept in coop-backup) runs on as bin\winmm.dll (Chain-Plan): the copies are in the record before they
    # are made, so a run that breaks off leaves nothing the uninstaller does not know.
    $plan = Chain-Plan $g $old $backups
    foreach ($nm in @($plan.keep) + @($plan.make)) { $chains.Add($nm); if ($files -notcontains $nm) { $files.Add($nm) } }
    if ($plan.make.Count) { & $writeRecord }
    foreach ($nm in $plan.make) { Copy-Item -LiteralPath (Join-Path $g $script:CHAIN[$nm]) (Join-Path $g $nm) -Force; Ui-Log ((L "asi_copy") + $nm) }
    # Files of an older version that this one no longer has (by the package's list: never a file it has; a loader copy the player
    # changed is his now).
    foreach ($rel in $old.files) {
        if ($packageFiles -notcontains $rel -and $files -notcontains $rel -and $rel -notin "coop-installer.ps1", "Uninstall Co-op.bat" -and $old.chains -notcontains $rel) {
            Remove-Item -LiteralPath (Join-Path $g $rel) -Force -ErrorAction SilentlyContinue
        }
    }
    # The uninstaller stays in the game folder.
    # (not onto itself when the archive was extracted into the game folder)
    $src, $dst = (Join-Path $here "coop-installer.ps1"), (Join-Path $g "coop-installer.ps1")
    if ([IO.Path]::GetFullPath($src) -ne [IO.Path]::GetFullPath($dst)) { Copy-Item -LiteralPath $src $dst -Force }
    if ($files -notcontains "coop-installer.ps1") { $files.Add("coop-installer.ps1") }
    $un = "Uninstall Co-op.bat"
    # (hidden, with the started-flag check of "Install Co-op.bat": a PowerShell that cannot start leaves a message)
    Set-Content -LiteralPath (Join-Path $g $un) -Encoding ASCII -Value @('@echo off', 'set "FLAG=%TEMP%\timeshift-coop-installer-started.flag"', 'del "%FLAG%" >nul 2>&1',
        'start "" powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0coop-installer.ps1" -Uninstall -Game "%~dp0."',
        'for /l %%i in (1,1,25) do (', '  if exist "%FLAG%" goto started', '  ping -n 2 127.0.0.1 >nul', ')',
        'echo TimeShift Co-op: the removal window did not start (PowerShell blocked?). Log: %TEMP%\timeshift-coop-install.log', 'pause', 'exit /b 1', ':started', 'del "%FLAG%" >nul 2>&1')
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
    # The exe made large-address-aware: only the original patch 1.2 exe (its SHA-256 is checked above), kept first in
    # coop-backup (the uninstall puts it back); a reinstall finds the patched one (EXE_SHA_LAA) and leaves it.
    $exeRel = "bin\TimeShift.exe"
    $exePath = Join-Path $g $exeRel
    $exeNowInfo = Exe-Info $g
    $exeNow = $exeNowInfo.sha
    if ($exeNowInfo.ok -and -not $exeNowInfo.laa) {
        & $keep $exeRel
        $bytes = [IO.File]::ReadAllBytes($exePath)
        $pe = [BitConverter]::ToInt32($bytes, 0x3c)
        $ch = [BitConverter]::ToUInt16($bytes, $pe + 22)
        $ch = $ch -bor 0x20
        $bytes[$pe + 22] = [byte]($ch -band 0xff)
        $bytes[$pe + 23] = [byte]($ch -shr 8)
        Make-Writable $exePath
        [IO.File]::WriteAllBytes($exePath, $bytes)
        $exeRecord = Sha256-File $exePath
        Ui-Log (L "large_address_aware")
    } elseif ($exeNowInfo.ok) {
        if (-not $exeRecord) { $exeRecord = $exeNow }
        Ui-Log (L "already_large_address_aware")
    }
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
    @{ old = $old.version; paks = $pk.state; links = $links.Count; chain = $plan.state }
}

function Install-Message($g, $r) {
    $msg = L "start_msg"
    if ($r.links -gt 0) { $msg += "`n`n" + (L "the_shortcut") }
    # (another mod's dinput8.dll: what became of it)
    if ($r.chain -eq "asi") { $msg += "`n`n" + (L "asi_note") } elseif ($r.chain -eq "off") { $msg += "`n`n" + (L "off_note") }
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
            $u = Do-Uninstall $g
            Save-Log ""
            Write-Host ((L "removed_done") + $(if ($u.left) { L "left_note" } else { "" }))
            if ($u.steam) { Write-Host (L "removed_steam") }
        } else {
            $r = Do-Install $g
            Save-Log $g
            Write-Host ((L "is_installed" $version $g) + "`n" + (Install-Message $g $r))
        }
        exit 0
    } catch {
        Ui-Log ((L "error") + $_.Exception.Message)
        $script:logLines.Add("at: " + ($_.ScriptStackTrace -replace "`r?`n", " | "))
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
$form.ClientSize = New-Object Drawing.Size(660, 560)
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
$pSetup.Location = New-Object Drawing.Point(0, 96); $pSetup.Size = New-Object Drawing.Size(660, 464)
$form.Controls.Add($pSetup)
$lbFolder = New-Label $pSetup "" 28 22 400 22 (Font 10 "Bold") $C.text
$tbGame = New-Object Windows.Forms.TextBox
$tbGame.Location = New-Object Drawing.Point(28, 48); $tbGame.Size = New-Object Drawing.Size(474, 28)
$tbGame.BackColor = $C.panel; $tbGame.ForeColor = $C.text; $tbGame.BorderStyle = "FixedSingle"; $tbGame.Font = Font 10.5
$pSetup.Controls.Add($tbGame)
$btBrowse = New-Button $pSetup (L "browse") 512 45 120 $false
$lbInfo = New-Label $pSetup "" 28 96 604 280 (Font 10.5) $C.text
$btRemove = New-Button $pSetup (L "remove_co_op") 28 412 150 $false
$btCancel = New-Button $pSetup (L "cancel") 372 412 120 $false
$btGo = New-Button $pSetup (L "install") 502 412 130 $true
# A Steam copy (patch 1.0): the 1.2 update package the player downloaded (zip or folder).
$lbPkg = New-Label $pSetup "" 28 244 604 22 (Font 10 "Bold") $C.text
$tbPkg = New-Object Windows.Forms.TextBox
$tbPkg.Location = New-Object Drawing.Point(28, 270); $tbPkg.Size = New-Object Drawing.Size(366, 28)
$tbPkg.BackColor = $C.panel; $tbPkg.ForeColor = $C.text; $tbPkg.BorderStyle = "FixedSingle"; $tbPkg.Font = Font 10.5
$pSetup.Controls.Add($tbPkg)
$btPkg = New-Button $pSetup (L "browse_zip") 402 267 110 $false
$btPkgDir = New-Button $pSetup (L "browse_dir") 520 267 112 $false
$lbPkgState = New-Label $pSetup "" 28 304 604 62 (Font 9) $C.dim
# the player's explicit consent (the Steam copy's exe and data archives are replaced)
$cbSteam = New-Object Windows.Forms.CheckBox
$cbSteam.Location = New-Object Drawing.Point(28, 368); $cbSteam.Size = New-Object Drawing.Size(604, 36)
$cbSteam.Font = Font 9.5; $cbSteam.ForeColor = $C.warn; $cbSteam.BackColor = [Drawing.Color]::Transparent
$cbSteam.FlatStyle = "Flat"; $cbSteam.UseMnemonic = $false
$pSetup.Controls.Add($cbSteam)
foreach ($ctl in $lbPkg, $tbPkg, $btPkg, $btPkgDir, $lbPkgState, $cbSteam) { $ctl.Visible = $false }

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
$script:exitCode = 0   # (a closed window is not a failure; a failed run sets 1)
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
    $steamMode = $false
    if (-not $g) {
        $lines += (L "choose_folder")
    } elseif (-not $ok) {
        $lines += ([string][char]0x2716 + "  " + (L "no_exe"))
    } else {
        $m = Read-Manifest $g
        $sr = Read-Steam $g
        if ($m.files.Count -gt 0 -or $sr.replaced.Count -gt 0 -or $sr.added.Count -gt 0) { $btRemove.Visible = $true }
        $ei = Exe-Info $g
        $steamMode = (-not $ei.ok) -and (Is-Steam $g)
        if ($steamMode) {
            $ok = Check-Package
            $lines += ([string][char]0x2716 + "  " + (L "found_steam_old"))
            $lines += ""
            $lines += (L "steam_need" $script:pkgNames)
            $cbSteam.Text = L "steam_accept"
            $ok = $ok -and $cbSteam.Checked
        } elseif (-not $ei.ok) {
            $ok = $false
            $lines += ([string][char]0x2716 + "  " + (L "found_other"))
            $lines += ""
            $lines += (L "exe_wrong" $($ei.sha.ToLower()))
        } else {
            $lines += ([string][char]0x2714 + "  " + (L "found"))
            if ($ei.kind -eq "community") { $lines += ([string][char]0x2022 + "  " + (L "notice_community")) }
            if ($ei.kind -eq "variant") { $lines += ([string][char]0x2022 + "  " + (L "notice_variant")) }
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
            $others = @($script:allGames | Where-Object { $_ -and $_ -ne $g })
            if ($others.Count) { $lines += ""; $lines += (L "other_games" ($others -join "; ")) }
        }
    }
    foreach ($ctl in $lbPkg, $tbPkg, $btPkg, $btPkgDir, $lbPkgState, $cbSteam) { $ctl.Visible = $steamMode }
    $lbInfo.Height = $(if ($steamMode) { 150 } else { 280 })
    $lbInfo.Font = $(if ($steamMode) { Font 9.5 } else { Font 10.5 })
    if ($steamMode) { $btGo.Text = L "install_steam" }
    $lbInfo.Text = $lines -join "`n"
    $lbInfo.ForeColor = $(if ($ok) { $C.text } else { $C.warn })
    $btGo.Enabled = $ok
    $btGo.BackColor = $(if ($ok) { $C.accent } else { $C.btn })
}

# The Steam update package row: looks for a package by itself the first time, checks what is in the box.
$script:pkgSearched = $false
$script:pkgChecked = ""
$script:pkgOkCache = $false
function Check-Package {
    $lbPkg.Text = L "steam_pick"
    if (-not $script:pkgSearched -and -not $tbPkg.Text.Trim()) {
        $script:pkgSearched = $true
        $first = if ($SteamUpdate) { $SteamUpdate } else { Find-Package }
        if ($first) { $script:settingPkg = $true; $tbPkg.Text = $first; $script:settingPkg = $false; if (-not $SteamUpdate) { $script:pkgFoundPath = $first } }
    }
    $p = $tbPkg.Text.Trim().Trim('"')
    $script:pkgPath = ""
    $script:pkgNames = $script:STEAM_DEFAULT_NAMES; $script:pkgSha = ""
    if (-not $p) { $lbPkgState.ForeColor = $C.dim; $lbPkgState.Text = L "steam_pkg_none"; return $false }
    $key = $p + "|" + $(if (Test-Path -LiteralPath $p) { (Get-Item -LiteralPath $p).LastWriteTimeUtc.Ticks } else { "-" })
    if ($key -ne $script:pkgChecked) {
        $script:pkgChecked = $key
        $r = Open-Package $p
        Close-Package $r
        $script:pkgOkCache = $r.ok
        $script:pkgNamesCache = if ($r.ok) { Package-Names $r } else { $script:STEAM_DEFAULT_NAMES }
        $script:pkgShaCache = if ($r.ok) { $r.exesha } else { "" }
        $script:pkgMsg = if ($r.ok) { (L "steam_pkg_ok" $r.gfiles.Count) + " " + $(if ($r.known) { L "steam_pkg_known" } else { L "steam_pkg_unknown" }) + $(if ($p -eq $script:pkgFoundPath) { "  " + (L "steam_pkg_found") } else { "" }) + "`n" + (L "steam_pkg_exe" $r.exesha) } else { L "pkg_bad" $r.why }
    }
    $lbPkgState.ForeColor = $(if ($script:pkgOkCache) { $C.ok } else { $C.warn })
    $lbPkgState.Text = $script:pkgMsg
    $script:pkgNames = $script:pkgNamesCache; $script:pkgSha = $script:pkgShaCache
    if ($script:pkgOkCache) { $script:pkgPath = $p }
    $script:pkgOkCache
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

# The result text gets the room it needs (measured): its label, then also the progress bar's place, then (after a success) the
# log's, then a smaller font. With the note about another mod's dinput8.dll it no longer fits the 140 px label.
function Fit-Result($ok) {
    $flags = [Windows.Forms.TextFormatFlags]::WordBreak
    $lbResText.Font = Font 10.5
    $need = { $lbResText.Top + [Windows.Forms.TextRenderer]::MeasureText($lbResText.Text, $lbResText.Font, (New-Object Drawing.Size($lbResText.Width, 100000)), $flags).Height }
    $rooms = @(($lbAction.Top - 6), ($tbLog.Top - 8))
    if ($ok) { $rooms += ($btClose.Top - 10) }
    $r = 0
    while ($r -lt $rooms.Count - 1 -and (& $need) -gt $rooms[$r]) { $r++ }
    foreach ($size in 10, 9.5, 9, 8.5) { if ((& $need) -le $rooms[$r]) { break }; $lbResText.Font = Font $size }
    $lbResText.Height = $rooms[$r] - $lbResText.Top
    $bar.Visible = $r -lt 1; $lbPct.Visible = $r -lt 1; $lbAction.Visible = $r -lt 1
    $tbLog.Visible = $r -lt 2
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
    Fit-Result $ok
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
    if ($cbSteam.Visible -and $cbSteam.Checked) { $script:steamAccepted = $true }
    $g = (Resolve-Path -LiteralPath $g).Path.TrimEnd('\')
    if (Elevated-Instead $g) { return }
    $script:target = $g
    $ic = Window-Icon $g
    if ($ic) { $form.Icon = $ic }
    $list = @(@("check", (L "st_check")))
    if (-not (Exe-Info $g).ok) { $list += , @("steam", (L "st_steam")) }
    $list += , @("paks", (L "st_paks"))
    $list += , @("copy", (L "st_copy"))
    $list += , @("finish", (L "st_finish"))
    Show-Work (L "installing" $version) $list
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
        $script:logLines.Add("at: " + ($_.ScriptStackTrace -replace "`r?`n", " | "))
        if ($script:ui.running) { $script:ui.running.icon.Text = [string][char]0x2716; $script:ui.running.icon.ForeColor = $C.bad }
        Save-Log $g
        $script:exitCode = 1
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
        $u = Do-Uninstall $g
        $script:exitCode = 0
        Save-Log ""
        Show-Result $true (L "removed_title") ((L "removed_text") + $(if ($u.steam) { "`n`n" + (L "removed_steam") } else { "" }) + $(if ($u.left) { L "left_note" } else { "" }))
        $btReadme.Visible = $false
        if ($AutoClose) { Close-Soon }
    } catch {
        Ui-Log ("ERROR: " + $_.Exception.Message)
        Save-Log ""
        $script:exitCode = 1
        Show-Result $false (L "remove_failed") ($_.Exception.Message + "`n`n" + (L "log_in"))
    }
}

$tbGame.Add_TextChanged({ Update-Setup })
$tbPkg.Add_TextChanged({ if (-not $script:settingPkg) { Update-Setup } })
$cbSteam.Add_CheckedChanged({ Update-Setup })
$btPkg.Add_Click({
    $dlg = New-Object Windows.Forms.OpenFileDialog
    $dlg.Title = L "steam_pick"
    $dlg.Filter = "ZIP (*.zip)|*.zip|*.*|*.*"
    $dd = Join-Path $env:USERPROFILE "Downloads"
    if (Test-Path -LiteralPath $dd) { $dlg.InitialDirectory = $dd }
    if ($dlg.ShowDialog($form) -eq "OK") { $tbPkg.Text = $dlg.FileName }
})
$btPkgDir.Add_Click({
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = L "steam_pick_dir"
    if ($dlg.ShowDialog($form) -eq "OK") { $tbPkg.Text = $dlg.SelectedPath }
})
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
    # tests: TS_INSTALLER_SHOT=<png> saves the window's picture a moment after it shows (no screen capture) and closes it
    if ($env:TS_INSTALLER_SHOT) {
        $script:shotTimer = New-Object Windows.Forms.Timer
        $script:shotTimer.Interval = 1500
        $script:shotTimer.Add_Tick({
            $script:shotTimer.Stop()
            try {
                $bmp = New-Object Drawing.Bitmap($form.Width, $form.Height)
                $form.DrawToBitmap($bmp, (New-Object Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
                $bmp.Save($env:TS_INSTALLER_SHOT, [Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
            } catch { }
            $script:busy = $false
            $form.Close()
        })
        $script:shotTimer.Start()
        # (the page only: nothing starts)
        if ($Game -and (Test-Game $Game)) { $tbGame.Text = (Resolve-Path -LiteralPath $Game).Path.TrimEnd('\'); return }
    }
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
        } elseif (-not (Exe-Info $tbGame.Text).ok -and -not $SteamUpdate -and -not $AutoClose) {
            # (a copy that needs the 1.2 update package: the page asks for it, nothing starts by itself)
            $btGo.Focus()
        } else {
            Start-Install $tbGame.Text
        }
    } else {
        $found = @(Find-Games)
        $script:allGames = $found
        if ($found.Count -ge 1) { $tbGame.Text = $found[0] } else { Update-Setup }
        $btGo.Focus()
    }
})

[Windows.Forms.Application]::Run($form)
# (the .bat files wait for the start flag; a run that came from the launcher has no .bat to clear it)
if ($AutoClose) { Remove-Item -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) "timeshift-coop-installer-started.flag") -Force -ErrorAction SilentlyContinue }
exit $script:exitCode
