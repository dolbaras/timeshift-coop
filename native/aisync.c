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
    unsigned short wk, wk_sent;     /* server: counters (2 bits per kind) of the weapon's state events (SHOOT_QUEUE ...) */
    unsigned short wk_seen;         /* client: the counters read last */
    unsigned char wk_any;           /* server: a state event happened (the counters ride in every update from then on) */
    unsigned char wk_pend;          /* client: kinds still to set on the weapon (bit per kind) */
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
/* Markers 0x5c / 0x5d: as 0x5a / 0x5b, with the weapon's state counters (u16) just before the 8 bytes. */
static const unsigned char anim_mark_k[2] = {0xa5, 0x5c}, anim_mark_shot_k[2] = {0xa5, 0x5d};
#define KINDS_EXTRA 2
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
    AiState *s = state_of(ai);
    int shot = s && s->have_shot && s->shots != s->shots_sent && m[MSG_LEN] + ANIM_EXTRA + SHOT_EXTRA <= 255;
    int kn = s && s->wk_any && m[MSG_LEN] + ANIM_EXTRA + (shot ? SHOT_EXTRA : 0) + KINDS_EXTRA <= 255;
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
    }
    if (kn) {   /* the weapon's state counters: what the weapon's own program turns into its sounds */
        memcpy(b, &s->wk, KINDS_EXTRA);
        b += KINDS_EXTRA;
    }
    memcpy(b, &body, 2);
    memcpy(b + 2, &bhv, 2);
    b[4] = m[MSG_ANIM + 2];
    /* state byte: bit 0 dead on the server, bit 1 attack of the last shot, bits 2-7 its weapon shots (mod 64) */
    b[5] = (unsigned char)(((*(unsigned *)((char *)ai + AI_FLAGS) & AI_DEAD) ? 1 : 0)
                           | (s ? ((s->atk & 1) << 1) | ((s->shots & 63) << 2) : 0));
    memcpy(b + 6, shot ? (kn ? anim_mark_shot_k : anim_mark_shot) : (kn ? anim_mark_k : anim_mark), 2);
    m[MSG_LEN] += ANIM_EXTRA + (shot ? SHOT_EXTRA : 0) + (kn ? KINDS_EXTRA : 0);
    if (s) {
        s->anim = body;
        s->writes++;
        s->shots_sent = s->shots;
        if (kn)
            s->wk_sent = s->wk;
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

/* Weapon state events (the weapon's own sound program reacts to them: SHOOT_SINGLE / MINIQUEUE / QUEUE start the shot
 * sounds, AI_SHOOT_STOP stops the burst, the recharge states play the reload sound). The server's weapon sets them in its
 * state set (virtual 0x82ce90 of the weapon interface, kinds 0 1 2 5 6 7); a client's weapon never does. Two bits per kind
 * count the events (like the shot count), the client sets the state on its own copy of the weapon when a counter moved. */
static const signed char wk_kind[6] = {0, 1, 2, 5, 6, 7};
#define WK_STOP 7

static void wk_read(AiState *s, unsigned short wk)
{
    for (int i = 0; i < 6; i++)
        if (((wk >> (2 * i)) - (s->wk_seen >> (2 * i))) & 3)
            s->wk_pend |= (unsigned char)(1 << wk_kind[i]);
    s->wk_seen = wk;
}

static void TSCALL hook_read(void *ai, char *msg, int flags)
{
    unsigned char *m = (unsigned char *)msg, *b = *(unsigned char **)(m + MSG_BITS);
    AiState *s = state_of(ai);
    if (s)
        s->reads++;
    if (s && (s->corpse || s->noanim))
        return;   /* (a corpse: the server's "no animation" would stand it up again) */
    if (b && m[MSG_LEN] >= ANIM_EXTRA + KINDS_EXTRA &&
        (memcmp(b + m[MSG_LEN] - 2, anim_mark_k, 2) == 0 || memcmp(b + m[MSG_LEN] - 2, anim_mark_shot_k, 2) == 0)) {
        unsigned char *kd = b + m[MSG_LEN] - ANIM_EXTRA - KINDS_EXTRA;
        unsigned short wk;
        memcpy(&wk, kd, KINDS_EXTRA);
        if (s)
            wk_read(s, wk);
        int with_shot = b[m[MSG_LEN] - 1] == anim_mark_shot_k[1];
        memmove(kd, kd + KINDS_EXTRA, ANIM_EXTRA);   /* (then as an update without the counters) */
        m[MSG_LEN] -= KINDS_EXTRA;
        memcpy(b + m[MSG_LEN] - 2, with_shot ? anim_mark_shot : anim_mark, 2);
    }
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
    if (s && (anim_id(node) != s->anim || s->shots != s->shots_sent || (s->wk_any && s->wk != s->wk_sent)))
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

static void pjl_reset(void);
static void pjl_track(void);

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
    pjl_reset();
    replaying = 1;
    ((wshot_t)WPN_SHOT)(w, rec);
    replaying = 0;
    pjl_track();
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

/* The replayed shot's projectile: a local object without authority does not run its own flight and end, so its
 * tracer stayed in the air for good and a fight left a sheaf of hundreds of lines (split-net run 20261025-192307,
 * client shot157). The projectile is caught where the dispatcher 0x7f7ee0 creates it (the call at 0x7f80c3 after
 * the create 0x655460, eax = the object) and ended (vtbl+0xc0) PJL_LIFE ms later. */
#define PJL_LIFE 300
#define PJL_MAX 64
static char *volatile pjl_new;
static struct { char *o; DWORD vt, t; } pjls[PJL_MAX];
static int n_pjls;
static LONG n_pjl_logged;

__attribute__((naked)) static void pjl_catch(void)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "cmp dword ptr [_replaying], 0\n"
        "je 1f\n"
        "mov dword ptr [_pjl_new], eax\n"
        "1:\n"
        "mov ebx, dword ptr [ebx + 8]\n"   /* (the two instructions the call replaced) */
        "test ebx, ebx\n"
        "ret\n"
        ".att_syntax prefix\n");
}

static int pjl_catch_install(void)
{
    static const unsigned char orig[5] = {0x8b, 0x5b, 0x08, 0x85, 0xdb};
    unsigned char call[5] = {0xe8};
    *(DWORD *)(call + 1) = (DWORD)pjl_catch - (0x7f80c3 + 5);
    return ts_patch_bytes(0x7f80c3, orig, call, 5);
}

static void pjl_end_fn(char *o, void *ctx)
{
    DWORD now = *(DWORD *)ctx;
    for (int i = 0; i < n_pjls; i++)
        if (pjls[i].o == o && *(DWORD *)o == pjls[i].vt && now - pjls[i].t >= PJL_LIFE) {
            ((void (TSCALL *)(void *, float))(*(void ***)o)[0xc0 / 4])(o, 0.0f);
            pjls[i].o = NULL;
        }
}

static int shot_frame_in;

/* (client, every frame) */
static void pjl_expire(void)
{
    if (shot_frame_in && --shot_frame_in == 0)
        ts_shot_next();
    if (!n_pjls)
        return;
    DWORD now = GetTickCount();
    int due = 0;
    for (int i = 0; i < n_pjls; i++)
        due |= now - pjls[i].t >= PJL_LIFE;
    if (!due)
        return;
    for_each_obj(pjl_end_fn, &now);   /* (only objects that still exist are ended) */
    int w = 0;
    for (int i = 0; i < n_pjls; i++)
        if (pjls[i].o && now - pjls[i].t < PJL_LIFE + 2000)
            pjls[w++] = pjls[i];
    n_pjls = w;
}

static void pjl_reset(void)
{
    pjl_new = NULL;
}

static void pjl_track(void)
{
    char *o = pjl_new;
    pjl_new = NULL;
    if (!o)
        return;
    if (n_pjls < PJL_MAX) {
        pjls[n_pjls].o = o;
        pjls[n_pjls].vt = *(DWORD *)o;
        pjls[n_pjls++].t = GetTickCount();
    } else
        ((void (TSCALL *)(void *, float))(*(void ***)o)[0xc0 / 4])(o, 0.0f);   /* (full: no tracer this time) */
    if (fire_debug && InterlockedIncrement(&n_pjl_logged) <= 5)
        ts_log("aisync: replayed projectile %p vt %06lx role %08x, ended after %d ms", o, *(unsigned long *)o,
               *(unsigned *)(o + E_ROLE), PJL_LIFE);
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
            shot_frame_in = 4;   /* (TS_FIREDEBUG=2: a frame 4 frames after the shot) */
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

/* ---- weapon state events: sounds of an AI's shots on clients ----
 * The set-state virtual of the weapon interface (0x82ce90 in the seven vtables below, thiscall (interface, kind), ret 4)
 * is hooked: on the server it counts the events of AI weapons per soldier (AiState.wk); on a client wstate_frame sets the
 * counted kinds on the soldier's own weapon copy (the original function, the hook ignores that call). The interface sits
 * at a fixed offset from the weapon object (learned per interface vtable from the AI weapons that exist). TS_AISTATE=0: off. */
#define WST_SET 0x82ce90
static const DWORD wst_slots[] = {0xd93094, 0xd93374, 0xd93654, 0xd93934, 0xd93c14, 0xd93f44, 0xdbf1bc};
typedef void (TSCALL *wst_t)(void *iface, int kind);
static int wst_on = 1;
static volatile int wst_applying;
static LONG n_wst_logged;

/* the weapon's interface: the dword in the object that points at (a little before) one of the patched slots, i.e. at the
 * start of the interface's vtable */
static char *wst_iface_of(char *w)
{
    if (IsBadReadPtr(w, 0x400))
        return NULL;
    for (int off = 4; off < 0x400; off += 4) {
        DWORD v = *(DWORD *)(w + off);
        for (unsigned i = 0; i < sizeof wst_slots / sizeof wst_slots[0]; i++) {
            DWORD d = wst_slots[i] - v;
            if (d <= 0x40 && !(d & 3))
                return w + off;
        }
    }
    return NULL;
}

typedef struct { char *iface, *found; } FindIface;
static void find_iface_fn(char *o, void *ctx)
{
    FindIface *f = ctx;
    if (!f->found && is_ai_weapon(o) && wst_iface_of(o) == f->iface)
        f->found = o;
}

/* the AI weapon object an interface pointer belongs to (NULL: not an AI weapon) */
static char *wst_weapon(void *iface)
{
    static struct { char *iface, *w; } cache[32];   /* (a fight has a handful of firing soldiers; the objects are searched on a miss) */
    static unsigned next;
    for (int i = 0; i < 32; i++)
        if (cache[i].iface == iface && cache[i].w && is_ai_weapon(cache[i].w) && wst_iface_of(cache[i].w) == iface)
            return cache[i].w;
    FindIface f = {iface, NULL};
    for_each_obj(find_iface_fn, &f);
    if (f.found) {
        cache[next % 32].iface = iface;
        cache[next++ % 32].w = f.found;
        static LONG logged;
        if (InterlockedIncrement(&logged) <= 4)
            ts_log("aisync: weapon state interface %p is weapon %p + %x (weapon vt %06lx)", iface, f.found,
                   (unsigned)((char *)iface - f.found), *(unsigned long *)f.found);
    }
    return f.found;
}

static void TSCALL hook_wstate(void *iface, int kind)
{
    ((wst_t)WST_SET)(iface, kind);
    char *gm = GAME_MODE_OBJ;
    if (!wst_on || wst_applying || !engine_mode || !gm || *(int *)(gm + 0x3c) < 2 || *(int *)(gm + 0x3c) == 3)
        return;
    int idx = -1;
    for (int i = 0; i < 6; i++)
        if (wk_kind[i] == kind)
            idx = i;
    if (idx < 0)
        return;
    char *w = wst_weapon(iface), *owner = w ? weapon_owner(w) : NULL;
    if (!owner || !is_ai(owner))
        return;
    AiState *s = state_of(owner);
    if (!s)
        return;
    unsigned c = ((s->wk >> (2 * idx)) + 1) & 3;
    s->wk = (unsigned short)((s->wk & ~(3 << (2 * idx))) | (c << (2 * idx)));
    s->wk_any = 1;
    if (fire_debug && InterlockedIncrement(&n_wst_logged) <= 40)
        ts_log("aisync: weapon state %d of %s (counters %04x)", kind, ai_name(owner), s->wk);
}

/* (client, every frame) */
static void wstate_frame(char *ai)
{
    AiState *s = state_of(ai);
    if (!s || !s->wk_pend)
        return;
    unsigned char pend = s->wk_pend;
    s->wk_pend = 0;
    if (!wst_on || s->corpse || s->noanim)
        return;
    char *w = ai_weapon(ai, &s->wpn, s->atk), *iface = w ? wst_iface_of(w) : NULL;
    if (!iface)
        return;
    wst_applying = 1;
    for (int k = 0; k < 8; k++)
        if (pend & (1 << k))
            ((wst_t)WST_SET)(iface, k);
    wst_applying = 0;
    if (fire_debug && InterlockedIncrement(&n_wst_logged) <= 40)
        ts_log("aisync: weapon states %02x set on the weapon of %s here", pend, ai_name(ai));
}

static int wstate_install(void)
{
    int ok = 0;
    for (unsigned i = 0; i < sizeof wst_slots / sizeof wst_slots[0]; i++) {
        void *o = (void *)WST_SET, *h = (void *)hook_wstate;
        ok += ts_patch_bytes(wst_slots[i], &o, &h, 4);
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

static void put_near_host(float side, float back, float up, int n);

static void follow_host(void)
{
    FILE *f = follow_req[0] ? fopen(follow_req, "r") : NULL;
    if (!f)
        return;
    float side = 1, back = 0, up = 0;
    int n = fscanf(f, "%f %f %f", &side, &back, &up);
    fclose(f);
    DeleteFileA(follow_req);
    put_near_host(side, back, up, n);
}

/* This machine's player next to the host's body (metres in the host's frame: side, back, up), facing the way the host faces. */
static void put_near_host(float side, float back, float up, int n)
{
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

/* ---- lab: a walking test body ("!ai walk <seconds> [turn [hexkey [still]]]", console only) ----
 * Holds the forward key (W, code 0x11) and a constant mouse-x turn in the engine's raw input for a while, so this machine's
 * own player walks a circle for the smoothness probes (a background test window never acquires keyboard or mouse, so nothing
 * overwrites the values). turn: mouse axis value per frame (+ right, - left); 0 = straight. */
static DWORD walk_until;
static float walk_turn;
static int walk_key2, walk_fwd = 1;   /* an extra key held meanwhile (hex scan code, 0 = none); forward on/off */
/* The values are written right after the engine's keyboard poll (input class vtable +0x60, which the poll of a background
 * window leaves untouched or zeroes): from the script tick the game read zeros, from a hook behind the poll it reads these. */
typedef void (TSCALL *poll_t)(void *);
static poll_t walk_prev_poll;

static void walk_apply(char *raw)
{
    if (!walk_until)
        return;
    if ((int)(GetTickCount() - walk_until) >= 0) {
        walk_until = 0;
        *(float *)(raw + 0x10 + 0x11 * 4) = 0.f;
        if (walk_key2)
            *(float *)(raw + 0x10 + walk_key2 * 4) = 0.f;
        *(float *)(raw + 0x10 + 0x12c * 4) = *(float *)(raw + 0x10 + 0x12d * 4) = 0.f;
        return;
    }
    /* (an action a key set off marks the key "used" (bit 8 of the byte at +0x1154 + code) until the device reports its release,
     * which a background window never does: the mark is cleared every frame, as the lab key tool does) */
    int keys[2] = {0x11, walk_key2};
    for (int i = 0; i < 2; i++)
        if (keys[i]) {
            raw[0x1154 + keys[i]] &= (char)~8;
            *(float *)(raw + 0x10 + keys[i] * 4) = (i == 0 && !walk_fwd) ? 0.f : 1.f;
        }
    *(float *)(raw + 0x10 + (walk_turn >= 0 ? 0x12c : 0x12d) * 4) = walk_turn >= 0 ? walk_turn : -walk_turn;
}

static void TSCALL hook_walk_poll(void *raw)
{
    walk_prev_poll(raw);
    walk_apply((char *)raw);
}

static void walk_frame(void)   /* (the first call after "!ai walk": puts the hook behind the keyboard poll) */
{
    static int hooked;
    if (hooked || !walk_until)
        return;
    void *vtp = (void *)0xdb7034;
    walk_prev_poll = (poll_t)ts_patch_vtable(&vtp, 0x60 / 4, (void *)hook_walk_poll);
    hooked = walk_prev_poll != NULL;
    ts_log("aisync: walk: keyboard poll hook %s", hooked ? "set" : "NOT set");
}

/* ---- lab: a recording of every body's drawn place per frame ("!ai rec <seconds>", console only) ----
 * For the smoothness work: every frame, one line per AI soldier and per player body, with the machine's QPC time (the same
 * clock for two copies on one PC), into rec.txt beside TS_LOG (else ts_rec.txt in the game folder):
 *   r <ms> a<net id> x y z <updates read so far>      an AI soldier (net id; the server's number is the same on both machines)
 *   r <ms> p<peer> x y z                              a player body of the session slots (peer 0 = the host); a <peer>* label
 *                                                     is this machine's own body
 * tool: the lab script a_rec.py compares host and client files (lag, error, irregularity of the drawn path). */
static FILE *rec_file;
static DWORD rec_until;
static LARGE_INTEGER rec_qpf;
static int rec_net_only;

static void rec_ai_fn(char *ai)
{
    char *node = *(char **)(ai + E_NODE);
    int net = *(int *)(ai + E_NETID);
    if (!node || net < 0)
        return;
    AiState *s = state_of(ai);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    float *p = (float *)(node + 0x68);
    fprintf(rec_file, "r %.2f a%d %.3f %.3f %.3f %d\n", c.QuadPart * 1000.0 / rec_qpf.QuadPart, net, p[0], p[1], p[2],
            s ? s->reads : -1);
}

static void rec_frame(void)
{
    if (!rec_until || !rec_file)
        return;
    if ((int)(GetTickCount() - rec_until) >= 0) {
        fclose(rec_file);
        rec_file = NULL;
        rec_until = 0;
        ts_log("aisync: recording ended");
        return;
    }
    for_each_ai(rec_ai_fn);
    char *gm = GAME_MODE_OBJ;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    double ms = c.QuadPart * 1000.0 / rec_qpf.QuadPart;
    for (int i = 0; gm && i < *(int *)(gm + 0x2c); i++) {
        char *slot = *(char **)(gm + 0x28) + i * 0xa4, *e = *(char **)(slot + 4);
        char *node = e && !IsBadReadPtr(e, E_NODE + 4) ? *(char **)(e + E_NODE) : NULL;
        if (!node || IsBadReadPtr(node, 0x74))
            continue;
        float *p = (float *)(node + 0x68);
        fprintf(rec_file, "r %.2f p%d%s %.3f %.3f %.3f\n", ms, *(int *)slot, e == PLAYER ? "*" : "", p[0], p[1], p[2]);
    }
    if (PLAYER && !IsBadReadPtr(PLAYER, E_NODE + 4) && *(char **)(PLAYER + E_NODE) && !IsBadReadPtr(*(char **)(PLAYER + E_NODE), 0x74)) {
        float *p = (float *)(*(char **)(PLAYER + E_NODE) + 0x68);
        fprintf(rec_file, "r %.2f me %.3f %.3f %.3f\n", ms, p[0], p[1], p[2]);
    }
}

static void rec_start(int secs)
{
    char path[MAX_PATH];
    if (rec_file) {
        fclose(rec_file);
        rec_file = NULL;
    }
    rec_until = 0;
    if (secs <= 0)
        return;
    if (ts_env("TS_LOG", path, sizeof path - 16)) {
        char *sl = strrchr(path, '\\');
        strcpy(sl ? sl + 1 : path, "rec.txt");
    } else
        strcpy(path, "ts_rec.txt");
    QueryPerformanceFrequency(&rec_qpf);
    rec_file = fopen(path, "a");
    if (rec_file)
        rec_until = GetTickCount() + (DWORD)secs * 1000;
    ts_log("aisync: recording %d s into %s%s", secs, path, rec_file ? "" : " (cannot open)");
}

/* ---- lab: a snapshot of the named actors ("!ai dump <tag>", console only) ----
 * For the static comparison of a host and a client (lab script a_dump.py): every named object, one line "dump <tag> <name> net=<id> vt=<class> role=<bits> pos x y z [anim id@time hp fl]" in the log.
 * The same moment on both machines (a few seconds after a level start, both idle) shows what the client does not show. */
static void dump_named(const char *tag, int *count)
{
    char *mgr = MGR;
    if (!mgr || IsBadReadPtr(mgr + 0x14, 8))
        return;
    void **data = *(void ***)(mgr + 0x14);
    int n = *(int *)(mgr + 0x18);
    for (int i = 0; data && i < n && i < 20000; i++) {
        char *inst = data[i], *o = inst && !IsBadReadPtr(inst, 0x14) ? *(char **)(inst + 0xc) : NULL;
        if (!o || IsBadReadPtr(o, 0x240) || ((DWORD)o & 3) || !inst_of(o))
            continue;
        int net = *(int *)(o + E_NETID), ai = is_ai(o);
        unsigned role = *(unsigned *)(o + E_ROLE);
        float *p = (float *)(o + E_POS);
        if (ai) {
            char *node = *(char **)(o + E_NODE);
            float *np = node && !IsBadReadPtr(node, 0x74) ? (float *)(node + 0x68) : p;
            ts_log("dump %s %s net=%d vt=%06lx role=%x pos %.2f %.2f %.2f anim %d@%.2f hp %.2f fl %x", tag, inst_str(inst), net,
                   *(unsigned long *)o, role, np[0], np[1], np[2], node ? anim_id(node) : -9, node ? anim_time(node) : -9.f,
                   *(float *)(o + 0x234), *(unsigned *)(o + AI_FLAGS));
        } else
            ts_log("dump %s %s net=%d vt=%06lx role=%x pos %.2f %.2f %.2f", tag, inst_str(inst), net, *(unsigned long *)o,
                   role, p[0], p[1], p[2]);
        (*count)++;
    }
}

/* ---- lab: sound start counters ("!ai snd on|off|dump|reset", or TS_SNDCOUNT=1 from the start) ----
 * Every sound the engine starts goes through the FMOD wrapper's GetBuffer (class snd::SYSTEM_FMOD, vtable 0xdb77bc +0x18:
 * thiscall (system, name string, flag), ret 8; the name is a counted string, refs at +0, text at +0xc; it returns the sound's
 * buffer or NULL when the sound could not start). Counts per sound name (started / refused); "dump" logs them sorted, so the
 * sounds of a fight (shots, explosions, steps) can be compared between a host and a client without hearing them. */
#define SND_VT_GETBUFFER (0xdb77bc + 0x18)
#define SND_GETBUFFER 0x78a1c0
#define SND_MAX 512
typedef void *(TSCALL *getbuf_t)(void *sys, char *name, int flag);
static struct { char name[48]; int started, refused; } snd_tab[SND_MAX];
static int n_snd, snd_on, snd_installed, snd_total;

static char snd_trace[48];   /* "!ai snd trace <part of a name>": log the callers of the first sounds whose name contains it */
static int snd_trace_left;

/* (the trace string is a list of name parts separated by '|') */
static int snd_trace_match(const char *t)
{
    char part[48];
    const char *p = snd_trace;
    while (*p) {
        int n = 0;
        while (*p && *p != '|' && n < (int)sizeof part - 1)
            part[n++] = *p++;
        part[n] = 0;
        if (*p == '|')
            p++;
        if (n && strstr(t, part))
            return 1;
    }
    return 0;
}

static void snd_trace_callers(const char *t, void *r, int flag)
{
    char line[400];
    int n = snprintf(line, sizeof line, "snd-trace: %s -> %s flag %d callers:", t, r ? "started" : "refused", flag);
    DWORD *q = (DWORD *)__builtin_frame_address(0);
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    int found = 0;
    for (; q < (DWORD *)tib->StackBase && found < 10 && q < (DWORD *)__builtin_frame_address(0) + 400; q++) {
        DWORD a = *q;
        if (a < 0x401000 || a > 0xc00000)
            continue;
        const unsigned char *p = (const unsigned char *)a;
        if (IsBadReadPtr(p - 6, 6) || !(p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)))
            continue;
        n += snprintf(line + n, sizeof line - n, " %06lx", a);
        found++;
    }
    ts_log("%s", line);
}

static void *TSCALL hook_getbuffer(void *sys, char *name, int flag)
{
    void *r = ((getbuf_t)SND_GETBUFFER)(sys, name, flag);
    if (!snd_on || !name || IsBadReadPtr(name, 0x14))
        return r;
    const char *t = name + 0xc;
    if (snd_trace[0] && snd_trace_left > 0 && !IsBadReadPtr(t, 4) && snd_trace_match(t)) {
        snd_trace_left--;
        snd_trace_callers(t, r, flag);
    }
    int i;
    for (i = 0; i < n_snd; i++)
        if (!strncmp(snd_tab[i].name, t, sizeof snd_tab[i].name - 1))
            break;
    if (i == n_snd) {
        if (n_snd >= SND_MAX)
            return r;
        n_snd++;
        snprintf(snd_tab[i].name, sizeof snd_tab[i].name, "%.47s", IsBadReadPtr(t, 4) ? "?" : t);
    }
    if (r)
        snd_tab[i].started++;
    else
        snd_tab[i].refused++;
    snd_total++;
    return r;
}

static int snd_control(const char *what, char *out, int size)
{
    if (!strcmp(what, "on")) {
        if (!snd_installed) {
            void *o = (void *)SND_GETBUFFER, *h = (void *)hook_getbuffer;
            snd_installed = ts_patch_bytes(SND_VT_GETBUFFER, &o, &h, 4);
        }
        snd_on = snd_installed;
        return snprintf(out, size, "sound counters %s", snd_on ? "on" : "not installed (other exe)");
    }
    if (!strcmp(what, "off")) {
        snd_on = 0;
        return snprintf(out, size, "sound counters off");
    }
    if (!strcmp(what, "reset")) {
        n_snd = snd_total = 0;
        return snprintf(out, size, "sound counters reset");
    }
    /* dump: sorted by started, to the log (one line per sound) and a summary as the reply */
    int order[SND_MAX];
    for (int i = 0; i < n_snd; i++)
        order[i] = i;
    for (int i = 1; i < n_snd; i++)
        for (int j = i; j > 0 && snd_tab[order[j]].started > snd_tab[order[j - 1]].started; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    int started = 0, refused = 0;
    for (int k = 0; k < n_snd; k++) {
        started += snd_tab[order[k]].started;
        refused += snd_tab[order[k]].refused;
        ts_log("snd: %-40s started %d refused %d", snd_tab[order[k]].name, snd_tab[order[k]].started, snd_tab[order[k]].refused);
    }
    ts_log("snd: total %d sounds (%d names), started %d, refused %d", snd_total, n_snd, started, refused);
    return snprintf(out, size, "sounds: %d names, started %d, refused %d (log)", n_snd, started, refused);
}

/* ---- lab: frame rate cap (TS_FPSCAP=<n>) ----
 * Two test copies on one PC share the GPU: a client running at 120 fps made the host's render thread crawl at 8 fps (measured
 * 2026-10-30, N100), which spoils every smoothness figure. The cap holds a copy to <n> frames a second (sleep at the end of the
 * script tick; the last millisecond is waited out). Test use only; unset = no cap. */
static int fps_cap;
static void fps_cap_frame(void)
{
    static LARGE_INTEGER next, qpf;
    if (!fps_cap)
        return;
    if (!qpf.QuadPart)
        QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG step = qpf.QuadPart / fps_cap;
    if (next.QuadPart == 0 || now.QuadPart - next.QuadPart > step * 4)
        next.QuadPart = now.QuadPart;   /* (late by a lot: start again, no catching up) */
    next.QuadPart += step;
    for (;;) {
        QueryPerformanceCounter(&now);
        LONGLONG left = next.QuadPart - now.QuadPart;
        if (left <= 0)
            break;
        if (left * 1000 / qpf.QuadPart >= 2)
            Sleep(1);
        else
            Sleep(0);
    }
}

/* ---- per frame ---- */
static void frame(float dt)
{
    (void)dt;
    fps_cap_frame();
    walk_frame();
    rec_frame();
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
        for_each_ai(wstate_frame);
        pjl_expire();
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
    if (!strcmp(cmd, "put")) {   /* "!ai put <side> <back> <up>" (client, lab): this player next to the host's, as follow.req does */
        float sd = 0, bk = 0, upv = 0;
        int k = sscanf(text + 3, "%31s %f %f %f", cmd, &sd, &bk, &upv);
        put_near_host(sd, bk, upv, k - 1);
        return snprintf(out, size, "placed next to the host (side %.1f back %.1f up %.1f)", sd, bk, upv), 1;
    }
    if (!strcmp(cmd, "tick")) {   /* "!ai tick": the granularity of timeGetTime, which the net readers' age uses (lab) */
        DWORD (WINAPI *tgt)(void) = (DWORD (WINAPI *)(void))GetProcAddress(LoadLibraryA("winmm.dll"), "timeGetTime");
        if (!tgt)
            return snprintf(out, size, "no timeGetTime"), 1;
        DWORD t0 = tgt(), last = t0, now, steps = 0, big = 0, small = 0xffff;
        while ((now = tgt()) - t0 < 300) {
            if (now != last) {
                DWORD d = now - last;
                steps++;
                if (d > big)
                    big = d;
                if (d < small)
                    small = d;
                last = now;
            }
        }
        return snprintf(out, size, "timeGetTime: %lu changes in 300 ms (step %.2f ms avg), smallest step %lu, largest %lu",
                        steps, steps ? 300.0 / steps : 0.0, small, big), 1;
    }
    if (!strcmp(cmd, "pstate")) {   /* "!ai pstate": the stance words of every player body this machine knows (lab) */
        char *gm = GAME_MODE_OBJ;
        int n = snprintf(out, size, "players:");
        for (int i = -1; gm && i < *(int *)(gm + 0x2c) && n < size - 120; i++) {
            char *slot = i >= 0 ? *(char **)(gm + 0x28) + i * 0xa4 : NULL;
            char *e = i < 0 ? PLAYER : *(char **)(slot + 4);
            if (!e || IsBadReadPtr(e, 0x3a0) || (i >= 0 && e == PLAYER))
                continue;
            char *node = *(char **)(e + E_NODE);
            int ok = node && !IsBadReadPtr(node, 0x74);
            n += snprintf(out + n, size - n, " %s%d%s st %08x %08x %08x mv %d y %.3f an %d@%.2f hp %.0f;", i < 0 ? "self" : "slot",
                          i < 0 ? 0 : *(int *)slot, e == PLAYER ? "*" : "", *(unsigned *)(e + 0x394), *(unsigned *)(e + 0x398),
                          *(unsigned *)(e + 0x39c), *(unsigned char *)(e + 0x398) & 0xf, ok ? ((float *)(node + 0x68))[1] : -99.f,
                          ok ? anim_id(node) : -9, ok ? anim_time(node) : -1.f, (double)*(float *)(e + 0x234));
        }
        return n;
    }
    if (!strcmp(cmd, "snd")) {   /* "!ai snd on|off|dump|reset|trace <name part> [count]" (lab): the sound start counters above */
        if (!strcmp(a1, "trace")) {
            char part[48] = "", cnt[16] = "";
            sscanf(text + 3, "%31s %31s %47s %15s", cmd, a1, part, cnt);
            snprintf(snd_trace, sizeof snd_trace, "%s", part);
            snd_trace_left = cnt[0] ? atoi(cnt) : 6;
            snd_control("on", out, size);
            return snprintf(out, size, "sound trace of \"%s\" (%d times) into the log", snd_trace, snd_trace_left), 1;
        }
        return snd_control(a1, out, size), 1;
    }
    if (!strcmp(cmd, "dump")) {   /* "!ai dump <tag>" (lab): the named-actor snapshot above, into the log */
        int n = 0;
        dump_named(a1[0] ? a1 : "x", &n);
        return snprintf(out, size, "dumped %d objects (tag %s)", n, a1[0] ? a1 : "x"), 1;
    }
    if (!strcmp(cmd, "rec")) {   /* "!ai rec <seconds>" (lab): the per-frame recording above; 0 = stop */
        rec_start(atoi(a1));
        return snprintf(out, size, "recording %s", rec_file ? "on" : "off"), 1;
    }
    if (!strcmp(cmd, "walk")) {   /* "!ai walk <seconds> [turn]" (lab): own player walks, see walk_frame */
        float secs = (float)atof(a1);
        walk_turn = 0.f;
        char k2[16] = "", st[16] = "";
        sscanf(text + 3, "%31s %127s %f %15s %15s", cmd, a1, &walk_turn, k2, st);
        walk_key2 = k2[0] ? (int)strtol(k2, NULL, 16) & 0xff : 0;   /* "!ai walk 20 0 2e": crouch while walking */
        walk_fwd = strcmp(st, "still") != 0;                        /* "!ai walk 20 0 2e still": hold the key only */
        walk_until = secs > 0 ? GetTickCount() + (DWORD)(secs * 1000) : 0;
        return snprintf(out, size, "walking %.0f s, turn %.2f", secs, walk_turn), 1;
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
    if (ts_env("TS_FPSCAP", v, sizeof v) && atoi(v) >= 5)
        fps_cap = atoi(v);   /* (lab) */
    if (ts_env("TS_SNDCOUNT", v, sizeof v) && v[0] == '1') {   /* (lab: sound start counters from the start) */
        char o[64];
        snd_control("on", o, sizeof o);
    }
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
    else if (engine_mode && fire_full > 0 && (!gate_install() || !pjl_catch_install())) {
        ts_log("aisync: projectile dispatchers not found (other exe): AI shots on clients without projectiles");
        fire_full = 0;
    }
    if (ts_env("TS_AISTATE", v, sizeof v) && v[0] == '0')
        wst_on = 0;
    if (engine_mode && wst_on && wstate_install() != (int)(sizeof wst_slots / sizeof wst_slots[0])) {
        ts_log("aisync: weapon state virtuals not found (other exe): no AI shot sounds on clients");
        wst_on = 0;
    }
    if (engine_mode && !coopnet_listen(SUB_NAME_ASK, SUB_LAST, on_message))
        ts_log("aisync: no mod messages (names of new objects, the dead for late joiners)");
    ts_log("aisync: %s%s", engine_mode ? "AI replication on" : "AI diagnostics only",
           debug_secs ? ", diagnostics in the log" : "");
}
