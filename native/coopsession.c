/*
 * The co-op session (patch 1.2 exe only; addresses from the static analysis in the private notes).
 *
 * What a co-op session is: a network game (game mode 2 server, 3 client, 4 dedicated) whose level is a
 * campaign level ("ts2_..."). The mod's data offers campaign levels only through its own Co-op menu (the
 * stock map list is unchanged), so the stock multiplayer never runs one; every machine can tell from the
 * level name alone, at any moment, without waiting for a message. ts_coop_active() is that test.
 *
 * Session settings (ts_coop_feature) are the host's: its co-op menu writes them into UI strings
 * (CoopSession = "host", CoopDifficulty, CoopMaxPlayers), the environment can override them for tests
 * (TS_TIMESYNC, TS_STORYSYNC, TS_COOP_FF=1, TS_COOP_MPRULES=1), and the host sends them to every client that
 * joins (message 40 below). Clients take the host's difficulty and mark the session for the menu scripts
 * ($ui.SetStr("CoopSession", "client")).
 *
 * Version check: message 40 carries the host's mod version (TS_VERSION). A client with another version
 * answers 41 "mismatch", leaves the game and its main menu explains why (COOP_ERR_VERSION with both
 * versions). The message format (sub 40: u8 proto, ..., version string) must stay readable by later
 * versions so that old and new clients can still compare.
 *
 * Co-op rules applied in a co-op session only (stock multiplayer maps are untouched):
 * - No friendly fire between players: the MP damage factor 0x4e3fe0 (stdcall (victim, attacker id), ret 8,
 *   returns a float) compares the players' slots at 0x4e4099; the "different players" branch (jne 0x4e4142
 *   at 0x4e40a9) is sent through a stub that returns 0.0 (the function's own epilogue at 0x4e42fc).
 *   TS_COOP_FF=1 keeps the stock rule (friendly fire option, team check).
 * - No warm-up and no ready system: the game options bWarmUp (read at 0x5ee949, server level start
 *   0x5ee870) and bReadySystem (0x596cbb in the rules, 0x5ff66a for the HUD hint) are read through the
 *   option store's int getter 0x4027f0 (thiscall (store, index, int *out), ret 8); those three calls get a
 *   wrapper that reads 0. TS_COOP_MPRULES=1 keeps them.
 *   (Minimum players, time and score limit are profile keys: the co-op menu sets them for its server and
 *   puts the player's own values back at the main menu; see mod/docs/building.md.)
 * - No multiplayer score bar (match time, frags) on the HUD: the HUD element (type scoreBar) hides itself when
 *   its check 0x5b2b90, called at 0x5b2c80, returns 0; in a co-op session it does. (A hud.ui script cannot
 *   do it: the element sets its own visibility every frame.)
 *
 * Messages (coopnet.c, reliable, subtypes 40-49):
 *   40 server -> client  u8 proto (1), u8 flags (COOP_F_*), u8 difficulty (0-2, 255 = not set),
 *                        u8 max players, u8 len, version[len]
 *   41 client -> server  u8 proto, u8 result (0 = same version, 1 = other version), u8 len, version[len]
 *   42 client -> server  u8 proto, u8 0: "send me 40" (a client that runs the level and has no session info yet)
 * The server sends 40 once when a client's slot is joined and ready (the client may still be loading the level)
 * and answers every 42; a client asks every 3 s (20 times at most) from the moment its player exists. Blind
 * repeats to a loading client lost the connection in a test.
 * TS_COOP_VERSION=<v> (lab): this machine says it has version <v> (tests the version check).
 *
 * TS_COOP=0: none of this (no rule patches, no messages).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

#define GAME_MODE_OBJ (*(char **)0xfcab34)   /* +0x3c mode, +0x28 client slots, +0x2c slot count */
#define PEER_NETWORK (*(void **)0x1096148)   /* net::PEER_NETWORK; NULL outside network games */
#define MODE_OFF 0x3c
#define SLOTS_OFF 0x28
#define NSLOTS_OFF 0x2c
#define SLOT_SIZE 0xa4                       /* +0 client id (0 = the server), +8 flags */
#define SLOT_READY 0x80
#define SLOT_JOINED 0x100
#define MODE_SERVER 2
#define MODE_CLIENT 3
#define MODE_DEDICATED 4
#define LEVEL_NAME ((const char *)0xfcabc8)  /* the level file of the running map (char array) */
#define LEVEL_NAME_MAX 0x110

/* friendly fire: the "different players" branch of the MP damage factor */
#define FF_SITE 0x4e40a9                     /* jne 0x4e4142 (0f 85 93 00 00 00) */
#define FF_CONTINUE 0x4e4142
#define FF_EPILOGUE 0x4e42fc                 /* (after the fld1 of "factor 1") */
/* game option reads through the int getter */
#define OPT_GETINT 0x4027f0
#define SITE_WARMUP 0x5ee949
#define SITE_READY 0x596cbb
#define SITE_READY_HINT 0x5ff66a
/* HUD score bar: its update 0x5b2bf0 shows it in a network game when 0x5b2b90 (no arguments) returns non-zero */
#define SCOREBAR_WANTED 0x5b2b90
#define SITE_SCOREBAR 0x5b2c80

#define SUB_SESSION 40
#define SUB_ACK 41
#define SUB_ASK 42                           /* client -> server: "send me the session info" (u8 proto, u8 0) */
#define SUB_LAST 49
#define PROTO 1
#define ASK_MS 3000                          /* client: ask again every 3 s until the info arrives */
#define MAX_ASKS 20
#define ANSWER_MS 90000                      /* server: "never answered" after 90 s */
#define MAX_PEERS 32
#define VER_MAX 40

static int enabled, ff_off = 1, rules_off = 1;
static char my_version[VER_MAX + 1] = TS_VERSION;   /* (TS_COOP_VERSION: lab test of the version check) */
static int host_flags, host_difficulty = 255, host_maxplayers;
static int last_role = -1;
static char last_level[64];
static DWORD session_start;

/* client: what the host told us (written in the net pump, handled in the script tick) */
static volatile LONG got_session, session_seq;
static int srv_flags, srv_difficulty = 255, srv_maxplayers;
static char srv_version[VER_MAX + 1];
static int handled_seq, client_applied, client_left, client_warned, client_asks;
static DWORD client_asked_at;
static int client_reask, reask_seq;          /* (after a level change in the session: ask once more, so the new
                                                level's server records this client's answer) */

/* server: one record per client id */
static struct { int id, sends, acked, warned; DWORD at; } peers[MAX_PEERS];
static int n_peers;

static LONG n_logged;
#define CLOG(...) do { if (InterlockedIncrement(&n_logged) <= 400) ts_log(__VA_ARGS__); } while (0)

static int game_mode(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm ? *(int *)(gm + MODE_OFF) : 0;
}

static int campaign_level(void)
{
    const char *s = LEVEL_NAME;
    for (int i = 0; i < LEVEL_NAME_MAX - 4 && s[i]; i++)
        if ((s[i] | 0x20) == 't' && (s[i + 1] | 0x20) == 's' && s[i + 2] == '2' && s[i + 3] == '_')
            return 1;
    return 0;
}

int ts_coop_active(void)
{
    int m = game_mode();
    return enabled && (m == MODE_SERVER || m == MODE_CLIENT || m == MODE_DEDICATED) && PEER_NETWORK
           && campaign_level();
}

int ts_coop_feature(int flag)
{
    if (!ts_coop_active())
        return 0;
    int f = game_mode() == MODE_CLIENT && got_session ? srv_flags : host_flags;
    return (f & flag) != 0;
}

/* ---- rules ---------------------------------------------------------------------------------------- */
__attribute__((used)) int coop_ff_block(void) __asm__("coop_ff_block");
__attribute__((used)) int coop_ff_block(void)
{
    return ff_off && ts_coop_active();
}

/* Reached instead of "jne 0x4e4142" when victim and attacker are different players (flags from
 * "cmp ebx, esi"); eax/ecx/edx are kept for the original path. */
extern void coop_ff_stub(void) __asm__("coop_ff_stub");
__asm__(".text\n"
        ".globl coop_ff_stub\n"
        "coop_ff_stub:\n"
        "  pushl %eax\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  call coop_ff_block\n"
        "  testl %eax, %eax\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  popl %eax\n"
        "  jnz 1f\n"
        "  pushl $0x4e4142\n"                /* FF_CONTINUE: the stock team / friendly fire checks */
        "  ret\n"
        "1:\n"
        "  fldz\n"                           /* damage factor 0 */
        "  pushl $0x4e42fc\n"                /* FF_EPILOGUE */
        "  ret\n");

typedef int (TSCALL *getint_t)(void *store, int index, int *out);

/* bWarmUp / bReadySystem read as 0 in a co-op session. */
static int TSCALL opt_rule_off(void *store, int index, int *out)
{
    int r = ((getint_t)OPT_GETINT)(store, index, out);
    if (r && out && rules_off && ts_coop_active())
        *out = 0;
    return r;
}

/* The HUD score bar is wanted outside co-op only. */
static int score_bar_wanted(void)
{
    return ts_coop_active() ? 0 : ((int (*)(void))SCOREBAR_WANTED)();
}

static int patch_call(DWORD site, DWORD target, void *hook)
{
    unsigned char want[5] = {0xe8}, rep[5] = {0xe8};
    *(DWORD *)(want + 1) = target - (site + 5);
    *(DWORD *)(rep + 1) = (DWORD)hook - (site + 5);
    return ts_patch_bytes(site, want, rep, 5);
}

/* ---- settings and messages ------------------------------------------------------------------------- */
static int env_on(const char *name, int dflt)
{
    char v[16];
    if (!ts_env(name, v, sizeof v))
        return dflt;
    return v[0] != '0';
}

/* A version string as it may appear in SSL text and in the log: [0-9A-Za-z.+_-] only. */
static void clean_version(char *out, int size, const unsigned char *in, int len)
{
    int n = 0;
    for (int i = 0; i < len && n < size - 1; i++) {
        unsigned char c = in[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-'
            || c == '+' || c == '_')
            out[n++] = (char)c;
    }
    out[n] = 0;
}

static int slot_count(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm ? *(int *)(gm + NSLOTS_OFF) : 0;
}

static char *slot_at(int i)
{
    char *gm = GAME_MODE_OBJ;
    return gm && i >= 0 && i < *(int *)(gm + NSLOTS_OFF) ? *(char **)(gm + SLOTS_OFF) + i * SLOT_SIZE : NULL;
}

static void send_session(int slot, int id, int log)
{
    unsigned char b[8 + VER_MAX];
    int vl = (int)strlen(my_version);
    b[0] = PROTO;
    b[1] = (unsigned char)host_flags;
    b[2] = (unsigned char)host_difficulty;
    b[3] = (unsigned char)host_maxplayers;
    b[4] = (unsigned char)vl;
    memcpy(b + 5, my_version, vl);
    int n = coopnet_send(slot, SUB_SESSION, b, 5 + vl);
    if (log)
        CLOG("coopsession: session info to client %d (slot %d): flags %#x, difficulty %d -> %d", id, slot, host_flags,
         host_difficulty, n);
}

static void send_ack(int result)
{
    unsigned char b[4 + VER_MAX];
    int vl = (int)strlen(my_version);
    b[0] = PROTO;
    b[1] = (unsigned char)result;
    b[2] = (unsigned char)vl;
    memcpy(b + 3, my_version, vl);
    coopnet_send(-1, SUB_ACK, b, 3 + vl);   /* (-1 on a client = the server) */
}

/* Net pump (main thread, inside the game's message handler): copy and leave the work to the tick. */
static void on_message(int peer, unsigned short sub, const unsigned char *d, int len)
{
    if (!enabled || len < 2 || d[0] != PROTO)
        return;
    if (sub == SUB_SESSION && game_mode() == MODE_CLIENT && len >= 5) {
        int vl = d[4] <= len - 5 ? d[4] : len - 5;
        srv_flags = d[1];
        srv_difficulty = d[2];
        srv_maxplayers = d[3];
        clean_version(srv_version, sizeof srv_version, d + 5, vl);
        InterlockedExchange(&got_session, 1);
        InterlockedIncrement(&session_seq);
    } else if (sub == SUB_ASK && game_mode() != MODE_CLIENT) {
        /* (the client runs the level now: the first info, sent when it joined, may have gone to a client that was
         * still loading; a request is answered at once) */
        for (int i = 0; i < slot_count(); i++)
            if (*(int *)slot_at(i) == peer) {
                int k = 0;
                while (k < n_peers && peers[k].id != peer)
                    k++;
                if (k < n_peers) {
                    peers[k].sends++;
                    if (peers[k].acked)
                        break;
                }
                send_session(i, peer, k == n_peers || peers[k].sends == 2);
                break;
            }
    } else if (sub == SUB_ACK && game_mode() != MODE_CLIENT && len >= 3) {
        char ver[VER_MAX + 1];
        int vl = d[2] <= len - 3 ? d[2] : len - 3;
        clean_version(ver, sizeof ver, d + 3, vl);
        int sends = 0, again = 0;
        for (int i = 0; i < n_peers; i++)
            if (peers[i].id == peer) {
                again = peers[i].acked;
                peers[i].acked = 1;
                sends = peers[i].sends;
            }
        if (!again)
            CLOG("coopsession: client %d answers: %s (its version %s; session info sent %d time(s))", peer,
                 d[1] == 0 ? "same version" : "OTHER VERSION, it leaves", ver, sends);
    }
}

/* An SSL statement's string result (script tick only). */
static void ssl_str(const char *text, char *out, int size)
{
    ts_script_run(text, out, size);
    if (strncmp(out, "parse error", 11) == 0 || strncmp(out, "error ", 6) == 0 || strcmp(out, "Ok") == 0)
        out[0] = 0;
}

/* The data (coop.s3darc, built by tools/build_pak.py) carries the same version as this DLL; its main menu
 * leaves it in the UI string CoopDataVersion. */
static void check_data_version(void)
{
    char d[64];
    ssl_str("return $ui.GetStr(\"CoopDataVersion\")", d, sizeof d);
    if (strcmp(d, TS_VERSION) != 0)
        CLOG("coopsession: WARNING: the mod's data is version \"%s\", this DLL %s (reinstall the mod)", d, TS_VERSION);
}

static void host_session_start(void)
{
    char s[64], d[64], m[64];
    ssl_str("return $ui.GetStr(\"CoopSession\")", s, sizeof s);
    ssl_str("return $ui.GetStr(\"CoopDifficulty\")", d, sizeof d);
    ssl_str("return $ui.GetStr(\"CoopMaxPlayers\")", m, sizeof m);
    host_difficulty = d[0] >= '0' && d[0] <= '2' && !d[1] ? d[0] - '0' : 255;
    host_maxplayers = m[0] >= '1' && m[0] <= '9' ? atoi(m) : 0;
    host_flags = COOP_F_ACTIVE;
    if (env_on("TS_TIMESYNC", 0))
        host_flags |= COOP_F_TIMESYNC;
    if (env_on("TS_STORYSYNC", 0))
        host_flags |= COOP_F_STORYSYNC;
    if (!ff_off)
        host_flags |= COOP_F_FRIENDLY_FIRE;
    if (!rules_off)
        host_flags |= COOP_F_MP_RULES;
    n_peers = 0;
    CLOG("coopsession: co-op session hosted on \"%.80s\" (%s), flags %#x, difficulty %d, max players %d",
         LEVEL_NAME, strcmp(s, "host") == 0 ? "from the co-op menu" : "not from the co-op menu", host_flags,
         host_difficulty, host_maxplayers);
}

static void server_tick(void)
{
    DWORD now = GetTickCount();
    int n = slot_count();
    for (int i = 0; i < n; i++) {
        char *s = slot_at(i);
        if (!s)
            break;
        int id = *(int *)s, fl = *(int *)(s + 8);
        if (id == 0 || (fl & (SLOT_JOINED | SLOT_READY)) != (SLOT_JOINED | SLOT_READY))
            continue;
        int k = 0;
        while (k < n_peers && peers[k].id != id)
            k++;
        if (k == n_peers) {
            if (n_peers == MAX_PEERS)
                continue;
            peers[n_peers].id = id;
            peers[n_peers].sends = 1;
            peers[n_peers].acked = peers[n_peers].warned = 0;
            peers[n_peers].at = now;
            n_peers++;
            CLOG("coopsession: client %d joined (slot %d)", id, i);
            /* Once, at once (an old client only gets this one); a client that is still loading the level asks again
             * when it runs it (42). Repeated sends to a loading client lost the connection on the N100 pair
             * ("server shut down" on the client) in 2 of 3 runs. */
            send_session(i, id, 1);
        }
        if (!peers[k].acked && !peers[k].warned && now - peers[k].at >= ANSWER_MS) {
            peers[k].warned = 1;
            CLOG("coopsession: client %d never answered in %d s (no co-op mod, or an old one)", id, ANSWER_MS / 1000);
        }
    }
}

static void client_tick(void)
{
    char out[256], ssl[300];
    if (client_reask && session_seq != reask_seq)
        client_reask = 0;   /* (the host answered: handled below like any session message) */
    if ((!got_session || client_reask) && *(void **)0x1095b30 && client_asks < MAX_ASKS
        && (!client_asks || GetTickCount() - client_asked_at >= ASK_MS)) {   /* (in the level: its player exists) */
        unsigned char b[2] = {PROTO, 0};
        coopnet_send(-1, SUB_ASK, b, 2);
        client_asks++;
        client_asked_at = GetTickCount();
    }
    if (!got_session) {
        if (!client_warned && GetTickCount() - session_start > 20000) {
            client_warned = 1;
            CLOG("coopsession: no session info from the host after 20 s (host without the co-op mod's session part?)");
        }
        return;
    }
    int seq = session_seq;
    if (seq == handled_seq)
        return;
    handled_seq = seq;
    /* every session message is answered (the host sends it again after each of its level changes) */
    int same = strcmp(srv_version, my_version) == 0;
    send_ack(same ? 0 : 1);
    if (!same) {
        if (client_left)
            return;
        client_left = 1;
        char mine[VER_MAX + 1];
        clean_version(mine, sizeof mine, (const unsigned char *)my_version, (int)strlen(my_version));
        snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopError\", \"COOP_ERR_VERSION,%s,%s\")\n$ui.LoadLevel(\"menu.lg\")",
                 srv_version[0] ? srv_version : "?", mine);
        CLOG("coopsession: host version %s, here %s: leaving (%s)", srv_version, my_version,
             ts_script_run(ssl, out, sizeof out));
        return;
    }
    if (client_applied)
        return;
    client_applied = 1;
    CLOG("coopsession: host version %s (same); flags %#x, difficulty %d, max players %d", srv_version, srv_flags,
         srv_difficulty, srv_maxplayers);
    if ((srv_flags & COOP_F_TIMESYNC) && !env_on("TS_TIMESYNC", 0))
        CLOG("coopsession: the host shares time powers, this machine does not (TS_TIMESYNC)");
    if ((srv_flags & COOP_F_STORYSYNC) && !env_on("TS_STORYSYNC", 0))
        CLOG("coopsession: the host runs the story for everyone, this machine does not (TS_STORYSYNC)");
    if (srv_difficulty <= 2)
        snprintf(ssl, sizeof ssl,
                 "$ui.SetStr(\"CoopSession\", \"client\")\n$ui.SetVariable(\"Multiplayer\", \"Difficulty\", \"int\", \"%d\")",
                 srv_difficulty);
    else
        snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopSession\", \"client\")");
    CLOG("coopsession: joined the host's co-op session (%s)", ts_script_run(ssl, out, sizeof out));
}

static void frame(float dt)
{
    (void)dt;
    int role = ts_coop_active() ? game_mode() : 0;
    char lvl[64];
    snprintf(lvl, sizeof lvl, "%.63s", LEVEL_NAME);
    if (role != last_role || (role && strcmp(lvl, last_level) != 0)) {
        if (last_role > 0 && role != last_role)
            CLOG("coopsession: co-op session ended (game mode %d)", game_mode());
        last_role = role;
        snprintf(last_level, sizeof last_level, "%s", lvl);
        session_start = GetTickCount();
        if (role)
            check_data_version();
        if (role == MODE_SERVER || role == MODE_DEDICATED)
            host_session_start();
        else if (role == MODE_CLIENT && got_session) {
            /* a level change inside the same session: the host's info stays valid; ask once more (the host's
             * records start again with the level, and its first send went to this client while it loaded) */
            client_reask = 1;
            reask_seq = session_seq;
            client_asks = 0;
            CLOG("coopsession: next level \"%.80s\" in the host's session", LEVEL_NAME);
        } else if (role == MODE_CLIENT)
            CLOG("coopsession: joined a co-op server on \"%.80s\", waiting for its session info", LEVEL_NAME);
        if (!role) {
            InterlockedExchange(&got_session, 0);
            handled_seq = session_seq;
            client_applied = client_left = client_warned = client_asks = 0;
            srv_version[0] = 0;
            n_peers = 0;
        }
    }
    if (role == MODE_SERVER || role == MODE_DEDICATED)
        server_tick();
    else if (role == MODE_CLIENT)
        client_tick();
}

void coopsession_install(void)
{
    char v[16];
    if (ts_env("TS_COOP", v, sizeof v) && v[0] == '0') {
        ts_log("coopsession: off (TS_COOP=0)");
        return;
    }
    /* Time powers and the story are shared in co-op sessions by default (timesync.c acts on campaign levels only,
     * storysync.c only in a co-op session whose host has COOP_F_STORYSYNC); TS_TIMESYNC=0 / TS_STORYSYNC=0 switch
     * them off (on the host: for the whole session). */
    if (!ts_env("TS_TIMESYNC", v, sizeof v)) {
        SetEnvironmentVariableA("TS_TIMESYNC", "1");
        ts_log("coopsession: TS_TIMESYNC=1 (default for co-op sessions)");
    }
    if (!ts_env("TS_STORYSYNC", v, sizeof v)) {
        SetEnvironmentVariableA("TS_STORYSYNC", "1");
        ts_log("coopsession: TS_STORYSYNC=1 (default for co-op sessions)");
    }
    if (ts_env("TS_COOP_VERSION", v, sizeof v)) {   /* (lab: act as another version, to test the check) */
        clean_version(my_version, sizeof my_version, (const unsigned char *)v, (int)strlen(v));
        ts_log("coopsession: TS_COOP_VERSION: this machine says it has version %s", my_version);
    }
    ff_off = !env_on("TS_COOP_FF", 0);
    rules_off = !env_on("TS_COOP_MPRULES", 0);
    enabled = 1;
    static const unsigned char ff_want[6] = {0x0f, 0x85, 0x93, 0x00, 0x00, 0x00};
    unsigned char ff_rep[6] = {0x0f, 0x85};
    *(DWORD *)(ff_rep + 2) = (DWORD)coop_ff_stub - (FF_SITE + 6);
    int ok_ff = ts_patch_bytes(FF_SITE, ff_want, ff_rep, 6);
    int ok_rules = patch_call(SITE_WARMUP, OPT_GETINT, (void *)opt_rule_off)
                   + patch_call(SITE_READY, OPT_GETINT, (void *)opt_rule_off)
                   + patch_call(SITE_READY_HINT, OPT_GETINT, (void *)opt_rule_off);
    int ok_hud = patch_call(SITE_SCOREBAR, SCOREBAR_WANTED, (void *)score_bar_wanted);
    int ok_tick = ts_frame_add(frame), ok_net = coopnet_listen(SUB_SESSION, SUB_LAST, on_message);
    ts_log("coopsession: on, version %s (friendly fire %s %d, warm-up/ready %s %d/3, score bar off %d, session messages %s)",
           my_version, ff_off ? "off" : "stock", ok_ff, rules_off ? "off" : "stock", ok_rules, ok_hud,
           ok_tick && ok_net ? "on" : "OFF: no script tick or mod messages");
}
