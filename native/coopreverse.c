/*
 * Time reversal in co-op, the quick variant (patch 1.2 exe only). NOT YET RUN IN A GAME. Off unless TS_COOP_REVERSE=1;
 * acts only where timesync.c runs the campaign time powers (a co-op session on a campaign level).
 *
 * Why: in a network game the engine never starts its time recorder (0x5846b0 returns unless the game mode is 1), so
 * the reverse key fails (0x5847c0: "no recorded frames") and timesync.c refuses reverse anyway. Several campaign
 * puzzles only need the level scripts' reverse events, not a rewound world (mod/docs/campaign-audit.md, G1):
 * force fields and wind that flip while time runs backwards (SetSpeed in OnStartTReverse / OnStopTReverse), a
 * collision that hides during a reverse, kill zones locked during a reverse.
 *
 * What it does: a player presses the reverse key (the engine's own key path; enough energy as for any power) ->
 * the host decides (one world power at a time, with timesync.c's slow/stop) -> every machine fires
 * OnStartTReverse on the level's reverse-puzzle objects (table below) and shows the reverse screen effect; after
 * the reverse time (TS_COOP_REVERSE_SECONDS, default 5, less when the owner's energy is low) every machine fires
 * OnStopTReverse. Nothing is rewound. Only the owner spends energy (TS_COOP_REVERSE_COST per second, default 0.12
 * of a full bar). Objects that need their past state back (cranes, a lift, a bridge, a falling cage) are NOT covered:
 * see the audit (per-object rewind design).
 *
 * Engine facts (static analysis):
 * - reverse key: player +0x39c bit 0x1000000; the key path in 0x40d2c0 checks the energy (+0x2ac) and then asks the
 *   recorder (call 0x5847c0 at 0x40d491, eax = [0xfc8400]); 0 -> the "power refused" path 0x40d449, which ends at
 *   0x40d469 (key flag cleared, ret 4). The mod's thunk replaces that call: a co-op reverse request returns
 *   straight to 0x40d469 (key consumed, no refusal sound), anything else calls 0x5847c0 as before.
 * - world reverse messages: 0x3ff start / 0x400 end (recorder playback 0x584720 / 0x584870), entity handler 0x643fe0
 *   -> vtable +0xf0 / +0xf4. The script events OnStartTReverse / OnStopTReverse are fired here by name, as
 *   storysync.c fires events (0x8f9680); storysync treats them as local events (each machine runs its own).
 * - screen effect slot mode+3 (reverse = 5) of the effect manager 0x1096f18, as timesync.c.
 *
 * Messages (coopnet.c, reliable, subtypes 80-89):
 *   80 client -> server  {u8 1, u8 what (1 = start, 0 = stop), u8 tenths of a second}
 *   81 server -> client  {u8 1, u8 on, u8 yours, u8 tenths left, i32 owner}   (repeated every second while on)
 *
 * Switches: TS_COOP_REVERSE=1, TS_COOP_REVERSE_SECONDS=<s>, TS_COOP_REVERSE_COST=<energy per s>, TS_COOP_REVERSE_LOG=1.
 * Test console: "!rv state", "!rv start [s]" (as if this machine's player pressed reverse), "!rv stop".
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

#define GAME_MODE_OBJ (*(char **)0xfcab34)
#define MODE_OFF 0x3c
#define SLOTS_OFF 0x28
#define NSLOTS_OFF 0x2c
#define SLOT_SIZE 0xa4
#define SLOT_READY 0x80
#define SLOT_JOINED 0x100
#define MODE_SERVER 2
#define MODE_CLIENT 3
#define PLAYER (*(char **)0x1095b30)
#define LEVEL_NAME ((const char *)0xfcabc8)
#define MGR (*(void **)0xebedec)
#define ENERGY 0x2ac                         /* player: time-power energy 0..1 (controller +0x280, +0x2c) */
#define FX_MGR 0x1096f18
#define FX_SLOT(mode) (*(char **)(FX_MGR + 0x14f4 + ((mode) + 3) * 4))
#define MODE_REVERSE 2

typedef struct { int refs, len, cap; char s[1]; } StrHdr;
typedef StrHdr *String;
typedef struct { int code; String msg; } Status;
typedef struct { void **type; unsigned data; } Variant;
typedef void *ObjPtr;

static void (__cdecl *eng_free)(void *) = (void *)0x7e4700;
static String *(TSCALL *str_ctor)(String *, const char *, int, int) = (void *)0x402490;
static ObjPtr *(TSCALL *mgr_find_obj)(void *, ObjPtr *, const String *) = (void *)0x8fc720;
static void (TSCALL *inst_release)(ObjPtr *) = (void *)0x8f8050;
static int *(TSCALL *sym_ctor)(int *, const char *, char) = (void *)0x9c7530;
static Status *(TSCALL *fire_by_name)(ObjPtr *, Status *, int, int, Variant **) = (void *)0x8f9680;
static const DWORD fn_fx_start = 0x665f70;   /* esi = FX_MGR, edi = slot, [esp+4] float; ret 4 */

#define SUB_REQ 80
#define SUB_STATE 81
#define SUB_LAST 89
#define PROTO 1
#define NONE (-1)

extern int tsync_coop_now(void) __asm__("tsync_coop_now");   /* timesync.c: co-op time powers apply now */

/* ---- the reverse-puzzle objects (level scripts with OnStartTReverse / OnStopTReverse; mod/docs/campaign-audit.md).
 * Left out on purpose: the container traps *|dom_mis_fail and start_room_door2 (their OnStopTReverse fails the
 * mission or kills scientists when the world was really rewound), hide_dom* of the zeppelin levels (they show
 * objects for a real rewind). ---- */
typedef struct { const char *level, *obj; } RevObj;
static const RevObj rev_objs[] = {
    {"ts2_city_part03", "dom_kill1"}, {"ts2_city_part03", "dom_kill2"},
    {"ts2_skyscraper_part2", "force_field1"}, {"ts2_skyscraper_part2", "force_field2"},
    {"ts2_skyscraper_part2", "Shaft_coll_actor"}, {"ts2_skyscraper_part2", "Shaft_coll_actor1"},
    {"ts2_guardfactory_part01", "dom_force_field"}, {"ts2_guardfactory_part01", "dom_force_field1"},
    {"ts2_guardfactory_part01", "dom_force_field2"}, {"ts2_guardfactory_part01", "dom_force_field3"},
    {"ts2_guardfactory_part01", "dom_hint_guns"},
    {"ts2_guardfactory_part03", "dom_ffield_conveyer"},
    {"ts2_guardfactory_part04", "dom_force_field1"}, {"ts2_guardfactory_part04", "dom_force_field2"},
    {"ts2_guardfactory_part04", "PROPELLERS_1"},
    {"ts2_zep_factory_part1", "force_field"},
    {"ts2_zep_factory_part3", "Propellers_dom_WIND"}, {"ts2_zep_factory_part3", "Propellers_dom_WIND2"},
};
#define N_REV ((int)(sizeof rev_objs / sizeof rev_objs[0]))

/* ---- state ---- */
static int enabled, verbose, show_fx = 1;
static float rev_seconds = 5.f, rev_cost = 0.12f;
static int on;                               /* this machine runs a co-op reverse now (events fired, effect on) */
static int yours;                            /* ... owned by this machine's player (spends its energy) */
static char on_level[0x110];                 /* the level the start events were fired in */
/* server */
static int owner = NONE;
static DWORD until, last_repeat;
/* owner side */
static DWORD drain_last;
static LONG n_logged;
#define RLOG(...) do { if (InterlockedIncrement(&n_logged) <= 400) ts_log(__VA_ARGS__); } while (0)
#define VLOG(...) do { if (verbose) RLOG(__VA_ARGS__); } while (0)

static int game_mode(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm ? *(int *)(gm + MODE_OFF) : 0;
}

static char *slot_at(int i)
{
    char *gm = GAME_MODE_OBJ;
    return gm && i >= 0 && i < *(int *)(gm + NSLOTS_OFF) ? *(char **)(gm + SLOTS_OFF) + i * SLOT_SIZE : NULL;
}

static int slot_joined(char *s)
{
    return (*(int *)(s + 8) & (SLOT_JOINED | SLOT_READY)) == (SLOT_JOINED | SLOT_READY);
}

static int active(void)
{
    return enabled && tsync_coop_now();
}

static void level_base(const char *in, char *out, int size)
{
    const char *p = in, *slash;
    while ((slash = strpbrk(p, "\\/")) != NULL)
        p = slash + 1;
    snprintf(out, size, "%s", p);
    char *dot = strrchr(out, '.');
    if (dot && _stricmp(dot, ".lg") == 0)
        *dot = 0;
}

/* Fire `ev` (no arguments) on every reverse-puzzle object of the running level. */
static int fire_all(const char *ev)
{
    char base[0x110];
    level_base(LEVEL_NAME, base, sizeof base);
    int sym = 0, n = 0;
    sym_ctor(&sym, ev, 1);
    if (!sym)
        return 0;
    for (int i = 0; i < N_REV; i++) {
        if (_stricmp(rev_objs[i].level, base) != 0)
            continue;
        String nm = 0;
        ObjPtr inst = 0;
        str_ctor(&nm, rev_objs[i].obj, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        if (nm && --nm->refs == 0)
            eng_free(nm);
        if (!inst)
            continue;
        Variant dummy, *argv[1] = {&dummy};   /* (the dispatcher must not get a NULL argv) */
        Status st = {0, NULL};
        fire_by_name(&inst, &st, sym, 0, argv);
        if (st.code)
            RLOG("coopreverse: %s.%s: %s", rev_objs[i].obj, ev, st.msg ? st.msg->s : "error");
        if (st.msg && --st.msg->refs == 0)
            eng_free(st.msg);
        inst_release(&inst);
        n++;
    }
    return n;
}

static void fx_start(void)
{
    if (!show_fx || !FX_SLOT(MODE_REVERSE))
        return;
    int mgr = FX_MGR, idx = MODE_REVERSE + 3;
    __asm__ volatile("pushl $0\n\tcall *%[fn]"
                     : "+S"(mgr), "+D"(idx)
                     : [fn] "m"(fn_fx_start)
                     : "eax", "ecx", "edx", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
}

static void fx_stop(void)
{
    char *slot = FX_SLOT(MODE_REVERSE);
    if (slot && (*(int *)(slot + 8) & 1))
        ((void (TSCALL *)(void *))(*(void ***)slot)[2])(slot);
}

static void apply(int want, int mine)
{
    if (want == on) {
        yours = want && mine;
        return;
    }
    if (want) {
        snprintf(on_level, sizeof on_level, "%s", LEVEL_NAME);
        int n = fire_all("OnStartTReverse");
        fx_start();
        RLOG("coopreverse: reverse on%s (%d puzzle object(s))", mine ? " (mine)" : "", n);
    } else {
        int n = strcmp(on_level, LEVEL_NAME) == 0 ? fire_all("OnStopTReverse") : 0;
        fx_stop();
        RLOG("coopreverse: reverse off (%d puzzle object(s))", n);
    }
    on = want;
    yours = want && mine;
    drain_last = GetTickCount();
}

/* ---- server ---- */
static void send_state(int i)
{
    char *s = slot_at(i);
    if (!s || *(int *)s == 0 || !slot_joined(s))
        return;
    DWORD now = GetTickCount();
    int left = on && (int)(until - now) > 0 ? (int)(until - now) / 100 : 0;
    unsigned char b[8] = {PROTO, (unsigned char)on, (unsigned char)(on && owner == *(int *)s),
                          (unsigned char)(left > 255 ? 255 : left)};
    memcpy(b + 4, &owner, 4);
    coopnet_send(i, SUB_STATE, b, sizeof b);
}

static void broadcast(void)
{
    for (int i = 0; slot_at(i); i++)
        send_state(i);
    last_repeat = GetTickCount();
}

static void host_start(int who, float secs, const char *why)
{
    if (on || timesync_busy()) {
        RLOG("coopreverse: %s refused (%s)", why, on ? "a reverse runs" : "a slow/stop power runs");
        return;
    }
    if (secs <= 0.5f)
        return;
    owner = who;
    until = GetTickCount() + (DWORD)(secs * 1000.f);
    apply(1, who == 0);
    broadcast();
    RLOG("coopreverse: %s: reverse for %.1f s, owner %d", why, (double)secs, who);
}

static void host_stop(const char *why)
{
    if (!on)
        return;
    apply(0, 0);
    owner = NONE;
    broadcast();
    RLOG("coopreverse: reverse ended (%s)", why);
}

/* The reverse time the owner can pay for. */
static float affordable(char *pl)
{
    float e = pl ? *(float *)(pl + ENERGY) : 0.f, s = rev_cost > 0.f ? e / rev_cost : rev_seconds;
    return s < rev_seconds ? s : rev_seconds;
}

static void request(int start, float secs)
{
    if (game_mode() == MODE_CLIENT) {
        int t = (int)(secs * 10.f);
        unsigned char b[3] = {PROTO, (unsigned char)start, (unsigned char)(t > 255 ? 255 : t < 0 ? 0 : t)};
        coopnet_send(-1, SUB_REQ, b, sizeof b);
        VLOG("coopreverse: asked the host to %s (%.1f s)", start ? "start" : "stop", (double)secs);
    } else if (start)
        host_start(0, secs, "host's reverse key");
    else if (owner == 0)
        host_stop("host's player stopped");
}

static void on_message(int peer, unsigned short sub, const unsigned char *b, int len)
{
    if (!active() || len < 3 || b[0] != PROTO)
        return;
    if (sub == SUB_REQ && game_mode() == MODE_SERVER) {
        if (b[1])
            host_start(peer, b[2] / 10.f, "client's reverse key");
        else if (owner == peer)
            host_stop("client stopped (energy)");
    } else if (sub == SUB_STATE && game_mode() == MODE_CLIENT && len >= 8)
        apply(b[1] != 0, b[2] != 0);
}

/* ---- the reverse key (called from the thunk at 0x40d491, ebx = the player): 1 = consumed ---- */
__attribute__((used)) int crev_key(char *pl) __asm__("crev_key");
__attribute__((used)) int crev_key(char *pl)
{
    if (!active() || !pl || pl != PLAYER)
        return 0;
    if (on) {
        if (yours)
            request(0, 0.f);   /* (a second press ends one's own reverse early) */
        return 1;
    }
    if (timesync_busy()) {
        VLOG("coopreverse: reverse key refused, a slow/stop power runs");
        return 1;
    }
    request(1, affordable(pl));
    return 1;
}

extern void crev_thunk(void) __asm__("crev_thunk");
__asm__(".text\n"
        ".globl crev_thunk\n"
        "crev_thunk:\n"
        "  pushl %eax\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  pushl %ebx\n"
        "  call crev_key\n"
        "  addl $4, %esp\n"
        "  testl %eax, %eax\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  popl %eax\n"
        "  jz 1f\n"
        "  addl $4, %esp\n"              /* (drop the return to 0x40d496) */
        "  pushl $0x40d469\n"            /* key flag cleared, ret 4: as the refusal path ends */
        "  ret\n"
        "1:\n"
        "  pushl $0x5847c0\n"            /* the original call (its return address is still on the stack) */
        "  ret\n");

int coopreverse_busy(void)
{
    return enabled && on;
}

/* ---- per frame ---- */
static void frame(float dt)
{
    (void)dt;
    if (!active()) {
        if (on) {   /* (session or level over: the objects are gone, nothing to fire) */
            on = yours = 0;
            owner = NONE;
            fx_stop();
        }
        return;
    }
    DWORD now = GetTickCount();
    if (on && strcmp(on_level, LEVEL_NAME) != 0) {   /* a level change while a reverse ran */
        on = yours = 0;
        owner = NONE;
        fx_stop();
    }
    if (on && yours) {   /* the owner pays */
        char *pl = PLAYER;
        float dts = (now - drain_last) / 1000.f;
        drain_last = now;
        if (pl && rev_cost > 0.f) {
            float *e = (float *)(pl + ENERGY);
            *e = *e - rev_cost * dts < 0.f ? 0.f : *e - rev_cost * dts;
            if (*e <= 0.f)
                request(0, 0.f);
        }
    }
    if (game_mode() != MODE_SERVER)
        return;
    if (on && (int)(now - until) >= 0)
        host_stop("time up");
    if (on && owner > 0) {   /* the owner left */
        int found = 0;
        for (int i = 0; slot_at(i); i++)
            if (*(int *)slot_at(i) == owner && slot_joined(slot_at(i)))
                found = 1;
        if (!found)
            host_stop("owner left");
    }
    if (on && now - last_repeat >= 1000)
        broadcast();
}

int coopreverse_console(const char *text, char *out, int size)
{
    if (strncmp(text, "!rv", 3) != 0)
        return 0;
    char cmd[16] = "";
    float s = rev_seconds;
    sscanf(text + 3, "%15s %f", cmd, &s);
    if (!strcmp(cmd, "start") && active()) {
        request(1, s);
        return snprintf(out, size, "reverse asked for %.1f s", (double)s), 1;
    }
    if (!strcmp(cmd, "stop") && active()) {
        request(0, 0.f);
        return snprintf(out, size, "reverse stop asked"), 1;
    }
    snprintf(out, size, "coopreverse %s, active %d, on %d, yours %d, owner %d, %.1f s at %.2f energy/s, energy %.2f",
             enabled ? "enabled" : "off", active(), on, yours, owner, (double)rev_seconds, (double)rev_cost,
             PLAYER ? (double)*(float *)(PLAYER + ENERGY) : -1.0);
    return 1;
}

void coopreverse_install(void)
{
    char v[16];
    if (!ts_env("TS_COOP_REVERSE", v, sizeof v) || v[0] != '1')
        return;
    if (ts_env("TS_COOP_REVERSE_SECONDS", v, sizeof v) && atof(v) > 0)
        rev_seconds = (float)atof(v);
    if (ts_env("TS_COOP_REVERSE_COST", v, sizeof v) && atof(v) >= 0)
        rev_cost = (float)atof(v);
    show_fx = !(ts_env("TS_TIMESYNC_FX", v, sizeof v) && v[0] == '0');
    verbose = ts_env("TS_COOP_REVERSE_LOG", v, sizeof v) && v[0] == '1';
    if (!ts_frame_add(frame) || !coopnet_listen(SUB_REQ, SUB_LAST, on_message)) {
        ts_log("coopreverse: off (no frame slot or mod messages)");
        return;
    }
    static const unsigned char want[5] = {0xe8, 0x2a, 0x73, 0x17, 0x00};   /* call 0x5847c0 at 0x40d491 */
    unsigned char rep[5] = {0xe8};
    *(DWORD *)(rep + 1) = (DWORD)crev_thunk - (0x40d491 + 5);
    if (!ts_patch_bytes(0x40d491, want, rep, 5)) {
        ts_log("coopreverse: reverse key code not found (other exe), off");
        return;
    }
    enabled = 1;
    ts_log("coopreverse: on (NOT YET RUN: %.1f s per reverse, %.2f energy/s, %d puzzle objects in the table)",
           (double)rev_seconds, (double)rev_cost, N_REV);
}
