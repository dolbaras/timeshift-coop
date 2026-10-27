/*
 * Time reversal in co-op (patch 1.2 exe only). Live-tested on the N100 pair 2026-10-26 (campaign-audit.md 3b). On by
 * default in co-op sessions (coopsession.c sets TS_COOP_REVERSE=1 and TS_COOP_REWIND=1 when not set; =0 off); acts
 * only where timesync.c runs the campaign time powers (a co-op session on a campaign level).
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
 *   82 server -> client  {u8 1}   (trapped in a closed container after a rewind)
 *   83 server -> client  {u8 1, u8 actor, u8 clip, u8 flags, f32 time}   (rewind actors' clips, see send_anims)
 *
 * Real rewind on the host (TS_COOP_REWIND=1, with TS_COOP_REVERSE=1): the engine's own time recorder.
 * It takes a snapshot of every isSyncTR object every 0.1 s through the save serializer [0x1095f94] (0x5849c0, flags
 * 0x800000 save / 0x1000000 load in [0xfcace8]) and plays them back during a reverse (0x584170 asks, 0x584720
 * broadcasts 0x3ff and 0x584bf0 sets the playback flag 0x400000; 0x584870 ends with 0x400). Players are not
 * isSyncTR (plr_comm), so nobody's body is rewound. The recorder code tests "game mode 1" in nine places
 * (0x583912 ... 0x585b97); on a co-op host they read a fake game-mode object with mode 1, so the host records. The
 * host's own reverse key then runs the engine's reverse (energy, duration, effect as in the campaign); a client's
 * request starts the same playback through 0x4417f0 (mode 2, the host's player) and ends it through 0x441a70. The
 * host's world rewinds; replicated objects (dynamic actors, physics, doors, buttons, AI) reach the clients through
 * their normal updates; the clients fire the reverse events of the puzzle table (their own force fields) as in the
 * quick variant. After a rewind, a client inside a closed container trap (*|dom_mis_fail, levels 11 15 18 20) dies
 * (message 82); the host's own trap check is the level's script (FailMission = team restart).
 *
 * Switches: TS_COOP_REVERSE=1, TS_COOP_REWIND=1, TS_COOP_REVERSE_SECONDS=<s>, TS_COOP_REVERSE_COST=<energy per s>,
 * TS_COOP_REVERSE_LOG=1.
 * Test console: "!rv state", "!rv start [s]" (as if this machine's player pressed reverse), "!rv stop".
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
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
#define REC (*(char **)0xfc8400)             /* the time recorder (TR task) */
#define TR_SAVE (*(char **)0x1095f94)        /* the TR serializer (save manager subclass, vtable 0xd9cfac) */
#define REC_FRAMES 0x700ad0                  /* recorded snapshots */
#define REC_FLAGS 0x700ae0                   /* 8 = playback asked, 0x10 = recording off */
#define GAME_FLAGS (*(volatile unsigned *)0xfcace8)
#define GF_PLAYBACK 0x400000                 /* reverse playback runs */
#define DOM_LOCKS 0x88
#define DOM_UPDATE 0x6c2dd0
static int (TSCALL *dom_is_belong)(void *, void *) = (void *)0x6c3d60;
static const DWORD fn_rev_start = 0x4417f0;  /* eax = 2, [esp+4] = the player; ret 4 (the world part of a power start) */
static void (TSCALL *rev_end)(void *, int, int) = (void *)0x441a70;   /* (player, mode 2, 0): stops the playback */

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
#define SUB_TRAP 82                          /* server -> client {u8 1}: trapped in a closed container after a rewind */
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
/* the stock rewind on the host (TS_COOP_REWIND=1, see the header) */
static int stock;                            /* switched on */
static int stock_run;                        /* (host) the engine's reverse playback runs now */
static int stock_ours;                       /* (host) ... started by this file for a client (we end it) */
static DWORD stock_since, trap_until;
static int trap_kill;                        /* (client) the host says: trapped in a container */
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
        int n = stock_run ? 0 : fire_all("OnStartTReverse");   /* (stock rewind: the engine fires them) */
        if (!stock_run || stock_ours)
            fx_start();
        RLOG("coopreverse: reverse on%s (%d puzzle object(s))", mine ? " (mine)" : "", n);
    } else {
        int n = !stock_run && strcmp(on_level, LEVEL_NAME) == 0 ? fire_all("OnStopTReverse") : 0;
        fx_stop();
        RLOG("coopreverse: reverse off (%d puzzle object(s))", n);
    }
    on = want;
    yours = want && mine;
    drain_last = GetTickCount();
}

/* ---- the stock rewind on the host ---- */
static int playing(void)
{
    char *r = REC;
    return r && ((*(int *)(r + REC_FLAGS) & 8) || (GAME_FLAGS & GF_PLAYBACK));
}

/* The host may use the engine's recorder now: the server of a co-op session, the recorder and its serializer exist,
 * recording is on and holds enough snapshots (the engine wants 5). */
static int stock_ready(void)
{
    char *r = REC;
    return stock && game_mode() == MODE_SERVER && r && TR_SAVE && *(int *)(r + REC_FRAMES) >= 5
           && !(*(int *)(r + REC_FLAGS) & 0x10);
}

/* Called from the patched game-mode reads of the recorder code (0x583900-0x585c00): 1 = this host records. */
__attribute__((used)) int crw_record(void) __asm__("crw_record");
__attribute__((used)) int crw_record(void)
{
    return stock && game_mode() == MODE_SERVER && ts_coop_active();
}

int crw_fake_gm[0x40 / 4] __asm__("crw_fake_gm");   /* a game-mode object reading "single player" (+0x3c = 1) */
extern void crw_gm_eax(void) __asm__("crw_gm_eax");
extern void crw_gm_ecx(void) __asm__("crw_gm_ecx");
__asm__(".text\n"
        ".globl crw_gm_eax\n"
        "crw_gm_eax:\n"                  /* replaces `mov eax, [0xfcab34]` */
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  call crw_record\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  testl %eax, %eax\n"
        "  jnz 1f\n"
        "  movl 0xfcab34, %eax\n"
        "  ret\n"
        "1:\n"
        "  movl $crw_fake_gm, %eax\n"
        "  ret\n"
        ".globl crw_gm_ecx\n"
        "crw_gm_ecx:\n"                  /* replaces `mov ecx, [0xfcab34]` (eax kept: 0x583a67 compares with it) */
        "  pushl %eax\n"
        "  pushl %edx\n"
        "  call crw_record\n"
        "  testl %eax, %eax\n"
        "  popl %edx\n"
        "  popl %eax\n"
        "  jnz 2f\n"
        "  movl 0xfcab34, %ecx\n"
        "  ret\n"
        "2:\n"
        "  movl $crw_fake_gm, %ecx\n"
        "  ret\n");

/* Container traps (their OnStopTReverse fails the mission when the player is inside a closed container after a
 * rewind): on the host the engine runs them for the host's player; a client inside one dies instead (respawn). */
static const char *const traps[] = {
    "ts2_zepcrashsite_part03", "clonecontainer_3|dom_mis_fail", "clonecontainer_4|dom_mis_fail",
    "clonecontainer_6|dom_mis_fail", "clonecontainer_7|dom_mis_fail", NULL,
    "ts2_guardfactory_part02", "s3d_Container_1|dom_mis_fail", "s3d_Container_2|dom_mis_fail",
    "s3d_Container_3|dom_mis_fail", "s3d_Container_4|dom_mis_fail", "s3d_Container_5|dom_mis_fail",
    "s3d_Container_6|dom_mis_fail", "s3d_Container_7|dom_mis_fail", "s3d_Container_8|dom_mis_fail",
    "s3d_Container_9|dom_mis_fail", "s3d_Container_10|dom_mis_fail", "s3d_Container_11|dom_mis_fail",
    "s3d_Container_12|dom_mis_fail", "s3d_Container_16|dom_mis_fail", "s3d_Container_18|dom_mis_fail", NULL,
    "ts2_guardfactory_part05", "clonecontainer_1|dom_mis_fail", "clonecontainer_2|dom_mis_fail",
    "clonecontainer_3|dom_mis_fail", "clonecontainer_4|dom_mis_fail", "clonecontainer_5|dom_mis_fail", NULL,
    "ts2_zep_factory_part2", "clone1|dom_mis_fail", "clone2|dom_mis_fail", "clone3|dom_mis_fail",
    "clone4|dom_mis_fail", NULL,
    NULL};

static void trap_check(void)
{
    char base[0x110];
    level_base(LEVEL_NAME, base, sizeof base);
    const char *const *p = traps;
    while (*p && _stricmp(*p, base) != 0) {
        while (*p)
            p++;
        p++;
    }
    if (!*p)
        return;
    for (p++; *p; p++) {
        String nm = 0;
        ObjPtr inst = 0;
        str_ctor(&nm, *p, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        if (nm && --nm->refs == 0)
            eng_free(nm);
        if (!inst)
            continue;
        char *dom = *(char **)((char *)inst + 0xc);
        inst_release(&inst);
        if (!dom || (*(void ***)dom)[0x134 / 4] != (void *)DOM_UPDATE || *(int *)(dom + DOM_LOCKS) <= 0)
            continue;
        for (int i = 0; slot_at(i); i++) {
            char *s = slot_at(i), *ent = *(char **)(s + 4);
            if (*(int *)s == 0 || !slot_joined(s) || !ent)
                continue;
            int in = dom_is_belong(dom, ent) & 0xff;
            VLOG("coopreverse: trap %s (locks %d): client in slot %d %s", *p, *(int *)(dom + DOM_LOCKS), i, in ? "inside" : "outside");
            if (!in)
                continue;
            unsigned char b = PROTO;
            coopnet_send(i, SUB_TRAP, &b, 1);
            RLOG("coopreverse: client in slot %d trapped in %s after the rewind: it dies", i, *p);
        }
    }
}

/* During the engine's playback an entity's frame returns before its network update (0x6440ed: playback flag, entity
 * +0x98 bit 6 clear -> return), so clients would see the rewound puzzle actors only jump at the end. The host
 * therefore runs the network update (0x6931e0, esi = the entity) of the level's rewind actors itself every frame of a
 * playback. */
static const char *const rewind_actors[] = {
    "ts2_skyscraper_part2", "Kran_actor", "Conveyer_actor", NULL,
    "ts2_zepcrashsite_part01", "bridge_puzle_actor", NULL,
    "ts2_zepcrashsite_part02", "Lift_actor", NULL,
    "ts2_zep_factory_part1", "Actor_train", "Actor_ghost_train", NULL,
    "ts2_zepride2_part01", "cage1_actor", "cage2_actor", NULL,
    NULL};
static const DWORD fn_net_update = 0x6931e0;

static void push_rewind_actors(void)
{
    char base[0x110];
    level_base(LEVEL_NAME, base, sizeof base);
    const char *const *p = rewind_actors;
    while (*p && _stricmp(*p, base) != 0) {
        while (*p)
            p++;
        p++;
    }
    if (!*p)
        return;
    for (p++; *p; p++) {
        String nm = 0;
        ObjPtr inst = 0;
        str_ctor(&nm, *p, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        if (nm && --nm->refs == 0)
            eng_free(nm);
        if (!inst)
            continue;
        char *e = *(char **)((char *)inst + 0xc);
        inst_release(&inst);
        int net = e ? *(int *)(e + 0x18) : -1;
        if (!e || IsBadReadPtr(e, 0x30) || !(*(int *)(e + 0x2c) & 1) || net == -1 || net == -2 || net == -3)
            continue;
        __asm__ volatile("call *%[fn]"
                         : "+S"(e)
                         : [fn] "m"(fn_net_update)
                         : "eax", "ecx", "edx", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
    }
}

/* Rewound animations on clients. The rewind actors are animated actors: their clip and time are not in the network
 * update (a client's copy runs the clips the replayed story starts), so a client's crane stayed where it was after
 * the host's rewind (live, level 5). The host sends each rewind actor's clip and time every 0.1 s of a playback
 * (hold: the client shows that frame and stops the clip) and three times after it (the clip runs on from there when
 * it runs on the host). The client sets them through the actor's own script method Start(clip, time[, cycle]) and
 * Stop() (live: Start("FORTH", 1.0) on a client set the clip time, Stop() held it).
 * Engine facts: Start (0x6a9fb0, Kran_actor vtable +0x204) = clip index 0x952a60(node, name) -> 0x6aaea0 (entity
 * +0x280: bit 0 channel 0 runs, bit 4 its cycle flag); the clip running on channel 0 = 0x951660(node), its time =
 * 0x951500(node), node = entity +0x230. The clip names are the ones the level scripts start. */
#define SUB_ANIM 83                          /* server -> client {u8 1, u8 actor, u8 clip, u8 flags (1 runs, 2 cycle, 4 hold), f32 time} */
static const char *const actor_clips[] = {
    "Kran_actor", "FORTH", "BACK", NULL,
    "Conveyer_actor", "LEFT", "RIGHT", NULL,
    "bridge_puzle_actor", "DOWN", "UP", "IDLE", NULL,
    "Lift_actor", "DOWN", "UP", "IDLE", NULL,
    "Actor_train", "MOVE", "IDLE", NULL,
    "Actor_ghost_train", "MOVE", "IDLE", NULL,
    "cage1_actor", "FALL", "IDLE", NULL,
    "cage2_actor", "FALL", "IDLE", NULL,
    NULL};
#define MAX_ANIM_ACTORS 4
static int (TSCALL *node_clip_index)(void *, const char *) = (void *)0x952a60;
static int (TSCALL *node_clip)(void *) = (void *)0x951660;
static float (TSCALL *node_clip_time)(void *) = (void *)0x951500;
static DWORD anim_last, anim_final_at;
static int anim_finals;
static struct { int set, clip, flags; float t; } anim_in[MAX_ANIM_ACTORS];   /* (client) the latest per actor */

static const char *const *clips_of(const char *actor)
{
    for (const char *const *p = actor_clips; *p; p++) {
        if (!_stricmp(*p, actor))
            return p + 1;
        while (*p)
            p++;
    }
    return NULL;
}

/* The running level's rewind actors (NULL-terminated), or NULL. */
static const char *const *level_actors(void)
{
    char base[0x110];
    level_base(LEVEL_NAME, base, sizeof base);
    const char *const *p = rewind_actors;
    while (*p && _stricmp(*p, base) != 0) {
        while (*p)
            p++;
        p++;
    }
    return *p ? p + 1 : NULL;
}

static char *named_ent(const char *name)
{
    String nm = 0;
    ObjPtr inst = 0;
    str_ctor(&nm, name, -1, 0);
    mgr_find_obj(MGR, &inst, &nm);
    if (nm && --nm->refs == 0)
        eng_free(nm);
    if (!inst)
        return NULL;
    char *e = *(char **)((char *)inst + 0xc);
    inst_release(&inst);
    return e && !IsBadReadPtr(e, 0x284) ? e : NULL;
}

static void send_anims(int hold)
{
    const char *const *a = level_actors();
    for (int k = 0; a && a[k] && k < MAX_ANIM_ACTORS; k++) {
        char *e = named_ent(a[k]), *node = e ? *(char **)(e + 0x230) : NULL;
        const char *const *c = clips_of(a[k]);
        if (!node || IsBadReadPtr(node, 0x88) || !c)
            continue;
        int id = node_clip(node), ci = -1;
        for (int j = 0; id >= 0 && c[j]; j++)
            if (node_clip_index(node, c[j]) == id) {
                ci = j;
                break;
            }
        if (ci < 0)
            continue;
        unsigned fl = *(unsigned *)(e + 0x280);
        float t = node_clip_time(node);
        unsigned char b[8] = {PROTO, (unsigned char)k, (unsigned char)ci,
                              (unsigned char)((fl & 1 ? 1 : 0) | (fl & 0x10 ? 2 : 0) | (hold ? 4 : 0))};
        memcpy(b + 4, &t, 4);
        coopnet_send(-1, SUB_ANIM, b, sizeof b);
        if (!hold)
            VLOG("coopreverse: %s clip %s at %.2f s (%s) sent", a[k], c[ci], (double)t, fl & 1 ? "runs" : "stopped");
    }
}

/* (client, script tick) */
static void apply_anims(void)
{
    const char *const *a = level_actors();
    for (int k = 0; a && k < MAX_ANIM_ACTORS; k++) {
        if (!anim_in[k].set)
            continue;
        anim_in[k].set = 0;
        int n = 0;
        while (a[n])
            n++;
        const char *const *c = k < n ? clips_of(a[k]) : NULL;
        int nc = 0;
        while (c && c[nc])
            nc++;
        if (!c || anim_in[k].clip >= nc)
            continue;
        char stmt[160], res[120];
        int fl = anim_in[k].flags;
        snprintf(stmt, sizeof stmt, "$%s.Start(\"%s\",%.3f%s)", a[k], c[anim_in[k].clip], (double)anim_in[k].t,
                 fl & 2 ? ",1" : "");
        ts_script_run(stmt, res, sizeof res);
        if ((fl & 4) || !(fl & 1)) {
            snprintf(stmt, sizeof stmt, "$%s.Stop()", a[k]);
            ts_script_run(stmt, res, sizeof res);
        }
        if (!(fl & 4))
            VLOG("coopreverse: %s set to clip %s at %.2f s (%s) -> %s", a[k], c[anim_in[k].clip], (double)anim_in[k].t,
                 fl & 1 ? "runs" : "stopped", res);
    }
}

/* The host's own record of the rewind actors' animation (clip, time, running; 10 Hz, 33 s). Live 2026-10-27: the
 * engine's playback put the crane back but left the lift of level 10 (and the bridge of 9) at their end pose on the
 * host too, so nothing could reach the clients. During a playback the host therefore steps these actors back itself:
 * the engine's snapshot counter (REC_FRAMES) tells how much history it has played, the sample that far back is
 * set through Start(clip, time) + Stop() (held), and when the playback ends the last sample goes back in with its real
 * running state. The clients follow through message 83 as before. TS_COOP_REWIND_ANIMS=0 switches it off. */
#define RING 330
typedef struct { signed char ci; unsigned char fl; float t; } Samp;
static Samp ring[MAX_ANIM_ACTORS][RING];
static Samp ring_last[MAX_ANIM_ACTORS];
static int ring_n, ring_head, ring_frames0, use_ring = 1;
static DWORD ring_at, ring_log;
static char ring_level[0x110];

static int sample_actor(const char *name, Samp *s)
{
    char *e = named_ent(name), *node = e ? *(char **)(e + 0x230) : NULL;
    const char *const *c = clips_of(name);
    if (!node || IsBadReadPtr(node, 0x88) || !c)
        return 0;
    int id = node_clip(node), ci = -1;
    for (int j = 0; id >= 0 && c[j]; j++)
        if (node_clip_index(node, c[j]) == id) {
            ci = j;
            break;
        }
    if (ci < 0)
        return 0;
    unsigned fl = *(unsigned *)(e + 0x280);
    s->ci = (signed char)ci;
    s->fl = (unsigned char)((fl & 1 ? 1 : 0) | (fl & 0x10 ? 2 : 0));
    s->t = node_clip_time(node);
    return 1;
}

static void ring_sample(DWORD now)   /* (host script tick, not during a playback) */
{
    if (!use_ring || now - ring_at < 100)
        return;
    ring_at = now;
    if (strcmp(ring_level, LEVEL_NAME) != 0) {
        snprintf(ring_level, sizeof ring_level, "%s", LEVEL_NAME);
        ring_n = 0;
    }
    const char *const *a = level_actors();
    for (int k = 0; a && a[k] && k < MAX_ANIM_ACTORS; k++) {
        Samp s = {-1, 0, 0.f};
        sample_actor(a[k], &s);
        ring[k][ring_n % RING] = s;
    }
    ring_n++;
}

static void ring_begin(void)
{
    ring_head = ring_n - 1;
    ring_frames0 = *(int *)(REC + REC_FRAMES);
    for (int k = 0; k < MAX_ANIM_ACTORS; k++)
        ring_last[k].ci = -2;
}

static void ring_set(const char *name, const Samp *s, int hold)
{
    const char *const *c = clips_of(name);
    char stmt[160], res[120];
    if (!c || s->ci < 0)
        return;
    snprintf(stmt, sizeof stmt, "$%s.Start(\"%s\",%.3f%s)", name, c[s->ci], (double)s->t, s->fl & 2 ? ",1" : "");
    ts_script_run(stmt, res, sizeof res);
    if (hold || !(s->fl & 1)) {
        snprintf(stmt, sizeof stmt, "$%s.Stop()", name);
        ts_script_run(stmt, res, sizeof res);
    }
}

static void ring_play(void)   /* (recorder tick, host, during a playback) */
{
    if (!use_ring || ring_n <= 0)
        return;
    int frames = *(int *)(REC + REC_FRAMES), consumed = ring_frames0 - frames, lo = ring_n > RING ? ring_n - RING : 0;
    int idx = ring_head - (consumed > 0 ? consumed : 0);
    if (idx < lo)
        idx = lo;
    if (idx < 0)
        return;
    const char *const *a = level_actors();
    for (int k = 0; a && a[k] && k < MAX_ANIM_ACTORS; k++) {
        Samp s = ring[k][idx % RING];
        if (s.ci < 0 || (ring_last[k].ci == s.ci && fabsf(ring_last[k].t - s.t) < 0.04f))
            continue;
        ring_last[k] = s;
        ring_set(a[k], &s, 1);
    }
    if (verbose && GetTickCount() - ring_log > 1000) {
        ring_log = GetTickCount();
        RLOG("coopreverse: playback: %d snapshots left of %d, sample %d of %d (head %d)", frames, ring_frames0, idx, ring_n,
             ring_head);
    }
}

static void ring_finish(void)   /* (host, the playback ended: the last sample goes back with its real running state) */
{
    const char *const *a = level_actors();
    for (int k = 0; use_ring && a && a[k] && k < MAX_ANIM_ACTORS; k++)
        if (ring_last[k].ci >= 0)
            ring_set(a[k], &ring_last[k], 0);
    for (int k = 0; k < MAX_ANIM_ACTORS; k++)
        ring_last[k].ci = -2;
    ring_n = 0;   /* (the history before the rewind is gone, as the engine's) */
}

/* The recorder task's per-frame update (0x584960, its vtable +0x138) keeps running during a playback, when the
 * script tick (this file's frame) does not: the network updates of the rewind actors and the end of a client's reverse
 * happen here. */
static void host_stop(const char *why);
__attribute__((used)) void crw_tick(void) __asm__("crw_tick");
__attribute__((used)) void crw_tick(void)
{
    if (!stock || !on || !stock_run || game_mode() != MODE_SERVER)
        return;
    if (playing()) {
        ring_play();
        push_rewind_actors();
        if (GetTickCount() - anim_last >= 100) {
            anim_last = GetTickCount();
            send_anims(1);
        }
    }
    if (stock_ours && (int)(GetTickCount() - until) >= 0)
        host_stop("time up");
}
extern void crw_tick_thunk(void) __asm__("crw_tick_thunk");
__asm__(".text\n"
        ".globl crw_tick_thunk\n"
        "crw_tick_thunk:\n"
        "  pushl %eax\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  call crw_tick\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  popl %eax\n"
        "  movl 0xfcace8, %eax\n"        /* the 5 bytes the call replaced */
        "  ret\n");

/* The reverse key is offered only while the recorder holds at least the controller's minimum (0x43edd0: frames x
 * 0.1 s >= controller +0x48). A client never records (and a host without the stock rewind neither): the check reads
 * a fake frame count there, so the key reaches the request path. Returns the recorder pointer for `mov edx, [0xfc8400]`
 * (the fake one as "address of the count - 0x700ad0"). */
static int crw_fake_frames = 100000;
__attribute__((used)) char *crw_avail(void) __asm__("crw_avail");
__attribute__((used)) char *crw_avail(void)
{
    if (enabled && active() && (game_mode() == MODE_CLIENT || !stock_ready()))
        return (char *)&crw_fake_frames - REC_FRAMES;
    return REC;
}
extern void crw_avail_thunk(void) __asm__("crw_avail_thunk");
__asm__(".text\n"
        ".globl crw_avail_thunk\n"
        "crw_avail_thunk:\n"
        "  pushl %eax\n"
        "  pushl %ecx\n"
        "  call crw_avail\n"
        "  movl %eax, %edx\n"
        "  popl %ecx\n"
        "  popl %eax\n"
        "  ret\n");

/* Live (N100 pair, levels 5 and 7): with the host recording, a client that loaded the level crashed while it created
 * its player's weapons from the host's join data (0x4478e0 -> 0x655460 gave NULL, AV at 0x447a5c); without the
 * recording it did not. The host therefore stops recording while any client has not started running the level, and
 * starts it again 3 s after the last one runs it (recording = [0xfcace8] 0x200000; 0x584940 stops it the same way;
 * 0x5846b0, ecx = the recorder, starts it). */
#define SLOT_INGAME 1
static int rec_paused;
static DWORD rec_resume_at, rec_retry_at;
static void rec_pause_step(void)
{
    char *r = REC;
    if (!stock || game_mode() != MODE_SERVER || !ts_coop_active() || !r || playing())
        return;
    int loading = 0;
    for (int i = 0; slot_at(i); i++) {
        char *s = slot_at(i);
        if (*(int *)s != 0 && !(*(int *)(s + 8) & SLOT_INGAME))
            loading = 1;
    }
    DWORD now = GetTickCount();
    if (loading) {
        rec_resume_at = now + 3000;
        if (!rec_paused) {
            rec_paused = 1;
            GAME_FLAGS &= ~0x200000u;
            *(int *)(r + REC_FLAGS) |= 0x10;
            RLOG("coopreverse: recording paused (a client loads the level)");
        }
    } else if (rec_paused && (int)(now - rec_resume_at) >= 0) {
        rec_paused = 0;
        *(int *)(r + REC_FLAGS) &= ~0x10;
        __asm__ volatile("call *%[fn]"
                         : "+c"(r)
                         : [fn] "r"(0x5846b0)
                         : "eax", "edx", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
        RLOG("coopreverse: recording on again (%s)", (GAME_FLAGS & 0x200000u) ? "recording" : "not started");
    } else if (!rec_paused && !(GAME_FLAGS & 0x200000u) && (GAME_FLAGS & 0x80000000u) && now - rec_retry_at >= 2000) {
        /* (live, level 19: the start 3 s after the client's load was refused - the level opens with a film - and the
         * recording never began, so no rewind ran: ask again every 2 s until the engine accepts) */
        static int tries;
        rec_retry_at = now;
        __asm__ volatile("call *%[fn]"
                         : "+c"(r)
                         : [fn] "r"(0x5846b0)
                         : "eax", "edx", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
        if ((GAME_FLAGS & 0x200000u) || ++tries <= 3)
            RLOG("coopreverse: recording asked for again (%s)", (GAME_FLAGS & 0x200000u) ? "recording" : "not started");
        if (GAME_FLAGS & 0x200000u)
            tries = 0;
    }
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
    if (stock_ready()) {   /* the engine's own reverse, started for a client: the host's world rewinds */
        int a = MODE_REVERSE;
        char *pl = PLAYER;
        __asm__ volatile("pushl %[p]\n\tcall *%[fn]"
                         : "+a"(a)
                         : [p] "r"(pl), [fn] "m"(fn_rev_start)
                         : "ecx", "edx", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
        if (!playing()) {
            RLOG("coopreverse: %s: the engine did not start its playback (%d snapshots)", why, *(int *)(REC + REC_FRAMES));
            return;
        }
        stock_run = stock_ours = 1;
        stock_since = GetTickCount();
        ring_begin();
    }
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
    int was_stock = stock_run;
    if (stock_run && stock_ours && playing())
        rev_end(PLAYER, MODE_REVERSE, 0);   /* (ends the engine's playback) */
    apply(0, 0);   /* (with stock_run still set: the engine fires the host's OnStopTReverse itself) */
    if (was_stock) {
        ring_finish();
        trap_until = GetTickCount() + 1500;
        anim_final_at = GetTickCount() + 200;
        anim_finals = 3;
        stock_run = stock_ours = 0;
    }
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
    if (start && PLAYER && rev_cost > 0.f && !stock_ready()) {   /* the owner pays at once (the engine charges the host's own) */
        float *e = (float *)(PLAYER + ENERGY);
        *e = *e - secs * rev_cost < 0.f ? 0.f : *e - secs * rev_cost;
    }
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
    if (!active() || len < 1 || b[0] != PROTO || (sub != SUB_TRAP && len < 3))
        return;
    if (sub == SUB_REQ && game_mode() == MODE_SERVER) {
        if (b[1])
            host_start(peer, b[2] / 10.f, "client's reverse key");
        else if (owner == peer)
            host_stop("client stopped (energy)");
    } else if (sub == SUB_STATE && game_mode() == MODE_CLIENT && len >= 8)
        apply(b[1] != 0, b[2] != 0);
    else if (sub == SUB_TRAP && game_mode() == MODE_CLIENT)
        trap_kill = 1;   /* (run in the script tick) */
    else if (sub == SUB_ANIM && game_mode() == MODE_CLIENT && len >= 8 && b[1] < MAX_ANIM_ACTORS) {
        float t;
        memcpy(&t, b + 4, 4);
        anim_in[b[1]].clip = b[2];
        anim_in[b[1]].flags = b[3];
        anim_in[b[1]].t = t;
        anim_in[b[1]].set = 1;   /* (run in the script tick) */
    }
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
    if (stock_ready())
        return 0;   /* (the host's own key: the engine's reverse with the recorder; the frame announces it) */
    request(1, affordable(pl));
    return 1;
}

/* The power start gate (timesync.c's tsync_may_start, 0x43e1b0: the reverse key's usual way) for mode 2. */
int coopreverse_key(void)
{
    if (!active() || !PLAYER)
        return -1;
    int r = crev_key(PLAYER) ? 0 : 1;
    VLOG("coopreverse: reverse start of the local player: %s", r ? "the engine's reverse" : "handled here");
    return r;
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
    if (trap_kill) {
        char res[120];
        trap_kill = 0;
        /* (a client dies the multiplayer way: $player.Kill() there left it "waiting for respawn" for good, seen live;
         * FailMission on a client is a multiplayer death with respawn, multiplayer.md T5) */
        ts_script_run(game_mode() == MODE_CLIENT ? "$player.FailMission(\"\")" : "$player.Kill()", res, sizeof res);
        RLOG("coopreverse: trapped in a container after the host's rewind: killed -> %s", res);
    }
    if (game_mode() == MODE_CLIENT)
        apply_anims();
    if (game_mode() == MODE_SERVER && anim_finals > 0 && (int)(now - anim_final_at) >= 0) {
        send_anims(0);   /* (after a rewind: 0.2, 0.6 and 1.5 s after its end) */
        anim_final_at = now + (anim_finals == 3 ? 400 : 900);
        anim_finals--;
    }
    if (stock && game_mode() == MODE_SERVER) {
        rec_pause_step();
        if (!on && playing() && !timesync_busy()) {   /* the host's own key started the engine's reverse */
            stock_run = 1;
            stock_ours = 0;
            stock_since = now;
            ring_begin();
            owner = 0;
            until = now + 60000;   /* (the engine ends it) */
            apply(1, 0);
            broadcast();
            RLOG("coopreverse: the host's reverse (engine playback, %d snapshots)", *(int *)(REC + REC_FRAMES));
        } else if (on && stock_run && !playing() && now - stock_since > 300)
            host_stop("engine playback ended");
        if (on && stock_run && playing())
            push_rewind_actors();
        if (!stock_run && !playing())
            ring_sample(now);
        if (trap_until && (int)(now - trap_until) < 0)
            trap_check();
        else
            trap_until = 0;
    }
    if (on && strcmp(on_level, LEVEL_NAME) != 0) {   /* a level change while a reverse ran */
        on = yours = 0;
        owner = NONE;
        fx_stop();
    }
    (void)drain_last;   /* (the owner pays when it asks: request(); the engine's regeneration outran a per-frame drain) */
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
    if (!strcmp(cmd, "trapcheck")) {   /* (lab) the container trap check of the end of a rewind, now */
        trap_check();
        return snprintf(out, size, "trap check run"), 1;
    }
    if (!strcmp(cmd, "anims") && game_mode() == MODE_SERVER) {   /* (lab) send the rewind actors' clips now */
        send_anims(0);
        return snprintf(out, size, "rewind actors' clips sent"), 1;
    }
    if (!strcmp(cmd, "stop") && active()) {
        request(0, 0.f);
        return snprintf(out, size, "reverse stop asked"), 1;
    }
    snprintf(out, size, "coopreverse %s, active %d, on %d, yours %d, owner %d, %.1f s at %.2f energy/s, energy %.2f;"
             " stock rewind %d (ready %d, running %d, recorder %p, serializer %p, snapshots %d, flags %x, playback %d, game flags %x/%x)",
             enabled ? "enabled" : "off", active(), on, yours, owner, (double)rev_seconds, (double)rev_cost,
             PLAYER ? (double)*(float *)(PLAYER + ENERGY) : -1.0, stock, stock_ready(), stock_run, (void *)REC,
             (void *)TR_SAVE, REC ? *(int *)(REC + REC_FRAMES) : -1, REC ? *(int *)(REC + REC_FLAGS) : 0, playing(), (unsigned)GAME_FLAGS,
             (unsigned)*(volatile unsigned *)0xfcace0);
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
    {   /* 0x43edda: mov edx, [0xfc8400] (6 bytes) -> call crw_avail_thunk; nop */
        static const unsigned char want_av[6] = {0x8b, 0x15, 0x00, 0x84, 0xfc, 0x00};
        unsigned char r[6] = {0xe8, 0, 0, 0, 0, 0x90};
        *(DWORD *)(r + 1) = (DWORD)crw_avail_thunk - (0x43edda + 5);
        if (!ts_patch_bytes(0x43edda, want_av, r, 6))
            ts_log("coopreverse: reverse availability check not found: the key stays off where nothing is recorded");
    }
    use_ring = !(ts_env("TS_COOP_REWIND_ANIMS", v, sizeof v) && v[0] == '0');
    if (ts_env("TS_COOP_REWIND", v, sizeof v) && v[0] == '1') {
        /* the recorder code's game-mode reads (mov eax/ecx, [0xfcab34]) -> "single player" on a co-op host */
        static const DWORD sites_eax[] = {0x583912, 0x583952, 0x58398c, 0x5839d6, 0x584679, 0x5846b0, 0x585b97};
        static const DWORD sites_ecx[] = {0x583a61, 0x5840b0};
        static const unsigned char want_eax[5] = {0xa1, 0x34, 0xab, 0xfc, 0x00};
        static const unsigned char want_ecx[6] = {0x8b, 0x0d, 0x34, 0xab, 0xfc, 0x00};
        int ok = 0, n = 0;
        crw_fake_gm[0x3c / 4] = 1;
        for (int i = 0; i < (int)(sizeof sites_eax / sizeof sites_eax[0]); i++, n++) {
            unsigned char r[5] = {0xe8};
            *(DWORD *)(r + 1) = (DWORD)crw_gm_eax - (sites_eax[i] + 5);
            ok += ts_patch_bytes(sites_eax[i], want_eax, r, 5);
        }
        for (int i = 0; i < (int)(sizeof sites_ecx / sizeof sites_ecx[0]); i++, n++) {
            unsigned char r[6] = {0xe8, 0, 0, 0, 0, 0x90};
            *(DWORD *)(r + 1) = (DWORD)crw_gm_ecx - (sites_ecx[i] + 5);
            ok += ts_patch_bytes(sites_ecx[i], want_ecx, r, 6);
        }
        unsigned char tr[5] = {0xe8};
        *(DWORD *)(tr + 1) = (DWORD)crw_tick_thunk - (0x584960 + 5);
        static const unsigned char want_tick[5] = {0xa1, 0xe8, 0xac, 0xfc, 0x00};
        ok += ts_patch_bytes(0x584960, want_tick, tr, 5);
        n++;
        stock = ok == n;
        ts_log("coopreverse: stock rewind on the host %s (%d/%d recorder sites)", stock ? "on" : "off", ok, n);
    }
    ts_log("coopreverse: on (%.1f s per reverse, %.2f energy/s, %d puzzle objects in the table)",
           (double)rev_seconds, (double)rev_cost, N_REV);
}
