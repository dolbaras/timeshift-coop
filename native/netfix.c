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
 */
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>
#include <stdio.h>
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

void netfix_install(void)
{
    real_sendto = (sendto_t)ts_patch_ws2("sendto", (void *)hook_sendto);
    char v[64];
    if (ts_env("TS_MASTER", v, sizeof v))
        strcpy(master, _stricmp(v, "off") == 0 ? "" : v);
    if (master[0]) {
        real_gethostbyname = (gethostbyname_t)ts_patch_ws2("gethostbyname", (void *)hook_gethostbyname);
        real_WSAAsyncGetHostByName = (WSAAsyncGetHostByName_t)ts_patch_ws2("WSAAsyncGetHostByName",
                                                                           (void *)hook_WSAAsyncGetHostByName);
    }
}
