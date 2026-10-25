/*
 * The game's own multiplayer on today's networks (always on).
 *
 * LAN server list: the game asks for servers with a broadcast to 255.255.255.255 (UDP 7777-7877).
 * Windows sends such a broadcast through one adapter only (the lowest metric): on a PC with a VPN
 * adapter (Radmin, Hamachi, ZeroTier...) the request never reaches the real LAN and the list stays
 * empty (seen 2026-10-25). Each such send is repeated to the directed broadcast address of every
 * other IPv4 network the PC is on.
 *
 * Internet play: GameSpy's servers are gone (2014), so hosting an Internet game failed ("Unable to start
 * server": the lookups of timeshift.available.gamespy.com, timeshift.master.gamespy.com and
 * motd.gamespy.com fail). Every name under gamespy.com is looked up under a replacement domain instead,
 * by default openspy.net, the community's GameSpy replacement that serves TimeShift (the same change
 * the community's exe edit makes). TS_MASTER=<domain> picks another, TS_MASTER=off keeps gamespy.com.
 *
 * Tests, TS_PORT_SHIFT=<n>: the game binds its UDP sockets to port 7777 (and up to 7877 for the LAN list)
 * as a client too, so a second copy on the same PC could not open its socket. With the variable set those
 * binds move up by n; the copy then joins a server on 127.0.0.1 (the server keeps 7777).
 */
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

typedef int (WINAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
static sendto_t real_sendto;

#define MAX_NETS 16
static ULONG nets[MAX_NETS];   /* directed broadcast addresses, network byte order */
static int n_nets;
static DWORD nets_read;

/* The PC's networks, read again at most every 10 s (adapters come and go). */
static void read_networks(void)
{
    DWORD now = GetTickCount();
    if (nets_read && now - nets_read < 10000)
        return;
    nets_read = now ? now : 1;
    n_nets = 0;
    ULONG size = 0;
    if (GetIpAddrTable(NULL, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER)
        return;
    MIB_IPADDRTABLE *t = (MIB_IPADDRTABLE *)HeapAlloc(GetProcessHeap(), 0, size);
    if (!t)
        return;
    if (GetIpAddrTable(t, &size, FALSE) == NO_ERROR)
        for (DWORD i = 0; i < t->dwNumEntries && n_nets < MAX_NETS; i++) {
            ULONG ip = t->table[i].dwAddr, mask = t->table[i].dwMask;
            if (!ip || (ntohl(ip) >> 24) == 127 || !mask || mask == 0xffffffff)
                continue;
            ULONG b = ip | ~mask;
            int dup = 0;
            for (int k = 0; k < n_nets; k++)
                dup |= nets[k] == b;
            if (!dup)
                nets[n_nets++] = b;
        }
    HeapFree(GetProcessHeap(), 0, t);
}

static int WINAPI hook_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    int r = real_sendto(s, buf, len, flags, to, tolen);
    const struct sockaddr_in *a = (const struct sockaddr_in *)to;
    if (a && tolen >= (int)sizeof *a && a->sin_family == AF_INET && a->sin_addr.s_addr == INADDR_BROADCAST) {
        static LONG logged;
        read_networks();
        for (int i = 0; i < n_nets; i++) {
            struct sockaddr_in d = *a;
            d.sin_addr.s_addr = nets[i];
            real_sendto(s, buf, len, flags, (const struct sockaddr *)&d, sizeof d);
        }
        if (InterlockedIncrement(&logged) == 1) {
            char list[256] = "";
            for (int i = 0; i < n_nets; i++) {
                struct in_addr x;
                x.s_addr = nets[i];
                strncat(list, inet_ntoa(x), sizeof list - strlen(list) - 2);
                strncat(list, " ", sizeof list - strlen(list) - 1);
            }
            ts_log("net: LAN broadcast to port %u also sent to %s", ntohs(a->sin_port), list);
        }
    }
    return r;
}

/* ---- TS_PORT_SHIFT ---- */
typedef int (WINAPI *bind_t)(SOCKET, const struct sockaddr *, int);
static bind_t real_bind;
static int port_shift;

static int WINAPI hook_bind(SOCKET s, const struct sockaddr *name, int len)
{
    const struct sockaddr_in *a = (const struct sockaddr_in *)name;
    if (port_shift && a && len >= (int)sizeof *a && a->sin_family == AF_INET) {
        unsigned short p = ntohs(a->sin_port);
        if (p >= 7777 && p <= 7877) {
            struct sockaddr_in moved = *a;
            moved.sin_port = htons((u_short)(p + port_shift));
            ts_log("net: bind port %u moved to %u (TS_PORT_SHIFT)", p, p + port_shift);
            return real_bind(s, (const struct sockaddr *)&moved, sizeof moved);
        }
    }
    return real_bind(s, name, len);
}

/* ---- gamespy.com -> replacement domain ---- */
typedef struct hostent *(WINAPI *gethostbyname_t)(const char *);
typedef HANDLE (WINAPI *WSAAsyncGetHostByName_t)(HWND, u_int, const char *, char *, int);
static gethostbyname_t real_gethostbyname;
static WSAAsyncGetHostByName_t real_WSAAsyncGetHostByName;
static char master[64] = "openspy.net";

/* "x.gamespy.com" -> "x.<master>"; returns the name to look up. The mapped names live in a small ring of
 * static buffers: an asynchronous lookup may read its name after the call returned. */
static const char *map_name(const char *name)
{
    static char ring[8][256];
    static LONG next;
    char *out = ring[InterlockedIncrement(&next) & 7];
    int size = sizeof ring[0];
    static const char gs[] = "gamespy.com";
    size_t n = name ? strlen(name) : 0, g = sizeof gs - 1;
    if (!master[0] || n < g || _stricmp(name + n - g, gs) != 0 || (n > g && name[n - g - 1] != '.'))
        return name;
    snprintf(out, size, "%.*s%s", (int)(n - g), name, master);
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 8)
        ts_log("net: %s looked up as %s", name, out);
    return out;
}

static struct hostent *WINAPI hook_gethostbyname(const char *name)
{
    return real_gethostbyname(map_name(name));
}

static HANDLE WINAPI hook_WSAAsyncGetHostByName(HWND w, u_int msg, const char *name, char *out, int len)
{
    return real_WSAAsyncGetHostByName(w, msg, map_name(name), out, len);
}

/* ---- saves on a server ----
 * Campaign levels on the game's own server (co-op): the level loader's save step (0x6e3510) refuses unless the
 * game runs single-player (mode 1), so the level-start autosave failed, the engine opened its "continue
 * without saves?" frame (shown empty) and checkpoints never saved. The check now refuses only on a client
 * (mode 3): the host saves its world. Checked 2026-10-25 by changing the jump in memory: the start-of-level
 * autosave and a later checkpoint both wrote a save on the server. */
#define SAVE_CHECK 0x006e3510
#define SAVE_GO 0x006e3520

__attribute__((naked)) static void save_check(void)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "push ecx\n"
        "mov eax, dword ptr ds:[0xfcab34]\n"
        "cmp dword ptr [eax + 0x3c], 3\n"     /* client: no save, as before */
        "je 1f\n"
        "mov eax, 0x006e3520\n"
        "jmp eax\n"
        "1:\n"
        "xor eax, eax\n"
        "pop ecx\n"
        "ret\n"
        ".att_syntax prefix\n");
}

/* ---- AI deaths on a client ----
 * With the AI classes replicated (isSyncMP on ai_brain, which the shipped data leaves off) a client sets an AI's
 * new state from the server's message in 0x859090 through [ai+0x290]->vtbl[1](state, hitInfo) with hitInfo NULL
 * (0x8590e7 push 0); the death state handler 0x8496c0 reads it (0x8496d7) and the client crashed 1-4 minutes into
 * a campaign level (2026-10-25, N100, two copies). The call now passes a zeroed block of its own ("no hit info"),
 * the same bytes rewritten that kept a client alive through many AI deaths in that test. */
#define AI_STATE_CALL 0x008590d9
static unsigned char no_hit_info[4096];

static void ai_death_install(void)
{
    /* mov ecx,[ecx+0x290]; mov edx,[ecx]; mov eax,[eax+8]; mov edx,[edx+4]; push 0; push eax; call edx; ret 4; nop */
    static const unsigned char orig[23] = {0x8b, 0x89, 0x90, 0x02, 0x00, 0x00, 0x8b, 0x11, 0x8b, 0x40, 0x08, 0x8b,
                                           0x52, 0x04, 0x6a, 0x00, 0x50, 0xff, 0xd2, 0xc2, 0x04, 0x00, 0x90};
    /* mov ecx,[ecx+0x290]; mov eax,[eax+8]; mov edx,[ecx]; push no_hit_info; push eax; call [edx+4]; ret 4 */
    unsigned char repl[23] = {0x8b, 0x89, 0x90, 0x02, 0x00, 0x00, 0x8b, 0x40, 0x08, 0x8b, 0x11, 0x68, 0, 0, 0, 0,
                              0x50, 0xff, 0x52, 0x04, 0xc2, 0x04, 0x00};
    *(DWORD *)(repl + 12) = (DWORD)no_hit_info;
    if (!ts_patch_bytes(AI_STATE_CALL, orig, repl, sizeof repl))
        ts_log("net: AI state call not found (other exe)");
}

static void saves_install(void)
{
    /* push ecx; mov eax,[0xfcab34]; cmp dword [eax+0x3c],1; je +2 */
    static const unsigned char orig[12] = {0x51, 0xa1, 0x34, 0xab, 0xfc, 0x00, 0x83, 0x78, 0x3c, 0x01, 0x74, 0x04};
    unsigned char jmp5[5] = {0xe9};
    *(DWORD *)(jmp5 + 1) = (DWORD)save_check - (SAVE_CHECK + 5);
    if (memcmp((void *)SAVE_CHECK, orig, sizeof orig) != 0 || !ts_patch_bytes(SAVE_CHECK, orig, jmp5, 5))
        ts_log("net: save check not found (other exe), saves stay single-player only");
}

/* ---- connection events in the log ----
 * The network layer (net::PEER_NETWORK, vtable 0xdb5754) reports connects, leaves and lost connections through
 * its PollEvent (+0x54, 0x769b20: {type, peer, reason text, reason length, ...}; type 10 = data) and the server
 * drops a peer with Kick (+0x20, 0x76a060: thiscall (peer, reason text), ret 8). Both are logged (data events
 * not), so a client that drops out of a session says why on both machines. */
#define PEERNET_VT 0xdb5754
typedef int *(__attribute__((thiscall)) *poll_t)(void *);
typedef void (__attribute__((thiscall)) *kick_t)(void *, int, const char *);
static LONG n_net_events;

static int *__attribute__((thiscall)) hook_poll(void *self)
{
    int *e = ((poll_t)0x769b20)(self);
    if (e && e[0] != 10 && InterlockedIncrement(&n_net_events) <= 200) {
        const char *r = (const char *)e[2];
        int n = e[3] > 0 && e[3] < 200 && r && !IsBadReadPtr(r, e[3]) ? e[3] : 0;
        ts_log("net: event %d (%s) peer %d%s%.*s", e[0],
               e[0] == 1 ? "connected" : e[0] == 2 || e[0] == 6 ? "connection lost" : e[0] == 3 ? "connect request"
               : e[0] == 4 ? "peer joined" : e[0] == 5 ? "peer left" : e[0] == 7 ? "closed by the host" : "?",
               e[1], n ? ", reason: " : "", n, n ? r : "");
    }
    return e;
}

static void __attribute__((thiscall)) hook_kick(void *self, int peer, const char *reason)
{
    if (InterlockedIncrement(&n_net_events) <= 200)
        ts_log("net: kick peer %d (%s)", peer, reason && !IsBadReadPtr(reason, 4) ? reason : "");
    ((kick_t)0x76a060)(self, peer, reason);
}

static void events_install(void)
{
    void *p = (void *)0x769b20, *hp = (void *)hook_poll, *k = (void *)0x76a060, *hk = (void *)hook_kick;
    if (!ts_patch_bytes(PEERNET_VT + 0x54, &p, &hp, 4) || !ts_patch_bytes(PEERNET_VT + 0x20, &k, &hk, 4))
        ts_log("net: connection events not found (other exe)");
}

/* ---- GameSpy Transport 2 closes in the log ----
 * The transport's "closed" callback (0x77f6e0, fastcall connection, reason: 0 local close, 1 remote close, 2
 * communication error, 3 socket error, 4 out of memory) turns the reason into the UI text of event 6. Logged with
 * the caller stack, so a lost connection says which side closed it and from where. */
#define GT2_CLOSED_CB 0x0077f6e0
static LONG n_closes;

static void __attribute__((cdecl, used)) log_gt2_closed(void *conn, int reason, DWORD esp)
{
    static const char *const why[] = {"local close", "remote close", "communication error", "socket error",
                                      "out of memory"};
    if (InterlockedIncrement(&n_closes) > 20)
        return;
    ts_log("net: transport connection %p closed: %s (%d)", conn, reason >= 0 && reason < 5 ? why[reason] : "?", reason);
    DWORD ret[16], off[16];
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    int n = ts_stack_returns(esp, (DWORD)tib->StackBase, ret, off, 16);
    char line[16 * 10 + 1] = "";
    for (int i = 0; i < n; i++)
        sprintf(line + strlen(line), " %08lx", ret[i]);
    ts_log("net:   stack%s", line);
}

__attribute__((naked)) static void hook_gt2_closed(void)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "pushad\n"
        "lea eax, [esp + 0x20]\n"
        "push eax\n"
        "push edx\n"
        "push ecx\n"
        "call _log_gt2_closed\n"
        "add esp, 12\n"
        "popad\n"
        "mov eax, ecx\n"                         /* (the two instructions the jump replaced) */
        "mov ecx, dword ptr [edx*4 + 0xec9d30]\n"
        "push 0x0077f6e9\n"
        "ret\n"
        ".att_syntax prefix\n");
}

static void closes_install(void)
{
    static const unsigned char orig[9] = {0x8b, 0xc1, 0x8b, 0x0c, 0x95, 0x30, 0x9d, 0xec, 0x00};
    unsigned char jmp[9] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90};
    *(DWORD *)(jmp + 1) = (DWORD)hook_gt2_closed - (GT2_CLOSED_CB + 5);
    if (!ts_patch_bytes(GT2_CLOSED_CB, orig, jmp, sizeof jmp))
        ts_log("net: transport close callback not found (other exe)");
}

/* ---- joining a running level: state-machine signals held for a loading client ----
 * A client that joins a running level loads it for 10-20 s and acknowledges nothing meanwhile; the host keeps its
 * reliable packets until they are acknowledged, and its send pump (0x77e3c0) closes the connection when a peer has
 * more than 500 of them ("server closed" on the client; the host logs event 9 / 5). Reliable messages do go to
 * loading clients (0x697f30 skips only unreliable ones), and every frame the host broadcasts message 0x43a for each
 * isSyncMP + isSyncSML object whose state machine raised a signal in that frame (base entity frame 0x6442f0: the
 * 64-bit signal mask at +0x3c in a 16-byte body). On ts2_city_part03 that is ~650 messages a second, one more
 * unacknowledged packet every frame: the joiner was dropped 5.6-7 s after joining, always (TS_NETDEBUG run
 * n100pair-20261025-152109: 43a x552-678 per second, unacked 39 -> 526, then the close).
 * Now the host holds 0x43a for a slot that does not run the level yet (slot flag 1), ORs the masks per object, and
 * sends one 0x43a per object once that client runs the level (its earlier messages, incl. the join state, are then
 * all delivered). Other reliable messages still go at once (a few per second). TS_JOINHOLD=0 = stock behaviour.
 *
 * TS_NETDEBUG=1 (lab): every second each connection's unacknowledged reliable packets (channel +0xc) and the game
 * messages sent to loading slots, by id ("R" = reliable). TS_NETQUEUE=<n> (lab) moves the limit of 500. */
#define PEER_NETWORK_PTR (*(char **)0x1096148)
#define GAME_MODE_PTR (*(char **)0xfcab34)    /* +0x3c mode, +0x28 slots (stride 0xa4: +0 client id, +8 flags) */
#define SEND_TO_SLOT_FN 0x0063ea90            /* ecx body, edx flags; (slot, entity, u16 msg id); ret 0xc */
#define FIND_BY_NET_ID 0x00947c10             /* thiscall (scheduler 0xecb7f8, net id) -> entity or 0; ret 4 */
#define MSG_SML_SIGNALS 0x43a
#define HELD_MAX 512
#define HELD_SLOTS 32
static int netdebug, joinhold = 1;
static struct { unsigned short id; unsigned char reliable; int n; } dbg_sends[64];
static int n_dbg_sends;
typedef struct { int net_id; DWORD body[4]; } Held;
static Held held[HELD_SLOTS][HELD_MAX];
static int n_held[HELD_SLOTS], held_client[HELD_SLOTS], held_msgs[HELD_SLOTS], held_lost[HELD_SLOTS], held_gone[HELD_SLOTS];
static int sending_held;

/* (TS_NETDEBUG) packets a channel receives: count, sequence range, the channel's latency estimate +0x220 */
typedef void (__attribute__((thiscall)) *chrecv_t)(void *, unsigned char *, unsigned, int);
static int rx_n, rx_min = 1 << 30, rx_max = -1, rx_lat;
static void __attribute__((thiscall)) hook_chrecv(void *ch, unsigned char *buf, unsigned len, int flag)
{
    ((chrecv_t)0x79f390)(ch, buf, len, flag);
    if (len >= 4) {
        int seq = buf[2] | buf[3] << 8;
        rx_n++;
        if (seq < rx_min)
            rx_min = seq;
        if (seq > rx_max)
            rx_max = seq;
        rx_lat = *(int *)((char *)ch + 0x220);
    }
}

static int slot_index(char *slot)
{
    char *gm = GAME_MODE_PTR;
    if (!gm || !slot)
        return -1;
    int i = (int)(slot - *(char **)(gm + 0x28));
    return i >= 0 && i % 0xa4 == 0 && i / 0xa4 < *(int *)(gm + 0x2c) && i / 0xa4 < HELD_SLOTS ? i / 0xa4 : -1;
}

/* 1 = send now, 0 = held (the call is skipped) */
static int __attribute__((cdecl, used)) filter_send(DWORD *args, DWORD flags, DWORD *body)
{
    char *slot = (char *)args[0], *ent = (char *)args[1];
    unsigned id = args[2] & 0xffff;
    if (!slot || (*(int *)(slot + 8) & 1) || sending_held)
        return 1;
    if (netdebug) {
        int rel = (flags & 0x20000) != 0, k;
        for (k = 0; k < n_dbg_sends && !(dbg_sends[k].id == id && dbg_sends[k].reliable == rel); k++)
            ;
        static int logged_ids;
        if (id != MSG_SML_SIGNALS && id >= 2 && logged_ids < 20 && ent) {
            logged_ids++;
            ts_log("netdebug: %03x to a loading client: entity net %d vt %08lx, body %08lx %08lx %08lx", id,
                   *(int *)(ent + 0x18), *(unsigned long *)ent, body ? body[0] : 0, body ? body[1] : 0,
                   body ? body[2] : 0);
        }
        if (k < n_dbg_sends)
            dbg_sends[k].n++;
        else if (n_dbg_sends < 64) {
            dbg_sends[n_dbg_sends].id = (unsigned short)id;
            dbg_sends[n_dbg_sends].reliable = (unsigned char)rel;
            dbg_sends[n_dbg_sends++].n = 1;
        }
    }
    int s = slot_index(slot);
    if (!joinhold || id != MSG_SML_SIGNALS || s < 0 || !ent || !body || (body[0] >> 16) != 16)
        return 1;
    int net_id = *(int *)(ent + 0x18), k;
    if (n_held[s] && held_client[s] != *(int *)slot)
        n_held[s] = 0, held_msgs[s] = 0, held_lost[s] = 0, held_gone[s] = 0;   /* (another client in the slot) */
    if (!n_held[s] && held_client[s] != *(int *)slot)
        held_msgs[s] = 0, held_lost[s] = 0, held_gone[s] = 0;
    held_client[s] = *(int *)slot;
    held_msgs[s]++;
    for (k = 0; k < n_held[s] && held[s][k].net_id != net_id; k++)
        ;
    if (k == n_held[s]) {
        if (k == HELD_MAX) {
            held_lost[s]++;   /* (full: sent as before) */
            return 1;
        }
        held[s][k].net_id = net_id;
        memcpy(held[s][k].body, body, 16);
        n_held[s]++;
        if (netdebug && k < 12)
            ts_log("netdebug: held 0x43a of net %d vt %08lx", net_id, *(unsigned long *)ent);
    } else
        for (int w = 1; w < 4; w++)
            held[s][k].body[w] |= body[w];
    return 0;
}

__attribute__((naked)) static void hook_send_to_slot(void)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "pushad\n"
        "lea eax, [esp + 0x24]\n"
        "push ecx\n"
        "push edx\n"
        "push eax\n"
        "call _filter_send\n"
        "add esp, 12\n"
        "test eax, eax\n"
        "popad\n"                              /* (keeps the flags of the test) */
        "jz 1f\n"
        "mov eax, 0x2024\n"                    /* (the instruction the jump replaced) */
        "push 0x0063ea95\n"
        "ret\n"
        "1:\n"
        "xor eax, eax\n"
        "ret 12\n"
        ".att_syntax prefix\n");
}

static void send_held(char *slot, char *ent, DWORD *body)
{
    unsigned fn = SEND_TO_SLOT_FN, flags = 0x20000;
    void *b = body;
    __asm__ volatile("pushl $0x43a\n\tpushl %[ent]\n\tpushl %[slot]\n\tcall *%[fn]"   /* (MSG_SML_SIGNALS) */
                     : "+c"(b), "+d"(flags)
                     : [slot] "r"(slot), [ent] "r"(ent), [fn] "r"(fn)
                     : "eax", "memory", "cc");
}

static char *find_by_net_id(int net_id)
{
    char *e;
    void *sched = (void *)0xecb7f8;
    __asm__ volatile("pushl %[id]\n\tcall *%[fn]"
                     : "=a"(e), "+c"(sched)
                     : [id] "r"(net_id), [fn] "r"(FIND_BY_NET_ID)
                     : "edx", "memory", "cc");
    return e;
}

/* (main thread, every frame) */
static void held_frame(void)
{
    char *gm = GAME_MODE_PTR;
    int mode = gm ? *(int *)(gm + 0x3c) : 0;
    for (int s = 0; s < HELD_SLOTS; s++) {
        if (!n_held[s])
            continue;
        char *slot = (mode == 2 || mode == 4) && s < *(int *)(gm + 0x2c) ? *(char **)(gm + 0x28) + s * 0xa4 : NULL;
        if (!slot || *(int *)slot != held_client[s]) {
            ts_log("net: client %d left before it ran the level (%d held state-machine messages dropped)",
                   held_client[s], held_msgs[s]);
            n_held[s] = 0;
            continue;
        }
        if (!(*(int *)(slot + 8) & 1)) {
            static DWORD last_purge;
            DWORD now = GetTickCount();
            if (now - last_purge >= 1000) {   /* (objects that ended meanwhile, e.g. tracers, make room) */
                last_purge = now;
                int w = 0;
                for (int k = 0; k < n_held[s]; k++)
                    if (find_by_net_id(held[s][k].net_id))
                        held[s][w++] = held[s][k];
                    else
                        held_gone[s]++;
                n_held[s] = w;
            }
            continue;
        }
        int sent = 0;
        sending_held = 1;
        for (int k = 0; k < n_held[s]; k++) {
            char *ent = find_by_net_id(held[s][k].net_id);
            if (ent)
                send_held(slot, ent, held[s][k].body), sent++;
        }
        sending_held = 0;
        ts_log("net: client %d runs the level: %d state-machine messages held during its load sent as %d (%d objects "
               "gone, %d not held)", held_client[s], held_msgs[s], sent, n_held[s] - sent + held_gone[s], held_lost[s]);
        held_gone[s] = 0;
        n_held[s] = 0;
    }
}

/* ---- send rates in co-op sessions ----
 * The transport's per-connection rate control (0x77e890: starts at the minimum, x1.15 per second while the peer keeps
 * up, x0.8 or x0.5 on delays) works between limits set by 0x77ee10: on the server 1500 bytes/s minimum and
 * (players + 2 x ...) x 375 + 3625 maximum per client, 4000 bytes/s with one client; on a client 650-4000 upwards.
 * Made for 16 players of a 2007 match; a campaign level has hundreds of networked objects. A client that joined a
 * running ts2_city_part02 got its join state (the descriptors of every object, ~60 KB) at 1.5-4 KB/s: 13 s of
 * waiting after its load, the host's unacknowledged packets at 300+ (TS_NETDEBUG "c3 1500 (1500-4000)", run
 * n100pair-20261025-172429), and in 2 of 13 joins it never got there. In a co-op session the limits are raised every
 * frame to at least 16 KB/s minimum and 96 KB/s maximum per connection (TS_NETRATE=<min>,<max> in bytes/s; 0 = stock),
 * and a connection below the minimum is put at it. */
static int rate_min = 16000, rate_max = 96000;

static void rate_frame(void)
{
    char *pn = PEER_NETWORK_PTR;
    if (!rate_min || !pn || !ts_coop_active())
        return;
    for (int i = 0; i < 32; i++) {
        int *conn = (int *)(pn + 0xb94 + i * 0x6c);
        if (!conn[0] || !conn[1])
            continue;
        if (conn[0x17] < rate_min)
            conn[0x17] = rate_min;
        if (conn[0x18] < rate_max)
            conn[0x18] = rate_max;
        if (conn[3] < conn[0x17])
            conn[3] = conn[0x17];
    }
}

static void netfix_frame(float dt)
{
    static DWORD last;
    (void)dt;
    held_frame();
    rate_frame();
    if (!netdebug)
        return;
    DWORD now = GetTickCount();
    if (now - last < 1000)
        return;
    last = now;
    char *mgr = *(char **)0x1095b6c;
    if (mgr) {   /* (the AI manager's vehicle-type list: entries that are not live objects) */
        int n = *(int *)(mgr + 0x1a0), bad = 0;
        for (int i = 0; i < n && i < 100; i++) {
            char *o = *(char **)(mgr + 0x1a4 + i * 4);
            DWORD vt = o && !IsBadReadPtr(o, 0x234) ? *(DWORD *)o : 0;
            bad += !(vt >= 0xd11000 && vt < 0xe27000);
        }
        static int last_n = -1, last_bad = -1;
        if (n != last_n || bad != last_bad)
            ts_log("netdebug: AI manager list %d entries, %d not objects", n, bad);
        last_n = n, last_bad = bad;
    }
    char *pn = PEER_NETWORK_PTR;
    if (!pn)
        return;
    char line[512] = "";
    for (int i = 0; i < 32; i++) {
        int *conn = (int *)(pn + 0xb94 + i * 0x6c);
        if (!conn[0] || !conn[1])
            continue;
        char *ch = (char *)conn[1];
        snprintf(line + strlen(line), sizeof line - strlen(line),
                 " [%d] state %d c3 %d (%d-%d) c7 %d c8 %d lat %d rate %d unacked %d sent %d err %d seq %d/%d/%d/%d win %d fl %x",
                 i, conn[0], conn[3], conn[0x17], conn[0x18], conn[7], conn[8], *(int *)(ch + 0x220), *(int *)(pn + 0xb88 + 0xd98),
                 *(int *)(ch + 0xc), *(int *)(ch + 0x24), *(int *)(ch + 0x254), *(unsigned short *)(ch + 0x198),
                 *(unsigned short *)(ch + 0x19a), *(unsigned short *)(ch + 0x19c), *(unsigned short *)(ch + 0x19e),
                 *(int *)(ch + 0x1a0), *(unsigned *)(ch + 0x1a4));
    }
    if (line[0])
        ts_log("netdebug: connections%s; received %d packets, seq %d-%d, latency %d", line, rx_n, rx_min, rx_max, rx_lat);
    rx_n = 0, rx_min = 1 << 30, rx_max = -1;
    if (n_dbg_sends) {
        line[0] = 0;
        for (int i = 0; i < n_dbg_sends; i++)
            snprintf(line + strlen(line), sizeof line - strlen(line), " %03x%s x%d", dbg_sends[i].id,
                     dbg_sends[i].reliable ? "R" : "", dbg_sends[i].n);
        ts_log("netdebug: to loading clients:%s", line);
        n_dbg_sends = 0;
    }
}

static void held_install(void)
{
    char v[16];
    netdebug = ts_env("TS_NETDEBUG", v, sizeof v) && v[0] == '1';
    joinhold = !(ts_env("TS_JOINHOLD", v, sizeof v) && v[0] == '0');
    if (ts_env("TS_NETRATE", v, sizeof v)) {
        rate_min = atoi(v);
        const char *c = strchr(v, ',');
        rate_max = c ? atoi(c + 1) : rate_min * 6;
        ts_log("net: TS_NETRATE %d-%d bytes/s per connection in co-op sessions", rate_min, rate_max);
    }
    if (!joinhold)
        ts_log("net: TS_JOINHOLD=0: state-machine messages go to loading clients as in the stock game");
    if (joinhold || netdebug || rate_min) {
        static const unsigned char orig[5] = {0xb8, 0x24, 0x20, 0x00, 0x00};
        unsigned char jmp[5] = {0xe9};
        *(DWORD *)(jmp + 1) = (DWORD)hook_send_to_slot - (SEND_TO_SLOT_FN + 5);
        int ok = ts_patch_bytes(SEND_TO_SLOT_FN, orig, jmp, 5) && ts_tick_install() && ts_frame_add(netfix_frame);
        if (!ok)
            ts_log("net: per-peer sender not found (other exe): joins into running levels may drop");
        if (netdebug) {
            void *o = (void *)0x79f390, *h = (void *)hook_chrecv;
            ts_log("net: TS_NETDEBUG %s, receive count %s", ok ? "on" : "failed",
                   ts_patch_bytes(0xdb573c, &o, &h, 4) ? "on" : "failed");
        }
    }
    if (ts_env("TS_NETQUEUE", v, sizeof v) && atoi(v) > 0) {
        static const unsigned char o4[4] = {0xf4, 0x01, 0x00, 0x00};
        int lim = atoi(v);
        ts_log("net: TS_NETQUEUE unacked limit 500 -> %d: %s", lim, ts_patch_bytes(0x0077e4d5, o4, &lim, 4) ? "ok" : "failed");
    }
}

/* ---- the AI manager's list of vehicle-type targets on a client ----
 * The AI manager [0x1095b6c] keeps a list of objects of one class family (+0x1a0 count, +0x1a4 entries, at most 40;
 * added by 0x8671a0 from the class's init 0x8a1d70, removed by 0x867210 when such an object ends). Both refuse in
 * game mode 3, and the removal also needs the manager's flag +0xa8 bit 2. A client that joins a running level adds
 * them while its level loads and then ends the ones the server no longer has (destroyed before the join): their
 * pointers stayed in the list, and the next AI the client initialised read freed memory while building its target
 * list (0x867440 -> 0x8672a0, crash at 0x8672d3 reading [garbage + 4]; 3 of 4 joins into ts2_city_part03 once the
 * drops were fixed, runs n100pair-20261025-153042, -153225, -153550). The removal now runs in every mode and
 * whatever the flag: taking an ending object out of a list is always right. */
static void ailist_install(void)
{
    static const unsigned char je_mode[2] = {0x74, 0x69}, je_flag[2] = {0x74, 0x3a}, nop2[2] = {0x90, 0x90};
    char v[8];
    if (ts_env("TS_AILIST", v, sizeof v) && v[0] == '0') {   /* (lab: the stock behaviour) */
        ts_log("net: TS_AILIST=0: AI manager list as in the stock game");
        return;
    }
    if (!ts_patch_bytes(0x00867226, je_mode, nop2, 2) || !ts_patch_bytes(0x00867255, je_flag, nop2, 2))
        ts_log("net: AI manager list removal not found (other exe)");
}

void netfix_install(void)
{
    ailist_install();
    saves_install();
    ai_death_install();
    events_install();
    closes_install();
    held_install();
    real_sendto = (sendto_t)ts_patch_ws2("sendto", (void *)hook_sendto);
    char v[64];
    if (ts_env("TS_PORT_SHIFT", v, sizeof v) && (port_shift = atoi(v)) != 0)
        real_bind = (bind_t)ts_patch_ws2("bind", (void *)hook_bind);
    if (ts_env("TS_MASTER", v, sizeof v))
        strcpy(master, _stricmp(v, "off") == 0 ? "" : v);
    if (master[0]) {
        real_gethostbyname = (gethostbyname_t)ts_patch_ws2("gethostbyname", (void *)hook_gethostbyname);
        real_WSAAsyncGetHostByName = (WSAAsyncGetHostByName_t)ts_patch_ws2("WSAAsyncGetHostByName",
                                                                           (void *)hook_WSAAsyncGetHostByName);
    }
}
