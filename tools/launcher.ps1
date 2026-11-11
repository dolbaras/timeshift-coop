# TimeShift Co-op launcher (shipped as <game>\coop\coop-launcher.ps1; the installer's "TimeShift
# Co-op" shortcut runs it). Before the game starts it asks GitHub for the latest release of the
# mod; when that is newer than the installed one, it downloads the release's archive (checked
# against the SHA-256 GitHub gives for it), runs that version's installer for this game folder
# (its window shows the steps and closes by itself) and then starts bin\TimeShift.Exe from bin.
# Offline, on a missing release (404), on any error, or when the check takes too long, the game
# simply starts as it is.
# Usage: coop-launcher.ps1 [-NoUpdate] [-NoStart] [-Feed <url of a releases/latest JSON>]
param([switch] $NoUpdate, [switch] $NoStart, [string] $Feed = "")
$ErrorActionPreference = "Stop"
$coop = $PSScriptRoot
$gameDir = Split-Path $coop -Parent
$feedUrl = if ($Feed) { $Feed } elseif ($env:TIMESHIFT_COOP_FEED) { $env:TIMESHIFT_COOP_FEED } else {
    "https://api.github.com/repos/dolbaras/timeshift-coop/releases/latest" }
$log = Join-Path $coop "launcher-log.txt"
# Started from PowerShell 7, Windows PowerShell inherits its module path and some of its own
# commands fail to load: the installer started from here gets Windows PowerShell's own.
$env:PSModulePath = (@($env:PSModulePath -split ';') | Where-Object { $_ -and $_ -notmatch '\\PowerShell\\' }) -join ';'
function Log($s) { try { Add-Content -LiteralPath $log -Encoding UTF8 -Value ("{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $s) } catch { } }

$uiLang = (Get-UICulture).TwoLetterISOLanguageName
if ($uiLang -in "uk", "be", "kk") { $uiLang = "ru" }
$T = @{
    en = @{ title = "TimeShift Co-op"; found = "Co-op {0} is out (you have {1}). Downloading..."; verify = "Checking the download..."; failed = "The update could not be installed: {0}`nThe game starts with co-op {1}."; bad = "The download does not match the release's checksum."; nostart = "The game could not be started: {0}`nStart bin\TimeShift.exe yourself, or run the installer again. The log is coop\launcher-log.txt."; noinst = "the download holds no coop-installer.ps1 ({0})."; instexit = "its installer ended with exit code {0}." }
    ru = @{ title = "TimeShift Co-op"; found = "Вышел кооп-мод {0} (у вас {1}). Загрузка..."; verify = "Проверка загруженного файла..."; failed = "Обновление не удалось установить: {0}`nИгра запустится с кооп-модом {1}."; bad = "Загруженный файл не совпадает с контрольной суммой этой версии."; nostart = "Игру не удалось запустить: {0}`nЗапустите bin\TimeShift.exe сами или снова запустите установщик. Журнал: coop\launcher-log.txt."; noinst = "в загруженном файле нет coop-installer.ps1 ({0})."; instexit = "его установщик завершился с кодом {0}." }
    de = @{ title = "TimeShift Co-op"; found = "Koop-Mod {0} ist erschienen (Sie haben {1}). Download läuft..."; verify = "Download wird geprüft..."; failed = "Das Update konnte nicht installiert werden: {0}`nDas Spiel startet mit Koop-Mod {1}."; bad = "Der Download stimmt nicht mit der Prüfsumme der Version überein."; nostart = "Das Spiel konnte nicht gestartet werden: {0}`nStarten Sie bin\TimeShift.exe selbst oder führen Sie das Installationsprogramm erneut aus. Protokoll: coop\launcher-log.txt."; noinst = "der Download enthält keine coop-installer.ps1 ({0})."; instexit = "sein Installationsprogramm endete mit dem Code {0}." }
    fr = @{ title = "TimeShift Co-op"; found = "Le mod coop {0} est sorti (vous avez {1}). Téléchargement..."; verify = "Vérification du téléchargement..."; failed = "La mise à jour n'a pas pu être installée : {0}`nLe jeu démarre avec le mod coop {1}."; bad = "Le téléchargement ne correspond pas à la somme de contrôle de la version."; nostart = "Le jeu n'a pas pu être lancé : {0}`nLancez bin\TimeShift.exe vous-même ou relancez l'installateur. Journal : coop\launcher-log.txt."; noinst = "le téléchargement ne contient pas coop-installer.ps1 ({0})."; instexit = "son installateur s'est terminé avec le code {0}." }
    es = @{ title = "TimeShift Co-op"; found = "Ha salido el mod cooperativo {0} (tienes {1}). Descargando..."; verify = "Comprobando la descarga..."; failed = "No se pudo instalar la actualización: {0}`nEl juego se inicia con el mod cooperativo {1}."; bad = "La descarga no coincide con la suma de comprobación de la versión."; nostart = "No se pudo iniciar el juego: {0}`nInicia bin\TimeShift.exe manualmente o vuelve a ejecutar el instalador. Registro: coop\launcher-log.txt."; noinst = "la descarga no contiene coop-installer.ps1 ({0})."; instexit = "su instalador terminó con el código {0}." }
    it = @{ title = "TimeShift Co-op"; found = "È uscita la mod co-op {0} (hai la {1}). Download in corso..."; verify = "Verifica del download..."; failed = "Non è stato possibile installare l'aggiornamento: {0}`nIl gioco parte con la mod co-op {1}."; bad = "Il download non corrisponde al checksum della versione."; nostart = "Non è stato possibile avviare il gioco: {0}`nAvvia manualmente bin\TimeShift.exe o riesegui il programma di installazione. Registro: coop\launcher-log.txt."; noinst = "il download non contiene coop-installer.ps1 ({0})."; instexit = "il suo programma di installazione è terminato con il codice {0}." }
    pl = @{ title = "TimeShift Co-op"; found = "Wyszedł mod kooperacji {0} (masz {1}). Pobieranie..."; verify = "Sprawdzanie pobranego pliku..."; failed = "Nie udało się zainstalować aktualizacji: {0}`nGra uruchomi się z modem kooperacji {1}."; bad = "Pobrany plik nie zgadza się z sumą kontrolną wydania."; nostart = "Nie udało się uruchomić gry: {0}`nUruchom bin\TimeShift.exe samodzielnie albo ponownie uruchom instalator. Dziennik: coop\launcher-log.txt."; noinst = "pobrany plik nie zawiera coop-installer.ps1 ({0})."; instexit = "jego instalator zakończył się kodem {0}." }
    cs = @{ title = "TimeShift Co-op"; found = "Vyšel kooperativní mod {0} (máš {1}). Stahování..."; verify = "Kontrola staženého souboru..."; failed = "Aktualizaci se nepodařilo nainstalovat: {0}`nHra se spustí s kooperativním modem {1}."; bad = "Stažený soubor neodpovídá kontrolnímu součtu vydání."; nostart = "Hru se nepodařilo spustit: {0}`nSpusť bin\TimeShift.exe sám nebo znovu spusť instalátor. Protokol: coop\launcher-log.txt."; noinst = "stažený soubor neobsahuje coop-installer.ps1 ({0})."; instexit = "jeho instalátor skončil s kódem {0}." }
    hu = @{ title = "TimeShift Co-op"; found = "Megjelent a kooperatív mod {0} (neked {1} van). Letöltés..."; verify = "A letöltés ellenőrzése..."; failed = "A frissítést nem sikerült telepíteni: {0}`nA játék a kooperatív mod {1} verziójával indul."; bad = "A letöltés nem egyezik a kiadás ellenőrzőösszegével."; nostart = "A játékot nem sikerült elindítani: {0}`nIndítsd el magad a bin\TimeShift.exe-t, vagy futtasd újra a telepítőt. A napló: coop\launcher-log.txt."; noinst = "a letöltésben nincs coop-installer.ps1 ({0})."; instexit = "a telepítője {0} kilépési kóddal állt le." }
}
if (-not $T.ContainsKey($uiLang)) { $uiLang = "en" }
function L($k) { $T[$uiLang][$k] }

function Start-Game {
    $exe = Join-Path $gameDir "bin\TimeShift.Exe"
    Log ("start: $exe" + $(if ($NoStart) { " (-NoStart)" } else { "" }))
    if ($NoStart) { return }
    # (the game reads bin\steamwars.ini and its paths relative to bin)
    try { Start-Process -FilePath $exe -WorkingDirectory (Join-Path $gameDir "bin") }
    catch {
        # (the shortcut hides this window: a game that does not start must not fail in silence)
        Log "start failed: $($_.Exception.Message)"
        try {
            Add-Type -AssemblyName System.Windows.Forms
            [Windows.Forms.MessageBox]::Show(((L "nostart") -f $_.Exception.Message), (L "title"), "OK", "Error") | Out-Null
        } catch { }
    }
}

# What a failed update check was, in one line for the log: GitHub's rate limit (60 requests an hour without a login: 403 or 429
# with X-RateLimit-Remaining 0), no network, a missing release (404) or anything else. The game starts in every case.
function Explain-Error($ex) {
    $e = $ex
    while ($e.InnerException -and -not ($e -is [Net.WebException])) { $e = $e.InnerException }
    if ($e -is [Net.WebException] -and $e.Response) {
        $code = [int]$e.Response.StatusCode
        $left = $e.Response.Headers["X-RateLimit-Remaining"]
        $reset = $e.Response.Headers["X-RateLimit-Reset"]
        if (($code -eq 403 -or $code -eq 429) -and ($left -eq "0" -or $code -eq 429)) {
            $when = ""
            if ($reset -match '^\d+$') { $when = " (resets " + [DateTimeOffset]::FromUnixTimeSeconds([long]$reset).LocalDateTime.ToString("HH:mm") + ")" }
            return "GitHub's rate limit is used up${when}: no update check this time"
        }
        if ($code -eq 404) { return "no release yet (404)" }
        return "GitHub answered HTTP $code"
    }
    if ($e -is [Net.WebException]) { return "no network: " + $e.Message }
    $ex.Message
}

function Version-Of($s) {
    $s = ("$s".Trim()) -replace '^[vV]', ''
    try { return [version]$s } catch { return $null }
}

# A small window: one line and a bar.
$form = $null
function Show-Window($text) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    $script:form = New-Object Windows.Forms.Form
    $form.Text = L "title"; $form.FormBorderStyle = "FixedSingle"; $form.MaximizeBox = $false; $form.MinimizeBox = $false
    $form.StartPosition = "CenterScreen"; $form.ClientSize = New-Object Drawing.Size(460, 96)
    $form.BackColor = [Drawing.Color]::FromArgb(20, 24, 30); $form.ForeColor = [Drawing.Color]::FromArgb(230, 234, 240)
    $form.Font = New-Object Drawing.Font("Segoe UI", 10)
    $script:lbl = New-Object Windows.Forms.Label
    $lbl.Location = New-Object Drawing.Point(18, 16); $lbl.Size = New-Object Drawing.Size(424, 40); $lbl.Text = $text
    $form.Controls.Add($lbl)
    $bar = New-Object Windows.Forms.Panel
    $bar.Location = New-Object Drawing.Point(18, 64); $bar.Size = New-Object Drawing.Size(424, 10)
    $bar.BackColor = [Drawing.Color]::FromArgb(30, 36, 46)
    $script:fill = New-Object Windows.Forms.Panel
    $fill.Size = New-Object Drawing.Size(0, 10); $fill.BackColor = [Drawing.Color]::FromArgb(34, 124, 206)
    $bar.Controls.Add($fill); $form.Controls.Add($bar)
    try { $form.Icon = New-Object Drawing.Icon((Join-Path $coop "coop.ico"), 32, 32) } catch { }
    # a dark title bar like the installer's (Windows 10 2004+ / 11)
    $form.Add_HandleCreated({
        try {
            Add-Type -Namespace TsCoopLauncher -Name Dwm -MemberDefinition '[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int a, ref int v, int s);'
            $on = 1; [TsCoopLauncher.Dwm]::DwmSetWindowAttribute($this.Handle, 20, [ref]$on, 4) | Out-Null
        } catch { }
    })
    $form.Show()
    $form.Refresh()
    [Windows.Forms.Application]::DoEvents()
}
function Set-Progress($pct, $text) {
    if (-not $form) { return }
    if ($text) { $lbl.Text = $text }
    $fill.Width = [int](424 * [Math]::Max(0, [Math]::Min(100, $pct)) / 100)
    [Windows.Forms.Application]::DoEvents()
}

function Try-Update {
    $cur = Version-Of (Get-Content -LiteralPath (Join-Path $coop "version.txt") -TotalCount 1 -ErrorAction SilentlyContinue)
    if (-not $cur) { Log "no installed version (or a development build): no update check"; return }
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $req = [Net.HttpWebRequest]::Create($feedUrl)
    $req.UserAgent = "timeshift-coop-launcher/$cur"; $req.Accept = "application/vnd.github+json"; $req.Timeout = 6000; $req.ReadWriteTimeout = 6000
    $resp = $req.GetResponse()
    $json = (New-Object IO.StreamReader($resp.GetResponseStream())).ReadToEnd()
    $resp.Close()
    $rel = $json | ConvertFrom-Json
    $new = Version-Of $rel.tag_name
    Log "installed $cur, latest $($rel.tag_name)"
    if (-not $new -or $new -le $cur) { return }
    $asset = @($rel.assets | Where-Object { $_.name -like "timeshift-coop-*.zip" })[0]
    if (-not $asset) { Log "no archive in the release"; return }
    # (only the mod's own release archive; another feed is for tests)
    if (-not $Feed -and -not $env:TIMESHIFT_COOP_FEED -and "$($asset.browser_download_url)" -notlike "https://github.com/dolbaras/timeshift-coop/releases/download/*") {
        Log "unexpected download url $($asset.browser_download_url)"; return
    }
    Show-Window ((L "found") -f $new, $cur)
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("timeshift-coop-update-" + $new + "-" + $PID)
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $zip = Join-Path $tmp $asset.name
    # The download, chunk by chunk (the bar shows it).
    $dreq = [Net.HttpWebRequest]::Create($asset.browser_download_url)
    $dreq.UserAgent = "timeshift-coop-launcher/$cur"; $dreq.Timeout = 30000; $dreq.ReadWriteTimeout = 30000
    $dresp = $dreq.GetResponse()
    $total = [double]$dresp.ContentLength
    $in = $dresp.GetResponseStream()
    $out = [IO.File]::Create($zip)
    try {
        $buf = New-Object byte[] 65536; $got = 0.0
        while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
            $out.Write($buf, 0, $n); $got += $n
            if ($total -gt 0) { Set-Progress (100 * $got / $total) $null }
        }
    } finally { $out.Close(); $in.Close(); $dresp.Close() }
    Set-Progress 100 (L "verify")
    # GitHub gives each asset's SHA-256 ("digest": "sha256:...").
    if ($asset.digest -and "$($asset.digest)" -like "sha256:*") {
        $want = ("$($asset.digest)".Substring(7)).ToUpper()
        # (.NET, not Get-FileHash: a script cmdlet that fails to load when this runs under PowerShell 7's module path)
        $fs = [IO.File]::OpenRead($zip)
        try { $have = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($fs)) -replace "-", "" } finally { $fs.Close() }
        if ($have -ne $want) { throw (L "bad") }
        Log "checksum ok $have"
    } else { Log "the release gives no checksum" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, (Join-Path $tmp "x"))
    $inst = Join-Path $tmp "x\coop-installer.ps1"
    if (-not (Test-Path -LiteralPath $inst)) { throw ((L "noinst") -f $asset.name) }
    $form.Hide()
    # That version's installer, its window, for this folder; it closes by itself when done.
    $p = Start-Process powershell -PassThru -Wait -WindowStyle Hidden -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$inst`"", "-Game", "`"$gameDir`"", "-AutoClose", "-NoShortcut")
    Log "installer exit $($p.ExitCode)"
    Remove-Item -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) "timeshift-coop-installer-started.flag") -Force -ErrorAction SilentlyContinue
    if ($p.ExitCode -ne 0) { throw ((L "instexit") -f $p.ExitCode) }
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

# One launcher at a time (a double-clicked shortcut): the first one updates and starts the game.
$script:once = New-Object Threading.Mutex($false, "Local\TimeShiftCoopLauncher")
try { $first = $script:once.WaitOne(0) } catch [Threading.AbandonedMutexException] { $first = $true }
if (-not $first) { Log "another launcher runs"; exit 0 }
# This folder's game already running (another copy elsewhere does not count): nothing to do.
if (Get-Process -Name TimeShift -ErrorAction SilentlyContinue |
        Where-Object { -not $_.Path -or $_.Path.StartsWith($gameDir + "\", [StringComparison]::OrdinalIgnoreCase) }) { Log "the game runs already"; exit 0 }
if (-not $NoUpdate) {
    try { Try-Update } catch {
        $why = $_.Exception.Message
        Log ("update: " + (Explain-Error $_.Exception))
        # (offline, no release yet, GitHub slow: no message, the game just starts; a failed install is said)
        if ($form) {
            $form.Hide()
            $cur = Get-Content -LiteralPath (Join-Path $coop "version.txt") -TotalCount 1 -ErrorAction SilentlyContinue
            [Windows.Forms.MessageBox]::Show(((L "failed") -f $why, $cur), (L "title"), "OK", "Warning") | Out-Null
        }
    }
    if ($form) { $form.Close() }
}
Start-Game
