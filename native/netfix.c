/*
 * The game's own multiplayer on today's networks (always on).
 *
 * LAN server list: the game asks for servers with a broadcast to 255.255.255.255 (UDP 7777-7877).
 * Windows sends such a broadcast through one adapter only (the lowest metric): on a PC with a VPN
 * adapter (Radmin, Hamachi, ZeroTier...) the request never reaches the real LAN and the list stays
 * empty (seen 2026-10-25). Each such send is repeated to the directed broadcast address of every
 * other IPv4 network the PC is on.
 */
#include <winsock2.h>
#include <iphlpapi.h>
#include <windows.h>

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

void netfix_install(void)
{
    real_sendto = (sendto_t)ts_patch_ws2("sendto", (void *)hook_sendto);
}
