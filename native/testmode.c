/*
 * Test instances: window placement, render adapter, running unfocused (env driven; all off in a
 * player's game unless noted).
 *
 *   TS_WINPOS=x,y        create the game window with its client area at x,y (screen coordinates)
 *                        and keep it there (a test instance opens on the chosen monitor).
 *   TS_WINSIZE=w,h       keep the window's client area w x h.
 *   TS_ADAPTER=window    create the Direct3D 9 device on the adapter driving the window's monitor
 *                        (the game always asks for adapter 0); TS_ADAPTER=<n>: adapter n.
 *   TS_BACKGROUND=1      the game never takes focus and keeps running when it is not in front;
 *                        keyboard, mouse and GetAsyncKeyState reach it only while it really is
 *                        in front (it never reads the user's input meant for another window).
 *   TS_NETLOG=1          the game's socket calls in the log: sockets, bind/connect/listen/accept,
 *                        name lookups, options, and the first datagrams per socket and peer.
 *   TS_SHOTDIR=<dir>     rendered frames saved as <dir>\shotNNN.bmp (NNN: seconds since the start), at
 *   TS_SHOTS=s1,s2,...   these seconds and whenever a file <dir>\shot.req appears (it is deleted);
 *                        needs TS_BACKGROUND or TS_ADAPTER (the device hook).
 *
 * Hooks go through the exe's import table and COM vtables; the game code is not patched.
 */
#include <winsock2.h>
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

static int have_pos, win_x, win_y, have_size, win_w, win_h, background;
static char adapter_mode[16];
static HWND game_window;
static WNDPROC game_wndproc;

static int really_in_front(void)
{
    return game_window && GetForegroundWindow() == game_window;
}

/* ---- window ---- */
typedef HWND (WINAPI *CreateWindowExA_t)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef BOOL (WINAPI *SetWindowPos_t)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *MoveWindow_t)(HWND, int, int, int, int, BOOL);
typedef BOOL (WINAPI *ShowWindow_t)(HWND, int);
typedef HWND (WINAPI *SetFocus_t)(HWND);
typedef void (WINAPI *SwitchToThisWindow_t)(HWND, BOOL);
typedef SHORT (WINAPI *GetAsyncKeyState_t)(int);
typedef BOOL (WINAPI *GetKeyboardState_t)(PBYTE);
static CreateWindowExA_t real_CreateWindowExA;
static SetWindowPos_t real_SetWindowPos;
static MoveWindow_t real_MoveWindow;
static ShowWindow_t real_ShowWindow;
static SetFocus_t real_SetFocus;
static SwitchToThisWindow_t real_SwitchToThisWindow;
static GetAsyncKeyState_t real_GetAsyncKeyState;
static GetKeyboardState_t real_GetKeyboardState;

/* The outer rectangle for the wanted client rectangle at win_x, win_y. */
static void outer_rect(HWND w, int *x, int *y, int *cw, int *ch)
{
    DWORD style = (DWORD)GetWindowLongA(w, GWL_STYLE), ex = (DWORD)GetWindowLongA(w, GWL_EXSTYLE);
    RECT cr;
    GetClientRect(w, &cr);
    RECT r = {win_x, win_y, win_x + (have_size ? win_w : cr.right), win_y + (have_size ? win_h : cr.bottom)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    *x = r.left, *y = r.top, *cw = r.right - r.left, *ch = r.bottom - r.top;
}

static void place(HWND w)
{
    int x, y, cw, ch;
    outer_rect(w, &x, &y, &cw, &ch);
    real_SetWindowPos(w, NULL, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

static LRESULT CALLBACK bg_wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (background) {
        /* the game pauses (and may minimise) when it loses activation: it never hears of it */
        if (msg == WM_ACTIVATEAPP || msg == WM_ACTIVATE || msg == WM_NCACTIVATE) {
            if (msg == WM_NCACTIVATE)
                return DefWindowProcA(w, msg, TRUE, lp);
            return 0;
        }
        if (msg == WM_KILLFOCUS)
            return 0;
    }
    if (have_pos && msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS *p = (WINDOWPOS *)lp;
        int x, y, cw, ch;
        outer_rect(w, &x, &y, &cw, &ch);
        p->x = x, p->y = y;
        if (have_size)
            p->cx = cw, p->cy = ch;
        p->flags &= ~(UINT)(SWP_NOMOVE | (have_size ? SWP_NOSIZE : 0));
    }
    return CallWindowProcA(game_wndproc, w, msg, wp, lp);
}

static HWND WINAPI hook_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    int top = parent == NULL && !(style & WS_CHILD);
    if (top && have_pos)
        x = win_x, y = win_y;
    HWND hw = real_CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (hw && top && !game_window && (style & (WS_CAPTION | WS_POPUP))) {
        game_window = hw;
        game_wndproc = (WNDPROC)SetWindowLongA(hw, GWL_WNDPROC, (LONG)bg_wndproc);
        if (have_pos)
            place(hw);
        ts_log("window %p class=%s style=%08lx created", (void *)hw, IS_INTRESOURCE(cls) ? "#" : cls, style);
    }
    return hw;
}

static BOOL WINAPI hook_ShowWindow(HWND w, int cmd)
{
    if (background && w == game_window && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_RESTORE))
        cmd = SW_SHOWNOACTIVATE;
    return real_ShowWindow(w, cmd);
}

static BOOL WINAPI hook_SetWindowPos(HWND w, HWND after, int x, int y, int cx, int cy, UINT f)
{
    if (background && w == game_window) {
        f |= SWP_NOACTIVATE;
        if (after == HWND_TOP || after == HWND_TOPMOST)   /* (never over the owner's windows) */
            f |= SWP_NOZORDER;
    }
    return real_SetWindowPos(w, after, x, y, cx, cy, f);
}

static BOOL WINAPI hook_MoveWindow(HWND w, int x, int y, int cx, int cy, BOOL repaint)
{
    return real_MoveWindow(w, x, y, cx, cy, repaint);   /* (WM_WINDOWPOSCHANGING keeps the place) */
}

static HWND WINAPI hook_SetFocus(HWND w)
{
    if (background && !really_in_front())
        return NULL;
    return real_SetFocus(w);
}

static void WINAPI hook_SwitchToThisWindow(HWND w, BOOL alt)
{
    if (background)
        return;
    real_SwitchToThisWindow(w, alt);
}

static SHORT WINAPI hook_GetAsyncKeyState(int key)
{
    if (background && !really_in_front())
        return 0;
    return real_GetAsyncKeyState(key);
}

static BOOL WINAPI hook_GetKeyboardState(PBYTE keys)
{
    BOOL ok = real_GetKeyboardState(keys);
    if (ok && background && !really_in_front())
        memset(keys, 0, 256);
    return ok;
}

/* ---- message boxes and dialogs: always logged (a start that waits on one shows nothing else) ---- */
typedef int (WINAPI *MessageBoxA_t)(HWND, LPCSTR, LPCSTR, UINT);
typedef int (WINAPI *MessageBoxW_t)(HWND, LPCWSTR, LPCWSTR, UINT);
typedef INT_PTR (WINAPI *DialogBoxParamA_t)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
typedef HWND (WINAPI *CreateDialogParamA_t)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static MessageBoxA_t real_MessageBoxA;
static MessageBoxW_t real_MessageBoxW;
static DialogBoxParamA_t real_DialogBoxParamA;
static CreateDialogParamA_t real_CreateDialogParamA;

static int WINAPI hook_MessageBoxA(HWND w, LPCSTR text, LPCSTR cap, UINT type)
{
    ts_log("MessageBoxA \"%s\": %s", cap ? cap : "", text ? text : "");
    int r = real_MessageBoxA(w, text, cap, type);
    ts_log("MessageBoxA answered %d", r);
    return r;
}

static int WINAPI hook_MessageBoxW(HWND w, LPCWSTR text, LPCWSTR cap, UINT type)
{
    char t[512], c[128];
    WideCharToMultiByte(CP_UTF8, 0, text ? text : L"", -1, t, sizeof t, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, cap ? cap : L"", -1, c, sizeof c, NULL, NULL);
    ts_log("MessageBoxW \"%s\": %s", c, t);
    int r = real_MessageBoxW(w, text, cap, type);
    ts_log("MessageBoxW answered %d", r);
    return r;
}

static INT_PTR WINAPI hook_DialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    ts_log("DialogBoxParamA template %s", IS_INTRESOURCE(tmpl) ? "(id)" : tmpl);
    if (IS_INTRESOURCE(tmpl))
        ts_log("  id %u", (unsigned)(UINT_PTR)tmpl);
    INT_PTR r = real_DialogBoxParamA(inst, tmpl, parent, proc, lp);
    ts_log("DialogBoxParamA returned %ld", (long)r);
    return r;
}

static HWND WINAPI hook_CreateDialogParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp)
{
    ts_log("CreateDialogParamA template %u", IS_INTRESOURCE(tmpl) ? (unsigned)(UINT_PTR)tmpl : 0);
    return real_CreateDialogParamA(inst, tmpl, parent, proc, lp);
}

/* ---- TS_FILELOG=1: every file the game opens or fails to open (tests) ---- */
typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef HFILE (WINAPI *OpenFile_t)(LPCSTR, LPOFSTRUCT, UINT);
static CreateFileA_t real_CreateFileA;
static OpenFile_t real_OpenFile;

static HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    HANDLE h = real_CreateFileA(name, access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    ts_log("file %s %s%s", h == INVALID_HANDLE_VALUE ? "FAILED" : "open", name ? name : "(null)",
           (access & GENERIC_WRITE) ? " (write)" : "");
    SetLastError(err);
    return h;
}

typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static CreateFileW_t real_CreateFileW;

static HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp,
                                      DWORD flags, HANDLE tmpl)
{
    HANDLE h = real_CreateFileW(name, access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    char n[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, name ? name : L"(null)", -1, n, sizeof n, NULL, NULL);
    ts_log("fileW %s %s%s", h == INVALID_HANDLE_VALUE ? "FAILED" : "open", n, (access & GENERIC_WRITE) ? " (write)" : "");
    SetLastError(err);
    return h;
}

static HFILE WINAPI hook_OpenFile(LPCSTR name, LPOFSTRUCT of, UINT style)
{
    HFILE h = real_OpenFile(name, of, style);
    ts_log("OpenFile %s style=0x%x -> %d", name ? name : "(null)", style, (int)h);
    return h;
}

/* ---- TS_NETLOG=1: the game's socket calls (tests; finding what the network code does) ---- */
typedef SOCKET (WINAPI *socket_t)(int, int, int);
typedef int (WINAPI *bind_t)(SOCKET, const struct sockaddr *, int);
typedef int (WINAPI *listen_t)(SOCKET, int);
typedef SOCKET (WINAPI *accept_t)(SOCKET, struct sockaddr *, int *);
typedef int (WINAPI *sendto_t)(SOCKET, const char *, int, int, const struct sockaddr *, int);
typedef int (WINAPI *recvfrom_t)(SOCKET, char *, int, int, struct sockaddr *, int *);
typedef int (WINAPI *send_t)(SOCKET, const char *, int, int);
typedef int (WINAPI *recv_t)(SOCKET, char *, int, int);
typedef int (WINAPI *setsockopt_t)(SOCKET, int, int, const char *, int);
typedef int (WINAPI *closesocket_t)(SOCKET);
typedef struct hostent *(WINAPI *gethostbyname_t)(const char *);
typedef HANDLE (WINAPI *WSAAsyncGetHostByName_t)(HWND, u_int, const char *, char *, int);
static socket_t real_socket;
static bind_t real_bind, real_connect;
static listen_t real_listen;
static accept_t real_accept;
static sendto_t real_sendto;
static recvfrom_t real_recvfrom;
static send_t real_send;
static recv_t real_recv;
static setsockopt_t real_setsockopt;
static closesocket_t real_closesocket;
static gethostbyname_t real_gethostbyname;
static WSAAsyncGetHostByName_t real_WSAAsyncGetHostByName;

static const char *addr_str(const struct sockaddr *a, char *out, int size)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    if (!a || in->sin_family != AF_INET)
        return snprintf(out, size, "(family %d)", a ? a->sa_family : -1), out;
    const unsigned char *ip = (const unsigned char *)&in->sin_addr;
    snprintf(out, size, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], ntohs(in->sin_port));
    return out;
}

/* Datagrams: the first 4 per (socket, direction, peer) and then the 10th, 100th, 1000th... */
#define NET_PEERS 128
static struct { SOCKET s; int dir; ULONG ip; USHORT port; LONG n; } net_peers[NET_PEERS];
static CRITICAL_SECTION net_lock;

static LONG net_count(SOCKET s, int dir, const struct sockaddr *a)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)a;
    ULONG ip = a && in->sin_family == AF_INET ? in->sin_addr.s_addr : 0;
    USHORT port = a && in->sin_family == AF_INET ? in->sin_port : 0;
    LONG n = 0;
    EnterCriticalSection(&net_lock);
    for (int i = 0; i < NET_PEERS; i++) {
        if (net_peers[i].n && (net_peers[i].s != s || net_peers[i].dir != dir || net_peers[i].ip != ip
                               || net_peers[i].port != port))
            continue;
        if (!net_peers[i].n)
            net_peers[i].s = s, net_peers[i].dir = dir, net_peers[i].ip = ip, net_peers[i].port = port;
        n = ++net_peers[i].n;
        break;
    }
    LeaveCriticalSection(&net_lock);
    return n;   /* (0: table full, not logged) */
}

static int net_worth_logging(LONG n)
{
    if (n >= 1 && n <= 4)
        return 1;
    for (LONG m = 10; m > 0 && m <= n; m *= 10)
        if (n == m)
            return 1;
    return 0;
}

static void net_log_data(const char *what, SOCKET s, const struct sockaddr *peer, const char *buf, int len, LONG n)
{
    char a[32], hex[3 * 24 + 1], txt[25];
    int k = len < 24 ? len : 24;
    for (int i = 0; i < k; i++) {
        unsigned char c = (unsigned char)buf[i];
        snprintf(hex + 3 * i, 4, "%02x ", c);
        txt[i] = c >= 32 && c < 127 ? (char)c : '.';
    }
    hex[3 * k] = 0, txt[k] = 0;
    ts_log("net %s #%ld sock=%u %s len=%d: %s|%s|", what, n, (unsigned)s, addr_str(peer, a, sizeof a), len, hex, txt);
}

static SOCKET WINAPI hook_socket(int af, int type, int proto)
{
    SOCKET s = real_socket(af, type, proto);
    ts_log("net socket af=%d type=%s proto=%d -> %u", af, type == SOCK_DGRAM ? "udp" : type == SOCK_STREAM ? "tcp" : "?",
           proto, (unsigned)s);
    return s;
}

static int WINAPI hook_bind(SOCKET s, const struct sockaddr *a, int len)
{
    int r = real_bind(s, a, len);
    int err = r ? WSAGetLastError() : 0;
    char t[32];
    ts_log("net bind sock=%u %s -> %d (err %d)", (unsigned)s, addr_str(a, t, sizeof t), r, err);
    if (r)
        WSASetLastError(err);
    return r;
}

static int WINAPI hook_connect(SOCKET s, const struct sockaddr *a, int len)
{
    int r = real_connect(s, a, len);
    int err = r ? WSAGetLastError() : 0;
    char t[32];
    ts_log("net connect sock=%u %s -> %d (err %d)", (unsigned)s, addr_str(a, t, sizeof t), r, err);
    if (r)
        WSASetLastError(err);
    return r;
}

static int WINAPI hook_listen(SOCKET s, int backlog)
{
    int r = real_listen(s, backlog);
    ts_log("net listen sock=%u -> %d", (unsigned)s, r);
    return r;
}

static SOCKET WINAPI hook_accept(SOCKET s, struct sockaddr *a, int *len)
{
    SOCKET c = real_accept(s, a, len);
    if (c != INVALID_SOCKET) {
        char t[32];
        ts_log("net accept sock=%u from %s -> %u", (unsigned)s, addr_str(a, t, sizeof t), (unsigned)c);
    }
    return c;
}

static int WINAPI hook_sendto(SOCKET s, const char *buf, int len, int flags, const struct sockaddr *to, int tolen)
{
    int r = real_sendto(s, buf, len, flags, to, tolen);
    int err = r < 0 ? WSAGetLastError() : 0;
    LONG n = net_count(s, 0, to);
    if (net_worth_logging(n))
        net_log_data(r < 0 ? "sendto FAILED" : "sendto", s, to, buf, len, n);
    if (r < 0)
        WSASetLastError(err);
    return r;
}

static int WINAPI hook_recvfrom(SOCKET s, char *buf, int len, int flags, struct sockaddr *from, int *fromlen)
{
    int r = real_recvfrom(s, buf, len, flags, from, fromlen);
    if (r > 0) {
        LONG n = net_count(s, 1, from);
        if (net_worth_logging(n))
            net_log_data("recvfrom", s, from, buf, r, n);
    }
    return r;
}

static int WINAPI hook_send(SOCKET s, const char *buf, int len, int flags)
{
    int r = real_send(s, buf, len, flags);
    LONG n = net_count(s, 2, NULL);
    if (net_worth_logging(n))
        net_log_data("send", s, NULL, buf, len, n);
    return r;
}

static int WINAPI hook_recv(SOCKET s, char *buf, int len, int flags)
{
    int r = real_recv(s, buf, len, flags);
    if (r > 0) {
        LONG n = net_count(s, 3, NULL);
        if (net_worth_logging(n))
            net_log_data("recv", s, NULL, buf, r, n);
    }
    return r;
}

static int WINAPI hook_setsockopt(SOCKET s, int level, int opt, const char *val, int len)
{
    int r = real_setsockopt(s, level, opt, val, len);
    ts_log("net setsockopt sock=%u level=0x%x opt=0x%x val=%d -> %d", (unsigned)s, level, opt,
           val && len >= 4 ? *(const int *)val : val && len ? *val : -1, r);
    return r;
}

static int WINAPI hook_closesocket(SOCKET s)
{
    ts_log("net close sock=%u", (unsigned)s);
    return real_closesocket(s);
}

static struct hostent *WINAPI hook_gethostbyname(const char *name)
{
    struct hostent *h = real_gethostbyname(name);
    char t[32] = "failed";
    if (h && h->h_addr_list && h->h_addr_list[0]) {
        const unsigned char *ip = (const unsigned char *)h->h_addr_list[0];
        snprintf(t, sizeof t, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    ts_log("net gethostbyname %s -> %s", name ? name : "(null)", t);
    return h;
}

static HANDLE WINAPI hook_WSAAsyncGetHostByName(HWND w, u_int msg, const char *name, char *buf, int len)
{
    ts_log("net WSAAsyncGetHostByName %s", name ? name : "(null)");
    return real_WSAAsyncGetHostByName(w, msg, name, buf, len);
}

/* The exe imports ws2_32 by ordinal: its import slots are found by the address they hold. */
static void *patch_ws2(const char *func, void *hook)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    void *real = ws2 ? (void *)GetProcAddress(ws2, func) : NULL;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); real && d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), "WS2_32.dll") != 0)
            continue;
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++) {
            DWORD old;
            if (*slot != real || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old))
                continue;
            *slot = hook;
            VirtualProtect(slot, sizeof *slot, old, &old);
            return real;
        }
    }
    ts_log("net: import ws2_32!%s not found", func);
    return NULL;
}

static void netlog_install(void)
{
    InitializeCriticalSection(&net_lock);
#define NET_HOOK(fn) real_##fn = (fn##_t)patch_ws2(#fn, (void *)hook_##fn)
    NET_HOOK(socket);
    NET_HOOK(bind);
    real_connect = (bind_t)patch_ws2("connect", (void *)hook_connect);
    NET_HOOK(listen);
    NET_HOOK(accept);
    NET_HOOK(sendto);
    NET_HOOK(recvfrom);
    NET_HOOK(send);
    NET_HOOK(recv);
    NET_HOOK(setsockopt);
    NET_HOOK(closesocket);
    NET_HOOK(gethostbyname);
    NET_HOOK(WSAAsyncGetHostByName);
#undef NET_HOOK
    ts_log("net log on");
}

/* ---- DirectDraw (the game queries video memory through it): logged in test mode ---- */
typedef HRESULT (WINAPI *DirectDrawCreateEx_t)(GUID *, void **, REFIID, IUnknown *);
typedef HRESULT (WINAPI *DDSetCoop_t)(void *, HWND, DWORD);
static DirectDrawCreateEx_t real_DirectDrawCreateEx;
static DDSetCoop_t real_DDSetCoop;
static LONG n_ddraw;

static HRESULT WINAPI hook_DDSetCoop(void *dd, HWND w, DWORD flags)
{
    HRESULT hr = real_DDSetCoop(dd, w, flags);
    if (InterlockedIncrement(&n_ddraw) <= 20)
        ts_log("ddraw SetCooperativeLevel flags=0x%lx -> 0x%08lx", flags, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hook_DirectDrawCreateEx(GUID *guid, void **out, REFIID iid, IUnknown *outer)
{
    HRESULT hr = real_DirectDrawCreateEx(guid, out, iid, outer);
    if (InterlockedIncrement(&n_ddraw) <= 20)
        ts_log("DirectDrawCreateEx device=%s -> 0x%08lx", guid == NULL ? "primary" : "other", (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {   /* (IDirectDraw7::SetCooperativeLevel is slot 20) */
        void *p = ts_patch_vtable(*out, 20, (void *)hook_DDSetCoop);
        if (p)
            real_DDSetCoop = (DDSetCoop_t)p;
    }
    return hr;
}

/* ---- DirectInput: no keyboard/mouse data while another window is in front ---- */
typedef HRESULT (WINAPI *CreateDevice_DI_t)(void *, REFGUID, void **, void *);
typedef HRESULT (WINAPI *GetDeviceState_t)(void *, DWORD, void *);
typedef HRESULT (WINAPI *GetDeviceData_t)(void *, DWORD, void *, DWORD *, DWORD);
typedef HRESULT (WINAPI *SetCoopLevel_t)(void *, HWND, DWORD);
static CreateDevice_DI_t real_DICreateDevice;
static GetDeviceState_t real_GetDeviceState;
static GetDeviceData_t real_GetDeviceData;
static SetCoopLevel_t real_SetCoopLevel;

static HRESULT WINAPI bg_GetDeviceState(void *dev, DWORD size, void *data)
{
    HRESULT hr = real_GetDeviceState(dev, size, data);
    if (background && !really_in_front() && data)
        memset(data, 0, size);
    return hr;
}

static HRESULT WINAPI bg_GetDeviceData(void *dev, DWORD objsize, void *data, DWORD *count, DWORD flags)
{
    HRESULT hr = real_GetDeviceData(dev, objsize, data, count, flags);
    if (background && !really_in_front() && count && !(flags & 1 /* DIGDD_PEEK */))
        *count = 0;
    return hr;
}

/* Exclusive/foreground levels fail or lose the device for a window that is not in front:
 * a test instance asks for non-exclusive background access instead. */
static HRESULT WINAPI bg_SetCoopLevel(void *dev, HWND w, DWORD flags)
{
    if (background)
        flags = (flags & ~(DWORD)(0x1 | 0x4)) | 0x2 | 0x8;   /* EXCLUSIVE|FOREGROUND -> NONEXCLUSIVE|BACKGROUND */
    return real_SetCoopLevel(dev, w, flags);
}

/* IDirectInput8A::CreateDevice is slot 3; IDirectInputDevice8A: SetCooperativeLevel 13,
 * GetDeviceState 9, GetDeviceData 10. */
static HRESULT WINAPI hook_DICreateDevice(void *di, REFGUID guid, void **out, void *outer)
{
    HRESULT hr = real_DICreateDevice(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out) {
        void *p;
        if ((p = ts_patch_vtable(*out, 9, (void *)bg_GetDeviceState)))
            real_GetDeviceState = (GetDeviceState_t)p;
        if ((p = ts_patch_vtable(*out, 10, (void *)bg_GetDeviceData)))
            real_GetDeviceData = (GetDeviceData_t)p;
        if ((p = ts_patch_vtable(*out, 13, (void *)bg_SetCoopLevel)))
            real_SetCoopLevel = (SetCoopLevel_t)p;
    }
    return hr;
}

void testmode_wrap_dinput(void *dinput)
{
    void *p = ts_patch_vtable(dinput, 3, (void *)hook_DICreateDevice);
    if (p)
        real_DICreateDevice = (CreateDevice_DI_t)p;
}

/* ---- Direct3D 9 adapter ---- */
typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef HRESULT (WINAPI *D3DCreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                            IDirect3DDevice9 **);
static Direct3DCreate9_t real_Direct3DCreate9;
static D3DCreateDevice_t real_D3DCreateDevice;

static UINT pick_adapter(IDirect3D9 *d3d, HWND focus, UINT asked)
{
    UINT n = IDirect3D9_GetAdapterCount(d3d);
    if (adapter_mode[0] >= '0' && adapter_mode[0] <= '9') {
        UINT want = (UINT)atoi(adapter_mode);
        return want < n ? want : asked;
    }
    if (strcmp(adapter_mode, "window") == 0) {
        HMONITOR mon = have_pos ? MonitorFromPoint((POINT){win_x + 50, win_y + 50}, MONITOR_DEFAULTTONEAREST)
                                : MonitorFromWindow(focus, MONITOR_DEFAULTTONEAREST);
        for (UINT i = 0; i < n; i++)
            if (IDirect3D9_GetAdapterMonitor(d3d, i) == mon)
                return i;
        for (UINT i = 0; i < n; i++) {   /* (monitor not matched: stay off the owner's main GPU) */
            D3DADAPTER_IDENTIFIER9 id;
            memset(&id, 0, sizeof id);
            if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, i, 0, &id)) && strstr(id.Description, "Intel"))   /* (VendorId is reported as NVIDIA, compat.c) */
                return i;
        }
    }
    return asked;
}

/* ---- TS_SHOTS=s1,s2,... with TS_SHOTDIR=<dir>: the rendered frame at those seconds after the start as
 * <dir>\shotNNN.bmp (window captures of the D3D window come back blank or stale) ---- */
typedef HRESULT (WINAPI *Present_t)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
static Present_t real_Present;
static char shot_dir[MAX_PATH];
static int shot_secs[32], shot_count, shot_next;
static DWORD shot_t0;

static void save_frame(IDirect3DDevice9 *dev, int secs)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    char path[MAX_PATH + 32];
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) {
        ts_log("shot: back buffer format %d not supported", d.Format);
        goto done;
    }
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (SUCCEEDED(hr) && FAILED(IDirect3DDevice9_GetRenderTargetData(dev, bb, sys))) {
        /* (a multisampled or otherwise unreadable back buffer: through a plain render target) */
        IDirect3DSurface9 *rt = NULL;
        hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, rt, NULL, D3DTEXF_NONE);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9_GetRenderTargetData(dev, rt, sys);
        if (rt)
            IDirect3DSurface9_Release(rt);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        ts_log("shot: copy of the back buffer failed (hr=0x%08lx, %ux%u format %d multisample %d)", (unsigned long)hr,
               d.Width, d.Height, d.Format, d.MultiSampleType);
        goto done;
    }
    snprintf(path, sizeof path, "%s\\shot%03d.bmp", shot_dir, secs);
    FILE *f = fopen(path, "wb");
    if (f) {
        DWORD row = d.Width * 4, size = row * d.Height;
        BITMAPFILEHEADER fh = {0x4d42, sizeof fh + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof fh + sizeof(BITMAPINFOHEADER)};
        BITMAPINFOHEADER ih = {sizeof ih, (LONG)d.Width, -(LONG)d.Height, 1, 32, BI_RGB, size, 0, 0, 0, 0};
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        for (UINT y = 0; y < d.Height; y++)
            fwrite((BYTE *)lr.pBits + y * lr.Pitch, row, 1, f);
        fclose(f);
        ts_log("shot: %s (%ux%u)", path, d.Width, d.Height);
    }
    IDirect3DSurface9_UnlockRect(sys);
done:
    if (sys)
        IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}

static HRESULT WINAPI hook_Present(IDirect3DDevice9 *dev, const RECT *src, const RECT *dst, HWND w, const RGNDATA *dirty)
{
    static LONG fails;
    static unsigned frame;
    if (shot_next < shot_count && GetTickCount() - shot_t0 >= (DWORD)shot_secs[shot_next] * 1000)
        save_frame(dev, shot_secs[shot_next++]);
    if (shot_dir[0] && ++frame % 30 == 0) {   /* on request: a file shot.req in the folder (deleted when taken) */
        char req[MAX_PATH + 16];
        snprintf(req, sizeof req, "%s\\shot.req", shot_dir);
        if (GetFileAttributesA(req) != INVALID_FILE_ATTRIBUTES && DeleteFileA(req))
            save_frame(dev, (int)((GetTickCount() - shot_t0) / 1000));
    }
    HRESULT hr = real_Present(dev, src, dst, w, dirty);
    if (FAILED(hr) && InterlockedIncrement(&fails) <= 5)
        ts_log("Present failed hr=0x%08lx", (unsigned long)hr);
    return hr;
}

typedef HRESULT (WINAPI *TestCoop_t)(IDirect3DDevice9 *);
static TestCoop_t real_TestCoop;

static HRESULT WINAPI hook_TestCoop(IDirect3DDevice9 *dev)
{
    static LONG n;
    HRESULT hr = real_TestCoop(dev);
    if (FAILED(hr) && InterlockedIncrement(&n) <= 5)
        ts_log("device TestCooperativeLevel -> 0x%08lx", (unsigned long)hr);
    return hr;
}

/* A device reset (mode changes, lost devices): logged; a test instance stays windowed here too. */
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Reset_t real_Reset;

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    if (background && pp && !pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    HRESULT hr = real_Reset(dev, pp);
    ts_log("device Reset %ux%u windowed=%d -> hr=0x%08lx", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
           pp ? pp->Windowed : -1, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hook_D3DCreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                           D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    UINT use = pick_adapter(d3d, focus, adapter);
    if (background && pp && !pp->Windowed) {   /* a test instance never goes full screen */
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    D3DADAPTER_IDENTIFIER9 id;
    memset(&id, 0, sizeof id);
    IDirect3D9_GetAdapterIdentifier(d3d, use, 0, &id);
    HRESULT hr = real_D3DCreateDevice(d3d, use, type, focus, flags, pp, out);
    ts_log("CreateDevice asked=%u used=%u (%s) %ux%u windowed=%d hr=0x%08lx", adapter, use, id.Description,
           pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1, (unsigned long)hr);
    if (FAILED(hr) && use != adapter)
        hr = real_D3DCreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {   /* (IDirect3DDevice9: TestCooperativeLevel is slot 3, Reset 16, Present 17) */
        void *p = ts_patch_vtable(*out, 16, (void *)hook_Reset);
        if (p)
            real_Reset = (Reset_t)p;
        if ((p = ts_patch_vtable(*out, 3, (void *)hook_TestCoop)))
            real_TestCoop = (TestCoop_t)p;
        if (shot_dir[0] && (p = ts_patch_vtable(*out, 17, (void *)hook_Present)))
            real_Present = (Present_t)p;
    }
    return hr;
}

/* What the game asks Direct3D before it creates its device: logged (the first calls of each kind),
 * a start that stops at a capability check shows which one. */
typedef HRESULT (WINAPI *CheckDeviceType_t)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL);
typedef HRESULT (WINAPI *CheckDeviceFormat_t)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT);
typedef HRESULT (WINAPI *GetDeviceCaps_t)(IDirect3D9 *, UINT, D3DDEVTYPE, D3DCAPS9 *);
typedef UINT (WINAPI *GetAdapterModeCount_t)(IDirect3D9 *, UINT, D3DFORMAT);
static CheckDeviceType_t real_CheckDeviceType;
static CheckDeviceFormat_t real_CheckDeviceFormat;
static GetDeviceCaps_t real_GetDeviceCaps;
static GetAdapterModeCount_t real_GetAdapterModeCount;
static LONG n_cdt, n_cdf, n_caps, n_modes;

static HRESULT WINAPI hook_CheckDeviceType(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, D3DFORMAT f, D3DFORMAT bb, BOOL win)
{
    HRESULT hr = real_CheckDeviceType(d3d, a, t, f, bb, win);
    if (InterlockedIncrement(&n_cdt) <= 20)
        ts_log("d3d CheckDeviceType adapter=%u type=%d fmt=%d bb=%d windowed=%d -> 0x%08lx", a, t, f, bb, win, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hook_CheckDeviceFormat(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD use,
                                             D3DRESOURCETYPE rt, D3DFORMAT cf)
{
    HRESULT hr = real_CheckDeviceFormat(d3d, a, t, af, use, rt, cf);
    if (FAILED(hr) && InterlockedIncrement(&n_cdf) <= 40)
        ts_log("d3d CheckDeviceFormat adapter=%u usage=0x%lx rtype=%d fmt=0x%x -> 0x%08lx (failed)", a, use, rt, (unsigned)cf,
               (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hook_GetDeviceCaps(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, D3DCAPS9 *c)
{
    HRESULT hr = real_GetDeviceCaps(d3d, a, t, c);
    if (InterlockedIncrement(&n_caps) <= 4)
        ts_log("d3d GetDeviceCaps adapter=%u -> 0x%08lx vs=%08lx ps=%08lx maxtex=%lux%lu", a, (unsigned long)hr,
               c ? c->VertexShaderVersion : 0, c ? c->PixelShaderVersion : 0, c ? c->MaxTextureWidth : 0,
               c ? c->MaxTextureHeight : 0);
    return hr;
}

static UINT WINAPI hook_GetAdapterModeCount(IDirect3D9 *d3d, UINT a, D3DFORMAT f)
{
    UINT n = real_GetAdapterModeCount(d3d, a, f);
    if (InterlockedIncrement(&n_modes) <= 10)
        ts_log("d3d GetAdapterModeCount adapter=%u fmt=%d -> %u", a, f, n);
    return n;
}

static IDirect3D9 *WINAPI hook_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d = real_Direct3DCreate9(sdk);
    ts_log("Direct3DCreate9(%u) -> %p, %u adapters", sdk, (void *)d3d, d3d ? IDirect3D9_GetAdapterCount(d3d) : 0);
    if (d3d)
        compat_on_d3d(d3d);
    if (d3d && (adapter_mode[0] || background)) {
        void *p = ts_patch_vtable(d3d, 16, (void *)hook_D3DCreateDevice);
        if (p)
            real_D3DCreateDevice = (D3DCreateDevice_t)p;
    }
    if (d3d) {   /* (always: these only log) */
        void *p;
        if ((p = ts_patch_vtable(d3d, 9, (void *)hook_CheckDeviceType)))
            real_CheckDeviceType = (CheckDeviceType_t)p;
        if ((p = ts_patch_vtable(d3d, 10, (void *)hook_CheckDeviceFormat)))
            real_CheckDeviceFormat = (CheckDeviceFormat_t)p;
        if ((p = ts_patch_vtable(d3d, 14, (void *)hook_GetDeviceCaps)))
            real_GetDeviceCaps = (GetDeviceCaps_t)p;
        if ((p = ts_patch_vtable(d3d, 6, (void *)hook_GetAdapterModeCount)))
            real_GetAdapterModeCount = (GetAdapterModeCount_t)p;
    }
    return d3d;
}

void testmode_install(void)
{
    char v[64];
    if (ts_env("TS_WINPOS", v, sizeof v) && sscanf(v, "%d,%d", &win_x, &win_y) == 2)
        have_pos = 1;
    if (ts_env("TS_WINSIZE", v, sizeof v) && sscanf(v, "%d,%d", &win_w, &win_h) == 2 && win_w > 0 && win_h > 0)
        have_size = 1;
    background = ts_env("TS_BACKGROUND", v, sizeof v) && v[0] == '1';
    ts_env("TS_ADAPTER", adapter_mode, sizeof adapter_mode);
    real_CreateWindowExA = (CreateWindowExA_t)ts_patch_iat("USER32.dll", "CreateWindowExA", (void *)hook_CreateWindowExA);
    real_SetWindowPos = (SetWindowPos_t)ts_patch_iat("USER32.dll", "SetWindowPos", (void *)hook_SetWindowPos);
    real_MoveWindow = (MoveWindow_t)ts_patch_iat("USER32.dll", "MoveWindow", (void *)hook_MoveWindow);
    real_ShowWindow = (ShowWindow_t)ts_patch_iat("USER32.dll", "ShowWindow", (void *)hook_ShowWindow);
    real_SetFocus = (SetFocus_t)ts_patch_iat("USER32.dll", "SetFocus", (void *)hook_SetFocus);
    real_SwitchToThisWindow = (SwitchToThisWindow_t)ts_patch_iat("USER32.dll", "SwitchToThisWindow",
                                                                 (void *)hook_SwitchToThisWindow);
    real_GetAsyncKeyState = (GetAsyncKeyState_t)ts_patch_iat("USER32.dll", "GetAsyncKeyState", (void *)hook_GetAsyncKeyState);
    real_GetKeyboardState = (GetKeyboardState_t)ts_patch_iat("USER32.dll", "GetKeyboardState", (void *)hook_GetKeyboardState);
    real_Direct3DCreate9 = (Direct3DCreate9_t)ts_patch_iat("d3d9.dll", "Direct3DCreate9", (void *)hook_Direct3DCreate9);
    if (ts_env("TS_FILELOG", v, sizeof v) && v[0] == '1') {
        real_CreateFileA = (CreateFileA_t)ts_patch_iat("KERNEL32.dll", "CreateFileA", (void *)hook_CreateFileA);
        real_OpenFile = (OpenFile_t)ts_patch_iat("KERNEL32.dll", "OpenFile", (void *)hook_OpenFile);
        real_CreateFileW = (CreateFileW_t)ts_patch_iat("KERNEL32.dll", "CreateFileW", (void *)hook_CreateFileW);
    }
    if (ts_env("TS_NETLOG", v, sizeof v) && v[0] == '1')
        netlog_install();
    if (background || adapter_mode[0])
        real_DirectDrawCreateEx = (DirectDrawCreateEx_t)ts_patch_iat("DDRAW.dll", "DirectDrawCreateEx",
                                                                     (void *)hook_DirectDrawCreateEx);
    char shots[256];
    shot_t0 = GetTickCount();
    if (ts_env("TS_SHOTDIR", shot_dir, sizeof shot_dir) && ts_env("TS_SHOTS", shots, sizeof shots))
        for (char *t = strtok(shots, ","); t && shot_count < 32; t = strtok(NULL, ","))
            shot_secs[shot_count++] = atoi(t);   /* (ascending, as given) */
    real_MessageBoxA = (MessageBoxA_t)ts_patch_iat("USER32.dll", "MessageBoxA", (void *)hook_MessageBoxA);
    real_MessageBoxW = (MessageBoxW_t)ts_patch_iat("USER32.dll", "MessageBoxW", (void *)hook_MessageBoxW);
    real_DialogBoxParamA = (DialogBoxParamA_t)ts_patch_iat("USER32.dll", "DialogBoxParamA", (void *)hook_DialogBoxParamA);
    real_CreateDialogParamA = (CreateDialogParamA_t)ts_patch_iat("USER32.dll", "CreateDialogParamA",
                                                                 (void *)hook_CreateDialogParamA);
    if (have_pos || background || adapter_mode[0])
        ts_log("test mode: pos=%d,%d (%s) size=%dx%d (%s) background=%d adapter=%s", win_x, win_y, have_pos ? "on" : "off",
               win_w, win_h, have_size ? "on" : "off", background, adapter_mode[0] ? adapter_mode : "game's");
}
