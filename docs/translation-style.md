# Translation style (TimeShift Co-op)

Every text the mod shows exists in the game's nine languages (English, Russian, German, French, Italian, Spanish,
Polish, Czech, Hungarian): in the game (`data/coop_strings.py`, appended to the game's own string tables), in the
installer and uninstaller (`tools/installer.ps1`), the shortcut's launcher (`tools/launcher.ps1`) and the setup exe
(`tools/mkinstaller/stub.c`). `tests/test_texts.py` checks the coverage, the placeholders, the font and the quotes.
The players' readmes are English and Russian only (`docs/release-readme*.txt`).

This file is the style each language keeps, so new texts read like the ones already there. Written 2026-11-11 for the
i18n review; no workspace-wide style guide existed then, so the shared words below are chosen to match the Jericho
Co-op+ guide (`docs/translation-style.md` there) where the games allow it.

## Rules for every language

- **The game's own words first.** A thing the game names keeps the game's name in that language: buttons and frames
  (`UI_BTN_*`, `UI_FRAME_*`), "health" (`UI_HEALTH_*`, not "life"), "checkpoint" (`UI_MSG_CHECKPOINT_REACHED`), the
  controller (`UI_INIVERSE_Y_HINT`), Ready / Not ready (`UI_BTN_READY`), Direct connection (`UI_BTN_DIRECT_CONNECT`).
  Look them up with `tests/test_texts.py --strings <extracted Strings folder>` or the tables themselves
  (`work\extracted\patch\Strings\strings_<language>.txt` in the lab).
- **Register as the game's texts**: one register per language everywhere (game, installer, launcher, setup exe). A
  message to everybody in the session may use the plural of the same register (Italian "Giocate", Spanish "Jugad").
- **Menu buttons in capitals** like the stock buttons next to them (`COOP_BTN_*`). French capitals carry no accents
  there, as the stock French buttons (HEBERGER, CREER): ECRAN PARTAGE.
- **Placeholders**: `{STRING}` / `{PARAM}` in the game, `{0}` `{1}` in the tools; never reordered in a way the code
  does not support (the game fills `{STRING}` in order).
- **Font**: only characters `FONT_GLYPHS` lists (the stock tables use them). It has „ “ ” and the Latin and Cyrillic
  letters of the nine languages (Ё/ё included), but NO « » — and no em dash or ellipsis character: in the game write
  " - " and "...", as the game does.
- **Quotes around a name of a button or menu item**: in the game straight "..." for Russian, French, Italian and
  Spanish (their « » are not in the font), the language's own quotes where the font has them (de „…“, pl „…”,
  cs „…“, hu „…”). In the installer, launcher and setup exe (Windows' fonts): ru «…», de „…“, fr « … », es «…»,
  it «…», pl „…”, cs „…“, hu „…”; English keeps '…'.
- **PowerShell trap**: PowerShell reads „ “ ” as its own double quotes (and ‘ ’ ‚ as single ones). In
  `installer.ps1` / `launcher.ps1` every such quote needs a backtick: `` "Der Ordner `„files`“ fehlt" ``. Without it
  the installer does not parse; `test_texts.py` fails on it.
- **Colon**: a capital after a colon only where the language wants one (German before a full sentence: "Das Spiel
  wurde nicht gefunden: Geben Sie ..."). French puts a space before : ; ? ! in every text (TimeShift's French has
  always done so).
- **Counts**: "Removed {0} file(s)" style texts use the language's neutral form ("Удалено файлов: {0}.").
- **Length**: buttons and option rows have room for the stock texts of the same row (the Host frame's player-limit
  row was too short for the stock Czech text: `COOP_HOST_MAXPLAYERS`). A new text no longer than the longest stock
  text of that kind is safe; anything longer needs a look in the game.

## Shared words

| Concept | ru | de | fr | it | es | pl | cs | hu |
|---|---|---|---|---|---|---|---|---|
| the mod (in the tools) | кооп-мод | der Koop-Mod | le mod coop | la mod co-op | el mod cooperativo | mod kooperacji | kooperativní mod | kooperatív mod |
| co-op (menu entry) | Кооператив | Kooperativ | Coopération | Cooperativa | Cooperativo | Kooperacja | Kooperace | Kooperatív |
| co-op (adjective) | кооперативный | Koop- | coop / coopératif | cooperativa / co-op | cooperativo | kooperacji | kooperativní | kooperatív |
| host (person) | хост | Host | hôte | host | anfitrión | gospodarz | hostitel | házigazda |
| host (verb) | создать игру | hosten | héberger | ospitare | hospedar | poprowadzić grę | založit hru | játék indítása |
| split screen | разделённый экран | geteilter Bildschirm | écran partagé | schermo condiviso | pantalla dividida | podzielony ekran | rozdělená obrazovka | osztott képernyő |
| controller | геймпад | Controller | manette | controller | mando | kontroler | ovladač | irányító |
| checkpoint | контрольная точка | Kontrollpunkt | point de sauvegarde | checkpoint | punto de control | punkt kontrolny | kontrolní bod | ellenőrzési pont |
| health | здоровье | Gesundheit | santé | salute | salud | zdrowie | zdraví | életerő |
| pause menu | меню паузы | Pausenmenü | menu pause | menu di pausa | menú de pausa | menu pauzy | menu pauzy | szünetmenü |
| ready (stock) | ГОТОВ / НЕ ГОТОВ | BEREIT | PRET | PRONTO | PREPARADO | GOTOWY | PŘIPRAVEN | KÉSZ |
| install / remove | установить / удалить | installieren / entfernen | installer / supprimer | installare / rimuovere | instalar / quitar, desinstalar | zainstalować / usunąć | nainstalovat / odebrat | telepítés / eltávolítás |
| update | обновление | Update, aktualisieren | mise à jour | aggiornamento | actualización | aktualizacja | aktualizace | frissítés |
| version of a release | версия (not "релиз") | Version | version | versione | versión | wersja | verze | verzió |

## Per language

- **English**: the source. "co-op" lower case in running text, "Co-op" in titles. "health", not "life".
- **Russian**: «вы», polite imperatives ("Подключите"). "хост" for the host (the game says "сервер" for the machine;
  the person who hosts is "хост"), "кооп-мод" for the mod, "версия" (not "релиз"/"выпуск"). ё is written. Installer
  quotes «…», in-game straight "…" (no « » in the font). Dash: " - " in the game, "—" in the tools.
- **German**: "Sie" like the game ("Drücken Sie ..."), "der Koop-Mod" (masculine), "Controller", "Gesundheit",
  "wiederherstellen" (not "zurücklegen"), quotes „…“, a capital after a colon that opens a sentence.
- **French**: "vous", "le mod coop", "hôte", "manette", the game's "point de sauvegarde" and "santé", "installateur",
  quotes « … » in the tools and "…" in the game, a space before : ; ? !.
- **Italian**: "tu" (a group addressed with "voi"), "la mod co-op" (feminine), "host", "controller" (the game's),
  "vicino a" for "to someone" (never "da te"/"dall'host": it reads "by"), quotes «…» / "…" in the game.
- **Spanish** (Spain): "tú" everywhere, also in the setup exe; the group "vosotros"; "anfitrión", the stock
  "hospedar", "mando", "salud", "el mod cooperativo", quotes «…» / "…" in the game.
- **Polish**: "ty"; "gospodarz" (the game's), "kontroler" (the game's), "mod kooperacji", "w kooperacji"; quotes „…”.
- **Czech**: "ty" (a group: "Hrajte"); "kooperativní" (not "kooperační"), "hostitel", "ovladač", "menu" (as the
  game's "HLAVNÍ MENU", not "nabídka"); quotes „…“.
- **Hungarian**: "te"; "házigazda" (not "hoszt"), "irányító" (the game's), "ellenőrzési pont", "életerő",
  "szünetmenü" (one word); quotes „…”.
