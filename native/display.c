/*
 * Screen mode from the Video options (profile key COOP_DisplayMode, data/coop_display.py), read at the start:
 *   0 or no key  the game's own setting (VIDEO_FullScr: exclusive full screen or its window)
 *   1            borderless: the game is told it runs full screen, but its Direct3D device is a windowed one
 *                and its window covers the whole monitor without a frame; the picture is the profile's
 *                resolution scaled to the monitor. Alt-tab works like on the desktop, the display mode never
 *                changes.
 *   2            window: a framed window whose inside is the profile's resolution, centred on the monitor
 *                (also when the profile says full screen).
 *
 * Test instances (TS_WINPOS / TS_BACKGROUND) keep their own window handling (testmode.c) and ignore the
 * profile key; TS_DISPLAYMODE=<1|2> tests a mode there.
 */
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

#define SLOT(vtbl, m) ((int)(offsetof(vtbl, m) / sizeof(void *)))

static int mode;   /* 1 borderless, 2 window */
static HWND window;
static WNDPROC game_proc;
static RECT monitor;

/* The profile file the game reads: bin\steamwars.ini [Profile] Folder + Name, then user.ini. */
static int profile_path(char *out, int size)
{
    char exe[MAX_PATH], bin[MAX_PATH], ini[MAX_PATH], folder[MAX_PATH], name[128];
    if (!GetModuleFileNameA(NULL, exe, sizeof exe))
        return 0;
    strcpy(bin, exe);
    char *s = strrchr(bin, '\\');
    if (!s)
        return 0;
    *s = 0;
    snprintf(ini, sizeof ini, "%s\\steamwars.ini", bin);
    GetPrivateProfileStringA("Profile", "Folder", "..\\UserConfigs\\", folder, sizeof folder, ini);
    GetPrivateProfileStringA("Profile", "Name", "Default", name, sizeof name, ini);
    size_t n = strlen(folder);
    snprintf(out, size, "%s\\%s%s%s\\user.ini", bin, folder, n && folder[n - 1] != '\\' ? "\\" : "", name);
    return 1;
}

/* "KEY = value" anywhere in the profile (the game writes no sections for these keys). */
static int profile_value(const char *path, const char *key, char *out, int size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char line[512];
    size_t kl = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (_strnicmp(p, key, kl) != 0 || (p[kl] != ' ' && p[kl] != '\t' && p[kl] != '='))
            continue;
        p = strchr(p + kl, '=');
        if (!p)
            continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '"')
            p++;
        int i = 0;
        while (*p && *p != '\r' && *p != '\n' && *p != '"' && i < size - 1)
            out[i++] = *p++;
        while (i && (out[i - 1] == ' ' || out[i - 1] == '\t'))
            i--;
        out[i] = 0;
        found = 1;
    }
    fclose(f);
    return found;
}

static LRESULT CALLBACK proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_WINDOWPOSCHANGING) {   /* the window keeps covering the monitor */
        WINDOWPOS *p = (WINDOWPOS *)lp;
        p->x = monitor.left, p->y = monitor.top;
        p->cx = monitor.right - monitor.left, p->cy = monitor.bottom - monitor.top;
        p->flags &= ~(UINT)(SWP_NOMOVE | SWP_NOSIZE);
    }
    return CallWindowProcA(game_proc, w, msg, wp, lp);
}

static void framed_window(HWND w, HMONITOR mon, UINT cw, UINT ch)
{
    MONITORINFO mi = {sizeof mi};
    if (!w || !GetMonitorInfoA(mon, &mi))
        return;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    SetWindowLongA(w, GWL_STYLE, (LONG)style);
    RECT r = {0, 0, (LONG)cw, (LONG)ch};
    AdjustWindowRectEx(&r, style, FALSE, (DWORD)GetWindowLongA(w, GWL_EXSTYLE));
    int ow = r.right - r.left, oh = r.bottom - r.top;
    RECT wa = mi.rcWork;
    SetWindowPos(w, HWND_NOTOPMOST, wa.left + ((wa.right - wa.left) - ow) / 2, wa.top + ((wa.bottom - wa.top) - oh) / 2,
                 ow, oh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void cover_monitor(HWND w, HMONITOR mon)
{
    MONITORINFO mi = {sizeof mi};
    if (!w || !GetMonitorInfoA(mon, &mi))
        return;
    monitor = mi.rcMonitor;
    SetWindowLongA(w, GWL_STYLE, (LONG)(WS_POPUP | WS_VISIBLE));
    SetWindowLongA(w, GWL_EXSTYLE, GetWindowLongA(w, GWL_EXSTYLE) & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE));
    if (w != window) {
        window = w;
        game_proc = (WNDPROC)SetWindowLongA(w, GWL_WNDPROC, (LONG)proc);
    }
    SetWindowPos(w, HWND_TOP, monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void make_windowed(D3DPRESENT_PARAMETERS *pp)
{
    if (pp && !pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
}

typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Reset_t real_Reset;

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    make_windowed(pp);   /* (a resolution change in the Video options) */
    HRESULT hr = real_Reset(dev, pp);
    if (SUCCEEDED(hr) && mode == 2 && window && pp)
        framed_window(window, MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), pp->BackBufferWidth,
                      pp->BackBufferHeight);
    ts_log("display: windowed reset %ux%u -> hr=0x%08lx", pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0,
           (unsigned long)hr);
    return hr;
}

typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                         IDirect3DDevice9 **);
static CreateDevice_t real_CreateDevice;

static HRESULT WINAPI hook_CreateDevice(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, HWND focus, DWORD fl,
                                        D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    int was_full = pp && !pp->Windowed;
    make_windowed(pp);
    HWND w = pp && pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    HMONITOR mon = IDirect3D9_GetAdapterMonitor(d3d, a);
    if (was_full && mode == 1)
        cover_monitor(w, mon);
    else if (was_full && mode == 2) {
        window = w;
        framed_window(w, mon, pp->BackBufferWidth, pp->BackBufferHeight);
    }
    HRESULT hr = real_CreateDevice(d3d, a, t, focus, fl, pp, out);
    ts_log("display: %s, picture %ux%u -> hr=0x%08lx",
           !was_full ? "the game's own window (VIDEO_FullScr = no)" : mode == 1 ? "borderless" : "window",
           pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out && was_full) {
        void *p = ts_patch_vtable(*out, SLOT(IDirect3DDevice9Vtbl, Reset), (void *)hook_Reset);
        if (p)
            real_Reset = (Reset_t)p;
    }
    return hr;
}

void display_on_d3d(void *d3dv)
{
    if (!mode)
        return;
    void *p = ts_patch_vtable(d3dv, SLOT(IDirect3D9Vtbl, CreateDevice), (void *)hook_CreateDevice);
    if (p)
        real_CreateDevice = (CreateDevice_t)p;
}

void display_install(void)
{
    char path[MAX_PATH], v[32];
    if (ts_env("TS_WINPOS", v, sizeof v) || ts_env("TS_BACKGROUND", v, sizeof v)) {
        if (ts_env("TS_DISPLAYMODE", v, sizeof v))   /* (a test of these modes on a test PC) */
            mode = atoi(v);
        else
            return;
    } else if (profile_path(path, sizeof path) && profile_value(path, "COOP_DisplayMode", v, sizeof v)) {
        mode = atoi(v);
    }
    if (mode != 1 && mode != 2)
        mode = 0;
    if (mode)
        ts_log("display: screen mode %d (%s, COOP_DisplayMode)", mode, mode == 1 ? "borderless" : "window");
}
