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
 * TS_AIDEBUG=<s>: diagnostics. Counts AI writes (sender), reads and interpolation calls (receiver) and logs each
 * AI's net id, role bits, position and animations every <s> seconds (aligned to the clock, so a host and a client
 * on one PC log the same moments). Test helper with it: <TS_SHOTDIR>\follow.req (below).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    b[4] = m[MSG_ANIM + 2];
    b[5] = (*(unsigned *)((char *)ai + AI_FLAGS) & AI_DEAD) ? 1 : 0;
    memcpy(b + 6, anim_mark, 2);
    m[MSG_LEN] += ANIM_EXTRA;
    AiState *s = state_of(ai);
    if (s) {
        s->anim = body;
        s->writes++;
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
    if (!b || m[MSG_LEN] < ANIM_EXTRA || memcmp(b + m[MSG_LEN] - 2, anim_mark, 2) != 0) {
        m[MSG_ANIM] = (unsigned char)-1;   /* (no animation data: "none" rather than the buffer's leftovers) */
        m[MSG_ANIM + 1] = (unsigned char)-1;
        ((read_t)AI_READ)(ai, msg, flags);
        return;
    }
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
    if (s && anim_id(node) != s->anim)
        *role |= 8;
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
    } else if (!strcmp(cmd, "fire")) {   /* "!ai fire <ai> [slot]": strike entry vtbl+0x10(target) aimed at my player */
        char *sm = *(char **)(ai + 0x530), *me = PLAYER, *pn = me ? *(char **)(me + E_NODE) : NULL;
        int slot = (int)push;
        void **e = sm && slot >= 0 && slot < *(int *)(sm + 0xc) ? *(void ***)(sm + 0x14 + slot * 8) : NULL;
        if (!e || !pn)
            return snprintf(out, size, "no strike %d or no player", slot), 1;
        float tgt[3] = {*(float *)(pn + 0x68), *(float *)(pn + 0x6c) + 1.5f, *(float *)(pn + 0x70)};
        unsigned *role = (unsigned *)(ai + E_ROLE), saved = *role;
        *role |= 4;
        ((void (TSCALL *)(void *, float *))((void **)*e)[4])(e, tgt);
        *role = (*role & ~4u) | (saved & 4);
        snprintf(out, size, "fired strike %d at %.1f %.1f %.1f", slot, tgt[0], tgt[1], tgt[2]);
    } else if (!strcmp(cmd, "show")) {
        show_death(ai, push, 1);
        snprintf(out, size, "death shown: animation %d -> %d", before, node ? anim_id(node) : -9);
    } else
        snprintf(out, size, "!ai list <filter|*> | kill <ai> | show <ai> [push] | named");
    return 1;
}

void aisync_install(void)
{
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
    if (engine_mode && !coopnet_listen(SUB_NAME_ASK, SUB_LAST, on_message))
        ts_log("aisync: no mod messages (names of new objects, the dead for late joiners)");
    ts_log("aisync: %s%s", engine_mode ? "AI replication on" : "AI diagnostics only",
           debug_secs ? ", diagnostics in the log" : "");
}
