/*
 * Whole-campaign co-op rules on top of storysync.c (patch 1.2 exe only). Live-tested on the N100 pair 2026-10-26
 * (mod/docs/campaign-audit.md section 3b: what was proven and what is still open). On by default in co-op sessions
 * (coopsession.c sets TS_CAMPAIGN=1 when it is not set; TS_CAMPAIGN=0 off); it acts only in a co-op session whose host
 * runs the story (storysync.c, COOP_F_STORYSYNC).
 *
 * storysync.c makes a story domain count as entered while ANY player is inside (the first player to arrive starts
 * the story). A few campaign domains need another rule in co-op; they are listed per level in `rules` below
 * (from the audit of the level scripts):
 * - ALL  ("all players" gates): the domain counts as entered only while the host's player AND every joined
 *        client's body are inside. Lifts that leave when the player is inside (ts2_skyscraper_part4
 *        dom_lift__move_up, ts2_guardfactory_part04/05 dom_act_lift) and train starts that lock the player on
 *        board (ts2_zep_factory_part4, ts2_zepride2_part01 dom_train_start) wait for everybody instead of
 *        stranding the others. While the host waits inside, the missing clients are pulled in after
 *        TS_CAMPAIGN_GATHER seconds (default 20; 0 = never).
 * - HOST (the host's player only, the stock single-player test): the quad scanners of ts2_zepcrashsite_part01 and
 *        ts2_summeroutdoor_part01 (dom_scanner*), whose checker fails the mission when no quad is inside. With
 *        the "any player" rule a client on foot in the zone failed the mission for everybody (remote players
 *        may not be able to drive a quad).
 * - KILL (per player): the death-scene domains (ts2_guardfactory_part01 and ts2_zep_factory_part3
 *        dom_RIP_cine_start*). Their cinematic ends with $player.Kill(): with storysync a client falling in
 *        killed the HOST and every machine watched the death camera. In co-op the domain never fires its story;
 *        the player who is inside dies (multiplayer respawn at the last checkpoint), nobody else.
 *
 * How: storysync.c's domain test hook (ss_dom_count, host) asks campaign_dom_verdict() first; -1 = no rule here.
 * The per-frame part (host) handles the gather timer and the KILL domains, only for domains whose verdict
 * storysync has asked for in this level (proof that its hook is live).
 *
 * Messages (coopnet.c, reliable, subtypes 70-79):
 *   70 server -> one client  {u8 1, u8 ring, f32 x, y, z, u8 has_box, [f32 min x y z, max x y z]}
 *                            "move your player here" (gather / regroup; ring = the client's place around the point)
 *   71 server -> one client  {u8 1}  "your player dies" (KILL domains)
 *   72 server -> one client  {u8 1, u8 mask, u8 move, input, aim, tc slow, tc stop, tc reverse, i8 weapon x3, 0}
 *                            a late joiner gets the host's player state (mask: 1 move, 2 input, 4 aim, 8/16/32 the
 *                            three DisableTC modes, 64 SetupWeapon); only what the level's scripts set on the host
 *   73 server -> one client  {u8 1, u8 len, seat}          take this seat ($player.StartUse)
 *   74 server -> one client  {u8 1, u8 on, u8 place, u8 len, vehicle}   ride on / get off this vehicle
 * Client actions run in the script tick (frame), not in the network pump.
 *
 * More rules (round 2, also not yet run):
 * - Seats and rides (story events of the host, storysync.c calls campaign_story_event): zeppelin turret rides
 *   (ts2_zepride1_part01 intro and checkpoint restores, ts2_zepride2_part01 intro): each client takes a turret seat
 *   of the AI gunner the host removes (table seat_rules); armoured car (ts2_zep_factory_part1): the clients ride on
 *   it from actor_start_chase to the end BR_pl_3 (their player placed above the car every frame, side by side,
 *   view kept; TS_CAMPAIGN_RIDE_HEIGHT, default 2.2 m).
 * - Force fields on clients: storysync's client scripts keep SetSpeed (the force-field/wind domains' setter) instead
 *   of wrapping it as an AI call (campaign_local_setspeed).
 * - Late joiners (host): the level scripts' DisableMove/DisableInput/Aimable/DisableTC/SetupWeapon on the host's own
 *   player are recorded (native hooks); a client that starts running the level more than 20 s after the host gets
 *   that state (message 72, 3, 10 and 25 s after; the loadout only the first time).
 * - AI targets (host): SetAim/SetTarget("player") of AI, turrets and the boss name the nearest player instead of the
 *   host; remote players get a script name for that (coop_player<slot>).
 *
 * Test console (TS_CONSOLE): "!cp state", "!cp regroup" (host: every client to the host; client: itself to the
 * host), "!cp gather <domain>" (host: the clients that are not inside, into it), "!cp rules".
 *
 * Switches: TS_CAMPAIGN=1 on (default off), TS_CAMPAIGN_RULES=0 no domain rules (console only),
 * TS_CAMPAIGN_GATHER=<s> gather delay (0 = no automatic gather), TS_CAMPAIGN_KILL=0 KILL domains keep the
 * storysync behaviour (the cinematic), TS_CAMPAIGN_SEATS=0, TS_CAMPAIGN_SETSPEED=0, TS_CAMPAIGN_LATEJOIN=0,
 * TS_CAMPAIGN_AI=0 (each part off), TS_CAMPAIGN_LOG=1 every decision in the log.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

/* ---- engine state (the same facts storysync.c uses) ---- */
#define GAME_MODE_OBJ (*(char **)0xfcab34)   /* +0x3c mode, +0x28 client slots, +0x2c slot count */
#define MODE_OFF 0x3c
#define SLOTS_OFF 0x28
#define NSLOTS_OFF 0x2c
#define SLOT_SIZE 0xa4                       /* +0 client id (0 = the server), +4 player entity, +8 flags */
#define SLOT_READY 0x80
#define SLOT_JOINED 0x100
#define MODE_SERVER 2
#define MODE_CLIENT 3
#define PLAYER (*(char **)0x1095b30)         /* $player */
#define PEER_NETWORK (*(void **)0x1096148)
#define LEVEL_NAME ((const char *)0xfcabc8)
#define MGR (*(void **)0xebedec)
#define ENT_INST 0x80                        /* entity: its script instance */
#define ENT_NODE 0x230                       /* entity: scene node (position) */
#define INST_NAME 4                          /* script instance: name (String) */
#define INST_NATIVE 0xc                      /* script instance: the C++ object */
#define DOM_UPDATE 0x6c2dd0                  /* every domain class's test (vtable +0x134) */
#define DOM_STATE 0xe8                       /* bit 0 = entered */
#define DOM_LOCKS 0x88                       /* > 0: locked (no enter/leave) */
#define DOM_DESC 0x78                        /* its descriptor; +0x78 bit 0 = "act once" (isActOnce) */
#define DESC_ACT_ONCE 0x78
#define DOM_MIN 0xc8                         /* bounds: min x y z, max x y z (storysync "!ss goto") */
#define DOM_MAX 0xd4
#define NODE_DIR 0x58
#define PLAYER_PLACE 0x210                   /* player vtable: place (pos, dir, 1) */
#define GAME_FLAGS (*(volatile unsigned *)0xfcace8)
#define FLAG_LEVEL_RUNNING 0x80000000u       /* (host: cleared by the map change, set when the level runs) */
#define SLOT_INGAME 1                        /* (slot flag: the client runs the level, coopnet.c) */

typedef struct { int refs, len, cap; char s[1]; } StrHdr;
typedef StrHdr *String;
typedef void *ObjPtr;

static void (__cdecl *eng_free)(void *) = (void *)0x7e4700;
static String *(TSCALL *str_ctor)(String *, const char *, int, int) = (void *)0x402490;
static ObjPtr *(TSCALL *mgr_find_obj)(void *, ObjPtr *, const String *) = (void *)0x8fc720;
static void (TSCALL *inst_release)(ObjPtr *) = (void *)0x8f8050;
static int (TSCALL *dom_is_belong)(void *, void *) = (void *)0x6c3d60;   /* bool in al */
static void (TSCALL *node_pos)(void *, float *) = (void *)0x94c8e0;

#define SUB_GATHER 70
#define SUB_KILL 71
#define SUB_STATE 72                         /* late joiner: the host's player locks and loadout */
#define SUB_SEAT 73                          /* take this seat */
#define SUB_RIDE 74                          /* ride on this vehicle (on/off) */
#define SUB_VEH 76                           /* vehicle transforms (either way, see veh_step) */
#define SUB_LAST 79
#define PROTO 1

/* ---- settings ---- */
static int enabled, use_rules = 1, use_kill = 1, verbose;
static int gather_after = 20;                /* s; 0 = no automatic gather */
static int use_seats = 1, use_setspeed = 1, use_latejoin = 1, use_ai = 1, use_routes = 1;
static float ride_height = 2.2f;             /* m above the ridden vehicle's origin */
static LONG n_logged;
#define CLOG(...) do { if (InterlockedIncrement(&n_logged) <= 1000) ts_log(__VA_ARGS__); } while (0)
#define VLOG(...) do { if (verbose) CLOG(__VA_ARGS__); } while (0)

/* ---- the rules (mod/docs/campaign-audit.md) ---- */
enum { R_ALL = 1, R_HOST, R_KILL };
typedef struct { const char *level, *domain; int rule; } Rule;
static const Rule rules[] = {
    {"ts2_skyscraper_part4", "dom_lift__move_up", R_ALL},
    {"ts2_guardfactory_part04", "dom_act_lift", R_ALL},
    {"ts2_guardfactory_part05", "dom_act_lift", R_ALL},
    {"ts2_zep_factory_part4", "dom_train_start", R_ALL},
    {"ts2_zepride2_part01", "dom_train_start", R_ALL},
    {"ts2_zepcrashsite_part01", "dom_scanner", R_HOST},
    {"ts2_zepcrashsite_part01", "dom_scanner_1", R_HOST},
    {"ts2_zepcrashsite_part01", "dom_scanner_2", R_HOST},
    {"ts2_summeroutdoor_part01", "dom_scanner_1", R_HOST},
    {"ts2_summeroutdoor_part01", "dom_scanner_2", R_HOST},
    {"ts2_summeroutdoor_part01", "dom_scanner_3", R_HOST},
    {"ts2_guardfactory_part01", "dom_RIP_cine_start", R_KILL},
    {"ts2_zep_factory_part3", "dom_RIP_cine_start", R_KILL},
    {"ts2_zep_factory_part3", "dom_RIP_cine_start1", R_KILL},
    {"ts2_zep_factory_part3", "dom_RIP_cine_start2", R_KILL},
};
#define N_RULES ((int)(sizeof rules / sizeof rules[0]))
static const char *const rule_names[] = {"", "all players", "host only", "kill per player"};

/* The rules of the running level. */
#define ACT_MAX 8
typedef struct {
    const Rule *r;
    int consulted;                           /* storysync asked for this domain's verdict in this level */
    DWORD wait_since;                        /* (ALL) the host inside, somebody missing, since */
    DWORD killed_at[16];                     /* (KILL) per slot: last kill sent (0 = the host's player) */
    unsigned char in_dom[16];                /* (KILL) per slot: inside since the last kill (a dead body stays inside;
                                                killing it again restarted its respawn wait, seen live) */
} Active;
static Active act[ACT_MAX];
static int n_act;
static char act_level[0x110];

static void str_release(String s)
{
    if (s && --s->refs == 0)
        eng_free(s);
}

static int game_mode(void)
{
    char *gm = GAME_MODE_OBJ;
    return gm ? *(int *)(gm + MODE_OFF) : 0;
}

/* MODE_SERVER / MODE_CLIENT in a co-op session whose host runs the story, else 0. */
static int role(void)
{
    int m = game_mode();
    if (!enabled || (m != MODE_SERVER && m != MODE_CLIENT) || !PEER_NETWORK)
        return 0;
    return ts_coop_feature(COOP_F_STORYSYNC) ? m : 0;
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

static const char *ent_name(char *ent)
{
    void *inst = ent ? *(void **)(ent + ENT_INST) : NULL;
    String s = inst ? *(String *)((char *)inst + INST_NAME) : NULL;
    return s ? s->s : "";
}

static int is_domain(char *ent)
{
    return ent && *(void ***)ent && (*(void ***)ent)[0x134 / 4] == (void *)DOM_UPDATE;
}

static int ent_pos(char *ent, float *pos)
{
    if (!ent || IsBadReadPtr(ent, ENT_NODE + 4) || !*(void **)(ent + ENT_NODE)
        || IsBadReadPtr(*(void **)(ent + ENT_NODE), 0x74))
        return 0;
    node_pos(*(void **)(ent + ENT_NODE), pos);
    return 1;
}

static int inside(char *dom, char *ent)
{
    return ent && (dom_is_belong(dom, ent) & 0xff);
}

/* A named level object's entity, or NULL (the reference is released at once: valid for this tick). */
static char *find_ent(const char *name)
{
    String nm = 0;
    ObjPtr inst = 0;
    str_ctor(&nm, name, -1, 0);
    mgr_find_obj(MGR, &inst, &nm);
    str_release(nm);
    char *ent = inst ? *(char **)((char *)inst + INST_NATIVE) : NULL;
    if (inst)
        inst_release(&inst);
    return ent;
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

/* (re)build the rules of the running level when its name changed (also a team restart: same name, new objects;
 * the rules hold names only, the per-domain state is cleared through level_seen below) */
static void sync_level(void)
{
    if (strncmp(act_level, LEVEL_NAME, sizeof act_level - 1) == 0)
        return;
    snprintf(act_level, sizeof act_level, "%s", LEVEL_NAME);
    char base[0x110];
    level_base(LEVEL_NAME, base, sizeof base);
    memset(act, 0, sizeof act);
    n_act = 0;
    for (int i = 0; use_rules && i < N_RULES && n_act < ACT_MAX; i++)
        if (_stricmp(rules[i].level, base) == 0)
            act[n_act++].r = &rules[i];
    if (n_act)
        CLOG("campaign: %s: %d co-op domain rule(s)", base, n_act);
}

static Active *active_for(const char *name)
{
    for (int i = 0; i < n_act; i++)
        if (strcmp(act[i].r->domain, name) == 0)
            return &act[i];
    return NULL;
}

/* Every joined client's body is inside (a level without clients: true). */
static int clients_inside(char *dom, int *missing)
{
    int all = 1;
    if (missing)
        *missing = 0;
    for (int i = 0;; i++) {
        char *s = slot_at(i);
        if (!s)
            break;
        char *ent = *(char **)(s + 4);
        if (*(int *)s == 0 || !slot_joined(s) || !ent)
            continue;
        if (!inside(dom, ent)) {
            all = 0;
            if (missing)
                (*missing)++;
        }
    }
    return all;
}

/* storysync.c (host, domain test of a domain the players' shared object is watched by): -1 = no co-op rule for
 * this domain, else 0 = outside / 1 = inside. */
int campaign_dom_verdict(void *dom_, int host_inside)
{
    char *dom = dom_;
    if (role() != MODE_SERVER || !dom)
        return -1;
    sync_level();
    if (!n_act)
        return -1;
    Active *a = active_for(ent_name(dom));
    if (!a)
        return -1;
    if (!a->consulted) {
        a->consulted = 1;
        VLOG("campaign: %s: rule \"%s\" in effect", a->r->domain, rule_names[a->r->rule]);
    }
    switch (a->r->rule) {
    case R_ALL:
        /* (the host by the domain's point test or by its body: live, level 22, a host standing on the train had its
         * body inside but no test point, and the gate never opened) */
        return (host_inside || inside(dom, PLAYER)) && clients_inside(dom, NULL);
    case R_HOST:
        return host_inside;
    case R_KILL:
        return use_kill ? 0 : -1;
    }
    return -1;
}

/* ---- gather: a client moves its own player (movement is owned by each machine) ---- */
static void send_gather(int slot, int ring, const float *pos, const float *mn, const float *mx)
{
    unsigned char b[64];
    int n = 0;
    b[n++] = PROTO;
    b[n++] = (unsigned char)ring;
    memcpy(b + n, pos, 12);
    n += 12;
    b[n++] = mn && mx;
    if (mn && mx) {
        memcpy(b + n, mn, 12);
        n += 12;
        memcpy(b + n, mx, 12);
        n += 12;
    }
    coopnet_send(slot, SUB_GATHER, b, n);
}

/* The domain's middle at its floor (as storysync's "!ss goto"). */
static void dom_point(char *dom, float *pos, float *mn, float *mx)
{
    memcpy(mn, dom + DOM_MIN, 12);
    memcpy(mx, dom + DOM_MAX, 12);
    pos[0] = (mn[0] + mx[0]) / 2;
    pos[1] = mn[1] + 0.1f;
    pos[2] = (mn[2] + mx[2]) / 2;
}

/* (host) the clients whose bodies are not inside `dom` (or every client, dom NULL: to the host's player). */
static int gather(char *dom)
{
    float pos[3], mn[3], mx[3];
    if (dom)
        dom_point(dom, pos, mn, mx);
    else if (!ent_pos(PLAYER, pos))
        return 0;
    int n = 0;
    for (int i = 0;; i++) {
        char *s = slot_at(i);
        if (!s)
            break;
        char *ent = *(char **)(s + 4);
        if (*(int *)s == 0 || !slot_joined(s))
            continue;
        if (dom && ent && inside(dom, ent))
            continue;
        send_gather(i, n + 1, pos, dom ? mn : NULL, dom ? mx : NULL);
        n++;
    }
    CLOG("campaign: %d client(s) sent to %s (%.1f %.1f %.1f)", n, dom ? ent_name(dom) : "the host", pos[0], pos[1], pos[2]);
    return n;
}

/* (client) put this machine's player at pos, `ring` places around it (0.7 m apart), inside the box if given. */
static void place_local(const float *pos, int ring, const float *mn, const float *mx)
{
    char *pl = PLAYER;
    char *node = pl ? *(char **)(pl + ENT_NODE) : NULL;
    if (!node)
        return;
    float at[3] = {pos[0], pos[1] + 0.3f, pos[2]};
    if (ring > 0) {
        float ang = (float)ring * 1.5708f, rad = 0.7f * (float)((ring + 3) / 4);
        at[0] += rad * cosf(ang);
        at[2] += rad * sinf(ang);
    }
    if (mn && mx)
        for (int k = 0; k < 3; k += 2) {
            float lo = mn[k] + 0.3f, hi = mx[k] - 0.3f;
            if (lo > hi)
                lo = hi = (mn[k] + mx[k]) / 2;
            at[k] = at[k] < lo ? lo : at[k] > hi ? hi : at[k];
        }
    float dir[3] = {*(float *)(node + NODE_DIR), *(float *)(node + NODE_DIR + 4), *(float *)(node + NODE_DIR + 8)};
    ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)pl)[PLAYER_PLACE / 4])(pl, at, dir, 1);
    CLOG("campaign: player moved to %.1f %.1f %.1f (place %d)", at[0], at[1], at[2], ring);
}

static void kill_local(const char *why)
{
    char res[160];
    /* (a client dies the multiplayer way: $player.Kill() there left it "waiting for respawn" for good, seen live;
     * FailMission on a client is a multiplayer death with respawn, multiplayer.md T5) */
    ts_script_run(game_mode() == MODE_CLIENT ? "$player.FailMission(\"\")" : "$player.Kill()", res, sizeof res);
    CLOG("campaign: own player killed (%s) -> %s", why, res);
}

/* ---- client actions sent by the host; run in the script tick (frame), never in the network pump ---- */
static struct {
    int gather, kill, state, seat;
    float pos[3], mn[3], mx[3];
    int ring, box;
    unsigned char st[16];
    char seat_name[96];
    DWORD seat_at;
} pend;
static char ride_vehicle[64];                /* (client) the vehicle this player rides on, "" = none */
static float ride_host[5];                   /* (client) the host's place of it (x y z, forward x z) */
static DWORD ride_host_at;
static int ride_place;
static int ride_rel;                         /* (client) 1 = keep the place relative to the vehicle taken at the start */
static float ride_off[3];                    /* (client) that place in the vehicle's frame (right, up, forward) */

static void veh_receive(const unsigned char *b, int len);
static void on_message(int peer, unsigned short sub, const unsigned char *b, int len)
{
    (void)peer;
    if (sub == SUB_VEH && enabled && len >= 2 && b[0] == PROTO && (role() == MODE_CLIENT || role() == MODE_SERVER)) {
        veh_receive(b, len);
        return;
    }
    if (role() != MODE_CLIENT || len < 1 || b[0] != PROTO)
        return;
    if (sub == SUB_GATHER && len >= 15) {
        memcpy(pend.pos, b + 2, 12);
        pend.ring = b[1];
        pend.box = b[14] && len >= 39;
        if (pend.box) {
            memcpy(pend.mn, b + 15, 12);
            memcpy(pend.mx, b + 27, 12);
        }
        pend.gather = 1;
    } else if (sub == SUB_KILL)
        pend.kill = 1;
    else if (sub == SUB_STATE && len >= 12) {
        memcpy(pend.st, b, len < 16 ? len : 16);
        pend.state = 1;
    } else if (sub == SUB_SEAT && len >= 3 && b[1] < sizeof pend.seat_name && 2 + b[1] <= len) {
        memcpy(pend.seat_name, b + 2, b[1]);
        pend.seat_name[b[1]] = 0;
        pend.seat = 1;
        pend.seat_at = GetTickCount() + 500;   /* (after the replay of the same story event) */
    } else if (sub == 75 && len >= 21) {
        memcpy(ride_host, b + 1, 20);
        ride_host_at = GetTickCount();
    } else if (sub == SUB_RIDE && len >= 4 && b[3] < sizeof ride_vehicle && 4 + b[3] <= len) {
        if (b[1]) {
            memcpy(ride_vehicle, b + 4, b[3]);
            ride_vehicle[b[3]] = 0;
            ride_place = b[2];
            ride_rel = -1;   /* (decided at the first step: near the vehicle -> keep that place, else on top) */
            CLOG("campaign: riding on %s (place %d)", ride_vehicle, ride_place);
        } else {
            CLOG("campaign: ride on %s ended", ride_vehicle);
            ride_vehicle[0] = 0;
        }
    }
}

/* (client) apply the host's lock and loadout state (a client that joined a running level). */
static void apply_state(const unsigned char *s)
{
    static const char *const tc[3] = {"time_slow", "time_stop", "time_reverse"};
    char ssl[400], res[160];
    int n = 0, mask = s[1];
    ssl[0] = 0;
    if (mask & 1)
        n += snprintf(ssl + n, sizeof ssl - n, "$player.DisableMove(%d)\n", s[2]);
    if (mask & 2)
        n += snprintf(ssl + n, sizeof ssl - n, "$player.DisableInput(%d)\n", s[3]);
    if (mask & 4)
        n += snprintf(ssl + n, sizeof ssl - n, "$player.Aimable(%d)\n", s[4]);
    for (int i = 0; i < 3; i++)
        if (mask & (8 << i))
            n += snprintf(ssl + n, sizeof ssl - n, "$player.DisableTC(\"%s\", %d)\n", tc[i], s[5 + i]);
    if (mask & 64)
        n += snprintf(ssl + n, sizeof ssl - n, "$player.SetupWeapon(%d, %d, %d)\n", (signed char)s[8],
                      (signed char)s[9], (signed char)s[10]);
    if (!n)
        return;
    ts_script_run(ssl, res, sizeof res);
    CLOG("campaign: the host's player state applied (mask %x) -> %s", mask, res);
}

/* (client) keep this machine's player on the vehicle it rides: above the vehicle's origin, places side by side. */
static void ride_step(void)
{
    char *veh = find_ent(ride_vehicle), *pl = PLAYER;
    char *vnode = veh ? *(char **)(veh + ENT_NODE) : NULL, *pnode = pl ? *(char **)(pl + ENT_NODE) : NULL;
    if (!vnode || !pnode) {
        CLOG("campaign: ride on %s ended (no vehicle or player)", ride_vehicle);
        ride_vehicle[0] = 0;
        return;
    }
    float pos[3], f[3] = {*(float *)(vnode + NODE_DIR), 0.f, *(float *)(vnode + NODE_DIR + 8)};
    ent_pos(veh, pos);
    if (GetTickCount() - ride_host_at < 1000) {   /* (the host's vehicle: this machine's copy may stand still) */
        memcpy(pos, ride_host, 12);
        f[0] = ride_host[3];
        f[2] = ride_host[4];
    }
    float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (l < 1e-3f)
        f[0] = 1.f, f[2] = 0.f, l = 1.f;
    float fx = f[0] / l, fz = f[2] / l;   /* (forward; right = (fz, 0, -fx)) */
    if (ride_rel < 0) {   /* the first step: a player standing on or next to the vehicle keeps that place (the truck's
                           * bed of ts2_zepcrashsite_part01); one far away goes on top (the armoured car) */
        float p0[3];
        ent_pos(pl, p0);
        float d[3] = {p0[0] - pos[0], p0[1] - pos[1], p0[2] - pos[2]};
        ride_rel = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 64.f;
        ride_off[0] = d[0] * fz - d[2] * fx;
        ride_off[1] = d[1];
        ride_off[2] = d[0] * fx + d[2] * fz;
        CLOG("campaign: ride place %s (%.1f %.1f %.1f)", ride_rel ? "kept" : "on top", ride_off[0], ride_off[1], ride_off[2]);
    }
    float at[3];
    if (ride_rel) {
        at[0] = pos[0] + ride_off[0] * fz + ride_off[2] * fx;
        at[1] = pos[1] + ride_off[1];
        at[2] = pos[2] - ride_off[0] * fx + ride_off[2] * fz;
    } else {
        float side = (ride_place % 2 ? 1.f : -1.f) * (0.8f + 0.8f * (float)(ride_place / 2));
        at[0] = pos[0] + side * fz;
        at[1] = pos[1] + ride_height;
        at[2] = pos[2] - side * fx;
    }
    float dir[3] = {*(float *)(pnode + NODE_DIR), *(float *)(pnode + NODE_DIR + 4), *(float *)(pnode + NODE_DIR + 8)};
    ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)pl)[PLAYER_PLACE / 4])(pl, at, dir, 1);
}

static void client_frame(void)
{
    if (pend.gather) {
        pend.gather = 0;
        place_local(pend.pos, pend.ring, pend.box ? pend.mn : NULL, pend.box ? pend.mx : NULL);
    }
    if (pend.kill) {
        pend.kill = 0;
        kill_local("the host: inside a death domain");
    }
    if (pend.state && PLAYER) {
        pend.state = 0;
        apply_state(pend.st);
    }
    if (pend.seat && PLAYER && (int)(GetTickCount() - pend.seat_at) >= 0) {
        char ssl[160], res[160];
        pend.seat = 0;
        snprintf(ssl, sizeof ssl, "$player.StartUse(\"%s\")", pend.seat_name);
        ts_script_run(ssl, res, sizeof res);
        CLOG("campaign: seat %s taken -> %s", pend.seat_name, res);
    }
    if (ride_vehicle[0])
        ride_step();
}

/* ---- seats and rides of the host's story events (storysync.c calls campaign_story_event on the host) ---- */
enum { A_SEAT = 1, A_RIDE_ON, A_RIDE_OFF };
typedef struct {
    const char *level, *obj, *ev, *arg;      /* obj ending in '*': a prefix; arg "": any */
    int act;
    const char *seat[3], *gunner[3];         /* A_SEAT: client k takes seat[k]; the host removes gunner[k] */
    const char *vehicle;                     /* A_RIDE_* */
} SeatRule;
#define ZEP "Zeppelin|Zeppelin_cruiser_ACTOR|"
static const SeatRule seat_rules[] = {
    /* ts2_zepride1_part01: the host takes tur02 (intro) or tur05/tur02 (checkpoint restores); AI gunners (their AI
     * blocks' "turret ="): rebel_tur1 tur03, rebel_hmg1 tur04, rebel22 tur05, rebel25 tur06 */
    {"ts2_zepride1_part01", "Cine1_ACTOR", "OnAnimEnd", "CLIP_3", A_SEAT,
     {ZEP "tur04", ZEP "tur03", ZEP "tur06"}, {"rebel_hmg1", "rebel_tur1", "rebel25"}, NULL},
    {"ts2_zepride1_part01", "Check_Point_*", "OnUnlocked", "", A_SEAT,
     {ZEP "tur04", ZEP "tur03", ZEP "tur06"}, {"rebel_hmg1", "rebel_tur1", "rebel25"}, NULL},
    /* ts2_zepride2_part01: the host takes tur01; gunners rebel_hmg1 tur04, rebel_tur1 tur03, rebel_hmg2 tur02 */
    {"ts2_zepride2_part01", "Zep_Intro_actor", "OnAnimEnd", "MOVE", A_SEAT,
     {ZEP "tur04", ZEP "tur03", ZEP "tur02"}, {"rebel_hmg1", "rebel_tur1", "rebel_hmg2"}, NULL},
    {"ts2_zepride2_part01", "Zeppelin_cruiser_plc_iga", "OnAnimEnd", "MOVE", A_SEAT,
     {ZEP "tur04", ZEP "tur03", ZEP "tur02"}, {"rebel_hmg1", "rebel_tur1", "rebel_hmg2"}, NULL},
    /* ts2_zepcrashsite_part01: the truck ride at the level start (players on its bed, start_place); the truck has an AI
     * driver, so the clients follow the host's truck */
    {"ts2_zepcrashsite_part01", "truck_place", "OnAnimBegin", "MOVE", A_RIDE_ON, {NULL}, {NULL}, "vhc_truck"},
    {"ts2_zepcrashsite_part01", "truck_place", "OnAnimEnd", "MOVE", A_RIDE_OFF, {NULL}, {NULL}, "vhc_truck"},
    /* ts2_zep_factory_part1: the armoured car; the host mans its gun, the clients ride on it until the end */
    {"ts2_zep_factory_part1", "actor_start_chase", "OnUnlocked", "", A_RIDE_ON, {NULL}, {NULL}, "Bronevik"},
    {"ts2_zep_factory_part1", "BR_pl_3", "OnAnimEnd", "MOVE", A_RIDE_OFF, {NULL}, {NULL}, "Bronevik"},
};
#define N_SEAT ((int)(sizeof seat_rules / sizeof seat_rules[0]))

static int name_match(const char *pat, const char *s)
{
    size_t n = strlen(pat);
    return n && pat[n - 1] == '*' ? strncmp(pat, s, n - 1) == 0 : strcmp(pat, s) == 0;
}

/* (host) a seat for a client that was still loading the level when its seat event ran (live: lost otherwise) */
static unsigned char seat_later[16][120];
static int seat_later_len[16], seat_later_sub[16];
static void seat_later_step(void)
{
    for (int i = 0; i < 16; i++) {
        char *s = slot_at(i);
        if (!seat_later_len[i] || !s)
            continue;
        if (*(int *)s == 0 || !slot_joined(s)) {
            seat_later_len[i] = 0;
            continue;
        }
        if (coopnet_send(i, (unsigned short)seat_later_sub[i], seat_later[i], seat_later_len[i]) > 0) {
            CLOG("campaign: seat sent to the client in slot %d now that it runs the level", i);
            seat_later_len[i] = 0;
        }
    }
}

/* (host) the vehicle clients ride on: its place goes to them 10 times a second (their own copy of an AI-driven vehicle
 * does not move: the route runs on the host only) */
#define SUB_RIDEPOS 75
static char host_ride[64];
static void ride_broadcast(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (!host_ride[0] || now - last < 100)
        return;
    last = now;
    char *veh = find_ent(host_ride), *node = veh ? *(char **)(veh + ENT_NODE) : NULL;
    float m[5];
    if (!node || !ent_pos(veh, m))
        return;
    m[3] = *(float *)(node + NODE_DIR);
    m[4] = *(float *)(node + NODE_DIR + 8);
    unsigned char b[1 + sizeof m];
    b[0] = PROTO;
    memcpy(b + 1, m, sizeof m);
    coopnet_send(-1, SUB_RIDEPOS, b, sizeof b);
}

static DWORD level_start;   /* (host: set by latejoin_step when a level run starts) */
void campaign_story_event(const char *obj, const char *ev, const char *arg0)
{
    if (!use_seats || role() != MODE_SERVER || !obj || !ev)
        return;
    char base[0x110];
    level_base(LEVEL_NAME, base, sizeof base);
    /* (a seat rule acts once per level run: live, ts2_zepride2_part01's intro actor ends its animation every minute,
     * which pulled a client back into its turret each time; a team restart is a new level run) */
    static unsigned seat_done;
    static DWORD seat_level_start;
    if (seat_level_start != level_start) {
        seat_level_start = level_start;
        seat_done = 0;
    }
    for (int r = 0; r < N_SEAT; r++) {
        const SeatRule *s = &seat_rules[r];
        if (_stricmp(s->level, base) || strcmp(s->ev, ev) || !name_match(s->obj, obj)
            || (s->arg[0] && _stricmp(s->arg, arg0 ? arg0 : "")))
            continue;
        if (s->act == A_SEAT && strcmp(s->obj, "Check_Point_*") != 0) {   /* (restores may seat again) */
            if (seat_done & (1u << r))
                continue;
            seat_done |= 1u << r;
        }
        int k = 0;
        for (int i = 0; slot_at(i); i++) {
            char *sl = slot_at(i);
            if (*(int *)sl == 0 || !slot_joined(sl))
                continue;
            unsigned char b[120];
            if (s->act == A_SEAT && k < 3 && s->seat[k]) {
                char ssl[96], res[120];
                snprintf(ssl, sizeof ssl, "$%s.Die(\"REMOVE\")", s->gunner[k]);
                ts_script_run(ssl, res, sizeof res);
                int len = (int)strlen(s->seat[k]);
                b[0] = PROTO;
                b[1] = (unsigned char)len;
                memcpy(b + 2, s->seat[k], len);
                if (coopnet_send(i, SUB_SEAT, b, 2 + len) <= 0 && i < 16) {   /* (still loading: when it runs the level) */
                    memcpy(seat_later[i], b, 2 + len);
                    seat_later_len[i] = 2 + len;
                    seat_later_sub[i] = SUB_SEAT;
                }
                CLOG("campaign: %s.%s: client in slot %d takes %s (%s -> %s)", obj, ev, i, s->seat[k], ssl, res);
            } else if (s->act == A_RIDE_ON || s->act == A_RIDE_OFF) {
                int len = (int)strlen(s->vehicle);
                b[0] = PROTO;
                b[1] = s->act == A_RIDE_ON;
                b[2] = (unsigned char)k;
                b[3] = (unsigned char)len;
                memcpy(b + 4, s->vehicle, len);
                if (coopnet_send(i, SUB_RIDE, b, 4 + len) <= 0 && i < 16) {   /* (still loading: later) */
                    memcpy(seat_later[i], b, 4 + len);
                    seat_later_len[i] = 4 + len;
                    seat_later_sub[i] = SUB_RIDE;
                }
                snprintf(host_ride, sizeof host_ride, "%s", s->act == A_RIDE_ON ? s->vehicle : "");
                CLOG("campaign: %s.%s: client in slot %d %s %s", obj, ev, i, b[1] ? "rides on" : "gets off", s->vehicle);
            }
            k++;
        }
    }
}

int campaign_local_setspeed(void)
{
    return enabled && use_setspeed;
}

/* Calls storysync wraps as host-only that clients run themselves: SetSpeed (force fields and wind, see above) and the
 * vehicles' StartRoute/StopRoute (live: a route-driven vehicle - the zeppelin, helicopters, interceptors, the
 * armoured car - has a net id but no motion data to send, so it stood still on clients; run locally, each machine's
 * copy follows the same route from the same replayed start). */
int campaign_local_call(const char *stmt, const char *name, int len)
{
    /* (only vehicles without an AI driver: a client's StartRoute on an AI vehicle - Opp_Bronevik_1, bikes, helicopters -
     * crashed it at the start of ts2_zep_factory_part1, AV 0x49bddd: the vehicle's AI part is NULL on a client) */
    static const char *const routed[] = {"$Zeppelin|Zeppelin_cruiser_ACTOR.", "$DIRIGIBLE1|dirigible_ACTOR.",
                                         "$ref_dirigible|dirigible_ACTOR.", NULL};
    if (!enabled)
        return 0;
    if (use_setspeed && len == 8 && strncmp(name, "SetSpeed", 8) == 0)
        return 1;
    if (use_routes && ((len == 10 && strncmp(name, "StartRoute", 10) == 0) || (len == 9 && strncmp(name, "StopRoute", 9) == 0)))
        for (int i = 0; routed[i]; i++)
            if (stmt && strncmp(stmt, routed[i], strlen(routed[i])) == 0)
                return 1;
    return 0;
}

/* ---- native hooks: the host's player state for late joiners, "player" in AI targets ---- */
typedef struct { void **type; unsigned data; } Variant;
static void *(*type_int)(void) = (void *)0x9c8c50;
static void *(*type_bool)(void) = (void *)0x9c8d00;
static void *(*type_str)(void) = (void *)0x9c8ec0;
static int (TSCALL *named_cmp)(void *, void **, void **) = (void *)0x8fb500;      /* <0 / 0 / >0 by name */
static void (TSCALL *named_insert)(void *, int, void **, int) = (void *)0x8fb000; /* (array, index, &inst, 1) */

/* Trampolines: the 7 bytes the hook jump replaced (push -1; push <handler>), then the native's body. Every
 * script native has this prologue and the same convention (thiscall object; argc, argv, result, self; ret 0x10). */
#define TRAMP(sym, back, seh) \
    __asm__(".text\n.globl " sym "\n" sym ":\n  pushl $-1\n  pushl $" seh "\n  pushl $" back "\n  ret\n")
TRAMP("cp_t_input", "0x438d47", "0xc67c78");
TRAMP("cp_t_move", "0x438e97", "0xc67c48");
TRAMP("cp_t_tc", "0x439047", "0xc79130");
TRAMP("cp_t_wpn", "0x43a147", "0xc67a38");
TRAMP("cp_t_aim", "0x666d97", "0xc69418");
TRAMP("cp_t_setaim", "0x83d3f7", "0xc7a8b0");
TRAMP("cp_t_tgt1", "0x4b2ba7", "0xc82740");
TRAMP("cp_t_tgt2", "0x4cb6d7", "0xc82650");
TRAMP("cp_t_tgt3", "0x51a3d7", "0xc81e90");
extern void TSCALL cp_t_input(void *, int, Variant *, Variant *, void *) __asm__("cp_t_input");
extern void TSCALL cp_t_move(void *, int, Variant *, Variant *, void *) __asm__("cp_t_move");
extern void TSCALL cp_t_tc(void *, int, Variant *, Variant *, void *) __asm__("cp_t_tc");
extern void TSCALL cp_t_wpn(void *, int, Variant *, Variant *, void *) __asm__("cp_t_wpn");
extern void TSCALL cp_t_aim(void *, int, Variant *, Variant *, void *) __asm__("cp_t_aim");
extern void TSCALL cp_t_setaim(void *, int, Variant *, Variant *, void *) __asm__("cp_t_setaim");
extern void TSCALL cp_t_tgt1(void *, int, Variant *, Variant *, void *) __asm__("cp_t_tgt1");
extern void TSCALL cp_t_tgt2(void *, int, Variant *, Variant *, void *) __asm__("cp_t_tgt2");
extern void TSCALL cp_t_tgt3(void *, int, Variant *, Variant *, void *) __asm__("cp_t_tgt3");

/* (host) the host's own player's locks and loadout as the level scripts set them in this level */
static struct { int mask, move, input, aim, tc[3], wpn[3]; } hst;

static int arg_bool(int argc, Variant *argv, int i, int def)
{
    if (argc <= i || !argv)
        return def;
    if (argv[i].type == type_bool())
        return (argv[i].data & 0xff) != 0;
    if (argv[i].type == type_int())
        return argv[i].data != 0;
    return def;
}

static int recording(void *obj)
{
    return use_latejoin && obj && obj == (void *)PLAYER && role() == MODE_SERVER;
}

static void TSCALL hk_input(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj))
        hst.input = arg_bool(argc, argv, 0, 1), hst.mask |= 2;
    cp_t_input(obj, argc, argv, res, self);
}

static void TSCALL hk_move(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj))
        hst.move = arg_bool(argc, argv, 0, 1), hst.mask |= 1;
    cp_t_move(obj, argc, argv, res, self);
}

static void TSCALL hk_aim(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj))
        hst.aim = arg_bool(argc, argv, 0, 1), hst.mask |= 4;
    cp_t_aim(obj, argc, argv, res, self);
}

static void TSCALL hk_tc(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj) && argc > 0 && argv && argv[0].type == type_str() && argv[0].data) {
        const char *m = ((String)argv[0].data)->s;
        int i = !_stricmp(m, "time_slow") ? 0 : !_stricmp(m, "time_stop") ? 1 : !_stricmp(m, "time_reverse") ? 2 : -1;
        if (i >= 0)
            hst.tc[i] = arg_bool(argc, argv, 1, 1), hst.mask |= 8 << i;
    }
    cp_t_tc(obj, argc, argv, res, self);
}

static void TSCALL hk_wpn(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj) && argc >= 3 && argv) {
        for (int i = 0; i < 3; i++)
            hst.wpn[i] = argv[i].type == type_int() ? (int)argv[i].data : 0;
        hst.mask |= 64;
    }
    cp_t_wpn(obj, argc, argv, res, self);
}

/* Remote players get a script name on the host (their script instance is nameless), so AI natives can find them
 * (the same insertion into the script manager's sorted named list as aisync.c's names, from CreateObject 0x8fd2f0). */
static int name_remote(int slot, char *ent, char *out, int size)
{
    snprintf(out, size, "coop_player%d", slot);
    void *inst = ent ? *(void **)(ent + ENT_INST) : NULL;
    if (!inst || ((DWORD)inst & 3) || IsBadReadPtr(inst, 0x14) || *(char **)((char *)inst + INST_NATIVE) != ent)
        return 0;
    String cur = *(String *)((char *)inst + INST_NAME);
    if (cur && cur->s[0])
        return snprintf(out, size, "%s", cur->s), 1;   /* (named already: by us earlier, or otherwise) */
    if (find_ent(out))
        return 0;
    String nm = 0;
    str_ctor(&nm, out, -1, 0);
    *(String *)((char *)inst + INST_NAME) = nm;
    str_release(cur);
    char *mgr = MGR;
    void **data = *(void ***)(mgr + 0x14), *ip = inst;
    int lo = 0, hi = *(int *)(mgr + 0x18);
    while (lo != hi) {
        int mid = (lo + hi) >> 1, c = named_cmp(NULL, &data[mid], &ip);
        if (c == 0)
            return 1;
        if (c > 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    named_insert(mgr + 0x14, lo, &ip, 1);
    CLOG("campaign: remote player in slot %d named %s for scripted AI targets", slot, out);
    return 1;
}

/* (host) "player" in an AI aim/target call -> the nearest player's name. 1 = argv[0] was swapped (restore after). */
static int swap_player_arg(void *obj, int argc, Variant *argv, String *saved, String *mine)
{
    if (!use_ai || role() != MODE_SERVER || argc < 1 || !argv || argv[0].type != type_str() || !argv[0].data
        || _stricmp(((String)argv[0].data)->s, "player") != 0)
        return 0;
    float me[3], p[3];
    if (!ent_pos(obj, me) || !ent_pos(PLAYER, p))
        return 0;
    float best = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
    int best_slot = -1;
    char *best_ent = NULL;
    for (int i = 0; slot_at(i); i++) {
        char *s = slot_at(i), *ent = *(char **)(s + 4);
        if (*(int *)s == 0 || !slot_joined(s) || !ent_pos(ent, p))
            continue;
        float d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d < best)
            best = d, best_slot = i, best_ent = ent;
    }
    char name[48];
    if (best_slot < 0 || !name_remote(best_slot, best_ent, name, sizeof name))
        return 0;
    *mine = 0;
    str_ctor(mine, name, -1, 0);
    *saved = (String)argv[0].data;
    argv[0].data = (unsigned)*mine;
    VLOG("campaign: %s targets %s (nearest player) instead of the host", ent_name(obj), name);
    return 1;
}

#define AI_HOOK(fn, tramp) \
    static void TSCALL fn(void *obj, int argc, Variant *argv, Variant *res, void *self) \
    { \
        String saved = 0, mine = 0; \
        int sw = swap_player_arg(obj, argc, argv, &saved, &mine); \
        tramp(obj, argc, argv, res, self); \
        if (sw) { \
            argv[0].data = (unsigned)saved; \
            str_release(mine); \
        } \
    }
AI_HOOK(hk_setaim, cp_t_setaim)
AI_HOOK(hk_tgt1, cp_t_tgt1)
AI_HOOK(hk_tgt2, cp_t_tgt2)
AI_HOOK(hk_tgt3, cp_t_tgt3)

typedef struct { DWORD addr, seh; void *hook; int group; } Hook;   /* group 1 late join, 2 AI */
static const Hook hooks[] = {
    {0x438d40, 0xc67c78, (void *)hk_input, 1}, {0x438e90, 0xc67c48, (void *)hk_move, 1},
    {0x439040, 0xc79130, (void *)hk_tc, 1},    {0x43a140, 0xc67a38, (void *)hk_wpn, 1},
    {0x666d90, 0xc69418, (void *)hk_aim, 1},
    {0x83d3f0, 0xc7a8b0, (void *)hk_setaim, 2}, {0x4b2ba0, 0xc82740, (void *)hk_tgt1, 2},
    {0x4cb6d0, 0xc82650, (void *)hk_tgt2, 2},   {0x51a3d0, 0xc81e90, (void *)hk_tgt3, 2},
};
#define N_HOOKS ((int)(sizeof hooks / sizeof hooks[0]))

/* Client crash guard (live, also without the mod's campaign parts: ts2_zep_factory_part1, AV 0x49bddd when an AI
 * biker died on a client): the vehicle method 0x49bd70 (vtable 0xd7edfc slot; ecx = the vehicle, plain ret) uses the
 * vehicle's physics body +0x27c, which a client's copy does not have. Without a body it now returns at once. */
extern void cp_veh_guard(void) __asm__("cp_veh_guard");
__asm__(".text\n"
        ".globl cp_veh_guard\n"
        "cp_veh_guard:\n"
        "  cmpl $0, 0x27c(%ecx)\n"
        "  je 1f\n"
        "  subl $0x28, %esp\n"
        "  pushl %esi\n"
        "  movl %ecx, %esi\n"
        "  pushl $0x49bd76\n"
        "  ret\n"
        "1:\n"
        "  ret\n");

/* ---- late joiners (host): a client that joins a level running for more than 20 s gets the host's player state ---- */
#define LATE_MS 20000
static DWORD level_start;
static struct { int id; DWORD seen; int sends; } known[16];
static int n_known;

static void latejoin_step(DWORD now)
{
    static int was_running;
    int running = (GAME_FLAGS & FLAG_LEVEL_RUNNING) != 0;
    if (running && !was_running) {   /* a level (or the same level after a team restart) is up */
        level_start = now;
        CLOG("campaign: level run starts (%s)", LEVEL_NAME);
        memset(&hst, 0, sizeof hst);
        n_known = 0;
    }
    was_running = running;
    if (!running)
        return;
    for (int i = 0; slot_at(i); i++) {
        char *s = slot_at(i);
        int id = *(int *)s, k;
        if (id == 0 || !slot_joined(s) || !(*(int *)(s + 8) & SLOT_INGAME))
            continue;
        for (k = 0; k < n_known && known[k].id != id; k++)
            ;
        if (k == n_known) {
            if (n_known >= 16)
                continue;
            known[k].id = id;
            known[k].seen = now;
            /* (clients in the level from its start need nothing; a late joiner gets 3 sends, at 3, 10 and 25 s) */
            known[k].sends = now - level_start > LATE_MS ? 3 : 0;
            if (known[k].sends)
                CLOG("campaign: client %d joined %lu s into the level: it gets the host's player state", id,
                     (now - level_start) / 1000);
            n_known++;
        }
        static const DWORD at[3] = {25000, 10000, 3000};
        if (known[k].sends > 0 && now - known[k].seen >= at[known[k].sends - 1] && hst.mask) {
            unsigned char b[12] = {PROTO, (unsigned char)hst.mask, (unsigned char)hst.move, (unsigned char)hst.input,
                                   (unsigned char)hst.aim, (unsigned char)hst.tc[0], (unsigned char)hst.tc[1],
                                   (unsigned char)hst.tc[2], (unsigned char)hst.wpn[0], (unsigned char)hst.wpn[1],
                                   (unsigned char)hst.wpn[2], 0};
            if (known[k].sends < 3)
                b[1] &= ~64;   /* (the loadout only once: later pickups must not be replaced) */
            if (coopnet_send(i, SUB_STATE, b, sizeof b) > 0) {
                known[k].sends--;
                VLOG("campaign: player state (mask %x) sent to client %d", b[1], id);
            }
        }
    }
}

/* ---- story events each machine runs itself (storysync.c asks; NOT YET RUN) ---- */
/* Level 16's cyclic machinery (flame jets FL*, presses PRESS*, the fire_kill_actor flames, sparks, conveyor sounds)
 * fires animation-frame events all the time on every machine: their handlers only show/hide effects, play sounds and
 * lock/unlock the kill domains in step with the animation, so each machine runs its own (live: ~760 replays in 2
 * minutes before). Only on that level, and only handlers made of such statements. */
int campaign_frame_local(const char *obj, const char *ev, const char *stmt)
{
    static const char *const ok[] = {".Show(", ".Hide(", ".Lock(", ".Unlock(", "PlaySound(", "MuteSound(", ".Start(",
                                     ".Stop(", NULL};
    char base[0x110];
    if (!enabled || !obj || !ev || !stmt || strcmp(ev, "OnActionFrame") != 0)
        return 0;
    level_base(LEVEL_NAME, base, sizeof base);
    if (_stricmp(base, "ts2_guardfactory_part03") != 0)
        return 0;
    for (int i = 0; ok[i]; i++)
        if (strstr(stmt, ok[i]))
            return !strstr(stmt, "$player") && !strstr(stmt, "$farm");
    return 0;
}

/* A client holds the host's replay of an event whose handler talks to objects the host creates just before it
 * (their names arrive a moment later): level 8's intro dialogue (Intro_Cooke, Intro_rebel_1 are created in
 * OnAnimEnd("CLIP_1"), the lines are posted by the next OnAnimBegin("MOVE"); live, the client got "unknown object"
 * for every line). Up to storysync's 5 s. */
static int named_quiet(const char *name)   /* (the script manager's named objects, without its error message) */
{
    char *mgr = MGR;
    void **data = mgr ? *(void ***)(mgr + 0x14) : NULL;
    int n = mgr ? *(int *)(mgr + 0x18) : 0;
    for (int i = 0; data && i < n; i++) {
        String nm = data[i] ? *(String *)((char *)data[i] + INST_NAME) : NULL;
        if (nm && !strcmp(nm->s, name))
            return 1;
    }
    return 0;
}

int campaign_replay_wait(const char *obj, const char *ev)
{
    static const struct { const char *level, *obj, *ev, *need[3]; } waits[] = {
        {"ts2_zepride1_part01", "Zeppelin_cruiser_Intro_plc", "OnAnimBegin", {"Intro_Cooke", "Intro_rebel_1", NULL}},
        {"ts2_zepride2_part01", "Zep_Intro_actor", "OnAnimBegin", {"Intro_Cooke", "Intro_rebel_2", NULL}},
    };
    char base[0x110];
    if (!enabled || role() != MODE_CLIENT)
        return 0;
    level_base(LEVEL_NAME, base, sizeof base);
    for (int i = 0; i < (int)(sizeof waits / sizeof waits[0]); i++) {
        if (_stricmp(base, waits[i].level) || strcmp(obj, waits[i].obj) || strcmp(ev, waits[i].ev))
            continue;
        for (int k = 0; waits[i].need[k]; k++)
            if (!named_quiet(waits[i].need[k])) {
                static DWORD logged;
                if (GetTickCount() - logged > 1000)
                    VLOG("campaign: replay %s.%s waits for %s", obj, ev, waits[i].need[k]);
                logged = GetTickCount();
                return 1;
            }
    }
    return 0;
}

/* ---- vehicles: a transform stream (TS_CAMPAIGN_VEHSYNC, default on) ----
 * Vehicles (a family of nine engine classes: vtables 0xd90758 ... 0xd92b58, e.g. 0xd91610 bikes/quads, 0xd91970
 * trucks and armoured cars; all share the vehicle base's method 0x51c160 in slot +0x78, which nothing else has [V,
 * vtable scan of the exe]; live level 9: bike11/opp_bike1 0xd91610, vhc_truck/bronik1 0xd91970) have net ids but no
 * motion component (+0x34), so nothing carries their transform: on a client the route-driven ones stood still (their
 * StartRoute there crashed the AI ones, AV 0x49bddd), and a client's quad moved only on the client. Now every machine
 * sends the vehicles it moves itself: the host every vehicle that moves on the host (route, AI, physics, its own
 * player's quad), unless a client sent that vehicle within the last second; a client only the vehicle its own player
 * uses (+0x9c0 = the user, as the player's network update 0x420120 finds it). Receivers place them through the scene
 * node's matrix setter (0x94dbf0(node, 4x4, 0): the same call the network readers end in, 0x68e1b0), 100 ms behind,
 * interpolated. Vehicles a client runs on its own route (campaign_local_call) are left out. 10 Hz while moving, plus
 * one send 0.5 s after a vehicle stops.
 * Message 76 (either way): {u8 1, u8 n, n x {u32 name hash, f32 rows[3][3], f32 pos[3]}} (n <= 4: coopnet's 250). */
#define VEH_METHOD_SLOT 0x78
#define VEH_METHOD 0x51c160
#define VEH_USER 0x9c0
static int is_vehicle(char *o)
{
    void **vt = o && !IsBadReadPtr(o, VEH_USER + 4) ? *(void ***)o : NULL;
    return vt && !IsBadReadPtr(vt, VEH_METHOD_SLOT + 4) && vt[VEH_METHOD_SLOT / 4] == (void *)VEH_METHOD;
}
#define MAX_VEH 96
#define NODE_MAT 0x38                        /* scene node: 4x4 world matrix (rows x, y, z, position) */
#define ENT_BUCKETS (*(char **)0xecb7fc)
#define ENT_NBUCKETS (*(int *)0xecb800)
typedef struct {
    unsigned hash;
    char *ent;
    float sent[12];                          /* what this machine sent last */
    DWORD sent_at, still_since;
    int final_due;
    float a[12], b[12];                      /* received: the two latest samples */
    DWORD a_at, b_at;
    int have;
} Veh;
static Veh vehs[MAX_VEH];
static int n_veh, use_vehsync = 1, veh_sent, veh_got, veh_msgs_out, veh_msgs_in, veh_nopeer, veh_unknown;
static DWORD veh_send_at;
static char veh_level[0x110];

static unsigned name_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static int veh_local_route(const char *name)
{
    static const char *const local[] = {"Zeppelin|Zeppelin_cruiser_ACTOR", "DIRIGIBLE1|dirigible_ACTOR",
                                        "ref_dirigible|dirigible_ACTOR", NULL};
    for (int i = 0; local[i]; i++)
        if (!strcmp(name, local[i]))
            return use_routes;
    return 0;
}

static void veh_read(char *ent, float *m)
{
    char *node = *(char **)(ent + ENT_NODE);
    for (int r = 0; r < 4; r++)
        memcpy(m + r * 3, node + NODE_MAT + r * 16, 12);
}

static void veh_scan(void)
{
    if (strcmp(veh_level, LEVEL_NAME) != 0) {
        n_veh = 0;
        snprintf(veh_level, sizeof veh_level, "%s", LEVEL_NAME);
    }
    for (int k = 0; k < n_veh; k++)
        vehs[k].ent = NULL;
    char *b = ENT_BUCKETS;
    int nb = ENT_NBUCKETS;
    for (int i = 0; b && i < nb; i++)
        for (char *o = *(char **)(b + i * 12); o; ) {
            if (is_vehicle(o) && *(void **)(o + ENT_NODE) && !IsBadReadPtr(*(void **)(o + ENT_NODE), 0x78)) {
                const char *nm = ent_name(o);
                if (nm[0] && !veh_local_route(nm)) {
                    unsigned h = name_hash(nm);
                    int k;
                    for (k = 0; k < n_veh && vehs[k].hash != h; k++)
                        ;
                    if (k == n_veh && n_veh < MAX_VEH) {
                        memset(&vehs[k], 0, sizeof vehs[k]);
                        vehs[k].hash = h;
                        veh_read(o, vehs[k].sent);
                        vehs[k].still_since = GetTickCount();
                        n_veh++;
                    }
                    if (k < n_veh)
                        vehs[k].ent = o;
                }
            }
            char *next = *(char **)(o + 8);
            o = next ? next - 4 : NULL;
        }
}

static int veh_mine(Veh *v, DWORD now)
{
    if (!v->ent)
        return 0;
    if (role() == MODE_CLIENT)
        return PLAYER && *(char **)(v->ent + VEH_USER) == PLAYER;
    return (PLAYER && *(char **)(v->ent + VEH_USER) == PLAYER) || !v->have || now - v->b_at > 1000;   /* (host: unless a
                                                                                    * client drives it) */
}

static void veh_out(const unsigned char *msg, int n)
{
    int peers = coopnet_send(-1, SUB_VEH, msg, 2 + n * 52);
    veh_msgs_out++;
    if (peers > 0)
        veh_sent += n;
    else
        veh_nopeer++;
    if (veh_msgs_out <= 5 || (veh_msgs_out % 200) == 0)
        CLOG("campaign: vehicle message %d out: %d vehicle(s) to %d peer(s)", veh_msgs_out, n, peers);
}

static void veh_send(DWORD now)
{
    unsigned char msg[2 + 4 * 52];
    int n = 0;
    for (int k = 0; k < n_veh; k++) {
        Veh *v = &vehs[k];
        if (!veh_mine(v, now))
            continue;
        float m[12];
        veh_read(v->ent, m);
        float d = 0.f;
        for (int i = 0; i < 12; i++)
            d = fmaxf(d, fabsf(m[i] - v->sent[i]));
        int moved = d > 0.004f, final = !moved && v->final_due && now - v->sent_at >= 500;
        int keep = !moved && !final && now - v->sent_at >= 2000;   /* (a client that joins later gets the standing ones too:
                                                                   * live level 9, the AI vehicles had stopped before it came) */
        if (!moved && !final && !keep)
            continue;
        v->final_due = moved;
        memcpy(v->sent, m, sizeof m);
        v->sent_at = now;
        memcpy(msg + 2 + n * 52, &v->hash, 4);
        memcpy(msg + 2 + n * 52 + 4, m, 48);
        if (++n == 4) {
            msg[0] = PROTO, msg[1] = (unsigned char)n;
            veh_out(msg, n);
            n = 0;
        }
    }
    if (n) {
        msg[0] = PROTO, msg[1] = (unsigned char)n;
        veh_out(msg, n);
    }
}

static void veh_receive(const unsigned char *b, int len)
{
    DWORD now = GetTickCount();
    veh_msgs_in++;
    if (veh_msgs_in <= 5 || (veh_msgs_in % 200) == 0)
        CLOG("campaign: vehicle message %d in: %d vehicle(s), %d byte(s)", veh_msgs_in, b[1], len);
    for (int j = 0; j < b[1] && 2 + (j + 1) * 52 <= len; j++) {
        unsigned h;
        memcpy(&h, b + 2 + j * 52, 4);
        for (int k = 0; k < n_veh; k++)
            if (vehs[k].hash == h) {
                Veh *v = &vehs[k];
                if (v->have && now - v->b_at < 1000) {
                    memcpy(v->a, v->b, 48);
                    v->a_at = v->b_at;
                } else {
                    memcpy(v->a, b + 2 + j * 52 + 4, 48);
                    v->a_at = now - 100;
                }
                memcpy(v->b, b + 2 + j * 52 + 4, 48);
                v->b_at = now;
                v->have = 1;
                veh_got++;
                goto found;
            }
        veh_unknown++;   /* (a vehicle this machine does not have, or not yet) */
        if (veh_unknown <= 5)
            CLOG("campaign: vehicle %08x from the other machine is not here (%d known)", h, n_veh);
found:;
    }
}

static void veh_apply(DWORD now)
{
    for (int k = 0; k < n_veh; k++) {
        Veh *v = &vehs[k];
        if (!v->have || !v->ent || veh_mine(v, now) || now - v->b_at > 3000)
            continue;
        float span = (float)(v->b_at - v->a_at), t = span > 1.f ? (float)(now - v->b_at) / span : 1.f, m[12];
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;   /* (100 ms behind: from a to b over the gap between them) */
        for (int i = 0; i < 12; i++)
            m[i] = v->a[i] + (v->b[i] - v->a[i]) * t;
        char *node = *(char **)(v->ent + ENT_NODE);
        float mat[16];
        memcpy(mat, node + NODE_MAT, 64);
        for (int r = 0; r < 4; r++)
            memcpy(mat + r * 4, m + r * 3, 12);
        if (!memcmp(mat, node + NODE_MAT, 64))
            continue;
        ((void (TSCALL *)(void *, float *, int))0x94dbf0)(node, mat, 0);
    }
}

static void veh_step(DWORD now)
{
    if (!use_vehsync || !(GAME_FLAGS & 0x2000) || !LEVEL_NAME[0])
        return;
    veh_scan();   /* (every frame, as aisync.c walks its AI: no pointer to a vehicle outlives a frame) */
    if (now - veh_send_at >= 100) {
        veh_send_at = now;
        veh_send(now);
    }
    veh_apply(now);
}

/* ---- per frame (host) ---- */
static void frame(float dt)
{
    (void)dt;
    static DWORD last;
    DWORD now = GetTickCount();
    int r = role();
    if (r == MODE_CLIENT) {
        client_frame();
        veh_step(now);
        return;
    }
    if (r != MODE_SERVER) {
        ride_vehicle[0] = 0;
        return;
    }
    veh_step(now);
    if (use_latejoin)
        latejoin_step(now);
    if (use_seats) {
        seat_later_step();
        ride_broadcast();
    }
    if (now - last < 250)
        return;
    last = now;
    sync_level();
    for (int k = 0; k < n_act; k++) {
        Active *a = &act[k];
        if (!a->consulted)
            continue;   /* (storysync's hook has not asked for this domain: leave it alone) */
        char *dom = find_ent(a->r->domain);
        if (!is_domain(dom))
            continue;
        int locked = *(int *)(dom + DOM_LOCKS) > 0;
        if (a->r->rule == R_KILL && use_kill && !locked) {
            int hin = inside(dom, PLAYER);
            if (hin && !a->in_dom[0] && now - a->killed_at[0] > 8000) {
                a->killed_at[0] = now;
                kill_local("inside a death domain");
            }
            a->in_dom[0] = (unsigned char)hin;
            for (int i = 0; i < 16; i++) {
                char *s = slot_at(i);
                if (!s)
                    break;
                char *ent = *(char **)(s + 4);
                int in = *(int *)s != 0 && slot_joined(s) && inside(dom, ent), was = a->in_dom[i];
                a->in_dom[i] = (unsigned char)in;
                if (!in || was || now - a->killed_at[i] <= 8000)
                    continue;
                a->killed_at[i] = now;
                unsigned char b = PROTO;
                coopnet_send(i, SUB_KILL, &b, 1);
                CLOG("campaign: client in slot %d inside %s: killed", i, a->r->domain);
            }
        } else if (a->r->rule == R_ALL && gather_after > 0) {
            int missing = 0;
            unsigned st = *(unsigned *)(dom + DOM_STATE);
            /* (an "act once" domain that has fired never enters again: the engine's test at 0x6c2e59) */
            char *desc = *(char **)(dom + DOM_DESC);
            int spent = desc && (*(unsigned char *)(desc + DESC_ACT_ONCE) & 1) && (st & 2);
            int waiting = !locked && !spent && !(st & 1) && inside(dom, PLAYER) && !clients_inside(dom, &missing);
            if (!waiting)
                a->wait_since = 0;
            else if (!a->wait_since) {
                a->wait_since = now;
                VLOG("campaign: %s waits for %d player(s)", a->r->domain, missing);
            } else if (now - a->wait_since >= (DWORD)gather_after * 1000) {
                a->wait_since = now;   /* (again after the same delay if somebody is still missing) */
                gather(dom);
            }
        }
    }
}

/* ---- test console: "!cp ..." ---- */
int campaign_console(const char *text, char *out, int size)
{
    if (strncmp(text, "!cp", 3) != 0)
        return 0;
    char cmd[32] = "", a1[128] = "";
    sscanf(text + 3, "%31s %127s", cmd, a1);
    int r = role();
    if (!strcmp(cmd, "state") || !cmd[0] || !strcmp(cmd, "rules")) {
        sync_level();
        int n = snprintf(out, size, "campaign %s, role %d, level %s, %d rule(s), gather after %d s:", enabled ? "on" : "off",
                         r, LEVEL_NAME, n_act, gather_after);
        for (int k = 0; k < n_act && n < size - 120; k++) {
            char *dom = find_ent(act[k].r->domain);
            n += snprintf(out + n, size - n, " %s=%s%s", act[k].r->domain, rule_names[act[k].r->rule],
                          act[k].consulted ? "" : " (not asked yet)");
            if (is_domain(dom)) {
                int missing = 0;
                clients_inside(dom, &missing);
                n += snprintf(out + n, size - n, " [state %x, locks %d, host %s, clients missing %d]",
                              *(unsigned *)(dom + DOM_STATE), *(int *)(dom + DOM_LOCKS),
                              inside(dom, PLAYER) ? "in" : "out", missing);
            }
        }
        return 1;
    }
    if (!strcmp(cmd, "regroup")) {
        if (r == MODE_SERVER)
            return snprintf(out, size, "%d client(s) sent to the host", gather(NULL)), 1;
        if (r == MODE_CLIENT) {
            char *s = slot_at(0);
            float pos[3];
            if (!s || *(int *)s != 0 || !ent_pos(*(char **)(s + 4), pos))
                return snprintf(out, size, "the host's player is not known here"), 1;
            place_local(pos, 1, NULL, NULL);
            return snprintf(out, size, "moved to the host (%.1f %.1f %.1f)", pos[0], pos[1], pos[2]), 1;
        }
        return snprintf(out, size, "not in a co-op session with the story on (or TS_CAMPAIGN is off)"), 1;
    }
    if (!strcmp(cmd, "pos")) {   /* "!cp pos <object> [object2]": positions of named objects (lab) */
        char a2[128] = "";
        sscanf(text + 3, "%*s %*s %127s", a2);
        int n = 0;
        for (int k = 0; k < 2; k++) {
            const char *nm = k ? a2 : a1;
            float pos[3];
            if (!nm[0])
                continue;
            char *e = find_ent(nm);
            if (e && ent_pos(e, pos))
                n += snprintf(out + n, size - n, "%s at %.2f %.2f %.2f; ", nm, pos[0], pos[1], pos[2]);
            else
                n += snprintf(out + n, size - n, "%s: none; ", nm);
        }
        return 1;
    }
    if (!strcmp(cmd, "ent")) {   /* "!cp ent <object>": net id, role/net flags +0x2c, sync flags +0x98, position (lab) */
        char *e = find_ent(a1);
        float pos[3] = {0, 0, 0};
        if (!e)
            return snprintf(out, size, "%s: none", a1), 1;
        ent_pos(e, pos);
        return snprintf(out, size, "%s: net %d, +2c %x, +98 %x, motion %p, at %.2f %.2f %.2f", a1, *(int *)(e + 0x18),
                        *(unsigned *)(e + 0x2c), *(unsigned *)(e + 0x98), *(void **)(e + 0x34), pos[0], pos[1], pos[2]), 1;
    }
    if (!strcmp(cmd, "node")) {   /* "!cp node <object>": its scene node's floats +0x08..+0x7c, vtable, animation (lab) */
        char *e = find_ent(a1), *node = e ? *(char **)(e + ENT_NODE) : NULL;
        if (!node)
            return snprintf(out, size, "%s: no node", a1), 1;
        int n = snprintf(out, size, "%s vt %p [+204 %p] node %p anim %d t %.3f:", a1, *(void **)e,
                         (*(void ***)e)[0x204 / 4], node, ((int (TSCALL *)(void *))0x951660)(node),
                         (double)((float (TSCALL *)(void *))0x951500)(node));
        n += snprintf(out + n, size - n, " raw[4,8,c]=%08x %08x %08x user %p:", *(unsigned *)(node + 4), *(unsigned *)(node + 8),
                      *(unsigned *)(node + 0xc), *(void **)(e + VEH_USER));
        for (int o = 0x08; o < 0x80 && n < size - 16; o += 4)
            n += snprintf(out + n, size - n, " %x:%.2f", o, (double)*(float *)(node + o));
        return 1;
    }
    if (!strcmp(cmd, "nodeset")) {   /* "!cp nodeset <object> <hex offset> x y z" (lab) */
        char *e = find_ent(a1), *node = e ? *(char **)(e + ENT_NODE) : NULL;
        unsigned o = 0;
        float v[3];
        if (!node || sscanf(text + 3, "%*s %*s %x %f %f %f", &o, &v[0], &v[1], &v[2]) != 4 || o > 0x200)
            return snprintf(out, size, "usage: !cp nodeset <object> <hex offset> x y z"), 1;
        memcpy(node + o, v, 12);
        return snprintf(out, size, "%s node +%x set", a1, o), 1;
    }
    if (!strcmp(cmd, "find") || !strcmp(cmd, "near")) {   /* "!cp find <text>", "!cp near <object> <m>": named objects (lab) */
        char *mgr = MGR, *c = NULL;
        void **data = mgr ? *(void ***)(mgr + 0x14) : NULL;
        int cnt = mgr ? *(int *)(mgr + 0x18) : 0, n = 0;
        float rad = 10.f, cp[3] = {0, 0, 0};
        sscanf(text + 3, "%*s %*s %f", &rad);
        if (!strcmp(cmd, "near") && (!(c = find_ent(a1)) || !ent_pos(c, cp)))
            return snprintf(out, size, "%s: none", a1), 1;
        for (int i = 0; data && i < cnt && n < size - 70; i++) {
            String nm = data[i] && !IsBadReadPtr(data[i], INST_NATIVE + 4) ? *(String *)((char *)data[i] + INST_NAME) : NULL;
            char *e = nm ? *(char **)((char *)data[i] + INST_NATIVE) : NULL;
            float p[3];
            if (!nm || IsBadReadPtr(nm, 8) || IsBadStringPtrA(nm->s, 200) || !nm->s[0])
                continue;
            if (c) {
                if (!e || IsBadReadPtr(e, ENT_INST + 4) || *(void **)(e + ENT_INST) != data[i] || !ent_pos(e, p) || (p[0] - cp[0]) * (p[0] - cp[0]) + (p[1] - cp[1]) * (p[1] - cp[1])
                    + (p[2] - cp[2]) * (p[2] - cp[2]) > rad * rad)
                    continue;
            } else if (!strstr(nm->s, a1))
                continue;
            n += snprintf(out + n, size - n, " %s;", nm->s);
        }
        return 1;
    }
    if (!strcmp(cmd, "vehs")) {   /* "!cp vehs": the vehicles of the stream (lab) */
        DWORD now = GetTickCount();
        int n = snprintf(out, size, "%d vehicle(s), sent %d (%d msgs, %d without peer), got %d (%d msgs, %d unknown):", n_veh,
                         veh_sent, veh_msgs_out, veh_nopeer, veh_got, veh_msgs_in, veh_unknown);
        for (int k = 0; k < n_veh && n < size - 80; k++) {
            Veh *v = &vehs[k];
            float pos[3] = {0, 0, 0};
            if (v->ent)
                ent_pos(v->ent, pos);
            n += snprintf(out + n, size - n, " %s%s%s%s (%.1f %.1f %.1f);", v->ent ? ent_name(v->ent) : "?",
                          v->ent && PLAYER && *(char **)(v->ent + VEH_USER) == PLAYER ? " used" : "", veh_mine(v, now) ? " mine" : "",
                          v->have && now - v->b_at < 1000 ? " recv" : "", pos[0], pos[1], pos[2]);
        }
        return 1;
    }
    if (!strcmp(cmd, "vts")) {   /* "!cp vts <text>": vtables of the world's entities whose name has <text> (lab) */
        char *b = ENT_BUCKETS;
        int nb = ENT_NBUCKETS, n = 0;
        for (int i = 0; b && i < nb && n < size - 90; i++)
            for (char *o = *(char **)(b + i * 12); o && n < size - 90; ) {
                void *inst = *(void **)(o + ENT_INST);   /* (not every entry has a script instance: check) */
                String s = inst && !IsBadReadPtr(inst, INST_NAME + 4) ? *(String *)((char *)inst + INST_NAME) : NULL;
                const char *nm = s && !IsBadReadPtr(s, 8) && !IsBadStringPtrA(s->s, 256) ? s->s : "";
                if (nm[0] && strstr(nm, a1))
                    n += snprintf(out + n, size - n, " %s vt %lx%s%s;", nm, (unsigned long)*(DWORD *)o,
                                  is_vehicle(o) ? " vehicle" : "", is_vehicle(o) && PLAYER
                                  && *(char **)(o + VEH_USER) == PLAYER ? " used" : "");
                char *next = *(char **)(o + 8);
                o = next ? next - 4 : NULL;
            }
        return 1;
    }
    if (!strcmp(cmd, "where")) {   /* "!cp where": this machine's player position (lab) */
        float pos[3] = {0, 0, 0};
        int ok = ent_pos(PLAYER, pos);
        return snprintf(out, size, "player %s at %.2f %.2f %.2f", ok ? "" : "(none)", pos[0], pos[1], pos[2]), 1;
    }
    if (!strcmp(cmd, "putin")) {   /* "!cp putin <domain> [flag]": this machine's player into the middle of a domain (lab) */
        char *dom = find_ent(a1), *pl = PLAYER;
        char *node = pl ? *(char **)(pl + ENT_NODE) : NULL;
        int flag = 0;
        sscanf(text + 3, "%*s %*s %d", &flag);
        if (!is_domain(dom) || !node)
            return snprintf(out, size, "%s is not a domain (or no player)", a1), 1;
        float *mn = (float *)(dom + DOM_MIN), *mx = (float *)(dom + DOM_MAX);
        float at[3] = {(mn[0] + mx[0]) / 2, (mn[1] + mx[1]) / 2 < mn[1] + 1.2f ? (mn[1] + mx[1]) / 2 : mn[1] + 1.2f, (mn[2] + mx[2]) / 2};
        float dir[3] = {*(float *)(node + NODE_DIR), *(float *)(node + NODE_DIR + 4), *(float *)(node + NODE_DIR + 8)};
        ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)pl)[PLAYER_PLACE / 4])(pl, at, dir, flag);
        float pos[3];
        ent_pos(pl, pos);
        return snprintf(out, size, "player placed (flag %d) at %.2f %.2f %.2f, now %.2f %.2f %.2f, inside %d", flag, at[0],
                        at[1], at[2], pos[0], pos[1], pos[2], inside(dom, pl)), 1;
    }
    if (!strcmp(cmd, "scan")) {   /* "!cp scan <object> x y z": where the entity and the memory it points to hold that position (lab) */
        char *e = find_ent(a1);
        float v[3];
        if (!e || sscanf(text + 3, "%*s %*s %f %f %f", &v[0], &v[1], &v[2]) != 3)
            return snprintf(out, size, "usage: !cp scan <object> x y z"), 1;
        int n = 0;
        for (int pass = 0; pass < 2 + 1; pass++) {   /* (the entity, its node, then every pointer field of the entity) */
            for (int pf = 0; pf < (pass == 2 ? 0x400 : 1); pf += 4) {
                char *base = pass == 0 ? e : pass == 1 ? *(char **)(e + ENT_NODE) : *(char **)(e + pf);
                if (!base || (pass == 2 && ((unsigned)base < 0x10000 || IsBadReadPtr(base, 0x400))) || IsBadReadPtr(base, 0x400))
                    continue;
                for (int o = 0; o < 0x400 && n < size - 60; o += 4) {
                    float *f = (float *)(base + o);
                    if (fabsf(f[0] - v[0]) < 0.06f && fabsf(f[1] - v[1]) < 0.06f && fabsf(f[2] - v[2]) < 0.06f)
                        n += snprintf(out + n, size - n, pass == 2 ? " [ent+%x]+%x;" : pass == 1 ? " node+%x;" : " ent+%x;",
                                      pass == 2 ? pf : o, o);
                }
            }
        }
        return 1;
    }
    if (!strcmp(cmd, "entset")) {   /* "!cp entset <object> <hex offset> x y z": write 3 floats into the entity itself (lab) */
        char *e = find_ent(a1);
        unsigned o = 0;
        float v[3];
        if (!e || sscanf(text + 3, "%*s %*s %x %f %f %f", &o, &v[0], &v[1], &v[2]) != 4 || o > 0xa00)
            return snprintf(out, size, "usage: !cp entset <object> <hex offset> x y z"), 1;
        memcpy(e + o, v, 12);
        return snprintf(out, size, "%s ent +%x set", a1, o), 1;
    }
    if (!strcmp(cmd, "ptrset")) {   /* "!cp ptrset <object> <hex ptr offset> <hex offset> x y z": 3 floats into an object the entity points to (lab) */
        char *e = find_ent(a1), *q;
        unsigned po = 0, o = 0;
        float v[3];
        if (!e || sscanf(text + 3, "%*s %*s %x %x %f %f %f", &po, &o, &v[0], &v[1], &v[2]) != 5 || po > 0x700 || o > 0x700)
            return snprintf(out, size, "usage: !cp ptrset <object> <hex ptr offset> <hex offset> x y z"), 1;
        q = *(char **)(e + po);
        if (!q || (unsigned)q < 0x10000 || IsBadReadPtr(q, o + 12))
            return snprintf(out, size, "%s: no object at +%x", a1, po), 1;
        memcpy(q + o, v, 12);
        return snprintf(out, size, "%s [+%x]+%x set", a1, po, o), 1;
    }
    if (!strcmp(cmd, "placeobj")) {   /* "!cp placeobj <object> x y z [flag]": the entity's own place() (vtable +0x210) (lab) */
        char *e = find_ent(a1), *node = e ? *(char **)(e + ENT_NODE) : NULL;
        float at[3], pos[3] = {0, 0, 0};
        int flag = 0;
        if (!node || sscanf(text + 3, "%*s %*s %f %f %f %d", &at[0], &at[1], &at[2], &flag) < 3)
            return snprintf(out, size, "usage: !cp placeobj <object> x y z [flag]"), 1;
        float dir[3] = {*(float *)(node + NODE_DIR), *(float *)(node + NODE_DIR + 4), *(float *)(node + NODE_DIR + 8)};
        ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)e)[PLAYER_PLACE / 4])(e, at, dir, flag);
        ent_pos(e, pos);
        return snprintf(out, size, "%s placed (flag %d), now %.2f %.2f %.2f", a1, flag, pos[0], pos[1], pos[2]), 1;
    }
    if (!strcmp(cmd, "dspeed")) {   /* "!cp dspeed <domain> [domain2 ...]": the force-field domains' speed vector (+0x100) (lab) */
        int n = 0;
        char names[8][64];
        int cnt = sscanf(text + 3, "%*s %63s %63s %63s %63s %63s %63s %63s %63s", names[0], names[1], names[2], names[3],
                         names[4], names[5], names[6], names[7]);
        for (int i = 0; i < cnt && n < size - 80; i++) {
            char *d = find_ent(names[i]);
            if (!is_domain(d) || IsBadReadPtr(d, 0x110))
                n += snprintf(out + n, size - n, "%s: none; ", names[i]);
            else
                n += snprintf(out + n, size - n, "%s: %.1f %.1f %.1f; ", names[i], (double)*(float *)(d + 0x100),
                              (double)*(float *)(d + 0x104), (double)*(float *)(d + 0x108));
        }
        return 1;
    }
    if (!strcmp(cmd, "at")) {   /* "!cp at x y z [object]": this machine's player to a point, facing the object (lab) */
        char *pl = PLAYER, *node = pl ? *(char **)(pl + ENT_NODE) : NULL, look[64] = "";
        float at[3], tp[3], pos[3];
        if (!node || sscanf(text + 3, "%*s %f %f %f %63s", &at[0], &at[1], &at[2], look) < 3)
            return snprintf(out, size, "usage: !cp at x y z [object]"), 1;
        float dir[3] = {*(float *)(node + NODE_DIR), *(float *)(node + NODE_DIR + 4), *(float *)(node + NODE_DIR + 8)};
        if (look[0] && ent_pos(find_ent(look), tp)) {
            float dx = tp[0] - at[0], dy = tp[1] - at[1], dz = tp[2] - at[2], l = sqrtf(dx * dx + dy * dy + dz * dz);
            if (l > 0.01f)
                dir[0] = dx / l, dir[1] = dy / l, dir[2] = dz / l;
        }
        ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)pl)[PLAYER_PLACE / 4])(pl, at, dir, 0);
        ent_pos(pl, pos);
        return snprintf(out, size, "player at %.2f %.2f %.2f (dir %.2f %.2f %.2f)", pos[0], pos[1], pos[2], dir[0], dir[1],
                        dir[2]), 1;
    }
    if (!strcmp(cmd, "gather") && r == MODE_SERVER) {
        char *dom = find_ent(a1);
        if (!is_domain(dom))
            return snprintf(out, size, "%s is not a domain", a1), 1;
        return snprintf(out, size, "%d client(s) sent into %s", gather(dom), a1), 1;
    }
    snprintf(out, size, "!cp state | rules | regroup | gather <domain> (host)");
    return 1;
}

/* ---- install ---- */
void campaign_install(void)
{
    char v[16];
    if (!ts_env("TS_CAMPAIGN", v, sizeof v) || v[0] != '1')
        return;
    if (ts_env("TS_CAMPAIGN_RULES", v, sizeof v) && v[0] == '0')
        use_rules = 0;
    if (ts_env("TS_CAMPAIGN_KILL", v, sizeof v) && v[0] == '0')
        use_kill = 0;
    if (ts_env("TS_CAMPAIGN_GATHER", v, sizeof v))
        gather_after = atoi(v) < 0 ? 0 : atoi(v);
    verbose = ts_env("TS_CAMPAIGN_LOG", v, sizeof v) && v[0] == '1';
    use_seats = !(ts_env("TS_CAMPAIGN_SEATS", v, sizeof v) && v[0] == '0');
    use_setspeed = !(ts_env("TS_CAMPAIGN_SETSPEED", v, sizeof v) && v[0] == '0');
    use_latejoin = !(ts_env("TS_CAMPAIGN_LATEJOIN", v, sizeof v) && v[0] == '0');
    use_ai = !(ts_env("TS_CAMPAIGN_AI", v, sizeof v) && v[0] == '0');
    use_routes = !(ts_env("TS_CAMPAIGN_ROUTES", v, sizeof v) && v[0] == '0');
    use_vehsync = !(ts_env("TS_CAMPAIGN_VEHSYNC", v, sizeof v) && v[0] == '0');
    if (ts_env("TS_CAMPAIGN_RIDE_HEIGHT", v, sizeof v))
        ride_height = (float)atof(v);
    if (!ts_tick_install() || !ts_frame_add(frame) || !coopnet_listen(SUB_GATHER, SUB_LAST, on_message)) {
        ts_log("campaign: off (no script tick, frame slot or mod messages)");
        return;
    }
    int n_hooked = 0;
    {
        static const unsigned char want_vg[6] = {0x83, 0xec, 0x28, 0x56, 0x8b, 0xf1};
        unsigned char r[6] = {0xe9, 0, 0, 0, 0, 0x90};
        *(DWORD *)(r + 1) = (DWORD)cp_veh_guard - (0x49bd70 + 5);
        if (!(ts_env("TS_CAMPAIGN_VEHGUARD", v, sizeof v) && v[0] == '0') && !ts_patch_bytes(0x49bd70, want_vg, r, 6))
            ts_log("campaign: vehicle guard site not as expected (other exe)");
    }
    for (int i = 0; i < N_HOOKS; i++) {
        const Hook *h = &hooks[i];
        if ((h->group == 1 && !use_latejoin) || (h->group == 2 && !use_ai))
            continue;
        unsigned char want[7] = {0x6a, 0xff, 0x68}, rep[7];
        memcpy(want + 3, &h->seh, 4);
        memset(rep, 0x90, sizeof rep);
        rep[0] = 0xe9;
        *(DWORD *)(rep + 1) = (DWORD)h->hook - (h->addr + 5);
        if (ts_patch_bytes(h->addr, want, rep, 7))
            n_hooked++;
        else
            ts_log("campaign: native at %08lx not as expected (other exe): not hooked", h->addr);
    }
    enabled = 1;
    ts_log("campaign: on (domain rules %s, gather after %d s, death domains %s, seats/rides %s, force"
           " fields on clients %s, late joiners %s, AI nearest player %s, vehicle stream %s, %d native hook(s))",
           use_rules ? "on" : "off",
           gather_after, use_kill ? "kill per player" : "as storysync", use_seats ? "on" : "off",
           use_setspeed ? "on" : "off", use_latejoin ? "on" : "off", use_ai ? "on" : "off", use_vehsync ? "on" : "off",
           n_hooked);
}
