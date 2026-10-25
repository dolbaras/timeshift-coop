"""The co-op menu's texts, appended to every string table of the game (the game picks the table of its
language: bin/steamwars.ini [Language]). Tables are UTF-16LE lines KEY<TAB x4>"text" with CRLF; "\\n" in a
text is a line break, {STRING} is filled from the text's arguments ("KEY,arg1,arg2").

A table of a language not listed here gets the English text (and the build says so).
The menu also uses the game's own strings: UI_BTN_LAN_SERVER, UI_BTN_LAN_CLIENT, UI_BTN_IP_CLIENT,
UI_BTN_DIRECT_CONNECT, UI_ADVSERV_DIFFICULTY, UI_ADVSERV_DIFF_0..2, UI_ADVSERV_MAX_PLAYERS,
UI_CREATE_SERVER_NETWORK, UI_CONNECTION_1, UI_CREATE_SERVER_INTERNET, UI_PLAYER_SETUP_PASSWORD,
UI_BTN_CREATE, UI_BTN_BACK and the level names GAME_<level>.
"""

LANGS = ["english", "russian", "german", "french", "italian", "spanish", "polish", "czech", "hungarian"]

STRINGS = [
    ("COOP_BTN_COOP", {   # main menu entry (the stock entries are upper case)
        "english": "CO-OP",
        "russian": "КООПЕРАТИВ",
        "german": "KOOPERATIV",
        "french": "COOPERATION",
        "italian": "COOPERATIVA",
        "spanish": "COOPERATIVO",
        "polish": "KOOPERACJA",
        "czech": "KOOPERACE",
        "hungarian": "KOOPERATÍV",
    }),
    ("COOP_FRAME_COOP", {   # frame title
        "english": "Co-op",
        "russian": "Кооператив",
        "german": "Kooperativ",
        "french": "Coopération",
        "italian": "Cooperativa",
        "spanish": "Cooperativo",
        "polish": "Kooperacja",
        "czech": "Kooperace",
        "hungarian": "Kooperatív",
    }),
    ("COOP_HINT_MENU", {
        "english": "Play the story campaign together: 2 to 4 players over a local network or the Internet. "
                   "Co-op servers have [COOP] in front of their name.",
        "russian": "Сюжетная кампания вместе: от 2 до 4 игроков по локальной сети или через Интернет. "
                   "Имена кооперативных серверов начинаются с [COOP].",
        "german": "Spielt die Kampagne gemeinsam: 2 bis 4 Spieler im lokalen Netzwerk oder über das Internet. "
                  "Koop-Server haben [COOP] vor ihrem Namen.",
        "french": "Jouez la campagne ensemble : 2 à 4 joueurs en réseau local ou sur Internet. "
                  "Le nom des serveurs coopératifs commence par [COOP].",
        "italian": "Giocate insieme la campagna: da 2 a 4 giocatori in rete locale o su Internet. "
                   "I server cooperativi hanno [COOP] davanti al nome.",
        "spanish": "Jugad juntos la campaña: de 2 a 4 jugadores en red local o por Internet. "
                   "Los servidores cooperativos llevan [COOP] delante del nombre.",
        "polish": "Wspólna kampania: od 2 do 4 graczy w sieci lokalnej lub przez Internet. "
                  "Nazwy serwerów kooperacji zaczynają się od [COOP].",
        "czech": "Hrajte kampaň společně: 2 až 4 hráči v místní síti nebo přes Internet. "
                 "Kooperační servery mají před názvem [COOP].",
        "hungarian": "Játsszátok együtt a kampányt: 2-4 játékos helyi hálózaton vagy az interneten. "
                     "A kooperatív szerverek neve előtt [COOP] áll.",
    }),
    ("COOP_FRAME_HOST", {
        "english": "Host Co-op Game",
        "russian": "Создать кооперативную игру",
        "german": "Koop-Spiel hosten",
        "french": "Héberger une partie coopérative",
        "italian": "Ospita partita cooperativa",
        "spanish": "Hospedar partida cooperativa",
        "polish": "Prowadź grę w kooperacji",
        "czech": "Založit kooperativní hru",
        "hungarian": "Kooperatív játék indítása",
    }),
    ("COOP_ERR_VERSION", {   # {STRING} 1 = the host's mod version, 2 = this player's
        "english": "The host plays with co-op mod version {STRING}, you have version {STRING}. "
                   "Install the same version as the host to join.",
        "russian": "У сервера мод кооператива версии {STRING}, у вас версия {STRING}. "
                   "Установите ту же версию, что у сервера.",
        "german": "Der Host spielt mit der Koop-Mod-Version {STRING}, du hast Version {STRING}. "
                  "Installiere dieselbe Version wie der Host.",
        "french": "L'hôte utilise la version {STRING} du mod coopératif, vous avez la version {STRING}. "
                  "Installez la même version que l'hôte.",
        "italian": "L'host usa la versione {STRING} della mod cooperativa, tu hai la versione {STRING}. "
                   "Installa la stessa versione dell'host.",
        "spanish": "El anfitrión usa la versión {STRING} del mod cooperativo, tú tienes la versión {STRING}. "
                   "Instala la misma versión que el anfitrión.",
        "polish": "Gospodarz ma mod kooperacji w wersji {STRING}, ty masz wersję {STRING}. "
                  "Zainstaluj tę samą wersję co gospodarz.",
        "czech": "Hostitel hraje s kooperačním modem verze {STRING}, ty máš verzi {STRING}. "
                 "Nainstaluj stejnou verzi jako hostitel.",
        "hungarian": "A házigazda a kooperatív mod {STRING} verzióját használja, neked a {STRING} verzió van meg. "
                     "Telepítsd ugyanazt a verziót, mint a házigazda.",
    }),
    ("COOP_MOD_VERSION", {   # {STRING} = version
        "english": "Co-op mod {STRING}",
        "russian": "Мод кооператива {STRING}",
        "german": "Koop-Mod {STRING}",
        "french": "Mod coopératif {STRING}",
        "italian": "Mod cooperativa {STRING}",
        "spanish": "Mod cooperativo {STRING}",
        "polish": "Mod kooperacji {STRING}",
        "czech": "Kooperační mod {STRING}",
        "hungarian": "Kooperatív mod {STRING}",
    }),
    # Split screen (Co-op menu -> Split screen; native/split.c)
    ("COOP_BTN_SPLIT", {   # Co-op menu entry (upper case like the stock entries next to it)
        "english": "SPLIT SCREEN",
        "russian": "РАЗДЕЛЁННЫЙ ЭКРАН",
        "german": "GETEILTER BILDSCHIRM",
        "french": "ÉCRAN PARTAGÉ",
        "italian": "SCHERMO CONDIVISO",
        "spanish": "PANTALLA DIVIDIDA",
        "polish": "PODZIELONY EKRAN",
        "czech": "ROZDĚLENÁ OBRAZOVKA",
        "hungarian": "OSZTOTT KÉPERNYŐ",
    }),
    ("COOP_FRAME_SPLIT", {   # frame title
        "english": "Split Screen",
        "russian": "Разделённый экран",
        "german": "Geteilter Bildschirm",
        "french": "Écran partagé",
        "italian": "Schermo condiviso",
        "spanish": "Pantalla dividida",
        "polish": "Podzielony ekran",
        "czech": "Rozdělená obrazovka",
        "hungarian": "Osztott képernyő",
    }),
    ("COOP_HINT_SPLIT", {
        "english": "Two players on this PC, the screen split in two. Connect two gamepads: player 2 plays "
                   "with the second one, player 1 with the first one or keyboard and mouse.",
        "russian": "Два игрока на этом ПК, экран разделён на две части. Подключите два геймпада: игрок 2 "
                   "играет на втором, игрок 1 на первом или на клавиатуре и мыши.",
        "german": "Zwei Spieler an diesem PC, der Bildschirm wird geteilt. Schließe zwei Gamepads an: "
                  "Spieler 2 spielt mit dem zweiten, Spieler 1 mit dem ersten oder mit Tastatur und Maus.",
        "french": "Deux joueurs sur ce PC, l'écran partagé en deux. Branchez deux manettes : le joueur 2 "
                  "joue avec la deuxième, le joueur 1 avec la première ou au clavier et à la souris.",
        "italian": "Due giocatori su questo PC, lo schermo diviso in due. Collega due gamepad: il giocatore 2 "
                   "usa il secondo, il giocatore 1 il primo oppure tastiera e mouse.",
        "spanish": "Dos jugadores en este PC, con la pantalla dividida en dos. Conecta dos mandos: el jugador 2 "
                   "juega con el segundo y el jugador 1 con el primero o con teclado y ratón.",
        "polish": "Dwóch graczy na tym komputerze, ekran podzielony na dwie części. Podłącz dwa pady: gracz 2 "
                  "gra na drugim, gracz 1 na pierwszym albo na klawiaturze i myszy.",
        "czech": "Dva hráči na tomto PC, obrazovka rozdělená na dvě části. Připoj dva gamepady: hráč 2 hraje "
                 "na druhém, hráč 1 na prvním nebo na klávesnici a myši.",
        "hungarian": "Két játékos ezen a gépen, a képernyő kettéosztva. Csatlakoztass két gamepadet: a 2. játékos "
                     "a másodikkal játszik, az 1. játékos az elsővel vagy billentyűzettel és egérrel.",
    }),
    ("COOP_ERR_PAD2", {
        "english": "Split screen needs a second gamepad for player 2. Connect two gamepads and try again.",
        "russian": "Для разделённого экрана нужен второй геймпад для игрока 2. Подключите два геймпада и попробуйте снова.",
        "german": "Der geteilte Bildschirm braucht ein zweites Gamepad für Spieler 2. Schließe zwei Gamepads an "
                  "und versuche es erneut.",
        "french": "L'écran partagé a besoin d'une deuxième manette pour le joueur 2. Branchez deux manettes et "
                  "réessayez.",
        "italian": "Lo schermo condiviso richiede un secondo gamepad per il giocatore 2. Collega due gamepad e "
                   "riprova.",
        "spanish": "La pantalla dividida necesita un segundo mando para el jugador 2. Conecta dos mandos y "
                   "vuelve a intentarlo.",
        "polish": "Podzielony ekran wymaga drugiego pada dla gracza 2. Podłącz dwa pady i spróbuj ponownie.",
        "czech": "Rozdělená obrazovka potřebuje druhý gamepad pro hráče 2. Připoj dva gamepady a zkus to znovu.",
        "hungarian": "Az osztott képernyőhöz kell egy második gamepad a 2. játékosnak. Csatlakoztass két "
                     "gamepadet, és próbáld újra.",
    }),
    ("COOP_BTN_START", {   # starts the chosen level (upper case like the stock buttons)
        "english": "START",
        "russian": "НАЧАТЬ",
        "german": "STARTEN",
        "french": "COMMENCER",
        "italian": "INIZIA",
        "spanish": "EMPEZAR",
        "polish": "ROZPOCZNIJ",
        "czech": "SPUSTIT",
        "hungarian": "INDÍTÁS",
    }),
    # Video options (PC): screen mode, applied at the next start (native/display.c)
    ("COOP_VIDEO_DISPLAY", {
        "english": "Screen mode",
        "russian": "Режим экрана",
        "german": "Bildschirmmodus",
        "french": "Mode d'affichage",
        "italian": "Modalità schermo",
        "spanish": "Modo de pantalla",
        "polish": "Tryb ekranu",
        "czech": "Režim obrazovky",
        "hungarian": "Képernyőmód",
    }),
    ("COOP_VIDEO_DISPLAY_HINT", {
        "english": "Full screen, full screen without a frame (quick switching to other programs) or a window. "
                   "Takes effect at the next start of the game.",
        "russian": "Полный экран, полный экран без рамки (быстрое переключение на другие программы) или окно. "
                   "Вступает в силу при следующем запуске игры.",
        "german": "Vollbild, rahmenloses Vollbild (schneller Wechsel zu anderen Programmen) oder Fenster. "
                  "Wirkt beim nächsten Spielstart.",
        "french": "Plein écran, plein écran sans bordure (passage rapide aux autres programmes) ou fenêtre. "
                  "Pris en compte au prochain lancement du jeu.",
        "italian": "Schermo intero, schermo intero senza bordi (passaggio rapido ad altri programmi) o finestra. "
                   "Ha effetto al prossimo avvio del gioco.",
        "spanish": "Pantalla completa, pantalla completa sin bordes (cambio rápido a otros programas) o ventana. "
                   "Se aplica en el próximo inicio del juego.",
        "polish": "Pełny ekran, pełny ekran bez ramki (szybkie przełączanie do innych programów) lub okno. "
                  "Działa od następnego uruchomienia gry.",
        "czech": "Celá obrazovka, celá obrazovka bez rámečku (rychlé přepínání do jiných programů) nebo okno. "
                 "Projeví se při příštím spuštění hry.",
        "hungarian": "Teljes képernyő, keret nélküli teljes képernyő (gyors váltás más programokra) vagy ablak. "
                     "A játék következő indításakor lép életbe.",
    }),
    ("COOP_VIDEO_FULL", {
        "english": "Full screen",
        "russian": "Полный экран",
        "german": "Vollbild",
        "french": "Plein écran",
        "italian": "Schermo intero",
        "spanish": "Pantalla completa",
        "polish": "Pełny ekran",
        "czech": "Celá obrazovka",
        "hungarian": "Teljes képernyő",
    }),
    ("COOP_VIDEO_BORDERLESS", {
        "english": "Borderless",
        "russian": "Без рамки",
        "german": "Rahmenlos",
        "french": "Sans bordure",
        "italian": "Senza bordi",
        "spanish": "Sin bordes",
        "polish": "Bez ramki",
        "czech": "Bez rámečku",
        "hungarian": "Keret nélkül",
    }),
    ("COOP_VIDEO_WINDOW", {
        "english": "Window",
        "russian": "Окно",
        "german": "Fenster",
        "french": "Fenêtre",
        "italian": "Finestra",
        "spanish": "Ventana",
        "polish": "Okno",
        "czech": "Okno",
        "hungarian": "Ablak",
    }),
]


def table_line(key, text):
    return '%s\t\t\t\t"%s"\r\n' % (key, text.replace('"', '\\"').replace("\n", "\\n"))


def build(ctx):
    for table in ctx.string_tables():
        lang = table.lower().replace("strings_", "", 1)
        member = "Strings/" + table
        text = ctx.text(member)
        keys = {line.split("\t", 1)[0] for line in text.split("\r\n")}
        if lang not in LANGS:
            ctx.log("strings: no texts for %s, English used" % table)
        add = []
        for key, texts in STRINGS:
            if key in keys:
                raise ValueError("%s: the game already has a string %s" % (table, key))
            add.append(table_line(key, texts.get(lang, texts["english"])))
        if text and not text.endswith("\r\n"):
            text += "\r\n"
        ctx.set_text(member, text + "".join(add))
