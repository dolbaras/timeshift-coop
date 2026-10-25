/*
 * AI soldiers on the clients of a campaign level run on the game's own server (patch 1.2 exe only).
 *
 * The AI classes carry network code (vtbl+0x64 0x858d30 writes, vtbl+0x68 0x859170 reads: the base entity's
 * motion data plus the AI's animation, behaviour animation and speed), and with isSyncMP on ai_brain (data
 * patch) the server gives them net ids and clients match them. Three things kept it from working, so a client's
 * soldiers stood in their first pose:
 *   1. nothing starts their network updates: the entity's "net start" (vtbl+0x44) is never called for AI, so the
 *      server never puts them into its regular updates (role bit 0x10 at +0x2c) and they have no motion
 *      component (+0x34) for their transform. Every AI with a server net id now gets the item classes' net
 *      start (0x4f6b60): a writer with authority, an interpolating reader on a client, and the bits;
 *   2. the update codec drops the AI's animation bytes, and a client played whatever its receive buffer held
 *      (animation 0): they now ride at the end of the motion bytes (hook_write / hook_read);
 *   3. an AI standing still is never sent again (unchanged motion data): the server marks an AI whose animation
 *      changed for the next update.
 * Only for entities of the AI classes with isSyncMP, in a network session. TS_AISYNC=0 switches it off.
 *
 * Also (sections below): corpses on clients (a client shows each death itself), soldiers dead before a client joined
 * (a "dead on the server" bit in the updates, and a list of the dead on request), and the names of objects the
 * server creates in play (clients ask for them). Mod messages 50-59.
 *
 * TS_AIDEBUG=<s>: diagnostics. Counts AI writes (sender), reads and interpolation calls (receiver) and logs each
 * AI's net id, role bits, position and animations every <s> seconds (aligned to the clock, so a host and a client
 * on one PC log the same moments). Test helper with it: <TS_SHOTDIR>\follow.req (below).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

#define GAME_MODE_OBJ (*(char **)0xfcab34)
#define GAME_FLAGS (*(unsigned *)0xfcace8)   /* 0x2000 = network session */
#define ENT_BUCKETS (*(char **)0xecb7fc)     /* the scheduler's objects: buckets of 12 bytes, +0 first object */
#define ENT_NBUCKETS (*(int *)0xecb800)      /* (next object: [obj+8] - 4, as 0x947e90 walks them) */
#define AI_WRITE 0x858d30                    /* AI vtbl+0x64: (peer, msg out, flags), ret 0xc */
#define AI_READ 0x859170                     /* AI vtbl+0x68: (msg, flags), ret 8 */
#define ITEM_NET_START 0x4f6b60              /* thiscall (entity, int kind, int has_motion, int flag), ret 0xc */
#define PROXY_VT 0xd8cb00                    /* the item reader's vtable: +0x1c per frame (0x4f84e0) */
#define PROXY_FRAME 0x4f84e0

/* Entity fields */
#define E_NETID 0x18
#define E_ROLE 0x2c                          /* 1 proxy, 2 networked, 4 authority, 0x10 in regular updates, 0x40 motion */
#define E_COMP 0x34                          /* motion component */
#define E_INST 0x80                          /* script object (Inst: +4 name, +0xc this entity) */
#define E_SYNC 0x98                          /* class flags: 1 = isSyncMP */
#define E_POS 0xb0
#define E_NODE 0x230                         /* scene node: +0x68 world position */

/* The eight vtables that use the AI network code (ai_inf, swat, knight, alt_swift, mech and three more). */
static const DWORD ai_vtables[] = {0xd764d8, 0xd771e0, 0xd77870, 0xd788a8, 0xd78d28, 0xd79510, 0xd7a008, 0xdbfa68};
#define N_VT (sizeof ai_vtables / sizeof ai_vtables[0])

typedef void (TSCALL *write_t)(void *ai, int peer, int *msg, int flags);
typedef void (TSCALL *read_t)(void *ai, char *msg, int flags);
typedef void (TSCALL *net_start_t)(void *ent, int kind, int motion, int flag);
typedef void (TSCALL *pframe_t)(void *comp, void *ent, float dt);

static int engine_mode, debug_secs;

/* the scene node's animation (what the physics objects' network component syncs): current id, time */
static int (TSCALL *anim_id)(void *node) = (void *)0x951660;
static float (TSCALL *anim_time)(void *node) = (void *)0x951500;

static int is_ai(const char *obj)
{
    if (!obj)
        return 0;
    DWORD vt = *(const DWORD *)obj;
    for (unsigned i = 0; i < N_VT; i++)
        if (vt == ai_vtables[i])
            return 1;
    return 0;
}

static const char *ai_name(const char *ai)
{
    const char *inst = *(char *const *)(ai + E_INST);
    if (!inst || IsBadReadPtr(inst, 0x10) || *(const char *const *)(inst + 0xc) != ai)
        return "";
    const char *s = *(const char *const *)(inst + 4);
    return s ? s + 0xc : "";
}

/* Every AI entity of the level (main thread). */
static int for_each_ai(void (*fn)(char *ai))
{
    char *b = ENT_BUCKETS;
    int n = 0, nb = ENT_NBUCKETS;
    for (int i = 0; b && i < nb; i++)
        for (char *o = *(char **)(b + i * 12); o; ) {
            if (is_ai(o)) {
                fn(o);
                n++;
            }
            char *next = *(char **)(o + 8);
            o = next ? next - 4 : NULL;
        }
    return n;
}

/* ---- engine path ---- */
static void adopt(char *ai)
{
    int net = *(int *)(ai + E_NETID);
    unsigned role = *(unsigned *)(ai + E_ROLE);
    if (net < 0 || *(void **)(ai + E_COMP) || (role & 0x40) || !(*(unsigned *)(ai + E_SYNC) & 1))
        return;
    ((net_start_t)ITEM_NET_START)(ai, 2, 1, 0);
    static LONG logged;
    if (InterlockedIncrement(&logged) <= 8)
        ts_log("aisync: %s (net %d) role %08x -> %08x", ai_name(ai), net, role, *(unsigned *)(ai + E_ROLE));
}

/* Per-AI state (main thread): what the server sent last, diagnostics counters. */
#define MAX_AI 1024
typedef struct {
    void *ai;
    int net, writes, reads, frames;
    int anim;          /* server: the body animation sent last (NO_ANIM = none yet) */
    int corpse;        /* client: its death was shown */
    int noanim;        /* client: dead on the server: no more animation from the server */
    int srv_dead;      /* client: the server says it is dead */
    int shots, shots_sent;          /* server: weapon shots of this soldier, the count sent last */
    int shots_seen, seen_init;      /* client: the server's count read last */
    int pending;                    /* client: shots still to show */
    DWORD last_shot;                /* client: tick of the last shot shown */
    unsigned char atk;              /* the attack of the last shot (0 primary, 1 secondary) */
    char *wpn;                      /* client: the soldier's weapon found last */
    int have_shot;                  /* the last shot's geometry is known: */
    float sh_org[3];                /*   server: its origin (absolute); client: relative to the body */
    float sh_dir[3], sh_dist;       /*   its direction and the distance to its target */
} AiState;
#define NO_ANIM 0x7fffffff
static AiState states[MAX_AI];
static int n_states;

static AiState *state_of(char *ai)
{
    int net = *(int *)(ai + E_NETID), i = -1;
    for (int k = 0; k < n_states && i < 0; k++)
        if (states[k].ai == ai) {
            if (states[k].net == net)
                return &states[k];
            i = k;   /* (a new net id: the object joined the network game, or the memory holds another AI) */
        }
    if (i < 0 && n_states >= MAX_AI)
        n_states = 0;   /* (full after many levels: start again; only "sent last" values and counters are lost) */
    if (i < 0)
        i = n_states++;
    memset(&states[i], 0, sizeof states[i]);
    states[i].ai = ai;
    states[i].net = net;
    states[i].anim = NO_ANIM;
    return &states[i];
}

/* The AI's animation fields. The AI write puts three bytes after the base message (+0xc the body's animation,
 * +0xd the animation its behaviour wants, +0xe the speed) and a client's AI plays them (0x85a540 picks +8 or +0xc
 * of the AI's net state +0x4f4, 0x859f00 starts it), but the update codec (0x693240) writes only seq, length and
 * the motion bytes: a client read whatever its receive buffer held (animation 0: the "first pose"). They ride
 * here at the end of the motion bytes instead, with the ids as 16 bits (the AI's own bytes cut ids above 127):
 *   s16 body animation, s16 behaviour animation, u8 speed, u8 state (1 = dead on the server), 2 marker bytes.
 * A soldier that is dead on the server (a ragdoll, or one of the level's posed corpses "slo_*", whose body animation
 * id is not -1) must not get the server's animations on a client: they would stand it up. */
#define ANIM_EXTRA 8
#define AI_DEAD 0x20000
static const unsigned char anim_mark[2] = {0xa5, 0x5a};
static const unsigned char anim_mark_shot[2] = {0xa5, 0x5b};   /* (the last shot's geometry before the 8 bytes) */
#define SHOT_EXTRA 14
#define MSG_LEN 5                            /* update message: +5 u8 motion bytes, +8 motion bytes pointer */
#define MSG_BITS 8
#define MSG_ANIM 0xc                         /* the AI's three bytes */
#define AI_NETANIM 0x4f4                     /* AI net state: +8 body animation, +0xc behaviour animation */
#define AI_FLAGS 0x538                       /* 0x10 playing the behaviour's animation, 0x20 animation changed */

static int behaviour_anim(char *ai)
{
    char *stack = *(char **)(ai + 0x288);   /* behaviour stack: +0xc count, entries after it (as 0x858d30 reads) */
    int n = stack ? *(int *)(stack + 0xc) : 0;
    int *top = n > 0 ? *(int **)(stack + 0xc + n * 4) : NULL;
    return top ? ((int (TSCALL *)(void *))(*(void ***)top)[0x58 / 4])(top) : 0;
}

static void TSCALL hook_write(void *ai, int peer, int *msg, int flags)
{
    ((write_t)AI_WRITE)(ai, peer, msg, flags);
    unsigned char *m = msg ? (unsigned char *)*msg : NULL;
    char *node = *(char **)((char *)ai + E_NODE);
    if (!m || !node || !m[MSG_LEN] || !*(unsigned char **)(m + MSG_BITS) || m[MSG_LEN] + ANIM_EXTRA > 255)
        return;
    unsigned char *b = *(unsigned char **)(m + MSG_BITS) + m[MSG_LEN];
    short body = (short)anim_id(node), bhv = (short)behaviour_anim(ai);
    memcpy(b, &body, 2);
    memcpy(b + 2, &bhv, 2);
    AiState *s = state_of(ai);
    int shot = s && s->have_shot && s->shots != s->shots_sent && m[MSG_LEN] + ANIM_EXTRA + SHOT_EXTRA <= 255;
    if (shot) {   /* the last shot's geometry before the tail: s16 origin - body (cm) x3, s16 direction x3, u16 dm */
        float *np = (float *)(node + 0x68);
        short v[7];
        for (int i = 0; i < 3; i++) {
            float d = (s->sh_org[i] - np[i]) * 100.f;
            v[i] = (short)(d > 32000.f ? 32000.f : d < -32000.f ? -32000.f : d);
            v[3 + i] = (short)(s->sh_dir[i] * 32000.f);
        }
        float dm = s->sh_dist * 10.f;
        v[6] = (short)(unsigned short)(dm > 65000.f ? 65000.f : dm < 0.f ? 0.f : dm);
        memcpy(b, v, SHOT_EXTRA);
        b += SHOT_EXTRA;
        memcpy(b, &body, 2);
        memcpy(b + 2, &bhv, 2);
    }
    b[4] = m[MSG_ANIM + 2];
    /* state byte: bit 0 dead on the server, bit 1 attack of the last shot, bits 2-7 its weapon shots (mod 64) */
    b[5] = (unsigned char)(((*(unsigned *)((char *)ai + AI_FLAGS) & AI_DEAD) ? 1 : 0)
                           | (s ? ((s->atk & 1) << 1) | ((s->shots & 63) << 2) : 0));
    memcpy(b + 6, shot ? anim_mark_shot : anim_mark, 2);
    m[MSG_LEN] += ANIM_EXTRA + (shot ? SHOT_EXTRA : 0);
    if (s) {
        s->anim = body;
        s->writes++;
        s->shots_sent = s->shots;
    }
}

/* Client: the server's shot count of a soldier (state byte of its update); new shots are shown by fire_frame. */
static void shots_read(AiState *s, unsigned char state)
{
    int n = state >> 2;
    if (!s->seen_init) {
        s->seen_init = 1;   /* (the first update after the join: only the baseline) */
        s->shots_seen = n;
        return;
    }
    int d = (n - s->shots_seen) & 63;
    s->shots_seen = n;
    if (d > 0 && d < 32) {
        s->pending = s->pending + d > 6 ? 6 : s->pending + d;
        s->atk = (state >> 1) & 1;
    }
}

static void TSCALL hook_read(void *ai, char *msg, int flags)
{
    unsigned char *m = (unsigned char *)msg, *b = *(unsigned char **)(m + MSG_BITS);
    AiState *s = state_of(ai);
    if (s)
        s->reads++;
    if (s && (s->corpse || s->noanim))
        return;   /* (a corpse: the server's "no animation" would stand it up again) */
    if (b && m[MSG_LEN] >= ANIM_EXTRA + SHOT_EXTRA && memcmp(b + m[MSG_LEN] - 2, anim_mark_shot, 2) == 0) {
        unsigned char *sd = b + m[MSG_LEN] - ANIM_EXTRA - SHOT_EXTRA;
        if (s) {
            short v[7];
            memcpy(v, sd, SHOT_EXTRA);
            for (int i = 0; i < 3; i++) {
                s->sh_org[i] = v[i] / 100.f;
                s->sh_dir[i] = v[3 + i] / 32000.f;
            }
            s->sh_dist = (unsigned short)v[6] / 10.f;
            s->have_shot = 1;
        }
        memmove(sd, sd + SHOT_EXTRA, ANIM_EXTRA);   /* (then as an update without the shot) */
        m[MSG_LEN] -= SHOT_EXTRA;
        memcpy(b + m[MSG_LEN] - 2, anim_mark, 2);
    }
    if (!b || m[MSG_LEN] < ANIM_EXTRA || memcmp(b + m[MSG_LEN] - 2, anim_mark, 2) != 0) {
        m[MSG_ANIM] = (unsigned char)-1;   /* (no animation data: "none" rather than the buffer's leftovers) */
        m[MSG_ANIM + 1] = (unsigned char)-1;
        ((read_t)AI_READ)(ai, msg, flags);
        return;
    }
    if (s && !(b[m[MSG_LEN] - 3] & 1))
        shots_read(s, b[m[MSG_LEN] - 3]);
    if (b[m[MSG_LEN] - 3] & 1) {   /* dead on the server: shown here in the next frame (corpse_check) */
        if (s) {
            s->noanim = 1;
            s->srv_dead = 1;
        }
        return;
    }
    m[MSG_LEN] -= ANIM_EXTRA;   /* (the base reader gets its own bytes only) */
    b += m[MSG_LEN];
    short body, bhv;
    memcpy(&body, b, 2);
    memcpy(&bhv, b + 2, 2);
    m[MSG_ANIM] = (unsigned char)body;
    m[MSG_ANIM + 1] = (unsigned char)bhv;
    m[MSG_ANIM + 2] = b[4];
    unsigned before = *(unsigned *)((char *)ai + AI_FLAGS);
    ((read_t)AI_READ)(ai, msg, flags);
    int *net = *(int **)((char *)ai + AI_NETANIM);
    char *node = *(char **)((char *)ai + E_NODE);
    if (net && (net[2] != body || net[3] != bhv)) {   /* (an id above 127: the full one, and the reader's test again) */
        net[2] = body;
        net[3] = bhv;
        unsigned *fl = (unsigned *)((char *)ai + AI_FLAGS);
        if (!(before & 0x10) && node)
            *fl = body != anim_id(node) ? *fl | 0x20 : *fl & ~0x20u;
    }
}

/* After the reader placed the body (every frame): the entity's own copy of its position follows (the server's AI
 * code keeps it equal to the node's; scripts and distance checks read it). */
static void TSCALL hook_pframe(void *comp, void *ent, float dt)
{
    ((pframe_t)PROXY_FRAME)(comp, ent, dt);
    if (!is_ai(ent))
        return;
    char *node = *(char **)((char *)ent + E_NODE);
    if (node)
        memcpy((char *)ent + E_POS, node + 0x68, 12);
    if (debug_secs) {
        AiState *s = state_of(ent);
        if (s)
            s->frames++;
    }
}

/* Server: an AI that stands still is not sent (its motion data did not change), so a new animation (aiming,
 * crouching, an idle turn) would not reach the clients; such an AI gets the entity's "send now" bit (8). */
static void watch_anim(char *ai)
{
    unsigned *role = (unsigned *)(ai + E_ROLE);
    char *node = *(char **)(ai + E_NODE);
    if ((*role & 0x44) != 0x44 || !node)
        return;
    AiState *s = state_of(ai);
    if (s && (anim_id(node) != s->anim || s->shots != s->shots_sent))
        *role |= 8;
}

/* ---- AI weapon fire on clients ----
 * A soldier's shot runs only on the server: its strike logic (authority) sends 0x4e4 with a strike record to the
 * weapon, whose handler (weapon vtbl+0x200 = 0x82cf90: thiscall (weapon, record), ret 4) calls 0x827c60: the
 * attack's fire (the projectile: tracer, flash and sound of the shot; the dispatchers refuse on a client), the
 * ammunition (not for a proxy on a client) and the weapon's shot effects (0x7fa420: the ejected shells, "ai_shells").
 * The server counts each soldier's shots in that handler and keeps the last shot's origin, direction and target
 * distance (shot info at record +8); the count rides in the AI update's state byte and the geometry in 14 more bytes
 * (a soldier whose count changed is sent in the next update). A client that sees the count grow runs the weapon's
 * handler on its copy of the soldier's weapon with the same kind of record, one shot per ~70 ms (show_shot_full).
 * TS_AIFIRE=1 shows the shells only, TS_AIFIRE=0 nothing. TS_FIREDEBUG=1/2 (lab): log, 2 also frames. */
static const DWORD wpn_vtables[] = {0xd92e78, 0xd93158, 0xd93438, 0xd939f8, 0xd93d28, 0xdbefa0};
#define WPN_SHOT 0x82cf90
#define WPN_SLOT 0x200
typedef void (TSCALL *wshot_t)(void *w, unsigned char *rec);
static int fire_debug;
static LONG n_shot_logged, n_replay_logged;

static int is_ai_weapon(char *w)
{
    if (!w || IsBadReadPtr(w, 0x280))
        return 0;
    for (unsigned i = 0; i < sizeof wpn_vtables / sizeof wpn_vtables[0]; i++)
        if (*(DWORD *)w == wpn_vtables[i])
            return 1;
    return 0;
}

/* The weapon's owner as 0x82cf90 finds it (interface at +0x274, vtbl+0xc). */
static char *weapon_owner(char *w)
{
    char *vb = *(char **)(w + 0x274);
    char *obj = w + 0x274 + *(int *)(vb + 4);
    return ((char *(TSCALL *)(void *))(*(void ***)obj)[3])(obj);
}

/* The effect host of an attack of a weapon (0x827c60: the attack must have flag 1 at +0x10; edi = the clip entry
 * [set + atk * 0x14 + 0xc], its effects at +0x10, count +0x14), or NULL when the weapon shows nothing for it. */
static char *shot_clip(char *w, int atk)
{
    char *set = w + 0x278;
    char *p = *(char **)(set + 0xc);
    char **attacks = p && !IsBadReadPtr(p, 4) ? *(char ***)p : NULL;
    char *attack = attacks && !IsBadReadPtr(attacks + atk, 4) ? attacks[atk] : NULL;
    if (!attack || IsBadReadPtr(attack, 0x14) || !(*(unsigned char *)(attack + 0x10) & 1) || !*(char **)set)
        return NULL;
    char *clip = *(char **)(*(char **)set + atk * 0x14 + 0xc);
    return clip && !IsBadReadPtr(clip, 0x18) && *(int *)(clip + 0x14) > 0 ? clip : NULL;
}

typedef struct { char *ai, *found; int atk, owned; } FindWpn;
static void find_wpn_fn(char *o, void *ctx)
{
    FindWpn *f = ctx;
    if (f->found || !is_ai_weapon(o) || weapon_owner(o) != f->ai)
        return;
    f->owned++;
    if (shot_clip(o, f->atk))
        f->found = o;
}
static void for_each_obj(void (*fn)(char *o, void *ctx), void *ctx);
/* The soldier's weapon that shows shots: among the AI weapon objects whose owner (as the engine finds it) is the
 * soldier (soldiers carry two), the first with shot effects for the attack. The soldier's own fields did not hold
 * it at one fixed place, so the objects are searched; the result is kept while it still belongs to the soldier. */
static char *ai_weapon(char *ai, char **cache, int atk)
{
    if (*cache && is_ai_weapon(*cache) && weapon_owner(*cache) == ai)
        return *cache;
    FindWpn f = {ai, NULL, atk, 0};
    for_each_obj(find_wpn_fn, &f);
    if (fire_debug && InterlockedIncrement(&n_shot_logged) <= 40)
    {
        char *c = f.found ? shot_clip(f.found, atk) : NULL, list[200] = "";
        for (int i = 0; c && i < *(int *)(c + 0x14) && i < 8; i++) {
            void *e = (*(void ***)(c + 0x10))[i];
            snprintf(list + strlen(list), sizeof list - strlen(list), " %p", e ? *(void **)e : NULL);
        }
        ts_log("aisync: %s: %d weapon(s), %s%s", ai_name(ai), f.owned, f.found ? "one with shot effects, vtables" :
               "none with shot effects", list);
    }
    return *cache = f.found;
}

static void TSCALL hook_wpn_shot(void *w, unsigned char *rec)
{
    ((wshot_t)WPN_SHOT)(w, rec);
    char *gm = GAME_MODE_OBJ;
    if (!engine_mode || !gm || *(int *)(gm + 0x3c) == 3 || *(int *)(gm + 0x3c) < 2 || !rec)
        return;
    char *owner = weapon_owner(w);
    if (!is_ai(owner))
        return;
    AiState *s = state_of(owner);
    if (!s)
        return;
    s->shots++;
    s->atk = rec[4];
    float *si = *(float **)(rec + 8);   /* shot info: +8 weapon, +0xc shooter, +0x18 origin, +0x24 direction, +0x3c target */
    if (si && !IsBadReadPtr(si, 0x48)) {
        float d2 = 0;
        for (int i = 0; i < 3; i++) {
            s->sh_org[i] = si[6 + i];
            s->sh_dir[i] = si[9 + i];
            d2 += (si[15 + i] - si[6 + i]) * (si[15 + i] - si[6 + i]);
        }
        s->sh_dist = sqrtf(d2);
        s->have_shot = 1;
    }
    if (fire_debug > 1 && s->shots < 30)
        ts_shot_next();   /* (lab: the host's frame of a shot, to compare) */
    if (fire_debug && s->shots <= 2) {   /* (lab: the shot info the attack gets, next to the soldier's position) */
        float *si = *(float **)(rec + 8);
        char *node = *(char **)(owner + E_NODE);
        float *np = node ? (float *)(node + 0x68) : NULL;
        if (si && !IsBadReadPtr(si, 0x60))
            ts_log("aisync: shot info of %s (at %.2f %.2f %.2f): %08x %08x %p %p | %.3f %.3f %.3f | %.3f %.3f %.3f | "
                   "%.3f %.3f %.3f | %.3f %.3f %.3f | %08x %08x %08x %08x", ai_name(owner), np ? np[0] : 0.f,
                   np ? np[1] : 0.f, np ? np[2] : 0.f, ((unsigned *)si)[0], ((unsigned *)si)[1], ((void **)si)[2],
                   ((void **)si)[3], si[4], si[5], si[6], si[7], si[8], si[9], si[10], si[11], si[12], si[13], si[14],
                   si[15], ((unsigned *)si)[16], ((unsigned *)si)[17], ((unsigned *)si)[18], ((unsigned *)si)[19]);
    }
    if (fire_debug && InterlockedIncrement(&n_shot_logged) <= 12)
        ts_log("aisync: shot by %s: weapon %p (shot effects: %s) record %02x %02x %02x %02x %02x %02x %02x "
               "%02x (+8 %p) count %d", ai_name(owner), w, shot_clip(w, rec[4] & 1) ? "effects" : "none", rec[0], rec[1], rec[2], rec[3],
               rec[4], rec[5], rec[6], rec[7], *(void **)(rec + 8), s->shots);
}

/* Client: the shot effects of the soldier's weapon, as the server's 0x827c60 runs them after the attack's fire. */
static int show_shot(char *ai, int atk, char **cache)
{
    char *w = ai_weapon(ai, cache, atk), *clip = w ? shot_clip(w, atk) : NULL, *owner = w ? weapon_owner(w) : NULL;
    if (!clip || !owner)
        return 0;
    unsigned fn = 0x7fa420;
    __asm__ volatile("pushl $1\n\tpushl %[owner]\n\tcall *%[fn]"
                     : "+D"(clip)
                     : [owner] "r"(owner), [fn] "r"(fn)
                     : "eax", "ecx", "edx", "memory", "cc");
    return 1;
}

/* The whole shot on a client: the weapon's own 0x4e4 handler with a strike record and shot info like the server's
 * (origin, direction and target from the server's last shot, relative to the body here). Its attack creates the
 * shot's projectile (muzzle flash, tracer, sound: the shells alone are the weapon's shot effects); the projectile
 * dispatchers refuse in game mode 3 and are let through for this call only (dispatch_gate). The projectile is a
 * local object of the client (no authority), the server's shot does the damage. */
static volatile int replaying;
static DWORD replay_info[0x20];

static void log_pjl_fn(char *o, void *ctx)
{
    (void)ctx;
    DWORD vt = *(DWORD *)o;
    if (vt == 0xd97ea8 || vt == 0xd9a1c8 || vt == 0xd9a648)
        ts_log("aisync: projectile %p vt %06lx role %08x net %d", o, vt, *(unsigned *)(o + E_ROLE), *(int *)(o + E_NETID));
}

static int show_shot_full(char *ai, AiState *s)
{
    char *w = ai_weapon(ai, &s->wpn, s->atk);
    char *node = *(char **)(ai + E_NODE);
    if (!w || !node || !s->have_shot)
        return 0;
    float *np = (float *)(node + 0x68), *f = (float *)replay_info;
    memset(replay_info, 0, sizeof replay_info);
    replay_info[0] = 0x0054400d;   /* (the values the server's shot info has in these fields) */
    replay_info[1] = 2;
    ((char **)replay_info)[2] = w;
    ((char **)replay_info)[3] = ai;
    f[4] = 1.0f;
    for (int i = 0; i < 3; i++) {
        f[6 + i] = np[i] + s->sh_org[i];
        f[9 + i] = s->sh_dir[i];
        f[15 + i] = f[6 + i] + s->sh_dir[i] * s->sh_dist;
    }
    replay_info[19] = 0x2002;
    unsigned char rec[12] = {0x14, 0x40, 0x0c, 0x00, s->atk, 1, 0x3f, 0x01};
    *(DWORD **)(rec + 8) = replay_info;
    replaying = 1;
    ((wshot_t)WPN_SHOT)(w, rec);
    replaying = 0;
    static int checked;
    if (fire_debug && checked < 3) {   /* (lab: the projectiles here, their role bits: no authority bit 4 expected) */
        checked++;
        for_each_obj(log_pjl_fn, NULL);
    }
    return 1;
}

/* The two projectile dispatchers' mode test (mov eax,[eax+0x3c]; cmp eax,3) as a call: mode 3 reads as 1 while a
 * client replays a shot. */
__attribute__((naked)) static void dispatch_gate(void)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "mov eax, dword ptr [eax + 0x3c]\n"
        "cmp dword ptr [_replaying], 0\n"
        "je 1f\n"
        "cmp eax, 3\n"
        "jne 1f\n"
        "mov eax, 1\n"
        "1:\n"
        "cmp eax, 3\n"
        "ret\n"
        ".att_syntax prefix\n");
}

static int gate_install(void)
{
    static const unsigned char orig[6] = {0x8b, 0x40, 0x3c, 0x83, 0xf8, 0x03};
    unsigned char call[6] = {0xe8, 0, 0, 0, 0, 0x90};
    int ok = 0;
    DWORD at[2] = {0x7f7f25, 0x7f838e};
    for (int i = 0; i < 2; i++) {
        *(DWORD *)(call + 1) = (DWORD)dispatch_gate - (at[i] + 5);
        ok += ts_patch_bytes(at[i], orig, call, 6);
    }
    return ok == 2;
}

static int fire_full = 1;

static void fire_frame(char *ai)
{
    AiState *s = state_of(ai);
    DWORD t = GetTickCount();
    if (fire_full < 0 || !s || !s->pending || s->corpse || s->noanim || t - s->last_shot < 70)
        return;
    s->pending--;
    s->last_shot = t;
    int ok = fire_full > 0 && show_shot_full(ai, s) ? 2 : show_shot(ai, s->atk, &s->wpn);
    if (fire_debug && InterlockedIncrement(&n_replay_logged) <= 40) {
        ts_log("aisync: shot of %s shown here: %s (server count %d)", ai_name(ai),
               ok == 2 ? "effects, projectile" : ok ? "effects" : "no weapon", s->shots_seen);
        if (ok && fire_debug > 1)
            ts_shot_next();   /* (TS_FIREDEBUG=2: the frame with the shot) */
    }
}

static int fire_install(void)
{
    int ok = 0;
    for (unsigned i = 0; i < sizeof wpn_vtables / sizeof wpn_vtables[0]; i++) {
        void *o = (void *)WPN_SHOT, *h = (void *)hook_wpn_shot;
        ok += ts_patch_bytes(wpn_vtables[i] + WPN_SLOT, &o, &h, 4);
    }
    return ok;
}

/* ---- diagnostics ---- */
static void dump_one(char *ai)
{
    float *p = (float *)(ai + E_POS);
    char *node = *(char **)(ai + E_NODE);
    float *m = node ? (float *)(node + 0x68) : p;
    int *net = *(int **)(ai + AI_NETANIM);
    AiState *s = state_of(ai);
    ts_log("aisync: %-36s net=%-5d role=%08x node=%7.2f %6.2f %7.2f anim=%d@%.2f net=%d,%d fl=%x w=%d r=%d f=%d",
           ai_name(ai), *(int *)(ai + E_NETID), *(unsigned *)(ai + E_ROLE), m[0], m[1], m[2],
           node ? anim_id(node) : -9, node ? anim_time(node) : -9.f, net ? net[2] : -9, net ? net[3] : -9,
           *(unsigned *)(ai + AI_FLAGS), s ? s->writes : -1, s ? s->reads : -1, s ? s->frames : -1);
}

static int hook_slots(void)
{
    int ok = 0;
    for (unsigned i = 0; i < N_VT; i++) {
        void *w = (void *)AI_WRITE, *r = (void *)AI_READ, *hw = (void *)hook_write, *hr = (void *)hook_read;
        ok += ts_patch_bytes(ai_vtables[i] + 0x64, &w, &hw, 4);
        ok += ts_patch_bytes(ai_vtables[i] + 0x68, &r, &hr, 4);
    }
    void *f = (void *)PROXY_FRAME, *hf = (void *)hook_pframe;
    ok += ts_patch_bytes(PROXY_VT + 0x1c, &f, &hf, 4);
    return ok == (int)(2 * N_VT + 1);
}

/* Test helper (diagnostics on, client): a file <TS_SHOTDIR>\follow.req "side back up" puts the local player next to
 * the host's body (metres in the host's frame) facing the same way, so both machines' frames show the same scene. */
static char follow_req[MAX_PATH];

static void follow_host(void)
{
    FILE *f = follow_req[0] ? fopen(follow_req, "r") : NULL;
    if (!f)
        return;
    float side = 1, back = 0, up = 0;
    int n = fscanf(f, "%f %f %f", &side, &back, &up);
    fclose(f);
    DeleteFileA(follow_req);
    char *gm = GAME_MODE_OBJ, *me = *(char **)0x1095b30, *host = NULL;
    for (int i = 0; gm && i < *(int *)(gm + 0x2c); i++) {
        char *slot = *(char **)(gm + 0x28) + i * 0xa4;
        if (*(int *)slot == 0)
            host = *(char **)(slot + 4);
    }
    char *node = host ? *(char **)(host + E_NODE) : NULL;
    if (!me || !node) {
        ts_log("aisync: follow: no host body");
        return;
    }
    float *fw = (float *)(node + 0x58), *hp = (float *)(node + 0x68);
    float pos[3] = {hp[0] - fw[0] * back + fw[2] * side, hp[1] + up, hp[2] - fw[2] * back - fw[0] * side};
    float dir[3] = {fw[0], fw[1], fw[2]};
    void (TSCALL *place)(void *, float *, float *, int) = (*(void ***)me)[0x210 / 4];
    place(me, pos, dir, 1);
    ts_log("aisync: follow (%d values): host %.2f %.2f %.2f, me %.2f %.2f %.2f", n, hp[0], hp[1], hp[2], pos[0], pos[1],
           pos[2]);
}

/* ---- names of objects created during play ----
 * A client builds the replica of an object the server created at run time ($farm.Create) without the object's
 * name: its script instance is "" and the level scripts (and storysync's replays) cannot find it. The client asks
 * the server for the names of networked objects whose instance has no name (50), the server answers with the
 * names it has (51), and the client gives its instance that name (inst+4) and puts it into the script manager's
 * sorted list of named objects, as CreateObject (0x8fd2f0) does. */
#define SUB_NAME_ASK 50
#define SUB_NAME_TELL 51
#define SUB_DEAD 52
#define SUB_LAST 59
#define NPROTO 1
#define MSG_MAX 200                          /* (payload bytes per message: well below the codec's limit) */
#define MGR (*(char **)0xebedec)             /* script manager: +0x14 named objects (Inst *[], sorted), +0x18 count */
#define PLAYER (*(char **)0x1095b30)
typedef struct { int refs, len, cap; char s[1]; } StrHdr;
static void (__cdecl *eng_free)(void *) = (void *)0x7e4700;
static StrHdr **(TSCALL *str_ctor)(StrHdr **, const char *, int, int) = (void *)0x402490;
static void **(TSCALL *mgr_find_obj)(void *, void **, StrHdr **) = (void *)0x8fc720;
static void (TSCALL *inst_release)(void **) = (void *)0x8f8050;
static int (TSCALL *named_cmp)(void *, void **, void **) = (void *)0x8fb500;      /* <0 / 0 / >0 by name */
static void (TSCALL *named_insert)(void *, int, void **, int) = (void *)0x8fb000; /* (array, index, &inst, 1) */

static void str_release(StrHdr *s)
{
    if (s && --s->refs == 0)
        eng_free(s);
}

/* The script instance of a level object (entity +0x80), or NULL. */
static char *inst_of(char *o)
{
    if (IsBadReadPtr(o, 0x84))
        return NULL;
    char *inst = *(char **)(o + E_INST);
    if (!inst || ((DWORD)inst & 3) || IsBadReadPtr(inst, 0x14) || *(char **)(inst + 0xc) != o)
        return NULL;
    return inst;
}

static const char *inst_str(char *inst)
{
    StrHdr *s = *(StrHdr **)(inst + 4);
    return s ? s->s : "";
}

/* Every object in the scheduler's lists (main thread). */
static void for_each_obj(void (*fn)(char *o, void *ctx), void *ctx)
{
    char *b = ENT_BUCKETS;
    int nb = ENT_NBUCKETS;
    for (int i = 0; b && i < nb; i++)
        for (char *o = *(char **)(b + i * 12); o; ) {
            fn(o, ctx);
            char *next = *(char **)(o + 8);
            o = next ? next - 4 : NULL;
        }
}

typedef struct { int net; char *found; } FindNet;
static void find_net_fn(char *o, void *ctx)
{
    FindNet *f = ctx;
    if (!f->found && *(int *)(o + E_NETID) == f->net && inst_of(o))
        f->found = o;
}
static char *obj_by_net(int net)
{
    FindNet f = {net, NULL};
    for_each_obj(find_net_fn, &f);
    return f.found;
}

/* Give an unnamed script instance its name (client). 1 = done. */
static int name_inst(char *inst, const char *name)
{
    char *mgr = MGR;
    StrHdr *nm = NULL;
    void *found = NULL;
    str_ctor(&nm, name, -1, 0);
    mgr_find_obj(mgr, &found, &nm);
    if (found) {   /* (a local object of that name exists: keep both as they are) */
        inst_release(&found);
        str_release(nm);
        return 0;
    }
    StrHdr *old = *(StrHdr **)(inst + 4);
    *(StrHdr **)(inst + 4) = nm;
    str_release(old);
    void **data = *(void ***)(mgr + 0x14), *ip = inst;
    int lo = 0, hi = *(int *)(mgr + 0x18);
    while (lo != hi) {
        int mid = (lo + hi) >> 1, c = named_cmp(NULL, &data[mid], &ip);
        if (c == 0)
            return 0;
        if (c > 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    named_insert(mgr + 0x14, lo, &ip, 1);
    return 1;
}

static int slot_of_peer(int peer)
{
    char *gm = GAME_MODE_OBJ;
    for (int i = 0; gm && i < *(int *)(gm + 0x2c); i++)
        if (*(int *)(*(char **)(gm + 0x28) + i * 0xa4) == peer)
            return i;
    return -1;
}

static int is_player_ent(char *o)
{
    char *gm = GAME_MODE_OBJ;
    if (o == PLAYER)
        return 1;
    for (int i = 0; gm && i < *(int *)(gm + 0x2c); i++)
        if (*(char **)(*(char **)(gm + 0x28) + i * 0xa4 + 4) == o)
            return 1;
    return 0;
}

/* client: the objects asked for (pointer + net id), reset with the level */
#define ASKED_MAX 2048
static struct { char *o; int net; } asked[ASKED_MAX];
static int n_asked;
static char asked_level[64];
/* client: names received, applied in the next frame */
#define TOLD_MAX 256
static struct { int net; char name[64]; DWORD until; } told[TOLD_MAX];
static int n_told;
static LONG n_named_logged;

typedef struct { unsigned char b[MSG_MAX]; int n, count; } AskMsg;
static void ask_fn(char *o, void *ctx)
{
    AskMsg *m = ctx;
    int net = *(int *)(o + E_NETID);
    if (net < 0 || net > 0xffffff || m->n + 4 > MSG_MAX || is_player_ent(o))
        return;
    char *inst = inst_of(o);
    if (!inst || inst_str(inst)[0])
        return;
    for (int i = 0; i < n_asked; i++)
        if (asked[i].o == o && asked[i].net == net)
            return;
    if (n_asked < ASKED_MAX) {
        asked[n_asked].o = o;
        asked[n_asked++].net = net;
    }
    memcpy(m->b + m->n, &net, 4);
    m->n += 4;
    m->count++;
}

static void names_client(void)
{
    char lvl[64];
    snprintf(lvl, sizeof lvl, "%.63s", (const char *)0xfcabc8);
    if (strcmp(lvl, asked_level) != 0) {
        strcpy(asked_level, lvl);
        n_asked = n_told = 0;
    }
    DWORD now = GetTickCount();
    for (int i = 0; i < n_told; ) {
        char *o = obj_by_net(told[i].net), *inst = o ? inst_of(o) : NULL;
        if (inst && !inst_str(inst)[0]) {
            int ok = name_inst(inst, told[i].name);
            if (InterlockedIncrement(&n_named_logged) <= 200)
                ts_log("aisync: object net %d named \"%s\"%s", told[i].net, told[i].name,
                       ok ? "" : " - not done (a local object has that name)");
        } else if (!o && (int)(now - told[i].until) < 0) {
            i++;
            continue;
        }
        told[i] = told[--n_told];
    }
    static DWORD last;
    if (now - last < 500 || !ts_coop_feature(COOP_F_ACTIVE))   /* (asks once the host's session info arrived: the */
        return;                                                /* server answers clients that run the level only) */
    last = now;
    AskMsg m;
    m.b[0] = NPROTO;
    m.n = 2;
    m.count = 0;
    for_each_obj(ask_fn, &m);
    if (m.count) {
        m.b[1] = (unsigned char)m.count;
        coopnet_send(-1, SUB_NAME_ASK, m.b, m.n);
    }
}

static void on_names(int peer, unsigned short sub, const unsigned char *d, int len)
{
    char *gm = GAME_MODE_OBJ;
    int mode = gm ? *(int *)(gm + 0x3c) : 0;
    if (len < 2 || d[0] != NPROTO)
        return;
    if (sub == SUB_NAME_ASK && mode != 3) {   /* server: answer with the names it has */
        unsigned char out[MSG_MAX];
        int n = 2, count = 0;
        out[0] = NPROTO;
        for (int i = 0; i < d[1] && 2 + i * 4 + 4 <= len; i++) {
            int net;
            memcpy(&net, d + 2 + i * 4, 4);
            char *o = obj_by_net(net), *inst = o && !is_player_ent(o) ? inst_of(o) : NULL;
            const char *nm = inst ? inst_str(inst) : "";
            int l = (int)strlen(nm);
            if (!l || l > 63)
                continue;
            if (n + 5 + l > MSG_MAX) {   /* (full: send this part) */
                out[1] = (unsigned char)count;
                coopnet_send(slot_of_peer(peer), SUB_NAME_TELL, out, n);
                n = 2;
                count = 0;
            }
            memcpy(out + n, &net, 4);
            out[n + 4] = (unsigned char)l;
            memcpy(out + n + 5, nm, l);
            n += 5 + l;
            count++;
        }
        if (count) {
            out[1] = (unsigned char)count;
            coopnet_send(slot_of_peer(peer), SUB_NAME_TELL, out, n);
        }
    } else if (sub == SUB_NAME_TELL && mode == 3) {
        int n = 2;
        for (int i = 0; i < d[1] && n + 5 <= len && n_told < TOLD_MAX; i++) {
            int l = d[n + 4];
            if (n + 5 + l > len || l > 63)
                break;
            memcpy(&told[n_told].net, d + n, 4);
            memcpy(told[n_told].name, d + n + 5, l);
            told[n_told].name[l] = 0;
            told[n_told++].until = GetTickCount() + 5000;
            n += 5 + l;
        }
    }
}

/* ---- corpses on clients ----
 * An AI's death reaches a client as the death state (0x859090 case 2 -> [ai+0x290]->vtbl[1] = 0x8496c0, with
 * netfix.c's empty hit info); that handler shows nothing without the authority bit: the visual part (the class's
 * vtbl+0x2e8, e.g. 0x466b90: hit reaction, death animation or ragdoll from the damage) runs on the server only, so
 * the client's soldier stood in its idle. A client runs that visual part itself for every dead AI (AI_FLAGS
 * 0x20000, set by the death state on both machines) with the authority bit set for the call and a neutral damage
 * (no attacker, no damage type, a small push): the soldier falls as a ragdoll where it stands. Afterwards its
 * network reads are ignored (the server sends "no animation" for its ragdoll, which would stand it up).
 * Measured on the N100 pair (2026-10-25): police4_v2 fell like the host's ragdoll, a few cm apart. */
#define AI_DEATHSTATE 0x290
static unsigned char fake_dmg[0x200], fake_type[0x200], fake_hit[0x1000];
static LONG n_corpse_logged;

static void show_death(char *ai, float push, int auth)
{
    unsigned *role = (unsigned *)(ai + E_ROLE), saved = *role;
    memset(fake_dmg, 0, sizeof fake_dmg);
    memset(fake_type, 0, sizeof fake_type);
    memset(fake_hit, 0, sizeof fake_hit);
    *(int *)(fake_dmg + 8) = -1;               /* attacker: none */
    *(void **)(fake_dmg + 0xc) = fake_type;    /* damage type: nothing special */
    *(float *)(fake_dmg + 0x14) = push;        /* impulse */
    *(float *)(fake_dmg + 0x1c) = 1e9f;        /* hit point: none (the class uses its own) */
    *(int *)(fake_dmg + 0x34) = -1;
    char *mgr = MGR;
    int before = mgr ? *(int *)(mgr + 0xc) : 0;   /* (script manager: +8 every object, +0xc count; new ones appended) */
    if (auth)
        *role |= 4;
    ((void (TSCALL *)(void *, void *, void *))(*(void ***)ai)[0x2e8 / 4])(ai, fake_dmg, fake_hit);
    *role = (*role & ~4u) | (saved & 4);
    /* The visual death also drops the soldier's weapon and grenade pack as pickups. The server's pickups reach
     * the client through the network; the client's own copies would be duplicates a player could pick up (seen:
     * "Received: carbine" on the client). Objects this call created that the network would carry (isSyncMP) but
     * that have no net id are such copies: they end at once (entity vtbl+0xc0 = Terminate). */
    int after = mgr ? *(int *)(mgr + 0xc) : 0, removed = 0;
    void **all = mgr ? *(void ***)(mgr + 8) : NULL;
    for (int i = before; all && i < after; i++) {
        char *inst = all[i], *o = inst && !IsBadReadPtr(inst, 0x14) ? *(char **)(inst + 0xc) : NULL;
        if (!o || o == ai || IsBadReadPtr(o, 0xb0) || *(int *)(o + E_NETID) >= 0 || !(*(unsigned *)(o + E_SYNC) & 1))
            continue;
        if (InterlockedIncrement(&n_corpse_logged) <= 100)
            ts_log("aisync: %s: local copy \"%s\" of a networked object removed", ai_name(ai), inst_str(inst));
        ((void (TSCALL *)(void *, float))(*(void ***)o)[0xc0 / 4])(o, 0.0f);
        removed++;
    }
    (void)removed;
}

static void kill_here(char *ai);

/* 1 = the body is a ragdoll already (the body component +0x4fc, its ragdoll +0x7c, vtbl+0x18 "active", as the
 * level's posed corpses get it at load, 0x612670). */
static int ragdoll_active(char *ai)
{
    char *body = *(char **)(ai + 0x4fc);
    void *r = body ? *(void **)(body + 0x7c) : NULL;
    return r && ((int (TSCALL *)(void *))(*(void ***)r)[0x18 / 4])(r) != 0;
}

static void corpse_check(char *ai)
{
    if (*(unsigned *)(ai + E_ROLE) & 4)
        return;
    AiState *s = state_of(ai);
    if (!s || s->corpse)
        return;
    int dead = (*(unsigned *)(ai + AI_FLAGS) & AI_DEAD) != 0, killed = 0;
    if (!dead && s->srv_dead) {   /* (dead on the server, alive here: it died before this client joined) */
        kill_here(ai);
        killed = 1;
        dead = (*(unsigned *)(ai + AI_FLAGS) & AI_DEAD) != 0;
    }
    if (!dead)
        return;
    s->corpse = s->noanim = 1;
    char *node = *(char **)(ai + E_NODE);
    int before = node ? anim_id(node) : -9, rag = ragdoll_active(ai);
    if (!rag)
        show_death(ai, 1.0f, 1);
    if (InterlockedIncrement(&n_corpse_logged) <= 100)
        ts_log("aisync: %s (net %d) %s: %s (animation %d -> %d)", ai_name(ai), *(int *)(ai + E_NETID),
               killed ? "is dead on the server" : "died", rag ? "a ragdoll already" : "death shown here", before,
               node ? anim_id(node) : -9);
}

/* The death state on a client, as the server's message sets it (a soldier that died before this client joined). */
static void kill_here(char *ai)
{
    char *st = *(char **)(ai + AI_DEATHSTATE);
    memset(fake_hit, 0, sizeof fake_hit);
    if (st)
        ((void (TSCALL *)(void *, int, void *))(*(void ***)st)[1])(st, 0, fake_hit);
}

/* Dead before the join: AIs that died on the server before a client loaded the level stand alive on the client
 * (the death was a one-time message). A client asks for the list of the dead (52) when the host's session info has
 * arrived and 10 s later; the server answers with the names and positions of every dead AI it has (53, several
 * messages when long), from
 * the script manager's named objects (a dead soldier leaves the scheduler's lists). The client puts each of them
 * where the server's body lies, gives it the death state and shows its death. */
#define SUB_DEAD_ASK 52
#define SUB_DEAD_TELL 53
#define DEAD_MAX 256
static struct { char name[64]; float pos[3]; } dead_told[DEAD_MAX];
static int n_dead_told;
static LONG n_dead_logged;

static void dead_list_send(int peer)
{
    char *mgr = MGR;
    void **data = mgr ? *(void ***)(mgr + 0x14) : NULL;
    int count = mgr ? *(int *)(mgr + 0x18) : 0, total = 0;
    unsigned char out[MSG_MAX];
    int n = 2, k = 0;
    out[0] = NPROTO;
    for (int i = 0; data && i <= count; i++) {
        char *inst = i < count ? data[i] : NULL, *ai = inst ? *(char **)(inst + 0xc) : NULL;
        int last = i == count;
        const char *nm = "";
        int l = 0;
        if (!last) {
            if (!ai || !is_ai(ai) || (*(unsigned *)(inst + 8) & 1) || !(*(unsigned *)(ai + AI_FLAGS) & AI_DEAD))
                continue;
            nm = inst_str(inst);
            l = (int)strlen(nm);
            if (!l || l > 63)
                continue;
        }
        if (last || n + 13 + l > MSG_MAX) {
            if (k || (last && !total)) {
                out[1] = (unsigned char)k;
                coopnet_send(slot_of_peer(peer), SUB_DEAD_TELL, out, n);
            }
            n = 2;
            k = 0;
            if (last)
                break;
        }
        char *node = *(char **)(ai + E_NODE);
        float *p = node ? (float *)(node + 0x68) : (float *)(ai + E_POS);
        out[n] = (unsigned char)l;
        memcpy(out + n + 1, nm, l);
        memcpy(out + n + 1 + l, p, 12);
        n += 13 + l;
        k++;
        total++;
    }
    if (InterlockedIncrement(&n_dead_logged) <= 50)
        ts_log("aisync: client %d asked for the dead: %d AI", peer, total);
}

static void dead_list_apply(void)
{
    for (int i = 0; i < n_dead_told; i++) {
        char *ai = NULL;
        StrHdr *nm = NULL;
        void *inst = NULL;
        str_ctor(&nm, dead_told[i].name, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        str_release(nm);
        if (inst) {
            ai = *(char **)((char *)inst + 0xc);
            inst_release(&inst);
        }
        if (!ai || !is_ai(ai) || (*(unsigned *)(ai + E_ROLE) & 4) || (*(unsigned *)(ai + AI_FLAGS) & AI_DEAD))
            continue;
        char *node = *(char **)(ai + E_NODE);
        float *p = node ? (float *)(node + 0x68) : (float *)(ai + E_POS), *q = dead_told[i].pos;
        float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
        int moved = 0;
        if (node && dx * dx + dy * dy + dz * dz > 0.25f) {   /* (put the body where the server's lies) */
            float dir[3] = {*(float *)(node + 0x58), *(float *)(node + 0x5c), *(float *)(node + 0x60)};
            ((void (TSCALL *)(void *, float *, float *, int))(*(void ***)ai)[0x210 / 4])(ai, q, dir, 1);
            memcpy(ai + E_POS, q, 12);
            moved = 1;
        }
        kill_here(ai);
        corpse_check(ai);
        if (InterlockedIncrement(&n_dead_logged) <= 100)
            ts_log("aisync: %s died before this client joined: dead here too%s", dead_told[i].name,
                   moved ? " (moved to the server's body)" : "");
    }
    n_dead_told = 0;
}

static void on_dead(int peer, unsigned short sub, const unsigned char *d, int len)
{
    char *gm = GAME_MODE_OBJ;
    int mode = gm ? *(int *)(gm + 0x3c) : 0;
    if (len < 2 || d[0] != NPROTO)
        return;
    if (sub == SUB_DEAD_ASK && mode != 3)
        dead_list_send(peer);
    else if (sub == SUB_DEAD_TELL && mode == 3) {
        int n = 2;
        for (int i = 0; i < d[1] && n < len && n_dead_told < DEAD_MAX; i++) {
            int l = d[n];
            if (n + 13 + l > len || l > 63)
                break;
            memcpy(dead_told[n_dead_told].name, d + n + 1, l);
            dead_told[n_dead_told].name[l] = 0;
            memcpy(dead_told[n_dead_told++].pos, d + n + 1 + l, 12);
            n += 13 + l;
        }
    }
}

static void on_message(int peer, unsigned short sub, const unsigned char *d, int len)
{
    if (sub == SUB_NAME_ASK || sub == SUB_NAME_TELL)
        on_names(peer, sub, d, len);
    else if (sub == SUB_DEAD_ASK || sub == SUB_DEAD_TELL)
        on_dead(peer, sub, d, len);
}

/* client: ask for the dead when the host's session info has arrived (the server talks to a client only once it
 * runs the level) and again 10 s later */
static void dead_ask_tick(void)
{
    static char level[64];
    static DWORD since;
    static int asked_n;
    char lvl[64];
    snprintf(lvl, sizeof lvl, "%.63s", (const char *)0xfcabc8);
    if (!PLAYER || !ts_coop_feature(COOP_F_ACTIVE)) {
        since = 0;
        return;
    }
    if (strcmp(lvl, level) != 0 || !since) {
        strcpy(level, lvl);
        since = GetTickCount();
        asked_n = 0;
    }
    DWORD age = GetTickCount() - since;
    if ((asked_n == 0 && age > 500) || (asked_n == 1 && age > 10000)) {
        unsigned char b[2] = {NPROTO, 0};
        coopnet_send(-1, SUB_DEAD_ASK, b, 2);
        asked_n++;
    }
}

/* ---- per frame ---- */
static void frame(float dt)
{
    (void)dt;
    char *gm = GAME_MODE_OBJ;
    int mode = gm ? *(int *)(gm + 0x3c) : 0;
    if (mode < 2 || !(GAME_FLAGS & 0x2000))
        return;
    DWORD t = GetTickCount();
    static DWORD last_adopt;
    if (engine_mode && t - last_adopt >= 500) {
        last_adopt = t;
        for_each_ai(adopt);
    }
    if (engine_mode && mode != 3)
        for_each_ai(watch_anim);
    if (engine_mode && mode == 3) {
        if (n_dead_told)
            dead_list_apply();
        for_each_ai(corpse_check);
        for_each_ai(fire_frame);
        if (ts_coop_active()) {
            dead_ask_tick();
            names_client();
        }
    }
    if (debug_secs && mode == 3)
        follow_host();
    if (debug_secs) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        int now = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
        static int last;
        if (now != last && now % debug_secs == 0) {
            last = now;
            ts_log("aisync: --- mode %d", mode);
            ts_log("aisync: %d AI", for_each_ai(dump_one));
        }
    }
}

/* ---- test console: "!ai ..." (experiments) ---- */
static char *ai_by_name_found;
static const char *ai_by_name_want;
static void ai_by_name_fn(char *ai)
{
    if (!ai_by_name_found && strcmp(ai_name(ai), ai_by_name_want) == 0)
        ai_by_name_found = ai;
}
static char *ai_by_name(const char *name)
{
    ai_by_name_found = NULL;
    ai_by_name_want = name;
    for_each_ai(ai_by_name_fn);
    return ai_by_name_found;
}

static char *con_out;
static int con_size, con_n;
static const char *con_filter;
static void con_list_fn(char *ai)
{
    const char *nm = ai_name(ai);
    if (con_filter[0] && !strstr(nm, con_filter))
        return;
    char *node = *(char **)(ai + E_NODE);
    AiState *s = state_of(ai);
    if (con_n < con_size - 200)
        con_n += snprintf(con_out + con_n, con_size - con_n,
                          "%s vt=%p net=%d role=%x fl=%x hp=%.2f anim=%d rag=%d corpse=%d/%d\n", nm, *(void **)ai,
                          *(int *)(ai + E_NETID), *(unsigned *)(ai + E_ROLE), *(unsigned *)(ai + AI_FLAGS),
                          *(float *)(ai + 0x234), node ? anim_id(node) : -9, ragdoll_active(ai), s ? s->corpse : -1,
                          s ? s->srv_dead : -1);
}

/* Test console (TS_CONSOLE): "!ai list <part of a name>|*", "!ai kill <ai>" (the death state, as the server's
 * message sets it), "!ai show <ai> [push]" (show its death here), "!ai named" (client: objects named so far). */
static void con_wpn_fn(char *o, void *ctx)
{
    (void)ctx;
    if (!is_ai_weapon(o) || con_n > con_size - 120)
        return;
    char *ow = weapon_owner(o);
    con_n += snprintf(con_out + con_n, con_size - con_n, " %p vt %06lx owner %p %s;", o, *(unsigned long *)o, ow,
                      is_ai(ow) ? ai_name(ow) : "(not AI)");
}

int aisync_console(const char *text, char *out, int size)
{
    if (strncmp(text, "!ai", 3) != 0)
        return 0;
    char cmd[32] = "", a1[128] = "";
    float push = 1.0f;
    sscanf(text + 3, "%31s %127s %f", cmd, a1, &push);
    if (!strcmp(cmd, "list")) {
        con_out = out;
        con_size = size;
        con_n = 0;
        con_filter = strcmp(a1, "*") ? a1 : "";
        out[0] = 0;
        int n = for_each_ai(con_list_fn);
        snprintf(out + con_n, size - con_n, "%d AI", n);
        return 1;
    }
    if (!strcmp(cmd, "weapons")) {   /* every AI weapon object: pointer, vtable, owner */
        con_out = out;
        con_size = size;
        con_n = snprintf(out, size, "AI weapons:");
        for_each_obj(con_wpn_fn, NULL);
        return 1;
    }
    if (!strcmp(cmd, "mem")) {   /* "!ai mem <object> <hex offset> [dwords]": a named object's memory (lab) */
        unsigned off = 0;
        int cnt = 16;
        sscanf(text + 3, "%31s %127s %x %d", cmd, a1, &off, &cnt);
        StrHdr *nm = NULL;
        void *inst = NULL;
        str_ctor(&nm, a1, -1, 0);
        mgr_find_obj(MGR, &inst, &nm);
        str_release(nm);
        char *o = inst ? *(char **)((char *)inst + 0xc) : NULL;
        if (inst)
            inst_release(&inst);
        if (!o || cnt < 1 || cnt > 64 || IsBadReadPtr(o + off, cnt * 4))
            return snprintf(out, size, "no object %s or bad range", a1), 1;
        int n = snprintf(out, size, "%s %p vt %p:", a1, o, *(void **)o);
        for (int i = 0; i < cnt && n < size - 40; i++)
            n += snprintf(out + n, size - n, " +%x=%08x(%.2f)", off + i * 4, *(unsigned *)(o + off + i * 4),
                          *(float *)(o + off + i * 4));
        return 1;
    }
    if (!strcmp(cmd, "named"))
        return snprintf(out, size, "asked %d, waiting %d, named %ld (log)", n_asked, n_told, n_named_logged), 1;
    char *ai = ai_by_name(a1);
    if (!ai)
        return snprintf(out, size, "no AI %s", a1), 1;
    char *node = *(char **)(ai + E_NODE);
    int before = node ? anim_id(node) : -9;
    if (!strcmp(cmd, "kill")) {
        kill_here(ai);
        snprintf(out, size, "death state: fl %x hp %.2f", *(unsigned *)(ai + AI_FLAGS), *(float *)(ai + 0x234));
    } else if (!strcmp(cmd, "wpn")) {   /* the soldier's weapon holder (+0x520) and its current weapon (+0x38) */
        char *h = *(char **)(ai + 0x520);
        int **w = h && !IsBadReadPtr(h, 0x40) ? *(int ***)(h + 0x38) : NULL;
        char *wo = w && !IsBadReadPtr(w, 8) ? (char *)*w : NULL;
        snprintf(out, size, "holder %p vt %p; +38 %p -> %p vt %p; holder dwords %08x %08x %08x %08x", h,
                 h ? *(void **)h : NULL, w, wo, wo && !IsBadReadPtr(wo, 4) ? *(void **)wo : NULL,
                 h ? *(unsigned *)(h + 0x30) : 0, h ? *(unsigned *)(h + 0x34) : 0, h ? *(unsigned *)(h + 0x3c) : 0,
                 h ? *(unsigned *)(h + 0x40) : 0);
    } else if (!strcmp(cmd, "strikes")) {   /* the strike manager (+0x530): +0xc count, entries at +0x14 (8 bytes) */
        char *sm = *(char **)(ai + 0x530);
        int n = sm ? *(int *)(sm + 0xc) : -1, k = snprintf(out, size, "strikes %p count %d:", sm, n);
        for (int i = 0; sm && i < n && i < 8 && k < size - 120; i++) {
            void **e = *(void ***)(sm + 0x14 + i * 8);
            void **vt = e && !IsBadReadPtr(e, 4) ? (void **)*e : NULL;
            k += snprintf(out + k, size - k, " [%d] %p vt %p (+10 %p +18 %p +50 %p)", i, e, vt, vt ? vt[4] : NULL,
                          vt ? vt[6] : NULL, vt ? vt[0x14] : NULL);
        }
    } else if (!strcmp(cmd, "fire")) {   /* show one shot of the soldier's weapon here (effects only) */
        char *w = NULL;
        int ok = show_shot(ai, 0, &w);
        snprintf(out, size, "shot effects %s (weapon %p vt %p)", ok ? "run" : "not run", w, w ? *(void **)w : NULL);
    } else if (!strcmp(cmd, "show")) {
        show_death(ai, push, 1);
        snprintf(out, size, "death shown: animation %d -> %d", before, node ? anim_id(node) : -9);
    } else
        snprintf(out, size, "!ai list <filter|*> | kill <ai> | show <ai> [push] | named | mem <obj> <off> [n] |"
                            " wpn <ai> | strikes <ai> | fire <ai>");
    return 1;
}

/* ---- lab: who calls the projectile dispatch (TS_FIREDEBUG=3) ----
 * The projectile dispatchers (0x7f7ee0, in the vtables 0xd943d0 +0x1c, 0xd95658 +0x270, 0xdbd038 +0x58) create the
 * shot's projectile and return at once in game mode 3: a client never creates one. Logs the code addresses found
 * on the stack for the first shots, to find the weapon's own shot function (effects). */
typedef void (TSCALL *dsp_t)(void *, int *);
static LONG n_fire_logged;
static void TSCALL hook_dsp(void *dsp, int *shot)
{
    if (InterlockedIncrement(&n_fire_logged) <= 12) {
        DWORD *sp = (DWORD *)__builtin_frame_address(0);
        char buf[400];
        int n = snprintf(buf, sizeof buf, "fire: dsp %p shot %p (+8 %p +c %p) stack:", dsp, shot, shot ? (void *)shot[2] : NULL,
                         shot ? (void *)shot[3] : NULL);
        for (int i = 0, k = 0; i < 160 && k < 14 && n < (int)sizeof buf - 12; i++)
            if (sp[i] >= 0x401000 && sp[i] < 0xc00000) {
                n += snprintf(buf + n, sizeof buf - n, " %lx", (unsigned long)sp[i]);
                k++;
            }
        char *sh = shot ? (char *)shot[3] : NULL;
        ts_log("aisync: %s%s%s", buf, sh && is_ai(sh) ? " (AI " : "", sh && is_ai(sh) ? ai_name(sh) : "");
    }
    ((dsp_t)0x7f7ee0)(dsp, shot);
}

static void firedebug_install(void)
{
    void *o = (void *)0x7f7ee0, *h = (void *)hook_dsp;
    int ok = ts_patch_bytes(0xd943d0 + 0x1c, &o, &h, 4) + ts_patch_bytes(0xd95658 + 0x270, &o, &h, 4)
             + ts_patch_bytes(0xdbd038 + 0x58, &o, &h, 4);
    ts_log("aisync: fire diagnostics %d/3", ok);
}

void aisync_install(void)
{
    {
        char fv[8];
        if (ts_env("TS_FIREDEBUG", fv, sizeof fv) && fv[0] == '3')   /* (lab: the dispatch stacks of the first shots) */
            firedebug_install();
    }
    char v[16];
    engine_mode = !(ts_env("TS_AISYNC", v, sizeof v) && v[0] == '0');
    if (ts_env("TS_AIDEBUG", v, sizeof v))
        debug_secs = atoi(v);
    if (debug_secs && ts_env("TS_SHOTDIR", follow_req, MAX_PATH - 16))
        strcat(follow_req, "\\follow.req");
    if (!engine_mode && !debug_secs)
        return;
    if (!hook_slots()) {
        ts_log("aisync: AI network functions not found (other exe)");
        return;
    }
    ts_frame_add(frame);
    if (ts_env("TS_FIREDEBUG", v, sizeof v) && v[0] >= '1' && v[0] <= '2')
        fire_debug = v[0] - '0';
    if (ts_env("TS_AIFIRE", v, sizeof v))   /* (0 none, 1 shells only, 2 the whole shot: default) */
        fire_full = v[0] == '2' ? 1 : v[0] == '1' ? 0 : -1;
    if (engine_mode && fire_full >= 0 && fire_install() != (int)(sizeof wpn_vtables / sizeof wpn_vtables[0]))
        ts_log("aisync: AI weapons not found (other exe): no AI shots on clients");
    else if (engine_mode && fire_full > 0 && !gate_install()) {
        ts_log("aisync: projectile dispatchers not found (other exe): AI shots on clients without projectiles");
        fire_full = 0;
    }
    if (engine_mode && !coopnet_listen(SUB_NAME_ASK, SUB_LAST, on_message))
        ts_log("aisync: no mod messages (names of new objects, the dead for late joiners)");
    ts_log("aisync: %s%s", engine_mode ? "AI replication on" : "AI diagnostics only",
           debug_secs ? ", diagnostics in the log" : "");
}
