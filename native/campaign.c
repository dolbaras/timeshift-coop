/*
 * Whole-campaign co-op rules on top of storysync.c (patch 1.2 exe only). NOT YET RUN IN A GAME: built and checked
 * against the executable and the level scripts only (mod/docs/campaign-audit.md). Off unless TS_CAMPAIGN=1; it acts
 * only in a co-op session whose host runs the story (storysync.c, COOP_F_STORYSYNC).
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
        return host_inside && clients_inside(dom, NULL);
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
static int ride_place;

static void on_message(int peer, unsigned short sub, const unsigned char *b, int len)
{
    (void)peer;
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
    } else if (sub == SUB_RIDE && len >= 4 && b[3] < sizeof ride_vehicle && 4 + b[3] <= len) {
        if (b[1]) {
            memcpy(ride_vehicle, b + 4, b[3]);
            ride_vehicle[b[3]] = 0;
            ride_place = b[2];
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
    float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
    if (l < 1e-3f)
        f[0] = 1.f, f[2] = 0.f, l = 1.f;
    float side = (ride_place % 2 ? 1.f : -1.f) * (0.8f + 0.8f * (float)(ride_place / 2));
    float at[3] = {pos[0] + side * f[2] / l, pos[1] + ride_height, pos[2] - side * f[0] / l};
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
static int seat_later_len[16];
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
        if (coopnet_send(i, SUB_SEAT, seat_later[i], seat_later_len[i]) > 0) {
            CLOG("campaign: seat sent to the client in slot %d now that it runs the level", i);
            seat_later_len[i] = 0;
        }
    }
}

void campaign_story_event(const char *obj, const char *ev, const char *arg0)
{
    if (!use_seats || role() != MODE_SERVER || !obj || !ev)
        return;
    char base[0x110];
    level_base(LEVEL_NAME, base, sizeof base);
    for (int r = 0; r < N_SEAT; r++) {
        const SeatRule *s = &seat_rules[r];
        if (_stricmp(s->level, base) || strcmp(s->ev, ev) || !name_match(s->obj, obj)
            || (s->arg[0] && _stricmp(s->arg, arg0 ? arg0 : "")))
            continue;
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
                }
                CLOG("campaign: %s.%s: client in slot %d takes %s (%s -> %s)", obj, ev, i, s->seat[k], ssl, res);
            } else if (s->act == A_RIDE_ON || s->act == A_RIDE_OFF) {
                int len = (int)strlen(s->vehicle);
                b[0] = PROTO;
                b[1] = s->act == A_RIDE_ON;
                b[2] = (unsigned char)k;
                b[3] = (unsigned char)len;
                memcpy(b + 4, s->vehicle, len);
                coopnet_send(i, SUB_RIDE, b, 4 + len);
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
int campaign_local_call(const char *name, int len)
{
    if (!enabled)
        return 0;
    if (use_setspeed && len == 8 && strncmp(name, "SetSpeed", 8) == 0)
        return 1;
    if (use_routes && ((len == 10 && strncmp(name, "StartRoute", 10) == 0) || (len == 9 && strncmp(name, "StopRoute", 9) == 0)))
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

/* ---- per frame (host) ---- */
static void frame(float dt)
{
    (void)dt;
    static DWORD last;
    DWORD now = GetTickCount();
    int r = role();
    if (r == MODE_CLIENT) {
        client_frame();
        return;
    }
    if (r != MODE_SERVER) {
        ride_vehicle[0] = 0;
        return;
    }
    if (use_latejoin)
        latejoin_step(now);
    if (use_seats)
        seat_later_step();
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
    if (ts_env("TS_CAMPAIGN_RIDE_HEIGHT", v, sizeof v))
        ride_height = (float)atof(v);
    if (!ts_tick_install() || !ts_frame_add(frame) || !coopnet_listen(SUB_GATHER, SUB_LAST, on_message)) {
        ts_log("campaign: off (no script tick, frame slot or mod messages)");
        return;
    }
    int n_hooked = 0;
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
    ts_log("campaign: on (NOT YET RUN: domain rules %s, gather after %d s, death domains %s, seats/rides %s, force"
           " fields on clients %s, late joiners %s, AI nearest player %s, %d native hook(s))", use_rules ? "on" : "off",
           gather_after, use_kill ? "kill per player" : "as storysync", use_seats ? "on" : "off",
           use_setspeed ? "on" : "off", use_latejoin ? "on" : "off", use_ai ? "on" : "off", n_hooked);
}
