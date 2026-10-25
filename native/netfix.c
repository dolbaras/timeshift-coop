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

void netfix_install(void)
{
    saves_install();
    ai_death_install();
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
