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
 *
 * Hooks go through the exe's import table and COM vtables; the game code is not patched.
 */
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
            if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, i, 0, &id)) && id.VendorId == 0x8086)
                return i;
        }
    }
    return asked;
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
    return hr;
}

static IDirect3D9 *WINAPI hook_Direct3DCreate9(UINT sdk)
{
    IDirect3D9 *d3d = real_Direct3DCreate9(sdk);
    if (d3d && (adapter_mode[0] || background)) {
        void *p = ts_patch_vtable(d3d, 16, (void *)hook_D3DCreateDevice);
        if (p)
            real_D3DCreateDevice = (D3DCreateDevice_t)p;
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
    if (have_pos || background || adapter_mode[0])
        ts_log("test mode: pos=%d,%d (%s) size=%dx%d (%s) background=%d adapter=%s", win_x, win_y, have_pos ? "on" : "off",
               win_w, win_h, have_size ? "on" : "off", background, adapter_mode[0] ? adapter_mode : "game's");
}
