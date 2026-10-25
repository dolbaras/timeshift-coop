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
 *   s16 body animation, s16 behaviour animation, u8 speed, 2 marker bytes. */
#define ANIM_EXTRA 7
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
    memcpy(b + 5, anim_mark, 2);
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
    if (!b || m[MSG_LEN] < ANIM_EXTRA || memcmp(b + m[MSG_LEN] - 2, anim_mark, 2) != 0) {
        m[MSG_ANIM] = (unsigned char)-1;   /* (no animation data: "none" rather than the buffer's leftovers) */
        m[MSG_ANIM + 1] = (unsigned char)-1;
        ((read_t)AI_READ)(ai, msg, flags);
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
    ts_log("aisync: %s%s", engine_mode ? "AI replication on" : "AI diagnostics only",
           debug_secs ? ", diagnostics in the log" : "");
}
