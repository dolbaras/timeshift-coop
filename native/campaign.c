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
 *   75 server -> clients     the ridden vehicle's place;  76 vehicle transforms (both ways, the host relays a client's)
 *   77 server -> clients     an actor's ShowMsg / PlaySound line started (voice, see below)
 *   78 server -> clients     the host's player's DisableMove / DisableInput (lock watchdog)
 *   79 both ways             dynamic actor digest: bucket hashes, request, details
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
 *   that state (message 72, 3, 10 and 25 s after; the loadout only the first time); one that starts earlier gets it
 *   (without the loadout) when the scripts changed it while the client still loaded (latejoin.h).
 * - AI targets (host): SetAim/SetTarget("player") of AI, turrets and the boss name the nearest player instead of the
 *   host; remote players get a script name for that (coop_player<slot>).
 *
 * 0.1.1 (docs/campaign-audit.md section 3d): voice lines of actors the host creates, lock watchdog, client-driven quads (the player's
 * own record of its vehicle at +0x37c), loadout carry-over / respawn restore, digest of the dynamic actors, AI "nearest" skips the dead,
 * campaign_regroup() and campaign_gate_waiting() for the in-game menu (coopsession.c), spread teleports (off). Switches:
 * TS_CAMPAIGN_VOICE / _VOICELOG, _LOCKS, _LOCKWATCH=<s>, _LOADOUT, _DIGEST, _DIGEST_FIX=1, _SPREAD=1. Lab commands: !cp life, loadout,
 * wpn, pused, rd, vslot, dyn, vehs.
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
#include "latejoin.h"

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
#define PLAYER_USED 0x23c                    /* player vtable: "uses something" (what InBike() asks first); the object is *(player+0x37c) */
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
#define SUB_VOICE 77                         /* server -> clients: an actor's ShowMsg / PlaySound line started */
#define SUB_LOCKS 78                         /* server -> clients: the host's player's DisableMove / DisableInput (watchdog) */
#define SUB_DIGEST 79                        /* dynamic actor digest (both ways, see dyn_message) */
#define SUB_LAST 79
#define PROTO 1

/* ---- settings ---- */
static int enabled, use_rules = 1, use_kill = 1, verbose;
static int gather_after = 20;                /* s; 0 = no automatic gather */
static int use_seats = 1, use_setspeed = 1, use_latejoin = 1, use_ai = 1, use_routes = 1;
static float ride_height = 2.2f;             /* m above the ridden vehicle's origin */
static int lock_after = 12;                  /* s: a lock the host's player does not have is cleared after this long */
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
    int missing;                             /* (ALL) clients missing while the host waits (0 = not waiting), 4 Hz */
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

/* a player's life (+0x234, 100 at full; the same field the split screen and Continue read); dead = 0 or less */
static int player_alive(char *ent)
{
    return ent && !IsBadReadPtr(ent + 0x234, 4) && *(float *)(ent + 0x234) > 0.f;
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

static char *split_p2(void);

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
    /* the local player 2 (split screen on this machine, or a client's player 2 run here) is a player of the gate too */
    char *p2e = split_p2();
    if (p2e && p2e != PLAYER && player_alive(p2e) && !inside(dom, p2e)) {
        all = 0;
        if (missing)
            (*missing)++;
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
        if (ent && !player_alive(ent))   /* (a dead client respawns at the checkpoint: it is pulled in when it is back) */
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

static void veh_receive(int peer, const unsigned char *b, int len);
static void dyn_message(int peer, const unsigned char *b, int len);
static void voice_receive(const unsigned char *b, int len);
static void voice_client_step(void);
static void locks_receive(const unsigned char *b, int len);
static void lock_step(DWORD now);
static void spread_step(DWORD now);
static void loadout_step(DWORD now);
static void on_message(int peer, unsigned short sub, const unsigned char *b, int len)
{
    if (sub == SUB_VEH && enabled && len >= 2 && b[0] == PROTO && (role() == MODE_CLIENT || role() == MODE_SERVER)) {
        veh_receive(peer, b, len);
        return;
    }
    if (sub == SUB_DIGEST && enabled && len >= 2 && b[0] == PROTO && (role() == MODE_CLIENT || role() == MODE_SERVER)) {
        dyn_message(peer, b, len);
        return;
    }
    if (role() != MODE_CLIENT || len < 1 || b[0] != PROTO)
        return;
    if (sub == SUB_VOICE) {
        voice_receive(b, len);
        return;
    }
    if (sub == SUB_LOCKS) {
        locks_receive(b, len);
        return;
    }
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
    voice_client_step();
    lock_step(GetTickCount());
    spread_step(GetTickCount());
    loadout_step(GetTickCount());
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
static void *(*type_float)(void) = (void *)0x9c8e10;
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

/* (client) what the scripts (the host's replays included) last set on this machine's own player, for the lock watchdog */
static int use_locks = 1, use_spread;   /* (spread teleports: off, the engine pushes stacked bodies apart itself, see the audit) */
static struct { int move, input; DWORD move_at, input_at; } mine;
static int tracking_client(void *obj)
{
    return use_locks && obj && obj == (void *)PLAYER && role() == MODE_CLIENT;
}

static void TSCALL hk_input(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj))
        hst.input = arg_bool(argc, argv, 0, 1), hst.mask |= 2;
    else if (tracking_client(obj))
        mine.input = arg_bool(argc, argv, 0, 1), mine.input_at = GetTickCount();
    cp_t_input(obj, argc, argv, res, self);
}

static void TSCALL hk_move(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj))
        hst.move = arg_bool(argc, argv, 0, 1), hst.mask |= 1;
    else if (tracking_client(obj))
        mine.move = arg_bool(argc, argv, 0, 1), mine.move_at = GetTickCount();
    cp_t_move(obj, argc, argv, res, self);
}

/* a teleport of this machine's player (a replayed Transport): afterwards, when it stands on another player, it steps aside */
TRAMP("cp_t_transport", "0x438a47", "0xc79190");
extern void TSCALL cp_t_transport(void *, int, Variant *, Variant *, void *) __asm__("cp_t_transport");
static DWORD spread_at;
static void TSCALL hk_transport(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    cp_t_transport(obj, argc, argv, res, self);
    if (use_spread && obj && obj == (void *)PLAYER && role() == MODE_CLIENT)
        spread_at = GetTickCount() + 400;
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

/* This machine's player's loadout as far as it is known (TS_CAMPAIGN_LOADOUT=0 off): the last SetupWeapon(a, b, c) the scripts
 * ran on it (a weapon SET chosen by number: live, each number gives another set object) and the ammunition of the primary
 * weapon, sampled every 2 s on foot. A client that respawns (the stock multiplayer respawn hands out the start weapon: live,
 * 240/30 -> 128/16) gets its set and ammunition back 1.5 s after it is alive again; at the start of the next level of the session
 * the set and ammunition of the previous one come back 12 s into it unless the level's own scripts set a loadout meanwhile (levels
 * 1, 2, 24 take the weapons away at their start). Weapons picked up in play are not part of a set number: they are lost on a
 * respawn as before. */
static int use_loadout = 1;
static struct {
    int have_wpn, wpn[3], level_set, have_ammo, mag, was_alive, died, apply_kind;
    float total;
    DWORD snap_at, apply_at;
    char level[0x110];
} lo;

static void TSCALL hk_wpn(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (recording(obj) && argc >= 3 && argv) {
        for (int i = 0; i < 3; i++)
            hst.wpn[i] = argv[i].type == type_int() ? (int)argv[i].data : 0;
        hst.mask |= 64;
    }
    if (use_loadout && obj && obj == (void *)PLAYER && role() && argc >= 3 && argv) {
        for (int i = 0; i < 3; i++)
            lo.wpn[i] = argv[i].type == type_int() ? (int)argv[i].data : 0;
        lo.have_wpn = 1;
        lo.level_set = 1;
        lo.have_ammo = 0;   /* (a new set: the old ammunition is not its) */
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

/* The second player this machine runs (split screen on a host or in single player, a client's player 2 on its host), or NULL. */
static char *split_p2(void);

/* (host, or single player with split screen) "player" in an AI aim/target call -> the nearest player's name. 1 = argv[0]
 * was swapped (restore after). The candidates: the machine's own player ("player"), the local player 2 ("player2", the
 * name split.c gave it) and the clients' players ("coop_player<slot>"). */
static int swap_player_arg(void *obj, int argc, Variant *argv, String *saved, String *mine)
{
    int r = role();
    char *p2e = r != MODE_CLIENT ? split_p2() : NULL;
    if (!use_ai || (r != MODE_SERVER && !p2e) || argc < 1 || !argv || argv[0].type != type_str() || !argv[0].data
        || _stricmp(((String)argv[0].data)->s, "player") != 0)
        return 0;
    float me[3], p[3];
    if (!ent_pos(obj, me))
        return 0;
    /* the nearest LIVING player (the machine's own counts as "player"); nobody alive: the call stays as the script wrote it */
    float best = 3.4e38f;
    int best_slot = -1, best_p2 = 0;
    char *best_ent = NULL;
    if (player_alive(PLAYER) && ent_pos(PLAYER, p))
        best = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
    if (p2e && p2e != PLAYER && player_alive(p2e) && ent_pos(p2e, p)) {
        float d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d < best)
            best = d, best_p2 = 1;
    }
    for (int i = 0; r == MODE_SERVER && slot_at(i); i++) {
        char *s = slot_at(i), *ent = *(char **)(s + 4);
        if (*(int *)s == 0 || !slot_joined(s) || !player_alive(ent) || !ent_pos(ent, p))
            continue;
        float d = (p[0] - me[0]) * (p[0] - me[0]) + (p[1] - me[1]) * (p[1] - me[1]) + (p[2] - me[2]) * (p[2] - me[2]);
        if (d < best)
            best = d, best_slot = i, best_ent = ent, best_p2 = 0;
    }
    char name[48];
    if (best_p2)
        snprintf(name, sizeof name, "player2");
    else if (best_slot < 0 || !name_remote(best_slot, best_ent, name, sizeof name)) {
        VLOG("campaign: %s aims at \"player\" (nearest living player is the machine's own, %.1f m)", ent_name(obj),
             best < 3.4e38f ? sqrt(best) : -1.0);
        return 0;   /* (the machine's own player is the nearest living one, or nobody lives: "player" stays) */
    }
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

/* ---- actors' voice lines (host -> clients, TS_CAMPAIGN_VOICE, default on) ----
 * ShowMsg(strId, time, usage) and PlaySound(snd, obj) are natives of every level object (0x647d40, 0x6478e0: virtual
 * +0xc4 / +0xb8). Their handlers are "local" for the story sync (presentation only), so each machine runs its own, and a
 * client's copy fails when the actor is one the host created in play (`$farm.Create`: the name arrives from aisync a
 * moment later; "unknown object"): Cooke and the rebels of the intros of ts2_zepride1_part01 and ts2_zepride2_part01
 * (campaign_wrap_call skips the client's copy there), `Mitchell` on ts2_zep_factory_part2, the prison rebels of
 * ts2_zepcrashsite_part03, ... (survey in the audit). So the host tells the clients every such line the moment it
 * STARTS (the hook runs when a delayed `:N` line fires): message 77 {u8 1, u8 kind (0 ShowMsg, 1 PlaySound), name, string,
 * f32 time, string usage/object}. A client waits 1.2 s, skips the line when it heard the same one meanwhile (its own
 * copy of the handler ran, or an earlier message) within 30 s - and a local copy that comes after a played message is
 * suppressed the same way - else plays it on its copy of the actor as soon as the actor has its name (up to 12 s).
 * Players, sound managers/actors ("snd*") and music are left out; at most 12 messages a second, the same line once a
 * second. Log: TS_CAMPAIGN_VOICELOG=1 lists every ShowMsg/PlaySound call on both machines ("voice:" lines: the audit
 * of which lines the clients play) and what the client did with each message. */
TRAMP("cp_t_showmsg", "0x647d47", "0xc7eba8");
TRAMP("cp_t_playsnd", "0x6478e7", "0xc7ec28");
extern void TSCALL cp_t_showmsg(void *, int, Variant *, Variant *, void *) __asm__("cp_t_showmsg");
extern void TSCALL cp_t_playsnd(void *, int, Variant *, Variant *, void *) __asm__("cp_t_playsnd");

static unsigned name_hash(const char *s);
static int use_voice = 1, voice_log;
static LONG voice_logged;
#define VOLOG(...) do { if (voice_log && InterlockedIncrement(&voice_logged) <= 6000) ts_log(__VA_ARGS__); } while (0)
static char *split_p2(void) { return (char *)split_player2(); }

static int voice_skip_name(const char *n)
{
    return !n[0] || !strncmp(n, "snd", 3) || !strncmp(n, "sound", 5) || !strncmp(n, "music", 5) || !strncmp(n, "Music", 5);
}

static int is_player_ent(char *e)
{
    if (!e || e == PLAYER || e == split_p2())
        return 1;
    for (int i = 0; slot_at(i); i++)
        if (*(char **)(slot_at(i) + 4) == e)
            return 1;
    return 0;
}

static const char *var_str(int argc, Variant *argv, int i)
{
    if (argc <= i || !argv || argv[i].type != type_str() || !argv[i].data || IsBadReadPtr((void *)argv[i].data, 16))
        return "";
    return ((String)argv[i].data)->s;
}

static float var_float(int argc, Variant *argv, int i)
{
    float f = 0.f;
    if (argc > i && argv && argv[i].type == type_float())
        memcpy(&f, &argv[i].data, 4);
    else if (argc > i && argv && argv[i].type == type_int())
        f = (float)(int)argv[i].data;
    return f;
}

/* the lines this machine heard lately: (name, kind, string) hash -> tick */
#define HEARD_MAX 128
static struct { unsigned key; DWORD at; } heard[HEARD_MAX];
static int heard_next;
#define HEARD_MS 30000

static unsigned voice_key(const char *name, int kind, const char *id)
{
    return name_hash(name) * 31u + name_hash(id) + (unsigned)kind * 7u;
}

static int heard_recently(unsigned key, DWORD now)
{
    for (int i = 0; i < HEARD_MAX; i++)
        if (heard[i].key == key && heard[i].at && now - heard[i].at < HEARD_MS)
            return 1;
    return 0;
}

static void heard_set(unsigned key, DWORD now)
{
    for (int i = 0; i < HEARD_MAX; i++)
        if (heard[i].key == key) {
            heard[i].at = now;
            return;
        }
    heard[heard_next].key = key;
    heard[heard_next].at = now;
    heard_next = (heard_next + 1) % HEARD_MAX;
}

static DWORD voice_ignore_until;   /* (the client's own replay of a forwarded line passes the hook again) */
static int voice_sent, voice_played, voice_dup, voice_dropped, voice_calls;

/* (host) tell the clients; (client) remember it as heard, or drop a copy that was played from a message just before.
 * Returns 1 = the client must not run the native (heard a moment ago). */
static int voice_call(void *obj, int kind, int argc, Variant *argv)
{
    int r = role();
    if (!use_voice || !r || !obj || IsBadReadPtr(obj, ENT_NODE + 4))
        return 0;
    const char *name = ent_name(obj), *id = var_str(argc, argv, 0);
    if (!id[0] || is_player_ent(obj))
        return 0;
    DWORD now = GetTickCount();
    voice_calls++;
    VOLOG("voice: %s %s.%s(\"%s\")%s", r == MODE_SERVER ? "host" : "client", name, kind ? "PlaySound" : "ShowMsg", id,
          now < voice_ignore_until ? " [from the host's message]" : "");
    if (voice_skip_name(name))
        return 0;
    unsigned key = voice_key(name, kind, id);
    if (r == MODE_CLIENT) {
        if (now < voice_ignore_until)
            return 0;
        if (heard_recently(key, now)) {   /* (played from the host's message a moment ago) */
            voice_dup++;
            VOLOG("voice: client skips its own %s.%s(\"%s\"): played from the host's message", name, kind ? "PlaySound" : "ShowMsg", id);
            return 1;
        }
        heard_set(key, now);
        return 0;
    }
    /* host: at most 12 messages a second, the same line once a second */
    static DWORD sec_at;
    static int sec_n;
    static struct { unsigned key; DWORD at; } last[16];
    static int last_next;
    if (now - sec_at >= 1000)
        sec_at = now, sec_n = 0;
    if (sec_n >= 12)
        return 0;
    for (int i = 0; i < 16; i++)
        if (last[i].key == key && now - last[i].at < 1000)
            return 0;
    last[last_next].key = key;
    last[last_next].at = now;
    last_next = (last_next + 1) % 16;
    unsigned char b[250];
    int n = 0, nl = (int)strlen(name), il = (int)strlen(id);
    const char *extra = kind ? var_str(argc, argv, 1) : var_str(argc, argv, 2);
    int el = (int)strlen(extra);
    float t = kind ? 0.f : var_float(argc, argv, 1);
    if (nl > 80 || il > 80 || el > 40)
        return 0;
    b[n++] = PROTO;
    b[n++] = (unsigned char)kind;
    b[n++] = (unsigned char)nl;
    memcpy(b + n, name, nl);
    n += nl;
    b[n++] = (unsigned char)il;
    memcpy(b + n, id, il);
    n += il;
    memcpy(b + n, &t, 4);
    n += 4;
    b[n++] = (unsigned char)el;
    memcpy(b + n, extra, el);
    n += el;
    sec_n++;
    if (coopnet_send(-1, SUB_VOICE, b, n) > 0)
        voice_sent++;
    return 0;
}

static void TSCALL hk_showmsg(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (voice_call(obj, 0, argc, argv))
        return;
    cp_t_showmsg(obj, argc, argv, res, self);
}

static void TSCALL hk_playsnd(void *obj, int argc, Variant *argv, Variant *res, void *self)
{
    if (voice_call(obj, 1, argc, argv))
        return;
    cp_t_playsnd(obj, argc, argv, res, self);
}

/* (client) messages wait here for the script tick */
#define VQ_MAX 16
static struct { int kind; char name[84], id[84], extra[44]; float t; DWORD at; } vq[VQ_MAX];
static volatile int vq_head, vq_count;

static void voice_receive(const unsigned char *b, int len)
{
    int n = 1, kind;
    char name[84], id[84], extra[44];
    float t;
    int l;
    if (len < 4 || vq_count >= VQ_MAX)
        return;
    kind = b[n++] ? 1 : 0;
    l = b[n++];
    if (n + l + 1 > len || l >= (int)sizeof name)
        return;
    memcpy(name, b + n, l);
    name[l] = 0;
    n += l;
    l = b[n++];
    if (n + l + 5 > len || l >= (int)sizeof id)
        return;
    memcpy(id, b + n, l);
    id[l] = 0;
    n += l;
    memcpy(&t, b + n, 4);
    n += 4;
    l = b[n++];
    if (n + l > len || l >= (int)sizeof extra)
        return;
    memcpy(extra, b + n, l);
    extra[l] = 0;
    if (strchr(name, '"') || strchr(id, '"') || strchr(extra, '"') || strchr(id, '\\') || strchr(extra, '\\'))
        return;
    int slot = (vq_head + vq_count) % VQ_MAX;
    vq[slot].kind = kind;
    strcpy(vq[slot].name, name);
    strcpy(vq[slot].id, id);
    strcpy(vq[slot].extra, extra);
    vq[slot].t = t;
    vq[slot].at = GetTickCount();
    vq_count++;
}

static int named_quiet(const char *name);
static void voice_client_step(void)
{
    static DWORD polled;
    DWORD now = GetTickCount();
    if (!vq_count || now - polled < 250)
        return;
    polled = now;
    int n = vq_count, keep = 0;
    for (int i = 0; i < n; i++) {
        int k = (vq_head + i) % VQ_MAX;
        if (now - vq[k].at < 1200) {   /* (the client's own copy of the handler may still be coming) */
            vq[(vq_head + keep++) % VQ_MAX] = vq[k];
            continue;
        }
        unsigned key = voice_key(vq[k].name, vq[k].kind, vq[k].id);
        const char *what = vq[k].kind ? "PlaySound" : "ShowMsg";
        if (heard_recently(key, now)) {
            voice_dup++;
            VOLOG("voice: client heard %s.%s(\"%s\") itself, host's message skipped", vq[k].name, what, vq[k].id);
        } else if (named_quiet(vq[k].name)) {
            char ssl[300], res[120];
            if (vq[k].kind)
                snprintf(ssl, sizeof ssl, vq[k].extra[0] ? "$%s.PlaySound(\"%s\", \"%s\")" : "$%s.PlaySound(\"%s\")", vq[k].name,
                         vq[k].id, vq[k].extra);
            else if (vq[k].extra[0])
                snprintf(ssl, sizeof ssl, "$%s.ShowMsg(\"%s\", %g, \"%s\")", vq[k].name, vq[k].id, (double)vq[k].t, vq[k].extra);
            else
                snprintf(ssl, sizeof ssl, "$%s.ShowMsg(\"%s\", %g)", vq[k].name, vq[k].id, (double)vq[k].t);
            heard_set(key, now);
            voice_ignore_until = now + 400;
            ts_script_run(ssl, res, sizeof res);
            voice_ignore_until = GetTickCount() + 50;
            voice_played++;
            VOLOG("voice: client plays %s.%s(\"%s\") from the host's message -> %s", vq[k].name, what, vq[k].id, res);
        } else if (now - vq[k].at < 12000) {   /* (the actor has no name here yet: keep waiting) */
            vq[(vq_head + keep++) % VQ_MAX] = vq[k];
        } else {
            voice_dropped++;
            VOLOG("voice: client dropped %s.%s(\"%s\"): no such object here after 12 s", vq[k].name, what, vq[k].id);
        }
    }
    vq_count = keep;
}

/* ---- lock watchdog (client) ----
 * A DisableMove / DisableInput a replayed story event put on this machine's player stays until the matching release
 * arrives; a release the client missed (a replay dropped, a late join, a level restarted under it) left the player stuck
 * for good. The host tells the clients once a second what its own player's scripts last set (message 78: move, input);
 * a client whose own player holds a lock the host's player does not hold for `lock_after` seconds releases it itself
 * (and again every `lock_after` seconds while the difference stays: the lock may be a counter). TS_CAMPAIGN_LOCKS=0 off,
 * TS_CAMPAIGN_LOCKWATCH=<s> the delay (default 12). */
static struct { int move, input; DWORD at; } host_locks;

static void locks_receive(const unsigned char *b, int len)
{
    if (len < 3)
        return;
    host_locks.move = b[1];
    host_locks.input = b[2];
    host_locks.at = GetTickCount();
}

static void locks_send(DWORD now)   /* (host, 1 Hz) */
{
    static DWORD last;
    if (!use_locks || now - last < 1000)
        return;
    last = now;
    unsigned char b[4] = {PROTO, (unsigned char)hst.move, (unsigned char)hst.input, 0};
    coopnet_send(-1, SUB_LOCKS, b, sizeof b);
}

static int lock_released;
static void lock_step(DWORD now)
{
    static DWORD since_move, since_input;
    if (!use_locks || !PLAYER || !host_locks.at || now - host_locks.at > 4000) {
        since_move = since_input = 0;
        return;
    }
    char res[120];
    if (mine.move && !host_locks.move) {
        if (!since_move)
            since_move = now;
        else if (now - since_move >= (DWORD)lock_after * 1000) {
            since_move = now;
            ts_script_run("$player.DisableMove(0)", res, sizeof res);
            lock_released++;
            CLOG("campaign: lock watchdog: movement locked for %d s, the host's player is free: released (%s)", lock_after, res);
        }
    } else
        since_move = 0;
    if (mine.input && !host_locks.input) {
        if (!since_input)
            since_input = now;
        else if (now - since_input >= (DWORD)lock_after * 1000) {
            since_input = now;
            ts_script_run("$player.DisableInput(0)", res, sizeof res);
            lock_released++;
            CLOG("campaign: lock watchdog: input locked for %d s, the host's player is free: released (%s)", lock_after, res);
        }
    } else
        since_input = 0;
}

/* ---- spread teleports (client) ----
 * A replayed Transport puts every player on the same locator; a client that finds itself on another player's body
 * 0.4 s after its own teleport steps to its own place around that player (0.7 m ring, as the gather does; ring = its
 * number among the joined clients). */
#define NET_CLIENT_OBJ (*(char **)0xfcab38)
static int spread_count;
static void spread_step(DWORD now)
{
    if (!spread_at || (int)(now - spread_at) < 0)
        return;
    spread_at = 0;
    char *pl = PLAYER, *gm = GAME_MODE_OBJ, *nc = NET_CLIENT_OBJ;
    float me[3], ap[3];
    if (!pl || !gm || !nc || !ent_pos(pl, me))
        return;
    int myid = *(int *)(nc + 0x2c), ring = 0, k = 0, other = -1;
    for (int i = 0; slot_at(i); i++) {
        char *s = slot_at(i);
        if (*(int *)s == 0 || !*(char **)(s + 4))   /* (a client sees no "joined" flags in the slots: players with a body count) */
            continue;
        k++;
        if (*(int *)s == myid)
            ring = k;
    }
    for (int i = 0; slot_at(i); i++) {
        char *s = slot_at(i), *ent = *(char **)(s + 4);
        float q[3];
        if (!ent || ent == pl || *(int *)s == myid || !ent_pos(ent, q))
            continue;
        if ((q[0] - me[0]) * (q[0] - me[0]) + (q[2] - me[2]) * (q[2] - me[2]) < 0.45f * 0.45f && fabsf(q[1] - me[1]) < 2.0f) {
            memcpy(ap, q, sizeof ap);
            other = i;
            break;
        }
    }
    if (other < 0 || ring < 1)
        return;
    ap[1] -= 0.3f;   /* (place_local lifts by 0.3 m) */
    place_local(ap, ring, NULL, NULL);
    spread_count++;
    CLOG("campaign: after a teleport this player stood on the one in slot %d: moved to place %d around it", other, ring);
}

/* (host) clients still missing at a lift/train gate the host's player waits at right now; 0 = no gate waited at, on a
 * client, outside a co-op campaign session (chunk E shows "waiting for N player(s)" from it) */
int campaign_gate_waiting(void)
{
    int n = 0;
    if (role() != MODE_SERVER)
        return 0;
    for (int k = 0; k < n_act; k++)
        if (act[k].r->rule == R_ALL && act[k].missing > n)
            n = act[k].missing;
    return n;
}

/* The in-game "regroup" (E's menu entry / key calls this): the host brings every client (alive) to its player, a client
 * brings itself to the host's player. Returns the number of players moved, -1 = not in a co-op session of this module. */
int campaign_regroup(void)
{
    int r = role();
    if (r == MODE_SERVER)
        return gather(NULL);
    if (r == MODE_CLIENT) {
        char *s = slot_at(0);
        float pos[3];
        if (!s || *(int *)s != 0 || !ent_pos(*(char **)(s + 4), pos))
            return 0;
        place_local(pos, 1, NULL, NULL);
        return 1;
    }
    return -1;
}

static void loadout_step(DWORD now)
{
    char *pl = PLAYER;
    if (!use_loadout || !role() || !pl)
        return;
    int alive = player_alive(pl);
    void **vt = IsBadReadPtr(pl, 4) ? NULL : *(void ***)pl;
    int on_foot = vt && ((int (TSCALL *)(void *))vt[PLAYER_USED / 4])(pl) == 0;
    if (strcmp(lo.level, LEVEL_NAME) != 0) {   /* a new level in the session (or the first one) */
        int first = !lo.level[0];
        snprintf(lo.level, sizeof lo.level, "%s", LEVEL_NAME);
        lo.level_set = 0;
        lo.was_alive = 0;
        lo.died = 0;
        if (!first && lo.have_wpn) {
            lo.apply_at = now + 12000;
            lo.apply_kind = 1;
        } else
            lo.apply_at = 0;
    }
    if (lo.was_alive && !alive)
        lo.died = 1;
    if (!lo.was_alive && alive && lo.died && lo.have_wpn) {   /* respawned */
        lo.died = 0;
        lo.apply_at = now + 1500;
        lo.apply_kind = 2;
    }
    lo.was_alive = alive;
    if (alive && on_foot && now - lo.snap_at >= 2000 && !lo.apply_at) {
        lo.snap_at = now;
        float t;
        int m;
        if (lo.have_wpn && split_ammo_get((unsigned char *)pl, &t, &m) && t > 0.f) {
            lo.total = t;
            lo.mag = m;
            lo.have_ammo = 1;
        }
    }
    if (lo.apply_at && (int)(now - lo.apply_at) >= 0) {
        int kind = lo.apply_kind;
        lo.apply_at = 0;
        if (!alive || !on_foot || (kind == 1 && lo.level_set))
            return;
        if (!lo.wpn[0] && !lo.wpn[1] && !lo.wpn[2]) {   /* the last SetupWeapon(0, 0, 0) TOOK the weapons away (levels 1, 2, 24, a
                                                         * restart's start): putting that back would strip the respawn's start pistol */
            CLOG("campaign: loadout %s: the last set was 0 0 0 (weapons taken away), nothing to put back",
                 kind == 2 ? "after the respawn" : "carried over from the last level");
            return;
        }
        char ssl[100], res[100];
        snprintf(ssl, sizeof ssl, "$player.SetupWeapon(%d, %d, %d)", lo.wpn[0], lo.wpn[1], lo.wpn[2]);
        ts_script_run(ssl, res, sizeof res);
        lo.level_set = 0;   /* (that was ours, not the level's) */
        int ok = lo.have_ammo && split_ammo_set((unsigned char *)pl, lo.total, lo.mag);
        CLOG("campaign: loadout %s: weapon set %d %d %d%s (%.0f/%d)", kind == 2 ? "after the respawn" : "carried over from the last level",
             lo.wpn[0], lo.wpn[1], lo.wpn[2], ok ? " with its ammunition" : "", lo.have_ammo ? (double)lo.total : -1.0, lo.mag);
    }
}

typedef struct { DWORD addr, seh; void *hook; int group; } Hook;   /* group 1 late join / lock watchdog, 2 AI, 3 voice, 4 spread teleports */
static const Hook hooks[] = {
    {0x647d40, 0xc7eba8, (void *)hk_showmsg, 3}, {0x6478e0, 0xc7ec28, (void *)hk_playsnd, 3},
    {0x438a40, 0xc79190, (void *)hk_transport, 4},
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

/* ---- late joiners (host): a client that joins a level running for more than 20 s gets the host's player state; one that
 * runs it earlier gets what the scripts changed on the host's player while it still loaded (latejoin.h) ---- */
static DWORD level_start;
static struct { int id; DWORD seen; int sends, early; } known[16];
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
            /* (a client there from the level start needs nothing; a late joiner gets 3 sends, at 3, 10 and 25 s; an early one
             * the same when the scripts already changed the host's player while it loaded: their replays reached nobody) */
            known[k].sends = latejoin_plan(now - level_start, hst.mask, &known[k].early);
            if (known[k].sends)
                CLOG("campaign: client %d runs the level %lu s into it: it gets the host's player state%s (mask %x)", id,
                     (now - level_start) / 1000, known[k].early ? " set while it loaded" : "", hst.mask);
            n_known++;
        }
        static const DWORD at[3] = {25000, 10000, 3000};
        if (known[k].sends > 0 && now - known[k].seen >= at[known[k].sends - 1] && hst.mask) {
            unsigned char b[12] = {PROTO, (unsigned char)latejoin_bits(hst.mask, known[k].early, known[k].sends == 3),
                                   (unsigned char)hst.move, (unsigned char)hst.input,
                                   (unsigned char)hst.aim, (unsigned char)hst.tc[0], (unsigned char)hst.tc[1],
                                   (unsigned char)hst.tc[2], (unsigned char)hst.wpn[0], (unsigned char)hst.wpn[1],
                                   (unsigned char)hst.wpn[2], 0};
            /* (the loadout only once, to late joiners: later pickups must not be replaced) */
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

/* The intro dialogue of levels 8 and 23: the handler's delayed ShowMsg lines talk to Intro_Cooke / Intro_rebel_*, which the
 * host creates in the same handler; a client gets their names a moment after its own copy of the handler ran, so every
 * line logged "unknown object" (up to 53 per run). The client skips these lines (the host's own run plays them). */
int campaign_wrap_call(const char *stmt)
{
    static const char *const lines[] = {"$Intro_Cooke.ShowMsg", "$Intro_rebel_1.ShowMsg", "$Intro_rebel_2.ShowMsg",
                                      "$Intro_Cooke.Die", "$Intro_rebel_1.Die", "$Intro_rebel_2.Die", NULL};
    char base[0x110];
    if (!enabled || !stmt)
        return 0;
    level_base(LEVEL_NAME, base, sizeof base);
    if (_stricmp(base, "ts2_zepride1_part01") != 0 && _stricmp(base, "ts2_zepride2_part01") != 0)
        return 0;
    for (int i = 0; lines[i]; i++)
        if (strstr(stmt, lines[i]))
            return 1;
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
/* The vehicle this machine's player is on: the player's own record of what it uses, `*(player + 0x37c)` (live: it holds the
 * entity of the quad after StartUse, and InBike() = vtable +0x23c "uses something" + a class test on that object); a quad the level
 * script attached (`$start_bike.AttachPlayer()`) is the same. The vehicle's own +0x9c0 is NOT a user pointer (live values 0, 1,
 * 0x3c, 0x40, floats), the first version of the stream looked there and never saw a driver. */
static int veh_is_mine(char *ent)
{
    char *pl = PLAYER;
    if (!pl || !ent || IsBadReadPtr(pl + 0x380, 4))
        return 0;
    void **vt = *(void ***)pl;
    return *(char **)(pl + 0x37c) == ent && vt && ((int (TSCALL *)(void *))vt[PLAYER_USED / 4])(pl) != 0;
}

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
    char *last_ent;                          /* the entity of the previous scan (another one = the level restarted: forget the data) */
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
                    if (k < n_veh) {
                        if (vehs[k].last_ent && vehs[k].last_ent != o)
                            vehs[k].have = 0;
                        vehs[k].last_ent = o;
                        vehs[k].ent = o;
                        if (role() && veh_is_mine(o))
                            vehs[k].have = 0;   /* (this machine's physics rules it now: the others' older data is stale) */
                    }
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
        return veh_is_mine(v->ent);
    (void)now;
    return veh_is_mine(v->ent) || !v->have;   /* (host: unless a client drives it, or drove it: its last place stays, the data
                                               * never expires - the engine's node sync would put the quad back to the start) */
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

static void veh_receive(int peer, const unsigned char *b, int len)
{
    DWORD now = GetTickCount();
    if (role() == MODE_SERVER) {   /* (a vehicle a client drives: the other clients get it from the host) */
        for (int i = 0; slot_at(i); i++) {
            char *s = slot_at(i);
            if (*(int *)s == 0 || *(int *)s == peer || !slot_joined(s))
                continue;
            coopnet_send(i, SUB_VEH, b, len);
        }
    }
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
        if (!v->have || !v->ent || veh_mine(v, now))
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

/* The engine's node sync (0x571ad0, eax = a node controller: [eax+4] = the entity, [eax+0x14] = a component that hands out a
 * matrix, which goes through the node's matrix setter 0x94dbf0) rewrote the world matrix of a client's AI quad with the
 * start place EVERY frame (watchpoint on the node's +0x68, live level 9: writers 0x571b06 <- 0x94dc56, alternating with this
 * file's veh_apply), so the streamed position never held; the truck, route-driven, had no such component. On a client the
 * sync is skipped for the vehicles the stream places (the host's data is fresh and the client does not drive them). */
extern void cp_sync_hook(void) __asm__("cp_sync_hook");
__attribute__((used)) int cp_sync_skip(void *ctl) __asm__("cp_sync_skip");
static int use_syncskip = 1;
__attribute__((used)) int cp_sync_skip(void *ctl)
{
    DWORD now = GetTickCount();
    if (!use_syncskip || !use_vehsync || !enabled || !role() || !ctl || IsBadReadPtr(ctl, 8))
        return 0;   /* (a client: the host's vehicles; the host: the vehicle a client drives, `veh_mine` is false there) */
    char *ent = *(char **)((char *)ctl + 4);
    for (int k = 0; k < n_veh; k++)
        if (vehs[k].ent == ent && vehs[k].have && !veh_mine(&vehs[k], now))
            return 1;
    return 0;
}
__asm__(".text\n"
        ".globl cp_sync_hook\n"
        "cp_sync_hook:\n"
        "  pushl %eax\n"
        "  pushl %ecx\n"
        "  pushl %edx\n"
        "  pushl %eax\n"
        "  call cp_sync_skip\n"
        "  addl $4, %esp\n"
        "  testl %eax, %eax\n"
        "  popl %edx\n"
        "  popl %ecx\n"
        "  popl %eax\n"
        "  jnz 1f\n"
        "  pushl %ebp\n"
        "  movl %esp, %ebp\n"
        "  andl $-8, %esp\n"
        "  pushl $0x571ad6\n"
        "  ret\n"
        "1:\n"
        "  ret\n");

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

/* ---- dynamic actor digest (TS_CAMPAIGN_DIGEST, default on; TS_CAMPAIGN_DIGEST_FIX=0: log only) ----
 * Doors, lifts, machinery and other animated `dynamic` actors run from the replayed story events on every machine
 * and nothing checks that they still agree (only the rewind pushes clip and time). Every 4 s the host hashes the state
 * of every named dynamic actor that has a clip - clip index, running, cycle flag, and for a stopped clip its time to half a
 * second - into 16 buckets (by name hash) and sends the 16 hashes (message 79 type 0). A client scans its own actors, and
 * a bucket that differs in two rounds in a row (a clip that ends a moment apart is not a difference) is asked for
 * (type 1); the host answers with the bucket's entries (type 2: name hash, clip, flags, time); the client logs every
 * actor that differs ("digest: <name> host clip c ...") and puts it into the host's state: Start(clip index, time, cycle)
 * (the engine's 0x6aaea0 with esi = the entity: what the script's Start ends in) and Stop() when the host's clip is
 * stopped. Vehicles (their own stream), players and AI are left out. `!cp dyn` shows the counters. */
#define DYN_VSLOT 0x204
#define DYN_START 0x6a9fb0
#define MAX_DYN 2000
#define DYN_B 16
typedef struct { unsigned hash; char *ent; int clip; unsigned char run, cyc; float t; } Dyn;
static Dyn dyn[MAX_DYN];
static int n_dyn, use_digest = 1, digest_fix = 1;
static int dyn_miss[DYN_B], dyn_seq, dyn_rounds, dyn_requests, dyn_bad, dyn_fixed, dyn_details, dyn_sent;
static unsigned dyn_host_bh[DYN_B], dyn_own_bh[DYN_B];
static int dyn_pending;                      /* (client) a digest from the host waits for the next frame */
static unsigned char dyn_det_buf[8][250];    /* (client) detail messages that arrived (processed in the frame) */
static int dyn_det_len[8], dyn_det_n;
static int dyn_req_bucket[DYN_B], dyn_req_peer[DYN_B], dyn_req_n;   /* (host) requests waiting for the frame */
static DWORD dyn_ready_at, dyn_next_at;
static int (TSCALL *dyn_node_clip)(void *) = (void *)0x951660;
static float (TSCALL *dyn_node_time)(void *) = (void *)0x951500;
static const DWORD fn_anim_start = 0x6aaea0;

static unsigned dyn_state(const Dyn *d)
{
    unsigned t = d->run ? 0u : (unsigned)(int)(d->t * 2.f + 0.5f) & 0x3fff;   /* (a running clip: its time differs by the lag) */
    return ((unsigned)d->clip & 0xffff) | ((unsigned)d->run << 16) | ((unsigned)d->cyc << 17) | (t << 18);
}

static unsigned dyn_mix(const Dyn *d)
{
    unsigned h = d->hash * 2654435761u;
    h ^= dyn_state(d) * 40503u + 12345u;
    return h * 2246822519u + (h >> 13);
}

static void dyn_scan(void)
{
    n_dyn = 0;
    char *b = ENT_BUCKETS;
    int nb = ENT_NBUCKETS;
    for (int i = 0; b && i < nb; i++)
        for (char *o = *(char **)(b + i * 12); o; ) {
            void **vt = !IsBadReadPtr(o, 0x284) ? *(void ***)o : NULL;
            if (vt && n_dyn < MAX_DYN && !IsBadReadPtr(vt, DYN_VSLOT + 4) && vt[DYN_VSLOT / 4] == (void *)DYN_START && !is_vehicle(o)) {
                void *inst = *(void **)(o + ENT_INST);
                String nm = inst && !((DWORD)inst & 3) && !IsBadReadPtr(inst, INST_NAME + 4) ? *(String *)((char *)inst + INST_NAME) : NULL;
                char *node = *(char **)(o + ENT_NODE);
                if (nm && !IsBadReadPtr(nm, 8) && !IsBadStringPtrA(nm->s, 200) && nm->s[0] && node && !IsBadReadPtr(node, 0x88)) {
                    int clip = dyn_node_clip(node);
                    if (clip >= 0) {
                        Dyn *d = &dyn[n_dyn++];
                        unsigned fl = *(unsigned *)(o + 0x280);
                        d->hash = name_hash(nm->s);
                        d->ent = o;
                        d->clip = clip;
                        d->run = (fl & 1) != 0;
                        d->cyc = (fl & 0x10) != 0;
                        d->t = dyn_node_time(node);
                    }
                }
            }
            char *next = *(char **)(o + 8);
            o = next ? next - 4 : NULL;
        }
}

static void dyn_buckets(unsigned *bh, int *cnt)
{
    memset(bh, 0, DYN_B * sizeof *bh);
    if (cnt)
        memset(cnt, 0, DYN_B * sizeof *cnt);
    for (int i = 0; i < n_dyn; i++) {
        int k = (int)((dyn[i].hash >> 4) % DYN_B);
        bh[k] += dyn_mix(&dyn[i]);
        if (cnt)
            cnt[k]++;
    }
}

static void dyn_set_state(char *ent, int clip, float t, int cyc, int run)
{
    char *e = ent;
    unsigned tb;
    memcpy(&tb, &t, 4);
    __asm__ volatile("pushl %[c]\n\tpushl %[t]\n\tpushl %[i]\n\tcall *%[fn]"
                     : "+S"(e)
                     : [c] "m"(cyc), [t] "m"(tb), [i] "m"(clip), [fn] "m"(fn_anim_start)
                     : "eax", "ecx", "edx", "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
    if (!run) {
        char ssl[160], res[100];
        snprintf(ssl, sizeof ssl, "$%s.Stop()", ent_name(ent));
        ts_script_run(ssl, res, sizeof res);
    }
}

/* message 79: {u8 1, u8 type, ...}
 *   0 host -> clients {u8 seq, u8 16, 16 x u32 bucket hash}
 *   1 client -> host  {u8 bucket}
 *   2 host -> client  {u8 bucket, u8 n, n x {u32 name hash, u16 clip, u8 flags (1 runs, 2 cycle), f32 time}} (n <= 18) */
static void dyn_message(int peer, const unsigned char *b, int len)
{
    int r = role();
    if (!use_digest)
        return;
    if (b[1] == 0 && r == MODE_CLIENT && len >= 4 + DYN_B * 4) {
        memcpy(dyn_host_bh, b + 4, DYN_B * 4);
        dyn_pending = 1;
    } else if (b[1] == 1 && r == MODE_SERVER && len >= 3 && b[2] < DYN_B && dyn_req_n < DYN_B) {
        dyn_req_bucket[dyn_req_n] = b[2];
        dyn_req_peer[dyn_req_n++] = peer;
    } else if (b[1] == 2 && r == MODE_CLIENT && len >= 4 && dyn_det_n < 8) {
        memcpy(dyn_det_buf[dyn_det_n], b, len < 250 ? len : 250);
        dyn_det_len[dyn_det_n++] = len < 250 ? len : 250;
    }
}

static void dyn_send_detail(int peer, int bucket)
{
    int slot = -1;
    for (int i = 0; slot_at(i); i++)
        if (*(int *)slot_at(i) == peer)
            slot = i;
    if (slot < 0)
        return;
    unsigned char m[250];
    int n = 0, cnt = 0;
    for (int i = 0; i < n_dyn; i++) {
        if ((int)((dyn[i].hash >> 4) % DYN_B) != bucket)
            continue;
        if (!n) {
            m[0] = PROTO, m[1] = 2, m[2] = (unsigned char)bucket, m[3] = 0;
            n = 4;
        }
        unsigned short c = (unsigned short)dyn[i].clip;
        unsigned char fl = (unsigned char)(dyn[i].run | (dyn[i].cyc << 1));
        memcpy(m + n, &dyn[i].hash, 4);
        memcpy(m + n + 4, &c, 2);
        m[n + 6] = fl;
        memcpy(m + n + 7, &dyn[i].t, 4);
        n += 11;
        cnt++;
        if (cnt == 18) {
            m[3] = (unsigned char)cnt;
            coopnet_send(slot, SUB_DIGEST, m, n);
            dyn_sent++;
            n = 0;
            cnt = 0;
        }
    }
    if (cnt) {
        m[3] = (unsigned char)cnt;
        coopnet_send(slot, SUB_DIGEST, m, n);
        dyn_sent++;
    }
}

static void dyn_step(DWORD now)
{
    static int was_running;
    static char seen_level[0x110];
    int r = role();
    if (!use_digest || !r)
        return;
    /* (the host's "level running" flag is not set on a client: there the player's existence says the level runs) */
    int running = r == MODE_SERVER ? (GAME_FLAGS & FLAG_LEVEL_RUNNING) != 0 : PLAYER != NULL && LEVEL_NAME[0];
    if (running && strcmp(seen_level, LEVEL_NAME) != 0) {
        snprintf(seen_level, sizeof seen_level, "%s", LEVEL_NAME);
        was_running = 0;
    }
    if (running && !was_running) {   /* a level (or a team restart) is up: give it time to settle */
        dyn_ready_at = now + 20000;
        memset(dyn_miss, 0, sizeof dyn_miss);
        dyn_seq = 0;
    }
    was_running = running;
    if (!running || (int)(now - dyn_ready_at) < 0)
        return;
    if (r == MODE_SERVER) {
        if (dyn_req_n) {   /* (requests first: the scan of this frame serves them) */
            dyn_scan();
            for (int i = 0; i < dyn_req_n; i++)
                dyn_send_detail(dyn_req_peer[i], dyn_req_bucket[i]);
            dyn_req_n = 0;
        }
        if ((int)(now - dyn_next_at) < 0)
            return;
        dyn_next_at = now + 4000;
        dyn_scan();
        unsigned bh[DYN_B];
        dyn_buckets(bh, NULL);
        unsigned char m[4 + DYN_B * 4] = {PROTO, 0, (unsigned char)dyn_seq++, DYN_B};
        memcpy(m + 4, bh, sizeof bh);
        if (coopnet_send(-1, SUB_DIGEST, m, sizeof m) > 0)
            dyn_sent++;
        return;
    }
    /* client */
    if (dyn_pending) {
        dyn_pending = 0;
        dyn_scan();
        int cnt[DYN_B];
        dyn_buckets(dyn_own_bh, cnt);
        dyn_rounds++;
        for (int k = 0; k < DYN_B; k++) {
            if (dyn_own_bh[k] == dyn_host_bh[k]) {
                dyn_miss[k] = 0;
                continue;
            }
            dyn_miss[k]++;
            VLOG("campaign: digest: bucket %d differs (%d actors here), round %d in a row", k, cnt[k], dyn_miss[k]);
            if (dyn_miss[k] >= 2 && (dyn_miss[k] % 2) == 0) {
                unsigned char m[3] = {PROTO, 1, (unsigned char)k};
                coopnet_send(-1, SUB_DIGEST, m, sizeof m);
                dyn_requests++;
            }
        }
    }
    while (dyn_det_n) {
        const unsigned char *m = dyn_det_buf[--dyn_det_n];
        int len = dyn_det_len[dyn_det_n], nn = m[3];
        dyn_scan();
        dyn_details++;
        for (int j = 0; j < nn && 4 + (j + 1) * 11 <= len; j++) {
            unsigned h;
            unsigned short c;
            float t;
            memcpy(&h, m + 4 + j * 11, 4);
            memcpy(&c, m + 8 + j * 11, 2);
            unsigned char fl = m[10 + j * 11];
            memcpy(&t, m + 11 + j * 11, 4);
            Dyn *d = NULL;
            for (int i = 0; i < n_dyn; i++)
                if (dyn[i].hash == h)
                    d = &dyn[i];
            if (!d) {
                dyn_bad++;
                if (dyn_bad <= 200)
                    CLOG("campaign: digest: actor %08x (clip %d, %s) of the host is not here (no clip, or not created)", h, c,
                         fl & 1 ? "runs" : "stopped");
                continue;
            }
            int run = fl & 1, cyc = (fl >> 1) & 1;
            int differs = d->clip != c || d->run != run || d->cyc != cyc || (!run && fabsf(d->t - t) > 0.6f);
            if (!differs)
                continue;
            dyn_bad++;
            if (dyn_bad <= 200)
                CLOG("campaign: digest: %s host clip %d %s%s t %.2f, here clip %d %s%s t %.2f%s", ent_name(d->ent), c,
                     run ? "runs" : "stopped", cyc ? " cycle" : "", (double)t, d->clip, d->run ? "runs" : "stopped",
                     d->cyc ? " cycle" : "", (double)d->t, digest_fix && !run ? ": set to the host's" : "");
            /* Only an actor the host has AT REST is put right (its clip, final time, stopped): one that still runs on the host
             * differs by the phase of a timed machine every few seconds (live: trains, interceptor places), which is no
             * divergence; an actor at most once a minute. */
            if (digest_fix && !run) {
                static struct { unsigned h; DWORD at; } fixed[64];
                static int fixed_next;
                DWORD tnow = GetTickCount();
                int k;
                for (k = 0; k < 64 && fixed[k].h != d->hash; k++)
                    ;
                if (k < 64 && tnow - fixed[k].at < 60000)
                    continue;
                if (k == 64) {
                    k = fixed_next;
                    fixed_next = (fixed_next + 1) % 64;
                }
                fixed[k].h = d->hash;
                fixed[k].at = tnow;
                dyn_set_state(d->ent, c, t, cyc, run);
                dyn_fixed++;
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
        veh_step(now);
        dyn_step(now);
        return;
    }
    if (r != MODE_SERVER) {
        ride_vehicle[0] = 0;
        return;
    }
    veh_step(now);
    dyn_step(now);
    loadout_step(now);
    locks_send(now);
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
        } else if (a->r->rule == R_ALL) {
            int missing = 0;
            unsigned st = *(unsigned *)(dom + DOM_STATE);
            /* (an "act once" domain that has fired never enters again: the engine's test at 0x6c2e59) */
            char *desc = *(char **)(dom + DOM_DESC);
            int spent = desc && (*(unsigned char *)(desc + DESC_ACT_ONCE) & 1) && (st & 2);
            int waiting = !locked && !spent && !(st & 1) && inside(dom, PLAYER) && !clients_inside(dom, &missing);
            a->missing = waiting ? missing : 0;
            if (!waiting)
                a->wait_since = 0;
            else if (!a->wait_since) {
                a->wait_since = now;
                VLOG("campaign: %s waits for %d player(s)", a->r->domain, missing);
            } else if (gather_after > 0 && now - a->wait_since >= (DWORD)gather_after * 1000) {
                a->wait_since = now;   /* (again after the same delay if somebody is still missing) */
                gather(dom);
            }
        }
    }
}

/* ---- test console: "!cp ..." ---- */
/* ---- lab: a hardware write watchpoint on a 4-byte address of this machine's main thread ("!cp watch ...") ---- */
static volatile DWORD watch_addr, watch_tid;
static volatile LONG watch_hits;
static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *x)
{
    CONTEXT *c = x->ContextRecord;
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 1) || !watch_addr)
        return EXCEPTION_CONTINUE_SEARCH;
    LONG n = InterlockedIncrement(&watch_hits);
    if (n <= 12) {
        DWORD rets[6], offs[6];
        NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
        int k = ts_stack_returns(c->Esp, (DWORD)tib->StackBase, rets, offs, 6);
        ts_log("watch: %08lx written (now %.2f), eip after the write %08lx, returns: %08lx %08lx %08lx %08lx %08lx %08lx",
               (unsigned long)watch_addr, (double)*(volatile float *)watch_addr, c->Eip, k > 0 ? rets[0] : 0, k > 1 ? rets[1] : 0,
               k > 2 ? rets[2] : 0, k > 3 ? rets[3] : 0, k > 4 ? rets[4] : 0, k > 5 ? rets[5] : 0);
    }
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI watch_thread(void *arg)
{
    (void)arg;
    HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, watch_tid);
    if (!th)
        return 1;
    SuspendThread(th);
    CONTEXT c;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(th, &c)) {
        c.Dr0 = watch_addr;
        c.Dr6 = 0;
        c.Dr7 = watch_addr ? (1u | (1u << 16) | (3u << 18)) : 0;   /* local enable 0, write, 4 bytes */
        SetThreadContext(th, &c);
    }
    ResumeThread(th);
    CloseHandle(th);
    return 0;
}

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
        int n = campaign_regroup();
        if (n < 0)
            return snprintf(out, size, "not in a co-op session with the story on (or TS_CAMPAIGN is off)"), 1;
        return snprintf(out, size, r == MODE_SERVER ? "%d client(s) sent to the host" : "moved to the host (%d)", n), 1;
    }
    if (!strcmp(cmd, "dyn")) {   /* "!cp dyn": the digest's counters and this machine's bucket hashes (lab) */
        dyn_scan();
        int cnt[DYN_B];
        unsigned bh[DYN_B];
        dyn_buckets(bh, cnt);
        int n = snprintf(out, size, "digest %s fix %s: %d actors, rounds %d, requests %d, details %d, sent %d, differing %d, fixed %d; buckets:",
                         use_digest ? "on" : "off", digest_fix ? "on" : "log only", n_dyn, dyn_rounds, dyn_requests, dyn_details, dyn_sent,
                         dyn_bad, dyn_fixed);
        for (int k = 0; k < DYN_B && n < size - 30; k++)
            n += snprintf(out + n, size - n, " %d:%d/%08x", k, cnt[k], bh[k]);
        return 1;
    }
    if (!strcmp(cmd, "life")) {   /* "!cp life": the life of every player this machine knows, the locks seen (lab) */
        int n = snprintf(out, size, "me %.1f;", PLAYER ? (double)*(float *)(PLAYER + 0x234) : -1.0);
        for (int i = 0; slot_at(i) && n < size - 60; i++) {
            char *s = slot_at(i), *ent = *(char **)(s + 4);
            n += snprintf(out + n, size - n, " slot %d id %d joined %d life %.1f;", i, *(int *)s, slot_joined(s),
                          ent && !IsBadReadPtr(ent + 0x234, 4) ? (double)*(float *)(ent + 0x234) : -1.0);
        }
        n += snprintf(out + n, size - n, " locks mine move %d input %d, host's %d/%d (%lu ms ago), released %d, spread %d", mine.move, mine.input,
                      host_locks.move, host_locks.input, host_locks.at ? (unsigned long)(GetTickCount() - host_locks.at) : 0UL,
                      lock_released, spread_count);
        return 1;
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
    if (!strcmp(cmd, "loadout")) {   /* "!cp loadout": weapon slots, ammunition, life, time energy of this machine's player; the host also lists the clients' bodies (lab) */
        char *pl = PLAYER;
        float total = -1.f;
        int mag = -1;
        int n = 0;
        if (pl && !IsBadReadPtr(pl + 0x3a8, 12)) {
            split_ammo_get((unsigned char *)pl, &total, &mag);
            n += snprintf(out + n, size - n, "mine: slots %d %d %d ammo %.0f/%d life %.1f energy %.2f", *(int *)(pl + 0x3a8), *(int *)(pl + 0x3ac),
                          *(int *)(pl + 0x3b0), (double)total, mag, (double)*(float *)(pl + 0x234), (double)*(float *)(pl + 0x2ac));
        } else
            n += snprintf(out + n, size - n, "mine: no player");
        for (int i = 0; slot_at(i) && n < size - 120; i++) {
            char *s2 = slot_at(i), *ent = *(char **)(s2 + 4);
            if (!ent || ent == pl || IsBadReadPtr(ent + 0x3a8, 12))
                continue;
            n += snprintf(out + n, size - n, "; slot %d id %d body: slots %d %d %d life %.1f", i, *(int *)s2, *(int *)(ent + 0x3a8),
                          *(int *)(ent + 0x3ac), *(int *)(ent + 0x3b0), (double)*(float *)(ent + 0x234));
        }
        return 1;
    }
    if (!strcmp(cmd, "rd")) {   /* "!cp rd <player|object> <hex offset> [count]": dwords at base+offset (lab; player = $player) */
        unsigned o = 0, cnt = 8;
        char *base = !strcmp(a1, "player") ? PLAYER : find_ent(a1);
        sscanf(text + 3, "%*s %*s %x %u", &o, &cnt);
        if (!base || cnt > 32 || IsBadReadPtr(base + o, cnt * 4))
            return snprintf(out, size, "%s: not readable", a1), 1;
        int n = snprintf(out, size, "%s %p +%x:", a1, (void *)base, o);
        for (unsigned i = 0; i < cnt && n < size - 14; i++)
            n += snprintf(out + n, size - n, " %x", *(unsigned *)(base + o + i * 4));
        return 1;
    }
    if (!strcmp(cmd, "pused")) {   /* "!cp pused [object]": what the player's vtable +0x23c answers (InBike's test), next to an object's entity (lab) */
        char *pl = PLAYER;
        if (!pl || IsBadReadPtr(pl, 4))
            return snprintf(out, size, "no player"), 1;
        void *u = ((void *(TSCALL *)(void *))(*(void ***)pl)[PLAYER_USED / 4])(pl);
        char *e = a1[0] ? find_ent(a1) : NULL;
        float up[3] = {0, 0, 0};
        if (u && !IsBadReadPtr(u, ENT_NODE + 4))
            ent_pos(u, up);
        return snprintf(out, size, "player %p uses %p (vt %p, at %.1f %.1f %.1f, name '%s'); %s %p", (void *)pl, u,
                        u && !IsBadReadPtr(u, 4) ? *(void **)u : NULL, up[0], up[1], up[2], u && !IsBadReadPtr(u, ENT_INST + 4) ? ent_name(u) : "?",
                        a1, (void *)e), 1;
    }
    if (!strcmp(cmd, "wpn")) {   /* "!cp wpn": the raw weapon records 0..3 of this machine's player (lab: where the weapon types live) */
        char *pl = PLAYER;
        if (!pl || IsBadReadPtr(pl, 4))
            return snprintf(out, size, "no player"), 1;
        unsigned char *set = ((unsigned char *(TSCALL *)(void *))(*(void ***)pl)[0x248 / 4])(pl);
        if (!set || IsBadReadPtr(set, 8) || IsBadReadPtr(*(unsigned char **)(set + 4), 8))
            return snprintf(out, size, "no weapon set %p", (void *)set), 1;
        unsigned char *vb = set + 4 + *(int *)(*(unsigned char **)(set + 4) + 4);
        if (IsBadReadPtr(vb, 4) || IsBadReadPtr(*(void **)vb, 0x50))
            return snprintf(out, size, "no weapon base"), 1;
        int n = snprintf(out, size, "set %p:", (void *)set);
        for (int w = 0; w < 4 && n < size - 120; w++) {
            unsigned char *rec = ((unsigned char *(TSCALL *)(void *, int))(*(void ***)vb)[0x4c / 4])(vb, w);
            if (!rec || IsBadReadPtr(rec, 0x24)) {
                n += snprintf(out + n, size - n, " [%d] none;", w);
                continue;
            }
            n += snprintf(out + n, size - n, " [%d]", w);
            for (int k = 0; k < 9; k++)
                n += snprintf(out + n, size - n, " %x", *(unsigned *)(rec + k * 4));
            n += snprintf(out + n, size - n, ";");
        }
        return 1;
    }
    if (!strcmp(cmd, "vslot")) {   /* "!cp vslot <object> <hex slot offset> ...": the entity's vtable and the functions at those offsets (lab) */
        char *e = find_ent(a1);
        if (!e)
            return snprintf(out, size, "%s: none", a1), 1;
        void **vt = *(void ***)e;
        int n = snprintf(out, size, "%s vt %p:", a1, (void *)vt);
        const char *p = text + 3;
        char tok[32];
        int skip = 2;
        while (sscanf(p, " %31s", tok) == 1) {
            p = strstr(p, tok) + strlen(tok);
            if (skip) {
                skip--;
                continue;
            }
            unsigned o = (unsigned)strtoul(tok, NULL, 16);
            if (o < 0x800 && n < size - 30)
                n += snprintf(out + n, size - n, " +%x=%p", o, vt[o / 4]);
        }
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
                          v->ent && veh_is_mine(v->ent) ? " used" : "", veh_mine(v, now) ? " mine" : "",
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
                                  is_vehicle(o) ? " vehicle" : "", is_vehicle(o) && veh_is_mine(o) ? " used" : "");
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
    if (!strcmp(cmd, "watch")) {   /* "!cp watch <object> node|ent <hex offset>" or "!cp watch off": write watchpoint (lab) */
        static int installed;
        char where[8] = "";
        unsigned o = 0;
        char *e = strcmp(a1, "off") ? find_ent(a1) : NULL;
        if (!installed) {
            AddVectoredExceptionHandler(1, watch_handler);
            installed = 1;
        }
        watch_tid = GetCurrentThreadId();
        if (!strcmp(a1, "off")) {
            watch_addr = 0;
        } else {
            if (!e || sscanf(text + 3, "%*s %*s %7s %x", where, &o) != 2)
                return snprintf(out, size, "usage: !cp watch <object> node|ent <hex offset> | off"), 1;
            char *base = !strcmp(where, "node") ? *(char **)(e + ENT_NODE) : e;
            if (!base)
                return snprintf(out, size, "%s: no %s", a1, where), 1;
            watch_addr = (DWORD)(base + o);
        }
        watch_hits = 0;
        HANDLE h = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
        if (h) {
            WaitForSingleObject(h, 2000);
            CloseHandle(h);
        }
        return snprintf(out, size, "watch %s %08lx", watch_addr ? "on" : "off", (unsigned long)watch_addr), 1;
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
    use_voice = !(ts_env("TS_CAMPAIGN_VOICE", v, sizeof v) && v[0] == '0');
    use_digest = !(ts_env("TS_CAMPAIGN_DIGEST", v, sizeof v) && v[0] == '0');
    digest_fix = !(ts_env("TS_CAMPAIGN_DIGEST_FIX", v, sizeof v) && v[0] == '0');
    use_locks = !(ts_env("TS_CAMPAIGN_LOCKS", v, sizeof v) && v[0] == '0');
    use_loadout = !(ts_env("TS_CAMPAIGN_LOADOUT", v, sizeof v) && v[0] == '0');
    use_spread = ts_env("TS_CAMPAIGN_SPREAD", v, sizeof v) && v[0] == '1';
    if (ts_env("TS_CAMPAIGN_LOCKWATCH", v, sizeof v) && atoi(v) > 0)
        lock_after = atoi(v);
    voice_log = ts_env("TS_CAMPAIGN_VOICELOG", v, sizeof v) && v[0] == '1';
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
    {
        static const unsigned char want_sy[6] = {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8};
        unsigned char r[6] = {0xe9, 0, 0, 0, 0, 0x90};
        *(DWORD *)(r + 1) = (DWORD)cp_sync_hook - (0x571ad0 + 5);
        use_syncskip = !(ts_env("TS_CAMPAIGN_SYNCSKIP", v, sizeof v) && v[0] == '0');
        if (use_syncskip && !ts_patch_bytes(0x571ad0, want_sy, r, 6))
            ts_log("campaign: node sync site not as expected (other exe)");
    }
    for (int i = 0; i < N_HOOKS; i++) {
        const Hook *h = &hooks[i];
        if ((h->group == 1 && !use_latejoin && !use_locks) || (h->group == 2 && !use_ai) || (h->group == 3 && !use_voice)
            || (h->group == 4 && !use_spread))
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
