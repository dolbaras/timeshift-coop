/*
 * Players' time powers in co-op on a campaign level (patch 1.2 exe only; addresses from the static analysis
 * in the private notes). TS_TIMESYNC=1 until checked in the game.
 *
 * Engine facts this builds on:
 * - The world clock: every frame 0x6494c0 sets the world frame time [0xfcab8c] = real frame time [0xfcab74]
 *   x the current world scale [0xec00e0], which moves towards the target [0xebfbb4] (1, slow 0.2, stop 0.001)
 *   at the rate [0x1095f88]. Global flags [0xfcace0]: bit 0 slow, bit 1 stop. Player code runs on the real
 *   frame time, so the player keeps normal speed while the world is slow or stopped.
 * - World start 0x441700 (eax = mode 0 slow / 1 stop, ecx = the player): sets the flag and the target and
 *   tells every object (in-process messages 0x3fd / 0x401). World end 0x4419b0 (eax = mode, ecx, [esp+4] 1 =
 *   at once, 0 = ramp): clears it (0x3fe / 0x402). Nothing goes over the network.
 * - A player's own power: the controller at player+0x280 (+0 active mode, -1 none / 0 / 1 / 2 reverse,
 *   +0x2c energy). Every start goes through 0x43e1b0, which calls the world start; the end is 0x43e2b0.
 * - In a network game (game mode 2/3/4) the power keys never reach that code: the input function 0x40d2c0
 *   takes its multiplayer branch (chrono grenades, time shield), and the energy set-up (0x43de20), regen
 *   (0x43e3b0, 0x43eae0) and start threshold (0x43e1b0) use the multiplayer rules (StartEnergy, ...).
 *
 * What this file does (only in a network game on a campaign level, or on any map with TS_TIMESYNC=all):
 * - Those six game-mode reads see "single player", so the keys, energy and HUD work as in the campaign.
 * - Rule: one world power at a time, like the engine's own one-power rule. The server decides who owns it
 *   (first come, first served). The owner's machine runs the power as in single player; every other machine
 *   slows or stops its own world the same way and shows the same screen effect. Every human player moves at
 *   normal speed (each machine's own player runs on real time, remote bodies follow their owners). Energy is
 *   per player: only the owner spends it. While a teammate's power runs, the power keys do nothing.
 *   Time reversal is not shared yet: in co-op the reverse key does nothing (TS_TIMESYNC_REVERSE=1 lets it
 *   run on the local machine only).
 * - Messages (coopnet.c, reliable): 10 = client -> server "my power is now <mode>"; 11 = server -> each
 *   client "the world power: <mode>, owner, yours?". The server answers a refused request with 11, the
 *   client then ends its own power (a start in the same moment on two machines). The server repeats 11
 *   every second while a power runs (late joiners) and ends a power whose owner left.
 * - Every machine checks each frame that its world still shows a power started for a teammate (a death,
 *   a level script or the menu may reset the world) and starts it again.
 *
 * TS_TIMESYNC_FX=0: no screen effect on the other machines.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

#define GAME_MODE_OBJ (*(char **)0xfcab34)   /* +0x3c mode, +0x28 client slots, +0x2c slot count */
#define MODE_OFF 0x3c
#define SLOTS_OFF 0x28
#define NSLOTS_OFF 0x2c
#define SLOT_SIZE 0xa4                       /* +0 client id (0 = the server), +8 flags */
#define SLOT_READY 0x80
#define SLOT_JOINED 0x100
#define MODE_SERVER 2
#define MODE_CLIENT 3
#define MODE_DEDICATED 4
#define LEVEL_NAME ((const char *)0xfcabc8)  /* the level file of the running map (char array) */
#define LEVEL_NAME_MAX 0x110

#define PLAYER (*(char **)0x1095b30)         /* $player (also [0xfc8180]) */
#define PWR_CTL 0x280                        /* the player's time power controller; +0 active mode */
#define TIME_FLAGS (*(volatile unsigned *)0xfcace0)
#define WORLD_TARGET (*(volatile float *)0xebfbb4)
#define SCALE_SLOW (*(const float *)0xd7209c)   /* 0.2 */
#define SCALE_STOP (*(const float *)0xd71cb4)   /* 0.001 */
#define FX_MGR 0x1096f18                     /* screen effects; slot mode+3 = the power's effect */
#define FX_SLOT(mode) (*(char **)(FX_MGR + 0x14f4 + ((mode) + 3) * 4))

static const DWORD fn_world_start = 0x441700;   /* eax = mode, ecx = player; ret */
static const DWORD fn_world_stop = 0x4419b0;    /* eax = mode, ecx = player, [esp+4] 0 = ramp; ret 4 */
static const DWORD fn_fx_start = 0x665f70;      /* esi = FX_MGR, edi = slot, [esp+4] float; ret 4 */
static const DWORD fn_power_end = 0x43e2b0;     /* ecx = player, esi = player+0x280, edx = 0; ret */

#define SUB_REQ 10                           /* client -> server: {u8 version, i8 mode} */
#define SUB_STATE 11                         /* server -> client: {u8 version, i8 mode, u8 yours, u8 0, i32 owner} */
#define PROTO 1
#define NONE (-1)
#define HOST_ID 0                            /* the server's own client id */
#define REPEAT_MS 1000
#define REASSERT_MS 500

#pragma pack(push, 1)
typedef struct { unsigned char ver; signed char mode; } Req;
typedef struct { unsigned char ver; signed char mode; unsigned char yours, pad; int owner; } State;
#pragma pack(pop)

/* ---- state (main thread only: the network pump and the script tick) ---- */
static int enabled, any_level, show_fx = 1, allow_reverse;
static char *last_player;
static int local_last = NONE;                /* the local player's power as last seen / reported */
static int applied = NONE;                   /* the teammate's power this machine runs for them */
static DWORD last_reassert, n_reassert;
static volatile int tsync_block;   /* 1 = a teammate's power runs: refuse local starts */
/* server */
static int owner = NONE, owner_mode = NONE;
static DWORD last_repeat;
/* client */
static int team_mode = NONE, team_yours, team_owner = NONE;
static LONG n_logged;
#define LOG_MAX 400

#define TLOG(...) do { if (InterlockedIncrement(&n_logged) <= LOG_MAX) ts_log(__VA_ARGS__); } while (0)

static const char *mode_name(int m)
{
    return m == 0 ? "slow" : m == 1 ? "stop" : m == 2 ? "reverse" : "none";
}

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

static int slot_of(int id)
{
    for (int i = 0;; i++) {
        char *s = slot_at(i);
        if (!s)
            return -1;
        if (*(int *)s == id)
            return i;
    }
}

static int campaign_level(void)
{
    const char *s = LEVEL_NAME;
    for (int i = 0; i < LEVEL_NAME_MAX - 4 && s[i]; i++)
        if ((s[i] | 0x20) == 't' && (s[i + 1] | 0x20) == 's' && s[i + 2] == '2' && s[i + 3] == '_')
            return 1;
    return 0;
}

/* Co-op time powers apply now: network game, campaign level. Called from the patched game-mode reads too
 * (cdecl, changes eax/ecx/edx only). */
__attribute__((used)) int tsync_coop_now(void) __asm__("tsync_coop_now");
__attribute__((used)) int tsync_coop_now(void)
{
    static int last = -1;
    int m = game_mode();
    int on = enabled && (m == MODE_SERVER || m == MODE_CLIENT || m == MODE_DEDICATED)
             && (any_level || campaign_level());
    if (on != last) {
        last = on;
        TLOG("timesync: co-op powers %s (game mode %d, level \"%.80s\")", on ? "on" : "off", m, LEVEL_NAME);
    }
    return on;
}

/* The start gate (0x43e1b0): 0 = refuse this start. */
__attribute__((used)) int tsync_may_start(int mode) __asm__("tsync_may_start");
__attribute__((used)) int tsync_may_start(int mode)
{
    if (!tsync_coop_now())
        return 1;
    if (tsync_block) {
        TLOG("timesync: local %s refused, a teammate's power runs", mode_name(mode));
        return 0;
    }
    if (mode == 2 && !allow_reverse) {
        TLOG("timesync: local reverse refused (not shared in co-op yet)");
        return 0;
    }
    return 1;
}

/* A game-mode object whose mode reads "single player" (for the patched `mov ecx, [0xfcab34]`). */
int tsync_fake_gm[0x40 / 4] __asm__("tsync_fake_gm");

/* Thunks called from the patched sites; they keep every register the original instructions kept. */
extern void tsync_mode_thunk(void) __asm__("tsync_mode_thunk");    /* eax = game mode, or 1 in co-op */
extern void tsync_gm_thunk(void) __asm__("tsync_gm_thunk");        /* ecx = game-mode object, or the fake */
extern void tsync_start_thunk(void) __asm__("tsync_start_thunk");  /* jumped to from 0x43e1b0 */
__asm__(".text\n"
        ".globl tsync_mode_thunk\n"
        "tsync_mode_thunk:\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  call tsync_coop_now\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  testl %eax, %eax\n"
        "  jnz 1f\n"
        "  movl 0xfcab34, %eax\n"
        "  movl 0x3c(%eax), %eax\n"
        "  ret\n"
        "1:\n"
        "  movl $1, %eax\n"
        "  ret\n"
        ".globl tsync_gm_thunk\n"
        "tsync_gm_thunk:\n"
        "  pushl %eax\n"
        "  pushl %edx\n"
        "  call tsync_coop_now\n"
        "  testl %eax, %eax\n"
        "  popl %edx\n"
        "  popl %eax\n"
        "  jnz 2f\n"
        "  movl 0xfcab34, %ecx\n"
        "  ret\n"
        "2:\n"
        "  movl $tsync_fake_gm, %ecx\n"
        "  ret\n"
        ".globl tsync_start_thunk\n"
        "tsync_start_thunk:\n"
        "  pushl %eax\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  pushl %eax\n"
        "  call tsync_may_start\n"
        "  addl $4, %esp\n"
        "  testl %eax, %eax\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  popl %eax\n"
        "  jz 3f\n"
        "  cmpl $-1, (%esi)\n"          /* the 8 bytes the jump replaced */
        "  pushl %ebx\n"
        "  movl 8(%esp), %ebx\n"
        "  pushl $0x43e1b8\n"           /* (push/ret keep the flags of the cmp) */
        "  ret\n"
        "3:\n"
        "  ret $4\n");

/* ---- engine calls (main thread, in the script tick like the game's own StartTControl) ---- */
#define XMM_CLOBBERS "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"

static void world_start(char *pl, int mode)
{
    int a = mode;
    char *c = pl;
    __asm__ volatile("call *%[fn]"
                     : "+a"(a), "+c"(c)
                     : [fn] "m"(fn_world_start)
                     : "edx", "memory", "cc", XMM_CLOBBERS);
}

static void world_stop(char *pl, int mode)
{
    int a = mode;
    char *c = pl;
    __asm__ volatile("pushl $0\n\tcall *%[fn]"   /* (0 = back to normal speed with the game's ramp) */
                     : "+a"(a), "+c"(c)
                     : [fn] "m"(fn_world_stop)
                     : "edx", "memory", "cc", XMM_CLOBBERS);
}

static int fx_active(int mode)
{
    char *slot = FX_SLOT(mode);
    return slot && (*(int *)(slot + 8) & 1);
}

static void fx_start(int mode)
{
    if (!show_fx || !FX_SLOT(mode))
        return;
    int mgr = FX_MGR, idx = mode + 3;
    __asm__ volatile("pushl $0\n\tcall *%[fn]"   /* (float 0.0, as the script StartTControl(.., true)) */
                     : "+S"(mgr), "+D"(idx)
                     : [fn] "m"(fn_fx_start)
                     : "eax", "ecx", "edx", "memory", "cc", XMM_CLOBBERS);
}

static void fx_stop(int mode)
{
    if (fx_active(mode)) {
        char *slot = FX_SLOT(mode);
        ((void (TSCALL *)(void *))(*(void ***)slot)[2])(slot);   /* (as the script StopTControl) */
    }
}

/* End the local player's own power as its key would (energy bookkeeping included). */
static void power_end(char *pl)
{
    char *c = pl, *ctl = pl + PWR_CTL;
    int d = 0;
    __asm__ volatile("call *%[fn]"
                     : "+c"(c), "+S"(ctl), "+d"(d)
                     : [fn] "m"(fn_power_end)
                     : "eax", "memory", "cc", XMM_CLOBBERS);
}

static int local_mode(char *pl)
{
    int m = *(int *)(pl + PWR_CTL);
    return m == 0 || m == 1 ? m : NONE;   /* (reverse: local only) */
}

static int world_shows(int mode)
{
    float want = mode == 0 ? SCALE_SLOW : SCALE_STOP, d = WORLD_TARGET - want;
    return (TIME_FLAGS & (1u << mode)) && d < 1e-5f && d > -1e-5f;
}

/* ---- messages ---- */
static void send_state_to_slot(int i)
{
    char *s = slot_at(i);
    if (!s || *(int *)s == HOST_ID || !slot_joined(s))
        return;
    State st = {PROTO, (signed char)owner_mode, (unsigned char)(owner != NONE && owner == *(int *)s), 0, owner};
    coopnet_send(i, SUB_STATE, &st, sizeof st);
}

static void host_broadcast(void)
{
    for (int i = 0; slot_at(i); i++)
        send_state_to_slot(i);
    last_repeat = GetTickCount();
}

static void host_set(int who, int mode, const char *why)
{
    if (who == owner && mode == owner_mode)
        return;
    TLOG("timesync: world power %s -> %s, owner %d -> %d (%s)", mode_name(owner_mode), mode_name(mode), owner,
         who, why);
    owner = who;
    owner_mode = mode;
    host_broadcast();
}

static void on_message(int peer, unsigned short sub, const unsigned char *data, int len)
{
    int client = game_mode() == MODE_CLIENT;
    if (sub == SUB_REQ && !client && len >= (int)sizeof(Req) && data[0] == PROTO) {
        int mode = ((const Req *)data)->mode;
        TLOG("timesync: client %d asks %s (world %s, owner %d)", peer, mode_name(mode), mode_name(owner_mode), owner);
        if (!tsync_coop_now())
            return;
        if (mode == 0 || mode == 1) {
            if (owner == NONE || owner == peer)
                host_set(peer, mode, "client power");
            else
                send_state_to_slot(slot_of(peer));   /* (refused: the client ends its own power) */
        } else if (owner == peer)
            host_set(NONE, NONE, "client power ended");
    } else if (sub == SUB_STATE && client && len >= (int)sizeof(State) && data[0] == PROTO) {
        const State *st = (const State *)data;
        if (st->mode != team_mode || st->yours != team_yours || st->owner != team_owner)
            TLOG("timesync: world power %s, owner %d%s", mode_name(st->mode), st->owner, st->yours ? " (me)" : "");
        team_mode = st->mode == 0 || st->mode == 1 ? st->mode : NONE;
        team_yours = st->yours;
        team_owner = st->owner;
    } else
        TLOG("timesync: ignored sub %u (%d bytes) from peer %d", sub, len, peer);
}

/* ---- per frame ---- */
static void forget_world(void)   /* this machine's view of its world only (the engine has reset it) */
{
    applied = NONE;
    tsync_block = 0;
}

static void reset_all(const char *why)
{
    if (applied != NONE || owner != NONE || team_mode != NONE || local_last != NONE)
        TLOG("timesync: state reset (%s)", why);
    forget_world();
    owner = owner_mode = team_mode = team_owner = local_last = NONE;
    team_yours = 0;
}

/* Run (or stop running) a teammate's power on this machine. */
static void reconcile(char *pl, int local, int want)
{
    DWORD now = GetTickCount();
    if (local != NONE) {   /* the local power runs the world (only after a start in the same moment) */
        if (applied != NONE) {
            TLOG("timesync: teammate's %s dropped here, the local %s runs", mode_name(applied), mode_name(local));
            fx_stop(applied);
            world_stop(pl, applied);   /* (then the local power's world again: flag, target, messages) */
            world_start(pl, local);
            applied = NONE;
        }
        tsync_block = 0;
        return;
    }
    if (want != applied) {
        TLOG("timesync: teammate's power here: %s -> %s", mode_name(applied), mode_name(want));
        if (applied != NONE) {
            fx_stop(applied);
            world_stop(pl, applied);
        }
        if (want != NONE) {
            fx_start(want);
            world_start(pl, want);
        }
        applied = want;
    } else if (applied != NONE && !world_shows(applied) && now - last_reassert >= REASSERT_MS) {
        last_reassert = now;
        if (++n_reassert <= 20 || n_reassert % 100 == 0)
            TLOG("timesync: world lost the teammate's %s (flags %x, target %.3f): started again (%lu)",
                 mode_name(applied), TIME_FLAGS, (double)WORLD_TARGET, n_reassert);
        if (!fx_active(applied))
            fx_start(applied);
        world_start(pl, applied);
    }
    tsync_block = applied != NONE;
}

static void frame(float dt)
{
    static int was_on;
    (void)dt;
    if (!tsync_coop_now()) {   /* (level or session over: the engine resets the world clock) */
        if (was_on)
            reset_all("co-op powers off");
        was_on = 0;
        last_player = NULL;
        return;
    }
    was_on = 1;
    char *pl = PLAYER;
    if (pl != last_player) {   /* a new player object (level start, respawn) or none (loading) */
        forget_world();
        last_player = pl;
    }
    /* (a power of the old player object ends here: its mode differs from local_last) */
    int local = pl ? local_mode(pl) : NONE, want;
    if (game_mode() != MODE_CLIENT) {
        if (owner != NONE && owner != HOST_ID) {   /* the owner left */
            int i = slot_of(owner);
            if (i < 0 || !slot_joined(slot_at(i)))
                host_set(NONE, NONE, "owner left");
        }
        if (local != NONE && owner != NONE && owner != HOST_ID) {   /* started in the same moment as a client */
            TLOG("timesync: local %s ended, client %d owns the world power", mode_name(local), owner);
            power_end(pl);
            local = local_mode(pl);
        }
        if (local != local_last) {
            if (local != NONE)
                host_set(HOST_ID, local, "host power");
            else if (owner == HOST_ID)
                host_set(NONE, NONE, "host power ended");
            local_last = local;
        }
        if (owner != NONE && GetTickCount() - last_repeat >= REPEAT_MS)
            host_broadcast();
        want = owner != NONE && owner != HOST_ID ? owner_mode : NONE;
    } else {
        if (local != NONE && team_mode != NONE && !team_yours) {   /* refused by the server */
            TLOG("timesync: local %s ended, the world power is %s of %d", mode_name(local), mode_name(team_mode),
                 team_owner);
            power_end(pl);
            local = local_mode(pl);
        }
        if (local != local_last) {
            Req r = {PROTO, (signed char)local};
            if (coopnet_send(-1, SUB_REQ, &r, sizeof r) > 0) {
                TLOG("timesync: local power %s -> %s, told the server", mode_name(local_last), mode_name(local));
                local_last = local;
            }
        }
        want = team_mode != NONE && !team_yours ? team_mode : NONE;
    }
    if (pl)
        reconcile(pl, local, want);
}

/* ---- install ---- */
typedef struct { DWORD addr; int len; unsigned char want[13]; unsigned char rep[13]; void *to; } Site;

static Site sites[] = {
    /* 0x40d2c0 power input: `mov ecx,[gm]; cmp [ecx+0x3c],1` -> `call mode; cmp eax,1` (single-player keys) */
    {0x40d2ff, 10, {0x8b, 0x0d, 0x34, 0xab, 0xfc, 0x00, 0x83, 0x79, 0x3c, 0x01},
     {0xe8, 0, 0, 0, 0, 0x83, 0xf8, 0x01, 0x90, 0x90}, (void *)tsync_mode_thunk},
    /* its multiplayer branch (chrono grenades, time shield): `mov eax,[gm]; mov eax,[eax+0x3c]` -> 1 = skip */
    {0x40d723, 8, {0xa1, 0x34, 0xab, 0xfc, 0x00, 0x8b, 0x40, 0x3c}, {0xe8, 0, 0, 0, 0, 0x90, 0x90, 0x90},
     (void *)tsync_mode_thunk},
    /* energy set-up at spawn 0x43de20 (single player: full energy) */
    {0x43de4b, 8, {0xa1, 0x34, 0xab, 0xfc, 0x00, 0x8b, 0x40, 0x3c}, {0xe8, 0, 0, 0, 0, 0x90, 0x90, 0x90},
     (void *)tsync_mode_thunk},
    /* energy regeneration 0x43e3b0 */
    {0x43e3ed, 8, {0xa1, 0x34, 0xab, 0xfc, 0x00, 0x8b, 0x40, 0x3c}, {0xe8, 0, 0, 0, 0, 0x90, 0x90, 0x90},
     (void *)tsync_mode_thunk},
    /* start threshold in 0x43e1b0: `mov eax,[gm]; mov ebp,1; cmp [eax+0x3c],ebp` -> `call; mov ebp,1; cmp eax,ebp` */
    {0x43e1be, 13, {0xa1, 0x34, 0xab, 0xfc, 0x00, 0xbd, 0x01, 0x00, 0x00, 0x00, 0x39, 0x68, 0x3c},
     {0xe8, 0, 0, 0, 0, 0xbd, 0x01, 0x00, 0x00, 0x00, 0x39, 0xe8, 0x90}, (void *)tsync_mode_thunk},
    /* regeneration delay in 0x43eae0: `mov ecx,[gm]` -> `call gm` (the fake object reads mode 1) */
    {0x43eb8f, 6, {0x8b, 0x0d, 0x34, 0xab, 0xfc, 0x00}, {0xe8, 0, 0, 0, 0, 0x90}, (void *)tsync_gm_thunk},
    /* the start gate: the first 8 bytes of 0x43e1b0 -> jmp tsync_start_thunk (e9: same displacement) */
    {0x43e1b0, 8, {0x83, 0x3e, 0xff, 0x53, 0x8b, 0x5c, 0x24, 0x08}, {0xe9, 0, 0, 0, 0, 0xcc, 0xcc, 0xcc},
     (void *)tsync_start_thunk},
};
#define N_SITES (int)(sizeof sites / sizeof sites[0])

void timesync_install(void)
{
    char v[16];
    if (!ts_env("TS_TIMESYNC", v, sizeof v) || v[0] == '0')
        return;
    any_level = _stricmp(v, "all") == 0;
    show_fx = !(ts_env("TS_TIMESYNC_FX", v, sizeof v) && v[0] == '0');
    allow_reverse = ts_env("TS_TIMESYNC_REVERSE", v, sizeof v) && v[0] == '1';
    tsync_fake_gm[MODE_OFF / 4] = 1;
    if (!coopnet_listen(SUB_REQ, SUB_STATE, on_message)) {
        ts_log("timesync: mod messages are off (coopnet), not installed");
        return;
    }
    if (!ts_frame_add(frame)) {
        ts_log("timesync: no frame hook, not installed");
        return;
    }
    int done = 0;
    for (; done < N_SITES; done++) {
        Site *s = &sites[done];
        *(DWORD *)(s->rep + 1) = (DWORD)s->to - (s->addr + 5);
        if (!ts_patch_bytes(s->addr, s->want, s->rep, s->len))
            break;
    }
    if (done < N_SITES) {
        ts_log("timesync: code at %08lx not found (other exe), not installed", sites[done].addr);
        while (done-- > 0)
            ts_patch_bytes(sites[done].addr, sites[done].rep, sites[done].want, sites[done].len);
        return;   /* (the frame callback stays, harmless: enabled is 0) */
    }
    enabled = 1;
    ts_log("timesync: on (%s, screen effect %s, reverse %s)", any_level ? "every network map" : "campaign levels",
           show_fx ? "on" : "off", allow_reverse ? "local" : "off");
}
