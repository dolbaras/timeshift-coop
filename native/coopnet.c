/*
 * The mod's own messages between the machines of a network game, carried by the game's own network
 * messages (patch 1.2 exe only; addresses from the static analysis in the private notes).
 *
 * The game sends engine messages to a peer with 0x63ea90 (it writes message id, entity net id and a
 * codec-encoded body into one packet on channel 0, reliable with flag 0x20000), and the receiving side
 * hands every message that is not for an entity to the net object's handler as 0x41d
 * {entity -2, msg id, sender peer, body}. The mod uses an id the game never uses (0x7C01; the game's are
 * 0x3e8..0x4fa) and the game's generic small-message codec (the one its 0x439 "notify" messages use):
 *   { u8 codec index, u8 0x40, i16 size = 6 + payload, u16 sub, payload[<= 250] }
 * and puts its own function into the handler slot of the two net objects (server 0xdab6c0, client
 * 0xdaf3b4); everything that is not ours goes on to the game's handler unchanged. The hooks are always
 * in; they cost one compare per network message.
 *
 * Subtypes:
 *   1  run this SSL text here (queued for the script tick, like the test console)
 *   2  (client -> server) run on every machine: the server runs it and relays it as 1 to all other clients
 *   10+ other parts of the mod (coopnet_listen): 10-19 time powers (timesync.c)
 * Running received SSL needs TS_COOPNET=1 or the test console (TS_CONSOLE); otherwise it is logged and
 * dropped. TS_COOPNET=0 leaves the game's handlers alone.
 *
 * Console (console.c): "@all <ssl>" runs here and on every other machine, "@peers <ssl>" only on the
 * others, "@srv <ssl>" on the server.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

/* Game state (see the notes: game-mode object, net objects, peer network) */
#define GAME_MODE_OBJ (*(char **)0xfcab34)   /* +0x3c mode, +0x28 client slots, +0x2c slot count */
#define NET_CLIENT_OBJ (*(char **)0xfcab38)  /* +0x2c this client's id */
#define PEER_NETWORK (*(void **)0x1096148)   /* net::PEER_NETWORK; NULL outside network games */
#define NOTIFY_CODEC (*(int *)0xec823c)      /* index the generic codec got at start-up (-1 before) */
#define MODE_OFF 0x3c
#define SLOTS_OFF 0x28
#define NSLOTS_OFF 0x2c
#define MY_ID_OFF 0x2c
#define SLOT_SIZE 0xa4                       /* slot: +0 client id (0 = the server), +8 flags */
#define SLOT_READY 0x80                      /* (set when the joiner gets the world / says ready) */
#define SLOT_JOINED 0x100                    /* (set when the server accepted the connection) */
#define SLOT_INGAME 1                        /* (set once the client has loaded the level and runs it) */
#define MODE_SERVER 2
#define MODE_CLIENT 3
#define MODE_DEDICATED 4

#define SEND_TO_SLOT 0x63ea90                /* ecx = body, edx = flags; (slot, entity, u16 msg id); ret 0xc */
#define FLAG_RELIABLE 0x20000
#define MSG_SERVERCLIENT 0x41d               /* the envelope the net objects get */
#define MSG_FIRST 0x7c01                     /* ours: 0x7c01..0x7cff (must stay < 0x8000: the reader stops at a negative id) */
#define MSG_LAST 0x7cff
#define MSG_ID MSG_FIRST
#define MAX_PAYLOAD 250                      /* the codec's reader stops the game above size 0x100 */

#define SUB_RUN 1
#define SUB_RUN_ALL 2

#define SRV_HANDLER_SLOT 0xdab6c0            /* server net object vtable 0xdab6bc, slot 1 */
#define SRV_HANDLER 0x697620
#define CLI_HANDLER_SLOT 0xdaf3b4            /* client net object vtable 0xdaf3b0, slot 1 */
#define CLI_HANDLER 0x6fba90

#pragma pack(push, 1)
typedef struct {
    unsigned char codec, flags;
    short size;
    unsigned short sub;
    unsigned char data[MAX_PAYLOAD];
} Notify;
#pragma pack(pop)

/* Handler slot 1: thiscall (net object, message, data, sender), ret 0xc. The dispatcher (0x946e40) calls
 * slot 2 (returns 0), then slot 1, and slot 3 only when slot 1 returned 0. */
typedef int (TSCALL *handler_t)(void *self, int msg, int *data, void *sender);
static handler_t orig_server = (handler_t)SRV_HANDLER, orig_client = (handler_t)CLI_HANDLER;
static int remote_ssl, hooks_in;

/* Subtype ranges handled by other parts of the mod (registered at start-up, before any message). */
static struct { unsigned short first, last; coopnet_recv fn; } listeners[8];
static int n_listeners;

int coopnet_listen(unsigned short first, unsigned short last, coopnet_recv fn)
{
    if (n_listeners >= 8 || first <= SUB_RUN_ALL)
        return 0;
    listeners[n_listeners].first = first;
    listeners[n_listeners].last = last;
    listeners[n_listeners].fn = fn;
    n_listeners++;
    return hooks_in;
}
static LONG n_sent_logged, n_got_logged, n_ignored_logged;
#define LOG_FIRST 8

static int game_mode(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm ? *(int *)(gm + MODE_OFF) : 0;
}

static int in_network_game(void)
{
    int m = game_mode();
    return (m == MODE_SERVER || m == MODE_CLIENT || m == MODE_DEDICATED) && PEER_NETWORK && GAME_MODE_OBJ;
}

/* The game's id for this machine: the client's own id, 0 on the server (as 0x63acd0 does). */
static int my_id(void)
{
    char *cl = NET_CLIENT_OBJ;
    return game_mode() == MODE_CLIENT ? (cl ? *(int *)(cl + MY_ID_OFF) : -1) : 0;
}

static char *slot_at(int i)
{
    char *gm = GAME_MODE_OBJ;
    return i >= 0 && i < *(int *)(gm + NSLOTS_OFF) ? *(char **)(gm + SLOTS_OFF) + i * SLOT_SIZE : NULL;
}

/* One message to one peer through the game's own per-peer sender (entity NULL: net id -2 on the wire). */
static void send_to_slot(char *slot, const Notify *msg)
{
    unsigned fn = SEND_TO_SLOT, id = MSG_ID, flags = FLAG_RELIABLE;
    const void *body = msg;
    __asm__ volatile("pushl %[id]\n\tpushl $0\n\tpushl %[slot]\n\tcall *%[fn]"
                     : "+c"(body), "+d"(flags)   /* (inputs, and clobbered by the call) */
                     : [slot] "r"(slot), [id] "r"(id), [fn] "r"(fn)
                     : "eax", "memory", "cc");
}

/* target: -1 = every peer (server: every joined client; client: the server), else a slot index.
 * `except` = a client id not to send to (-1 = none). Returns the number of peers. */
static int send_msg(int target, int except, unsigned short sub, const void *data, int len)
{
    if (!in_network_game() || len < 0 || len > MAX_PAYLOAD || NOTIFY_CODEC < 0 || NOTIFY_CODEC > 99)
        return -1;
    Notify m;
    m.codec = (unsigned char)NOTIFY_CODEC;
    m.flags = 0x40;
    m.size = (short)(6 + len);
    m.sub = sub;
    if (len)
        memcpy(m.data, data, len);
    int me = my_id(), client = game_mode() == MODE_CLIENT, n = 0;
    for (int i = target < 0 ? 0 : target; ; i++) {
        char *s = slot_at(i);
        if (!s)
            break;
        int id = *(int *)s, fl = *(int *)(s + 8);
        /* (a server sends to a client only once it runs the level: messages that reached a client while it was
         * still loading came with lost connections, "server closed", in about half of the joins on the N100) */
        int ingame = client || (fl & SLOT_INGAME);
        int want = target >= 0 ? id != me && ingame
                 : client ? id == 0   /* (the server is client id 0 on a client, as 0x6fb8e0 finds it) */
                 : id != me && ingame && (fl & (SLOT_JOINED | SLOT_READY)) == (SLOT_JOINED | SLOT_READY);
        if (want && id != except) {
            send_to_slot(s, &m);
            n++;
            if (InterlockedIncrement(&n_sent_logged) <= LOG_FIRST)
                ts_log("coopnet: sent sub %u (%d bytes) to client %d (slot %d)", sub, len, id, i);
        }
        if (target >= 0)
            break;
    }
    return n;
}

int coopnet_send(int target, unsigned short sub, const void *data, int len)
{
    return send_msg(target, -1, sub, data, len);
}

/* ---- receiving ---- */
static void ran_done(int peer, const char *text, const char *result)
{
    ts_log("coopnet: ran from peer %d: %s -> %s", peer, text, result);
}

static void got_run(int peer, unsigned sub, const char *text, int len)
{
    if (!remote_ssl) {
        if (InterlockedIncrement(&n_ignored_logged) <= LOG_FIRST)
            ts_log("coopnet: SSL from peer %d ignored (TS_COOPNET=1 or TS_CONSOLE enables it): %s", peer, text);
        return;
    }
    if (sub == SUB_RUN_ALL && game_mode() != MODE_CLIENT) {
        int n = send_msg(-1, peer, SUB_RUN, text, len);   /* (to every other client) */
        ts_log("coopnet: relayed from peer %d to %d client(s)", peer, n);
    }
    if (!ts_ssl_queue(text, ran_done, peer))
        ts_log("coopnet: cannot queue SSL from peer %d", peer);
}

/* 1 = one of ours (consumed), 0 = the game's. */
static int catch_message(int msg, int *data)
{
    if (msg != MSG_SERVERCLIENT || !data || data[0] != -2)
        return 0;
    unsigned id = (unsigned short)data[1];   /* (a 16-bit field; the upper half is not set) */
    if (id < MSG_FIRST || id > MSG_LAST)
        return 0;
    int peer = data[2];
    const Notify *m = (const Notify *)data[3];
    if (!m || m->codec != (unsigned char)NOTIFY_CODEC || m->size < 6 || m->size > 6 + MAX_PAYLOAD) {
        ts_log("coopnet: message %04x from peer %d without a valid body", id, peer);
        return 1;
    }
    int len = m->size - 6;
    if (InterlockedIncrement(&n_got_logged) <= LOG_FIRST)
        ts_log("coopnet: got sub %u (%d bytes) from peer %d", m->sub, len, peer);
    switch (m->sub) {
    case SUB_RUN:
    case SUB_RUN_ALL: {
        char text[MAX_PAYLOAD + 1];   /* (the body lives in the codec's static buffer: copied) */
        memcpy(text, m->data, len);
        text[len] = 0;
        got_run(peer, m->sub, text, len);
        break;
    }
    default:
        for (int i = 0; i < n_listeners; i++)
            if (m->sub >= listeners[i].first && m->sub <= listeners[i].last) {
                listeners[i].fn(peer, m->sub, m->data, len);   /* (synchronous; the body is not kept) */
                return 1;
            }
        ts_log("coopnet: unknown sub %u from peer %d", m->sub, peer);
    }
    return 1;
}

static int TSCALL hook_server(void *self, int msg, int *data, void *sender)
{
    return catch_message(msg, data) ? 1 : orig_server(self, msg, data, sender);
}

static int TSCALL hook_client(void *self, int msg, int *data, void *sender)
{
    return catch_message(msg, data) ? 1 : orig_client(self, msg, data, sender);
}

/* ---- console: "@all <ssl>", "@peers <ssl>", "@srv <ssl>" (runs in the script tick) ---- */
int coopnet_console(const char *text, char *out, int size)
{
    static const char *const cmds[] = {"@all ", "@peers ", "@srv "};
    int c = -1;
    for (int i = 0; i < 3 && c < 0; i++)
        if (strncmp(text, cmds[i], strlen(cmds[i])) == 0)
            c = i;
    if (c < 0)
        return 0;
    const char *body = text + strlen(cmds[c]);
    int len = (int)strlen(body);
    if (!in_network_game())
        return snprintf(out, size, "not in a network game"), 1;
    if (len > MAX_PAYLOAD)
        return snprintf(out, size, "too long (%d bytes, at most %d)", len, MAX_PAYLOAD), 1;
    int client = game_mode() == MODE_CLIENT, n;
    char here[1024];
    if (c == 2) {   /* @srv */
        if (!client)
            return snprintf(out, size, "this is the server: %s", ts_script_run(body, here, sizeof here)), 1;
        n = send_msg(-1, -1, SUB_RUN, body, len);   /* (-1 on a client = the server) */
        return snprintf(out, size, "sent to %d", n), 1;
    }
    /* @all / @peers: a client asks the server to run it and pass it on to the other clients */
    n = send_msg(-1, -1, client ? SUB_RUN_ALL : SUB_RUN, body, len);
    if (c == 1)
        return snprintf(out, size, client ? "sent to %d (the server relays)" : "sent to %d", n), 1;
    snprintf(out, size, client ? "sent to %d (the server relays); here: %s" : "sent to %d; here: %s", n,
             ts_script_run(body, here, sizeof here));
    return 1;
}

/* ---- install ---- */
static int patch_slot(DWORD slot, void *expect, void *hook)
{
    return ts_patch_bytes(slot, &expect, &hook, sizeof(void *));
}

void coopnet_install(void)
{
    char v[16];
    int set = ts_env("TS_COOPNET", v, sizeof v);
    if (set && v[0] == '0')
        return;
    if (!patch_slot(SRV_HANDLER_SLOT, (void *)SRV_HANDLER, (void *)hook_server)
        || !patch_slot(CLI_HANDLER_SLOT, (void *)CLI_HANDLER, (void *)hook_client)) {
        patch_slot(SRV_HANDLER_SLOT, (void *)hook_server, (void *)SRV_HANDLER);   /* (undo a half install) */
        ts_log("coopnet: net handlers not found (other exe)");
        return;
    }
    hooks_in = 1;
    char con[16];
    remote_ssl = ((set && v[0] == '1') || ts_env("TS_CONSOLE", con, sizeof con)) && ts_tick_install();
    ts_log("coopnet: handlers in, remote SSL %s", remote_ssl ? "on" : "off");
}
