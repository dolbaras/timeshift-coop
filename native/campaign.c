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
 *
 * Test console (TS_CONSOLE): "!cp state", "!cp regroup" (host: every client to the host; client: itself to the
 * host), "!cp gather <domain>" (host: the clients that are not inside, into it), "!cp rules".
 *
 * Switches: TS_CAMPAIGN=1 on (default off), TS_CAMPAIGN_RULES=0 no domain rules (console only),
 * TS_CAMPAIGN_GATHER=<s> gather delay (0 = no automatic gather), TS_CAMPAIGN_KILL=0 KILL domains keep the
 * storysync behaviour (the cinematic), TS_CAMPAIGN_LOG=1 every decision in the log.
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
#define SUB_LAST 79
#define PROTO 1

/* ---- settings ---- */
static int enabled, use_rules = 1, use_kill = 1, verbose;
static int gather_after = 20;                /* s; 0 = no automatic gather */
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
    if (!ent || !*(void **)(ent + ENT_NODE))
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
    ts_script_run("$player.Kill()", res, sizeof res);
    CLOG("campaign: own player killed (%s) -> %s", why, res);
}

static void on_message(int peer, unsigned short sub, const unsigned char *b, int len)
{
    (void)peer;
    if (role() != MODE_CLIENT || len < 1 || b[0] != PROTO)
        return;
    if (sub == SUB_GATHER && len >= 15) {
        float pos[3], mn[3], mx[3];
        memcpy(pos, b + 2, 12);
        int box = b[14] && len >= 39;
        if (box) {
            memcpy(mn, b + 15, 12);
            memcpy(mx, b + 27, 12);
        }
        place_local(pos, b[1], box ? mn : NULL, box ? mx : NULL);
    } else if (sub == SUB_KILL)
        kill_local("the host: inside a death domain");
}

/* ---- per frame (host) ---- */
static void frame(float dt)
{
    (void)dt;
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 250 || role() != MODE_SERVER)
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
            if (inside(dom, PLAYER) && now - a->killed_at[0] > 8000) {
                a->killed_at[0] = now;
                kill_local("inside a death domain");
            }
            for (int i = 0; i < 16; i++) {
                char *s = slot_at(i);
                if (!s)
                    break;
                char *ent = *(char **)(s + 4);
                if (*(int *)s == 0 || !slot_joined(s) || !inside(dom, ent) || now - a->killed_at[i] <= 8000)
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
    if (!ts_tick_install() || !ts_frame_add(frame) || !coopnet_listen(SUB_GATHER, SUB_LAST, on_message)) {
        ts_log("campaign: off (no script tick, frame slot or mod messages)");
        return;
    }
    enabled = 1;
    ts_log("campaign: on (NOT YET RUN: domain rules %s, gather after %d s, death domains %s)", use_rules ? "on" : "off",
           gather_after, use_kill ? "kill per player" : "as storysync");
}
