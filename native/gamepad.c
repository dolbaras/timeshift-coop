/*
 * Gamepad (XInput) for the PC build, which reads no pad at all.
 *
 * What the exe has (patch 1.2, see docs/gamepad.md): the raw input object (_inpINPUT, [0xfcabb4]+0x3c)
 * keeps one float per input code at +0x10 + code*4: keys 0x00-0xff, mouse 0x100-0x107, joystick
 * buttons 0x108-0x127, mouse axes 0x12c-0x131, joystick axes 0x132-0x141 (one code per direction:
 * even = positive half, odd = negative half) and POV 0x142-0x151. The engine's command system
 * (contexts GAME, GAME_DEF and UI of the input manager) turns those codes into commands through
 * five binding slots per command (controls.ini "Command"/"DefaultCommand"; slots 4 and 5 are the
 * joystick slots of the key binding screen), and it already treats joystick axes as analog rate
 * input (command flag 0x10: the look code uses the console's gamepad sensitivity spline for them).
 * Only the device is missing: the "open joystick" virtual is a stub (0x7bc9f0: xor eax,eax; ret),
 * the real opener 0x7bca50 and the enumeration callback 0x7bcee0 have no callers, and XInput is
 * loaded for rumble on pad 0 only.
 *
 * What this file does, without patching game code:
 * - wraps the keyboard poll of _inpINPUT (vtable slot 0x60, called every frame right before the
 *   joystick poll) and writes the XInput pad of player 1 into the joystick codes;
 * - adds default pad bindings to the command contexts (GAME and GAME_DEF: the joystick slots 4/5 when
 *   no command has a pad binding yet; UI: menu navigation next to the built-in keys);
 * - replaces the rumble virtual (slot 0x2c), which the engine's own vibration mixer calls every frame
 *   (JoyVibrOff respected there), with XInputSetState on the player's pad;
 * - in the menus (UI context active): the left stick acts as a 4-way digital direction and held
 *   directions repeat (see "menus" below).
 * The pad is read only while the game window is in front (it never takes input meant for another
 * program).
 *
 * Settings (ts_coop.ini next to the exe, section [Gamepad]; all optional):
 *   Enabled=1        0 turns the pad off
 *   Pad=auto         auto: player 1 = the first connected pad; 1-4: that XInput pad
 *   LookSpeed=1.0    right stick look speed (multiplies the game's own mouse sensitivity setting)
 *   InvertY=0        1: right stick up looks down
 *   Vibration=1      0: no rumble
 * Tests: TS_PAD=1-4 picks that pad and reads it while the window is not in front (test windows never
 * are; use the virtual pad's number); TS_PADLOG=1 logs pad changes, command changes and the camera.
 */
#include <windows.h>
#include <xinput.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "ts.h"

#define THISCALL __attribute__((thiscall))

#define IM_PTR      0x00fcabb4   /* the game's input manager; +0x3c raw input, +0x40 active context, +0x44 contexts[8] */
#define CAM_PTR     0x00fcabc4   /* the current camera: +0x20 forward, +0x30 position */
#define RAW_VTBL    0x00db7034   /* _inpINPUT */
#define SLOT_RUMBLE (0x2c / 4)   /* 0x7bc6d0 (this, float low, float high, float force), ret 0xc */
#define SLOT_KBPOLL (0x60 / 4)   /* 0x7bc140 (this): keyboard poll; the joystick poll (0x80) follows it */
#define SLOT_NAME   (0x54 / 4)   /* 0x786700 (this, code, wchar_t *out), ret 8: key name for the controls screen */
#define VT_GAME     0x00d8bd28
#define VT_GAME_DEF 0x00d8bd78
#define VT_UI       0x00d8bdc8

/* Joystick input codes, laid out as DirectInput reports an Xbox pad (buttons 0-9, then the triggers
 * as buttons 10/11; X/Y/Z/Rx/Ry axes; POV 0), so the key names of the binding screen fit if a
 * DirectInput device is ever opened next to this. */
enum {
    J_A = 0x108, J_B, J_X, J_Y, J_LB, J_RB, J_BACK, J_START, J_LS, J_RS, J_LT, J_RT,
    J_LX_R = 0x132, J_LX_L, J_LY_D, J_LY_U, J_LT_AXIS, J_RT_AXIS, J_RX_R, J_RX_L, J_RY_D, J_RY_U,
    J_DUP = 0x142, J_DDOWN, J_DRIGHT, J_DLEFT,
    J_FIRST = 0x108, J_END = 0x154   /* codes the pad may own */
};

/* ---- the engine's command tables ---- */
struct cmd_entry {             /* 0x68 bytes */
    float value;
    struct { int n; int code[3]; } slot[5];   /* a slot holds a key combination of n codes */
    char unknown[0x10];
    unsigned flags;            /* bit0 held, bit2 pressed, 0x10 joystick axis, 0x20 mouse axis, 0x40 digital */
};
struct cmd_ctx {
    void **vtbl;               /* +0x10 name(), +0x14 id_name(id, char *out) */
    struct cmd_entry *e;
    int first, end;
};
typedef void (THISCALL *ctx_id_name_t)(struct cmd_ctx *, int, char *);

struct pad_bind { const char *cmd; int code, code2; };

/* In-game defaults (GAME, GAME_DEF): code -> joystick slot 4, code2 -> slot 5. */
static const struct pad_bind game_binds[] = {
    {"MOVE_FORWARD", J_LY_U, 0},       {"MOVE_BACKWARD", J_LY_D, 0},
    {"MOVE_LEFT", J_LX_L, 0},          {"MOVE_RIGHT", J_LX_R, 0},
    {"ROTATE_LEFT", J_RX_L, 0},        {"ROTATE_RIGHT", J_RX_R, 0},
    {"LOOK_UP", J_RY_U, 0},            {"LOOK_DOWN", J_RY_D, 0},
    {"ATTACK_1", J_RT, 0},             {"ATTACK_2", J_LT, 0},
    {"ATTACK_3", J_RS, 0},             {"MINIZOOM", J_LS, 0},
    {"JUMP", J_A, 0},                  {"CROUCH", J_B, 0},
    {"USE", J_X, 0},                   {"FORCE_RECHARGE", J_X, 0},
    {"WPN_NEXT", J_Y, 0},
    {"ACTIVATE_GREN_AUTO", J_LB, 0},   {"ACTIVATE_TIME_AUTO", J_RB, 0},
    {"ACTIVATE_TIME_FREEZE", J_DUP, 0},       {"ACTIVATE_SLOW_MOTION", J_DLEFT, 0},
    {"ACTIVATE_TIME_REVERSAL", J_DRIGHT, 0},  {"NEXT_GRENADE", J_DDOWN, 0},
    {"UI", J_START, 0},                {"STOP_CINEMATIC", J_START, 0},
    {"SHOWFRAGS", J_BACK, 0},
    {"BIKE_BOOST", J_A, 0},            {"BIKE_FORW", J_LY_U, 0},   {"BIKE_BACK", J_LY_D, 0},
};
/* Menus (UI context, built-in table at 0xec1b80): added to the first free slots. */
static const struct pad_bind ui_binds[] = {
    {"UP", J_DUP, J_LY_U},       {"DOWN", J_DDOWN, J_LY_D},
    {"LEFT", J_DLEFT, J_LX_L},   {"RIGHT", J_DRIGHT, J_LX_R},
    {"ENTER", J_A, 0},           {"ESC", J_B, J_START},
    {"PG_UP", J_LB, 0},          {"PG_DOWN", J_RB, 0},
};

/* ---- XInput ---- */
typedef DWORD (WINAPI *XInputGetState_t)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *XInputSetState_t)(DWORD, XINPUT_VIBRATION *);
static XInputGetState_t x_get;
static XInputSetState_t x_set;

static int enabled = 1, fixed_pad = -1, test_pad, invert_y, vibration = 1, padlog;
static float look_speed = 1.0f;
static int connected[XUSER_MAX_COUNT];
static DWORD last_scan;
static int player_slot[2] = {-1, -1};

typedef void (THISCALL *poll_t)(void *);
typedef void (THISCALL *rumble_t)(void *, float, float, float);
typedef int (THISCALL *key_name_t)(void *, unsigned, wchar_t *);
static poll_t real_kb_poll;
static key_name_t real_key_name;

static int game_in_front(void)
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

/* Which XInput pad each local player uses: a fixed one (Pad=, TS_PAD=) or, in auto mode, the first
 * and second connected pads. Empty slots are slow to query, so they are scanned once a second. */
static void scan_pads(void)
{
    DWORD now = GetTickCount();
    if (last_scan && now - last_scan < 1000)
        return;
    last_scan = now;
    int n = 0, slots[XUSER_MAX_COUNT];
    for (int i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE st;
        int c = x_get(i, &st) == ERROR_SUCCESS;
        if (c != connected[i])
            ts_log("gamepad: XInput pad %d %s", i + 1, c ? "connected" : "disconnected");
        connected[i] = c;
        if (c)
            slots[n++] = i;
    }
    int p0 = fixed_pad >= 0 ? fixed_pad : n > 0 ? slots[0] : -1;
    int p1 = -1;
    for (int i = 0; i < n && p1 < 0; i++)
        if (slots[i] != p0)
            p1 = slots[i];
    if (p0 != player_slot[0] || p1 != player_slot[1])
        ts_log("gamepad: player 1 -> pad %d, player 2 -> pad %d (0 = none)", p0 + 1, p1 + 1);
    player_slot[0] = p0;
    player_slot[1] = p1;
}

int gamepad_slot(int player)
{
    return player >= 0 && player < 2 ? player_slot[player] : -1;
}

/* Radial dead zone, the rest rescaled to 0..1 so a small push past the dead zone is a slow move. */
static void stick(SHORT sx, SHORT sy, int dead, float *ox, float *oy)
{
    float x = sx / 32767.0f, y = sy / 32767.0f, d = dead / 32767.0f;
    float m = sqrtf(x * x + y * y);
    if (m <= d) {
        *ox = *oy = 0;
        return;
    }
    float s = ((m > 1 ? 1 : m) - d) / (1 - d);
    *ox = x / m * s;
    *oy = y / m * s;
}

static float pos(float v) { return v > 0 ? v : 0; }

/* ---- menus ----
 * The menus get their navigation from the UI command context like the keyboard does: each frame the engine
 * passes the commands whose "pressed" (flag 4) or "released" edge is set to the element key handlers
 * (vtable +0xa8: OnKey(id, down, ...), called from 0x700776), so the pad's UI bindings are all they need.
 * Two things differ from keys: the engine counts any value above 0.0001 as pressed, so a stick would
 * press a direction at the edge of its dead zone and two directions when held at an angle; and nothing
 * repeats a held key. While the UI context is active the left stick is therefore a 4-way digital
 * direction (dominant axis, past half way), and held directions repeat. */
#define MENU_STICK   0.5f
#define REPEAT_DELAY 400    /* ms until a held direction repeats */
#define REPEAT_RATE  110    /* ms between repeats */

static int menu_active(BYTE *im)
{
    struct cmd_ctx *c = *(struct cmd_ctx **)(im + 0x40);
    return c && (DWORD)c->vtbl == VT_UI;
}

static void menu_stick(float *x, float *y)
{
    float ax = fabsf(*x), ay = fabsf(*y);
    if (ax < MENU_STICK && ay < MENU_STICK) {
        *x = *y = 0;
        return;
    }
    if (ax > ay) {
        *x = *x > 0 ? 1.0f : -1.0f;
        *y = 0;
    } else {
        *y = *y > 0 ? 1.0f : -1.0f;
        *x = 0;
    }
}

/* A held direction gets a new press edge: its code reads 0 for one poll every REPEAT_RATE ms after
 * REPEAT_DELAY (the engine compares each code with its value of the previous frame). */
static void menu_repeat(float *v, int menu)
{
    static const int codes[] = {J_DUP, J_DDOWN, J_DLEFT, J_DRIGHT, J_LY_U, J_LY_D, J_LX_L, J_LX_R, J_LB, J_RB};
    static DWORD next[sizeof codes / sizeof *codes];
    DWORD now = GetTickCount();
    for (unsigned i = 0; i < sizeof codes / sizeof *codes; i++) {
        if (!menu || v[codes[i]] <= 0) {
            next[i] = 0;
            continue;
        }
        if (!next[i])
            next[i] = now + REPEAT_DELAY;
        else if ((int)(now - next[i]) >= 0) {
            v[codes[i]] = 0;
            next[i] = now + REPEAT_RATE;
        }
    }
}

/* ---- bindings ---- */
static int ctx_id(struct cmd_ctx *c, const char *name)
{
    ctx_id_name_t id_name = (ctx_id_name_t)c->vtbl[0x14 / 4];
    char buf[256];
    for (int id = c->first; id < c->end; id++) {
        buf[0] = 0;
        id_name(c, id, buf);
        if (strcmp(buf, name) == 0)
            return id;
    }
    return -1;
}

static int has_code(const struct cmd_entry *e, int code)
{
    for (int s = 0; s < 5; s++)
        for (int k = 0; k < e->slot[s].n && k < 3; k++)
            if (e->slot[s].code[k] == code)
                return 1;
    return 0;
}

static int put(struct cmd_entry *e, int slot, int code)
{
    if (!code || e->slot[slot].n != 0)
        return 0;
    e->slot[slot].code[0] = code;
    e->slot[slot].n = 1;
    return 1;
}

/* GAME / GAME_DEF: the defaults go in only while no command has a pad code in the joystick slots, so
 * a player's own pad bindings (saved by the game into controls.ini) are never overwritten, and a
 * "restore defaults" that reloads the keyboard defaults gets the pad defaults back. */
static void bind_game(struct cmd_ctx *c, const char *what)
{
    int n = c->end - c->first;
    if (!c->e || n <= 0 || n > 0x400)
        return;
    for (int i = 0; i < n; i++)
        for (int s = 3; s < 5; s++)
            for (int k = 0; k < c->e[i].slot[s].n && k < 3; k++)
                if (c->e[i].slot[s].code[k] >= J_FIRST && c->e[i].slot[s].code[k] < J_END)
                    return;
    int added = 0, missing = 0;
    for (unsigned b = 0; b < sizeof game_binds / sizeof *game_binds; b++) {
        int id = ctx_id(c, game_binds[b].cmd);
        if (id < 0) {
            missing++;
            continue;
        }
        struct cmd_entry *e = &c->e[id - c->first];
        added += put(e, 3, game_binds[b].code);
        added += put(e, 4, game_binds[b].code2);
    }
    ts_log("gamepad: %d default pad bindings added to %s (%d commands not found)", added, what, missing);
}

/* UI: added next to the built-in keys wherever a pad code is missing (the built-in table is reloaded
 * with the context; nothing of the player's is stored there). */
static void bind_ui(struct cmd_ctx *c)
{
    int added = 0;
    for (unsigned b = 0; b < sizeof ui_binds / sizeof *ui_binds; b++) {
        int id = ctx_id(c, ui_binds[b].cmd);
        if (id < 0)
            continue;
        struct cmd_entry *e = &c->e[id - c->first];
        int codes[2] = {ui_binds[b].code, ui_binds[b].code2};
        for (int k = 0; k < 2; k++) {
            if (!codes[k] || has_code(e, codes[k]))
                continue;
            for (int s = 0; s < 5; s++)
                if (put(e, s, codes[k])) {
                    added++;
                    break;
                }
        }
    }
    if (added)
        ts_log("gamepad: %d menu pad bindings added", added);
}

static void bind_all(BYTE *im)
{
    struct cmd_ctx **ctx = (struct cmd_ctx **)(im + 0x44);
    for (int i = 0; i < 8; i++) {
        struct cmd_ctx *c = ctx[i];
        if (!c || !c->e)
            continue;
        if ((DWORD)c->vtbl == VT_GAME)
            bind_game(c, "GAME");
        else if ((DWORD)c->vtbl == VT_GAME_DEF)
            bind_game(c, "GAME_DEF");
        else if ((DWORD)c->vtbl == VT_UI)
            bind_ui(c);
    }
}

/* ---- test log: command changes of the active context, the camera ---- */
static void log_commands(BYTE *im)
{
    static struct cmd_ctx *last_ctx;
    static unsigned char held[0x400];
    struct cmd_ctx *c = *(struct cmd_ctx **)(im + 0x40);
    if (!c || !c->e || c->end - c->first <= 0 || c->end - c->first > 0x400)
        return;
    int n = c->end - c->first;
    if (c != last_ctx) {
        char name[64] = "";
        const char *(THISCALL *ctx_name)(struct cmd_ctx *) = (const char *(THISCALL *)(struct cmd_ctx *))c->vtbl[0x10 / 4];
        snprintf(name, sizeof name, "%s", ctx_name(c));
        ts_log("gamepad: active command context %s (ids %d-%d)", name, c->first, c->end - 1);
        memset(held, 0, sizeof held);
        last_ctx = c;
    }
    ctx_id_name_t id_name = (ctx_id_name_t)c->vtbl[0x14 / 4];
    for (int i = 0; i < n; i++) {
        unsigned char h = c->e[i].flags & 1;
        if (h != held[i]) {
            char buf[256] = "";
            id_name(c, c->first + i, buf);
            ts_log("gamepad: cmd %s %s value=%.3f flags=0x%x", buf, h ? "on" : "off", c->e[i].value, c->e[i].flags);
            held[i] = h;
        }
    }
}

static void log_camera(void)
{
    static DWORD last;
    static float lp[6];
    DWORD now = GetTickCount();
    BYTE *cam = *(BYTE **)CAM_PTR;
    if (!cam || now - last < 1000)
        return;
    last = now;
    float *f = (float *)(cam + 0x20), *p = (float *)(cam + 0x30);
    float cur[6] = {p[0], p[1], p[2], f[0], f[1], f[2]};
    if (memcmp(cur, lp, sizeof cur) != 0)
        ts_log("gamepad: camera pos %.2f %.2f %.2f forward %.3f %.3f %.3f", p[0], p[1], p[2], f[0], f[1], f[2]);
    memcpy(lp, cur, sizeof lp);
}

/* ---- per frame ---- */
static void pad_poll(BYTE *raw)
{
    static int frame;
    static WORD last_buttons;
    static int last_sticks;
    float *v = (float *)(raw + 0x10);
    BYTE *im = *(BYTE **)IM_PTR;
    if (!im || *(BYTE **)(im + 0x3c) != raw)
        return;
    if (frame++ % 30 == 0)
        bind_all(im);
    scan_pads();

    XINPUT_STATE st;
    memset(&st, 0, sizeof st);
    int slot = player_slot[0];
    int ok = slot >= 0 && (test_pad || game_in_front()) && x_get(slot, &st) == ERROR_SUCCESS;
    XINPUT_GAMEPAD *g = &st.Gamepad;
    if (!ok)
        memset(g, 0, sizeof *g);

    static const WORD xb[10] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_BACK,
                                XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB};
    for (int i = 0; i < 10; i++)
        v[J_A + i] = (g->wButtons & xb[i]) ? 1.0f : 0.0f;
    v[J_LT] = g->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1.0f : 0.0f;
    v[J_RT] = g->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? 1.0f : 0.0f;
    v[J_LT_AXIS] = g->bLeftTrigger / 255.0f;
    v[J_RT_AXIS] = g->bRightTrigger / 255.0f;

    float lx, ly, rx, ry;
    stick(g->sThumbLX, g->sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, &lx, &ly);
    stick(g->sThumbRX, g->sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, &rx, &ry);
    int menu = menu_active(im);
    if (menu)
        menu_stick(&lx, &ly);
    rx *= look_speed;
    ry *= invert_y ? -look_speed : look_speed;
    v[J_LX_R] = pos(lx);
    v[J_LX_L] = pos(-lx);
    v[J_LY_U] = pos(ly);
    v[J_LY_D] = pos(-ly);
    v[J_RX_R] = pos(rx);
    v[J_RX_L] = pos(-rx);
    v[J_RY_U] = pos(ry);
    v[J_RY_D] = pos(-ry);
    v[J_DUP] = (g->wButtons & XINPUT_GAMEPAD_DPAD_UP) ? 1.0f : 0.0f;
    v[J_DDOWN] = (g->wButtons & XINPUT_GAMEPAD_DPAD_DOWN) ? 1.0f : 0.0f;
    v[J_DRIGHT] = (g->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) ? 1.0f : 0.0f;
    v[J_DLEFT] = (g->wButtons & XINPUT_GAMEPAD_DPAD_LEFT) ? 1.0f : 0.0f;
    menu_repeat(v, menu);

    if (padlog) {
        int sticks = (lx > 0.5f) | (lx < -0.5f) << 1 | (ly > 0.5f) << 2 | (ly < -0.5f) << 3 | (rx > 0.5f) << 4
                     | (rx < -0.5f) << 5 | (ry > 0.5f) << 6 | (ry < -0.5f) << 7 | (g->bLeftTrigger > 30) << 8
                     | (g->bRightTrigger > 30) << 9;
        if (g->wButtons != last_buttons || sticks != last_sticks)
            ts_log("gamepad: pad %d buttons=0x%04x LT=%d RT=%d L=%.2f,%.2f R=%.2f,%.2f", slot + 1, g->wButtons,
                   g->bLeftTrigger, g->bRightTrigger, lx, ly, rx, ry);
        last_buttons = g->wButtons;
        last_sticks = sticks;
        log_camera();
    }
}

static void THISCALL hook_kb_poll(void *raw)
{
    real_kb_poll(raw);
    pad_poll((BYTE *)raw);
}

/* The engine's vibration mixer (0x657930) sends the motor levels (0..1) every frame. */
static void THISCALL hook_rumble(void *raw, float low, float high, float force)
{
    static WORD last_l, last_r;
    static int last_slot = -1;
    (void)raw;
    if (padlog) {   /* (here the held flags of the last frame are still set; they are cleared before the poll. The
                     * edge flags 2/4 are often cleared again after the evaluation (seen in the menus), so "flags=0x11" is normal.) */
        BYTE *im = *(BYTE **)IM_PTR;
        if (im)
            log_commands(im);
    }
    int slot = player_slot[0];
    int on = vibration && slot >= 0 && (test_pad || game_in_front());
    float l = on ? low : 0, r = on ? high : 0;
    WORD wl = (WORD)((l < 0 ? 0 : l > 1 ? 1 : l) * 65535), wr = (WORD)((r < 0 ? 0 : r > 1 ? 1 : r) * 65535);
    if (slot < 0 || (wl == last_l && wr == last_r && slot == last_slot && force == 0))
        return;
    XINPUT_VIBRATION vib = {wl, wr};
    x_set(slot, &vib);
    if (padlog && (wl != last_l || wr != last_r))
        ts_log("gamepad: rumble pad %d %u %u", slot + 1, wl, wr);
    last_l = wl;
    last_r = wr;
    last_slot = slot;
}

/* Names of the pad codes on the controls screen (it shows the first two bound slots of a command, so a
 * command with one key also shows its pad binding). The engine asks the DirectInput device for joystick
 * names, and there is none. */
static int THISCALL hook_key_name(void *raw, unsigned code, wchar_t *out)
{
    static const wchar_t *btn[] = {L"PAD_A", L"PAD_B", L"PAD_X", L"PAD_Y", L"PAD_LB", L"PAD_RB", L"PAD_BACK",
                                   L"PAD_START", L"PAD_LS", L"PAD_RS", L"PAD_LT", L"PAD_RT"};
    static const wchar_t *axis[] = {L"PAD_LSTICK (Right)", L"PAD_LSTICK (Left)", L"PAD_LSTICK (Back)",
                                    L"PAD_LSTICK (Forw)", L"PAD_LT", L"PAD_RT", L"PAD_RSTICK (Right)",
                                    L"PAD_RSTICK (Left)", L"PAD_RSTICK (Down)", L"PAD_RSTICK (Up)"};
    static const wchar_t *pov[] = {L"PAD_DPAD_UP", L"PAD_DPAD_DOWN", L"PAD_DPAD_RIGHT", L"PAD_DPAD_LEFT"};
    const wchar_t *n = NULL;
    if (code >= J_A && code <= J_RT)
        n = btn[code - J_A];
    else if (code >= J_LX_R && code <= J_RY_U)
        n = axis[code - J_LX_R];
    else if (code >= J_DUP && code <= J_DLEFT)
        n = pov[code - J_DUP];
    if (!n || *(void **)((BYTE *)raw + 0x1bbc) || !out)   /* (a real DirectInput joystick names its own) */
        return real_key_name(raw, code, out);
    wcscpy(out, n);
    return 1;
}

static void load_settings(void)
{
    char ini[MAX_PATH], v[32];
    DWORD n = GetModuleFileNameA(NULL, ini, MAX_PATH);
    char *slash = strrchr(ini, '\\');
    if (!n || !slash || slash - ini + 13 >= MAX_PATH)
        return;
    strcpy(slash + 1, "ts_coop.ini");
    enabled = GetPrivateProfileIntA("Gamepad", "Enabled", 1, ini) != 0;
    invert_y = GetPrivateProfileIntA("Gamepad", "InvertY", 0, ini) != 0;
    vibration = GetPrivateProfileIntA("Gamepad", "Vibration", 1, ini) != 0;
    GetPrivateProfileStringA("Gamepad", "Pad", "auto", v, sizeof v, ini);
    if (v[0] >= '1' && v[0] <= '4' && !v[1])
        fixed_pad = v[0] - '1';
    GetPrivateProfileStringA("Gamepad", "LookSpeed", "1.0", v, sizeof v, ini);
    float s = (float)atof(v);
    if (s > 0.05f && s < 20.0f)
        look_speed = s;
    if (ts_env("TS_PAD", v, sizeof v) && v[0] >= '1' && v[0] <= '4') {
        fixed_pad = v[0] - '1';
        test_pad = 1;
    }
    padlog = ts_env("TS_PADLOG", v, sizeof v) && v[0] == '1';
}

void gamepad_install(void)
{
    load_settings();
    if (!enabled) {
        ts_log("gamepad: off (ts_coop.ini)");
        return;
    }
    void **vt = (void **)RAW_VTBL;
    if ((DWORD)vt[SLOT_KBPOLL] != 0x007bc140 || (DWORD)vt[SLOT_RUMBLE] != 0x007bc6d0 || (DWORD)vt[SLOT_NAME] != 0x00786700) {
        ts_log("gamepad: input class not found (other exe), no pad");
        return;
    }
    static const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    HMODULE x = NULL;
    for (int i = 0; i < 3 && !x; i++)
        x = LoadLibraryA(dlls[i]);
    if (x) {
        x_get = (XInputGetState_t)GetProcAddress(x, "XInputGetState");
        x_set = (XInputSetState_t)GetProcAddress(x, "XInputSetState");
    }
    if (!x_get || !x_set) {
        ts_log("gamepad: no XInput DLL, no pad");
        return;
    }
    /* (the slots were checked above: the originals are known before they are replaced) */
    real_kb_poll = (poll_t)vt[SLOT_KBPOLL];
    real_key_name = (key_name_t)vt[SLOT_NAME];
    void *vtp = vt;   /* (ts_patch_vtable takes an object: a pointer to the vtable pointer) */
    int n = !!ts_patch_vtable(&vtp, SLOT_KBPOLL, (void *)hook_kb_poll) + !!ts_patch_vtable(&vtp, SLOT_RUMBLE, (void *)hook_rumble)
            + !!ts_patch_vtable(&vtp, SLOT_NAME, (void *)hook_key_name);
    if (n != 3)
        ts_log("gamepad: only %d of 3 input hooks set", n);
    ts_log("gamepad: on (pad %s, look speed %.2f, invert %d, vibration %d%s)", fixed_pad >= 0 ? "fixed" : "auto",
           look_speed, invert_y, vibration, test_pad ? ", test: read in background" : "");
    if (fixed_pad >= 0)
        ts_log("gamepad: player 1 uses XInput pad %d", fixed_pad + 1);
}
