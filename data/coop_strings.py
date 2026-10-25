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
