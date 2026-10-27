/*
 * Local split screen: two players on one PC, player 2 on the second pad (prototype, TS_SPLIT=1).
 *
 * The PC exe has one local player, one camera, one command context and one HUD, each a single global
 * (work/re/splitscreen.md, docs/splitscreen.md). The split is made by swapping those globals:
 *
 * - Alternate frames: each frame renders one view with the engine's normal pipeline (visibility, shadows,
 *   post-processing and HUD follow by themselves). View 0 = player 1, view 1 = player 2. From the
 *   renderer's frame start (R vtable +0x1c, which pushes the camera into the view list) to its frame end
 *   (+0x24) the camera [0xfcabc4] is a copy of the view's camera with the field of view of a half screen,
 *   and the two local-player pointers [0x1095b30]/[0xfc8180] point to that view's player (HUD, first-person
 *   weapon). With the engine's single-thread frame (forced in split mode) nothing else runs in that span:
 *   the game update comes before it, the render job and the HUD update inside it.
 * - Composite: every frame is drawn into a pool render target and copied to the back buffer by one
 *   full-screen quad (call at 0x770f42 in 0x770e00). That call is replaced: the pool is kept as the view's
 *   picture, and both views' latest pictures go into their halves (side by side on wide screens, top and
 *   bottom otherwise), so each half shows its newest frame and nothing depends on the back buffer.
 * - Player 2 is a second instance of the campaign class `player` (entity spawn 0x655460). Its update
 *   (vtable +0x13c) runs with the globals swapped to it: the two player pointers, its own camera object
 *   (the player's camera code writes [0xfcabc4]) and its own command context, which the engine's own
 *   command evaluation fills from pad 2 (a second pass of 0x6595e0 per frame with the raw input codes of
 *   player 2: gamepad.c writes pad 2 there, the keyboard and mouse read as idle). Every message and update
 *   of player 2 and of its own weapons runs inside that swap (scheduler dispatch 0x946e40). It gets its own
 *   first-person weapons (template in-use mark lifted at its spawn), the script name `player2`, no damage
 *   from player 1 and none to it, and a respawn next to player 1 instead of the single-player death.
 * - Sound: the listener update runs in the game update, outside the render span, so it follows player 1.
 *
 *   TS_SPLIT=1           on (prototype; off unless set)
 *   TS_SPLIT_STAGE=n     1: the normal frame in one half only; 2: + a second camera (an offset copy of
 *                        player 1's) on alternate frames; 3 (default): + player 2
 *   TS_SPLIT_LAYOUT=     auto (default: side by side when the screen is wider than 3:2), v (side by side), h
 *   TS_SPLIT_MT=1        keep the engine's worker threads (default: single-thread frame in split mode)
 *   TS_SPLIT_P2DELAY=s   seconds after player 1 appears until player 2 is spawned (default 5)
 *   TS_SPLITLOG=1        frame phases, threads and globals of the first frames in the log (2: + weapon sets)
 *   TS_SPLIT_MSGLOG=1    tests: messages to the players and player 2's events in the log
 *   TS_SPLIT_P2FACE=1    tests: player 2 is placed 2.5 m in front of player 1, facing it
 */
#define COBJMACROS
#include <windows.h>
#include <xinput.h>
#include <d3d9.h>
#include <math.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))

/* globals (patch 1.2) */
#define CAM        (*(BYTE **)0xfcabc4)    /* the camera every system reads */
#define PLAYER_A   (*(BYTE **)0x1095b30)   /* the local player (AI, scripts) */
#define PLAYER_B   (*(BYTE **)0xfc8180)    /* the local player (HUD, triggers) */
#define DRV        (*(BYTE **)0x10363ec)   /* d3dDRIVER: +0x88c device, +0x10c back buffer description */
#define POOL(i)    (((BYTE **)0x10363e0)[i])   /* the two "BB_MNG_POOL" targets: +0x40 IDirect3DTexture9 */
#define POOL_CUR   (*(int *)0xebecb8)
#define CINE       (*(BYTE **)0x109606c)   /* cinematic camera manager: +8 count, +0x10 active entry */
#define CORES      (*(int *)0xebc570)      /* the frame's thread count (1 = single-thread path) */
#define MT_ONCE    (*(DWORD *)0x10de01c)   /* bit 0: [CONFIG] DisableMultithreading read */
#define MT_OFF     (*(DWORD *)0x10de018)   /* its value */
#define IM         (*(BYTE **)0xfcabb4)    /* input manager: +0x3c raw input, +0x40 active context, +0x44 contexts[8] */
#define CANVAS_W   (*(float *)0xebfaac)    /* the UI's virtual canvas (height 480, width by the screen's shape) */
#define CANVAS_H   (*(float *)0xebfab0)

#define R_VTBL      0x00d8d810
#define SLOT_FSTART (0x1c / 4)             /* 0x504b30 -> 0x67ee90 */
#define SLOT_FEND   (0x24 / 4)             /* 0x67f290 */
#define PLAYER_VTBL 0x00d72f78
#define SLOT_DTOR   0                      /* 0x406ab0 (this, flags), ret 4 */
#define SLOT_UPDATE (0x13c / 4)            /* 0x4098c0 (this) */
#define SLOT_PLACE  (0x210 / 4)            /* 0x409490 (this, float pos[3], float dir[3], int), ret 0xc */
#define IM_VTBL     0x00d8be18
#define SLOT_EVAL   (0x34 / 4)             /* 0x4f3190: 0x6595e0 + double-tap check */
#define VT_GAME     0x00d8bd28             /* the in-game command context */
#define QUAD_CALL   0x00770f42             /* call 0x987db0: the pool target -> back buffer quad */
#define QUAD_FN     0x00987db0
#define CAM_SIZE    0x360

/* Raw input (_inpINPUT) block swapped for player 2's pass: current codes +0x10, a per-code byte array
 * +0x560, previous codes +0x6b4, calibration +0xc04, per-code flags +0x1154 (0x154 codes each). */
#define RAW_FROM 0x10
#define RAW_TO   (0x1154 + 0x154)
/* Input manager fields the evaluation writes besides the context: look accumulators and their state. */
#define IMF_A 0x28   /* +0x28..+0x3b */
#define IMF_B 0x64   /* +0x64..+0x73 */

typedef void (TSCALL *void_t)(void *);
typedef void *(TSCALL *dtor_t)(void *, int);
typedef void (TSCALL *place_t)(void *, float *, float *, int);
typedef void (TSCALL *quad_t)(void *, float, float, float, float, float, float, float, float, float, void *);

static void_t real_fstart, real_fend, real_update, real_eval;
static dtor_t real_dtor;
static quad_t real_quad = (quad_t)QUAD_FN;
static void (TSCALL *eval_commands)(void *) = (void *)0x6595e0;   /* (fastcall ecx = IM) */
static void (TSCALL *node_name)(void *, const char *) = (void *)0x94c730;

static int stage, layout_cfg, splitlog, keep_mt, p2_delay = 5;
static int forced;   /* TS_SPLIT: 1 on, -1 never, 0 the player's choice (CoopSplit) */
static DWORD mt_saved;
static int mt_set;

/* The engine's single-thread frame (as [CONFIG] DisableMultithreading = 1): the frame takes the core count from
 * [0x10de018] every frame once [0x10de01c] bit 0 says the setting was read; split screen needs 1. */
static void set_single_thread(int on)
{
    if (keep_mt || on == mt_set)
        return;
    if (on) {
        mt_saved = (MT_ONCE & 1) ? MT_OFF : 0;
        MT_ONCE |= 1;
        MT_OFF = 1;
    } else
        MT_OFF = mt_saved;
    mt_set = on;
}

/* ---- engine calls with register arguments ---- */
/* 0x624690: rebuild the projection and frustum from near/far/fov (eax = camera). */
__attribute__((naked)) static void cam_rebuild(void *cam)
{
    __asm__("mov 4(%esp), %eax\n\t"
            "mov $0x624690, %ecx\n\t"
            "call *%ecx\n\t"
            "ret");
}

/* 0x624b40: set the position, re-normalise the axes and rebuild the view matrix (ecx = camera, eax = pos). */
__attribute__((naked)) static void cam_set_pos(void *cam, const float *pos)
{
    __asm__("mov 4(%esp), %ecx\n\t"
            "mov 8(%esp), %eax\n\t"
            "mov $0x624b40, %edx\n\t"
            "call *%edx\n\t"
            "ret");
}

/* 0x655460: spawn an entity (ecx = transform or 0, edx = object name, stack: class name, id), ret 8. */
__attribute__((naked)) static void *spawn_entity(void *xform, const char *name, const char *cls, int id)
{
    __asm__("mov 4(%esp), %ecx\n\t"
            "mov 8(%esp), %edx\n\t"
            "push 16(%esp)\n\t"
            "push 16(%esp)\n\t"
            "mov $0x655460, %eax\n\t"
            "call *%eax\n\t"
            "ret");
}

/* The player's own spawns: weapons (0x4478e0 calls 0x655460 at 0x447a2e/0x447a55), hands (0x442868) and body
 * (0x4074b4/0x4074f9). A second `player` gets no object for the first-person weapons and hands: their classes
 * are made from a placed template (nameTpl = wpn_plr_pistol, ...) that apparently only one object can use (likely
 * the check at 0x95dd3a, not traced). The
 * engine then writes through the NULL (0x447a5c) or skips the rest of the init (+0x478 stays NULL, 0x40b958).
 * For player 2 the network players' class of the same weapon is used instead (wpn_mp_*: parent wpn_plr_*, the
 * AI's third-person model as template), and an empty holster for the hands. */
static int spawning_p2;
#define MAX_OWN 64
static BYTE *own2[MAX_OWN], *own1[MAX_OWN];
static char own_cls[2][MAX_OWN][24];   /* (their classes, for the logs) */
/* Every object made for player 2 at the player's own spawn sites (weapons first-person and third-person, holster,
 * hands, body), with its vtable: the weapons are not removed with the player and went on updating with their
 * owner gone (0x5460d0 -> crash 0x81f09e, run 20261025-223352); they are removed with player 2 (p2_remove). */
static struct { BYTE *o; DWORD vt; } p2_objs[2 * MAX_OWN], p2_gone[2 * MAX_OWN];
static int n_p2_objs, n_p2_gone;   /* (p2_gone: of a removed player 2, until the level ends) */
static int n_own2, n_own1;

__attribute__((used)) void *__cdecl split_weapon_spawn(void *xform, const char *name, const char *cls, int id)
{
    void *r = spawn_entity(xform, name, cls, id);
    const char *used = cls;
    char alt[64];
    if (!r && spawning_p2 && cls && strncmp(cls, "wpn_plr_", 8) == 0 && strlen(cls) < sizeof alt - 1) {
        /* (the hands have no network class: an empty weapon object stands in, enough for the init) */
        snprintf(alt, sizeof alt, strcmp(cls, "wpn_plr_hands") == 0 ? "wpn_plr_holster" : "wpn_mp_%s", cls + 8);
        r = spawn_entity(xform, name, alt, id);
        used = alt;
    }
    if (!r && spawning_p2 && cls && strcmp(cls, "player_ghost") == 0 && name) {
        r = spawn_entity(xform, NULL, cls, id);   /* (the body without its placed template) */
        ts_log("split: player 2 body from template \"%s\" failed, without a template: %p", name, r);
    }
    if (r && spawning_p2 && n_p2_objs < 2 * MAX_OWN)
        p2_objs[n_p2_objs].o = r, p2_objs[n_p2_objs++].vt = *(DWORD *)r;
    if (r && spawning_p2 && used && strncmp(used, "wpn_plr_", 8) == 0 && n_own2 < MAX_OWN)
        snprintf(own_cls[1][n_own2], 24, "%s", used), own2[n_own2++] = r;
    else if (r && !spawning_p2 && used && strncmp(used, "wpn_plr_", 8) == 0 && n_own1 < MAX_OWN)
        snprintf(own_cls[0][n_own1], 24, "%s", used), own1[n_own1++] = r;   /* (player 1's: made once, at the start of the game) */
    if (splitlog || spawning_p2)
        ts_log("split: spawn%s class \"%s\" name \"%s\" -> %p", spawning_p2 ? " (player 2)" : "", used ? used : "(null)",
               name ? name : "(null)", r);
    return r;
}

/* The world's node creation for a spawn (thiscall 0x95dcf0(world, template, transform), ret 8; called at
 * 0x6551be). For first-person weapon templates (0x95c480: names "wpn_plr*" except proxy/holster) it makes a
 * copy of the template's shared geometry (+0xb8, 0x961f30), but refuses when that geometry is marked in use
 * (+0xbc, checked at 0x95dd3a). While player 2 is made, the mark is lifted for the call. */
typedef void *(TSCALL *mknode_t)(void *, void *, void *);
/* Player 2's copies of first-person geometry: the geometry's +0xbc names the node that owns it (player 1's
 * weapon node, the "in use" value); for view 1 player 2's node is made the owner (frame start to frame end). */
#define MAX_GEO 32
static struct { BYTE *geo, *node2; DWORD saved; } geo2[MAX_GEO];
static int n_geo2, geo_swapped;
static DWORD geo_upd_saved[MAX_GEO];
static int geo_upd_on;

/* While player 2's own objects update (inside the swap), its nodes own the shared first-person geometry too:
 * the weapons pose it then (tests: TS_SPLIT_NOGEO=1 turns all owner swaps off). */
static int no_geo_owner;
static void geo_owner_p2(int on)
{
    if (no_geo_owner || geo_swapped || on == geo_upd_on)
        return;
    for (int i = 0; i < n_geo2; i++) {
        if (on) {
            geo_upd_saved[i] = *(DWORD *)(geo2[i].geo + 0xbc);
            *(DWORD *)(geo2[i].geo + 0xbc) = (DWORD)geo2[i].node2;
        } else
            *(DWORD *)(geo2[i].geo + 0xbc) = geo_upd_saved[i];
    }
    geo_upd_on = on;
}
/* The other player's first-person objects are hidden for a view (node +4 bit 0: the render prep skips the node,
 * 0x9a1331): both players' weapon nodes share one geometry, and the last one animated in a frame would pose it. */
static struct { BYTE *node; DWORD saved; } fp_hidden[2 * MAX_OWN];
static int n_fp_hidden;
static int fp_share;
/* Player 2 in a network game (server). The game already shows a `player` to clients as `player_mp`: the player's
 * create descriptor (vtable +0x38 = 0x40b790) names its class through vtable +0x334, which in a network game gives
 * the multiplayer class; the join list (0x6525e0) and CREATE_ENT (0x652c00) take every isSyncMP entity with a
 * net id, and `player` is isSyncMP. So player 2 goes to clients as a `player_mp` puppet by the game's own way;
 * the mod adds the server's regular-update bit (role 0x10, which player 1 has and player 2 lacked) so its moves
 * are sent. TS_SPLIT_NETLOCAL=1 keeps player 2 out of the network game instead (isSyncMP cleared). Both need the
 * client test. */
static int net_local = 0;

/* Player 2 and its objects out of the network game (server): isSyncMP (+0x98 bit 0) cleared, so they are not in a
 * joining client's entity list (0x6525e0) and not sent as CREATE_ENT (0x652c00), and role bit 0x4000 (no create
 * message) set. Returns the number of objects changed. */
static int net_hide(BYTE *o, const char *what)
{
    if (!o || IsBadReadPtr(o, 0x230))
        return 0;
    unsigned before = *(unsigned *)(o + 0x98);
    *(unsigned *)(o + 0x98) &= ~1u;
    *(unsigned *)(o + 0x2c) |= 0x4000u;
    if (splitlog || (before & 1))
        ts_log("split: %s %p out of the network game (net id %d, sync flags %08x -> %08x)", what, (void *)o, *(int *)(o + 0x18),
               before, *(unsigned *)(o + 0x98));
    return (before & 1) != 0;
}   /* TS_SPLIT_FPSHARE=1: player 2's first-person nodes share player 1's geometry (owner swaps) */

/* The crosshairs are scene nodes too (templates "crs_*", made with each player's weapons, kept in front of the
 * player's camera): in the other player's view a player's crosshair floats in front of that player's body
 * (player 2's showed at player 2's head in player 1's half). Each view hides the other player's crosshair nodes
 * like its first-person nodes. Recorded at their creation, with their geometry to know they still exist. */
#define MAX_CRS 32
static struct { BYTE *node, *geo; } crs[2][MAX_CRS];
static int n_crs[2];
/* Some crosshairs are objects of their own (the sniper's: vtable 0xd9b9f8, update 0x5703a0; others with update
 * 0x56f040), made with the player's weapons and not removed with the player: after player 2's removal they went
 * on updating with player 2's weapons gone and stopped the game in __purecall (runs 20261025-215305 on a server,
 * -220418 and -221623 in single player). For a few seconds after player 2's weapons are made, the updated objects
 * whose node is one of player 2's crosshair nodes are recorded; they are removed with player 2, and one left over
 * is not updated any more. */
static struct { BYTE *o; DWORD vt; } crs_obj[MAX_CRS];
static int n_crs_obj;
static BYTE *crs_gone[MAX_CRS];
static int n_crs_gone;
static DWORD crs_scan;   /* (recording until this tick) */

static void record_crosshair(BYTE *tpl, BYTE *node)
{
    if (!tpl || !node || strncmp((char *)(tpl + 0x30), "crs_", 4) != 0)
        return;
    int who = spawning_p2 ? 1 : 0;
    if (n_crs[who] < MAX_CRS)
        crs[who][n_crs[who]].node = node, crs[who][n_crs[who]++].geo = *(BYTE **)(node + 0x1c0);
}

static int crosshair_alive(int who, int i)
{
    BYTE *n = crs[who][i].node;
    return n && !IsBadReadPtr(n, 0x1c4) && *(BYTE **)(n + 0x1c0) == crs[who][i].geo;
}

/* Tests: the scene nodes of the level's pick-ups (templates "item_*"), recorded at their creation, so the item
 * test can put player 2 on them (TS_SPLIT_ITEMTEST=nearitems,<s>). */
#define MAX_ITEMS 96
static struct { BYTE *node, *geo; char name[28]; } items_seen[MAX_ITEMS];
static int n_items_seen;

static void record_item(BYTE *tpl, BYTE *node)
{
    if (!tpl || !node || strncmp((char *)(tpl + 0x30), "item_", 5) != 0 || n_items_seen >= MAX_ITEMS)
        return;
    items_seen[n_items_seen].node = node;
    items_seen[n_items_seen].geo = *(BYTE **)(node + 0x1c0);
    snprintf(items_seen[n_items_seen++].name, 28, "%s", (char *)(tpl + 0x30));
}

/* A server announces a new entity to the connected clients from its init (0x6548f0: MP_CREATE_ENT 0x42a, sent at
 * 0x654bee, with the class, name and template strings of its scene node at message +0x14, +0x54, +0x94). For
 * player 2 those said "player", so a client connected when player 2 came in made a campaign `player` of it, whose
 * first-person weapons it could not make (crash 0x447a5c, runs 20261025-213310 to -222711). A client that joins
 * later gets it from the join list, through the player's create descriptor (vtable +0x38: the multiplayer class),
 * as a `player_mp`. While player 2 is made on a server, its announcement's class and template (+0x54, +0x94; +0x14
 * is the name) say "player_mp", as the server's own announcement of a client's player does (0x697620). */
static BYTE *spawn_partner;   /* (player 1 while player 2 is spawned) */

__attribute__((used)) void __cdecl split_create_msg(char *m)
{
    static int n;
    if (splitlog > 1 && m && (spawning_p2 || n++ < 10))   /* (tests: the announcements) */
        ts_log("split: announcement \"%.40s\" \"%.40s\" \"%.40s\"%s", m + 0x14, m + 0x54, m + 0x94, spawning_p2 ? " (player 2's spawn)" : "");
    if (!spawning_p2 || !m || strcmp(m + 0x54, "player") != 0)
        return;
    /* (the template: the multiplayer player model, as the player's create descriptor names it, vtable +0x334 =
     * 0x407070 with 0: the profile's PlayerModel; "player_mp" is no template, the client made nothing of it and
     * crashed at 0x6fbd1f, run 20261026-182411) */
    char model[64] = "";
    BYTE *p1 = spawn_partner;
    if (p1 && *(DWORD *)p1 == PLAYER_VTBL) {
        struct { LONG refs; int len, cap; char s[64]; } *e = NULL;   /* (a counted string, as EStr) */
        ((void *(TSCALL *)(void *, void **, int))(*(void ***)p1)[0x334 / 4])(p1, (void **)&e, 0);
        if (e && !IsBadReadPtr(e, 0x10) && e->len > 0 && e->len < (int)sizeof model)
            snprintf(model, sizeof model, "%s", e->s);
    }
    ts_log("split: player 2 announced to the clients as \"player_mp\" (name \"%.40s\", was \"%.40s\" \"%.40s\"; template \"%s\")",
           m + 0x14, m + 0x54, m + 0x94, model);
    if (!model[0])
        return;   /* (no model: the announcement as it was) */
    strcpy(m + 0x54, "player_mp");
    snprintf(m + 0x94, 0x40, "%s", model);
    /* (its name tag on the clients: player 1's name with " 2", not the same name twice) */
    char name[0x40];
    snprintf(name, sizeof name, "%.40s", m + 0x14);
    if (name[0])
        snprintf(m + 0x14, 0x40, "%s 2", name);
}

__attribute__((naked)) static void create_msg_stub(void)
{
    __asm__("pushal\n\t"
            "pushl 44(%esp)\n\t"   /* (the message: third stack argument of 0x698020, past the return address) */
            "call _split_create_msg\n\t"
            "addl $4, %esp\n\t"
            "popal\n\t"
            "push $0x698020\n\t"
            "ret");
}

static void *TSCALL hook_mknode(void *world, BYTE *tpl, void *xform)
{
    BYTE *geo = tpl ? *(BYTE **)(tpl + 0xb8) : NULL;
    DWORD mark = geo ? *(DWORD *)(geo + 0xbc) : 0;
    int fp = spawning_p2 && geo && mark;
    char first = 0;
    if (fp) {
        *(DWORD *)(geo + 0xbc) = 0;
        /* A first-person template (0x95c480: name "wpn_plr*") shares its geometry with every node made from it
         * (node +0x1c0 = template +0xb8, node +8 |= 0x1000); the geometry is bound to one node (+0xbc), so two
         * players cannot draw it. Other templates get their own copy (0x961f30). For player 2 the template's name
         * is changed for the call (its first letter), so the copy is made; the node's first-person flag is set
         * as the shared path would. */
        if (!fp_share) {
            first = tpl[0x30];
            tpl[0x30] = 'W';
        }
    }
    BYTE *r = ((mknode_t)0x95dcf0)(world, tpl, xform);
    if (fp) {
        *(DWORD *)(geo + 0xbc) = mark;
        if (!fp_share) {
            tpl[0x30] = first;
            if (r)
                *(DWORD *)(r + 8) |= 0x1000;
        } else if (r && n_geo2 < MAX_GEO)
            geo2[n_geo2].geo = geo, geo2[n_geo2++].node2 = r;
    }
    record_crosshair(tpl, r);
    record_item(tpl, r);
    if (splitlog > 2 && tpl)   /* (tests, TS_SPLITLOG=3: every node's template) */
        ts_log("split: node template \"%s\" -> %p", (char *)(tpl + 0x30), (void *)r);
    if (splitlog > 2 && tpl && r) {   /* (where the node keeps its template) */
        static int n;
        for (int off = 0; off < 0x238 && n < 40; off += 4)
            if (*(BYTE **)(r + off) == tpl)
                n++, ts_log("split: node %p keeps template %p at +%#x", (void *)r, (void *)tpl, off);
    }
    if (spawning_p2 || (splitlog && mark))
        ts_log("split: node for template \"%s\" geometry %p in use %08lx -> %p (own geometry %p)",
               tpl ? (char *)(tpl + 0x30) : "?", (void *)geo, mark, (void *)r, r ? *(void **)(r + 0x1c0) : NULL);
    return r;
}

__attribute__((naked)) static void spawn_trampoline(void)
{
    __asm__("push 8(%esp)\n\t"     /* id */
            "push 8(%esp)\n\t"     /* class */
            "push %edx\n\t"
            "push %ecx\n\t"
            "call _split_weapon_spawn\n\t"
            "add $16, %esp\n\t"
            "ret $8");
}

/* ---- state ---- */
typedef struct { LONG refs; int len, cap; char s[1]; } EStr;
int split_msg_owner(void);
/* ---- split screen over the network (see the section "network split screen" below) ---- */
static int remote_host;        /* host: player 2 is the player 2 of a client's split screen (no split screen here) */
static int remote_peer = -1;   /* that client's id */
static DWORD remote_seen;      /* its last pad message */
static BYTE *remote_mp;        /* that client's own player (player_mp here): player 2's partner */
static int client_split;       /* client: split screen, player 2 run by the host */
static int client_choice;      /* client: chosen in the menu (UI string CoopSplitJoin = "1") */
static int net_split_off;      /* TS_SPLIT_CLIENT=0 */
static int net_noshadow;       /* TS_SPLIT_SHADOW=0: no local copy of player 2 on a client (camera only) */
static int client_netid = -1;  /* client: the net id of our player 2 (its player_mp here) */
static BYTE *cp2;              /* client: that player_mp */
static float cl_rows[9], cl_pos[3], cl_spos[3], cl_life;
static int cl_have, cl_alive;
static BYTE *hud_hidden;       /* client: the HUD frame hidden for the view of player 2 */
static DWORD hud_hidden_flags;
static void client_cam(BYTE *dst);
static void client_view(int v);
static BYTE *find_cp2(void);
static void client_extra(int v);
static int copy_updates;       /* (tests: updates of a client's copy of player 2) */
static float cl_body[3];       /* client: the host's player 2's body position (message 61) */
static int cl_body_ok, copy_free;   /* copy_free: TS_SPLIT_COPYFREE=1, the copy walks on its own (tests) */
static void client_slave(void);
static BYTE *extra_node;       /* client: one more body hidden for a view (the shadow's, or our player 2's player_mp) */
static DWORD extra_flags;
static float extra_pos[3];
static int extra_moved;
static int node_visible(BYTE *n);
static int view = -1;           /* the view this frame renders (-1: no split this frame) */
static unsigned frame_no;
static int swapped;
static BYTE *saved_cam, *saved_pa, *saved_pb;
static float saved_canvas;
/* The HUD canvas of the half's shape is kept between frames too while split screen draws: widgets placed by a
 * share of the canvas (the vehicle health widget at 50 %, hints 85 % wide) are laid out in their updates,
 * outside the frame; with the whole screen's canvas there they landed at the right edge of a half and were cut.
 * The whole canvas comes back at the first frame without split screen. */
static float canvas_full, canvas_half;
static int canvas_kept;
/* The HUD's elements are laid out once, when the HUD is made: each keeps its place as a share of its parent, the
 * HUD frame itself takes the canvas (element vtable +0xbc = 0x6ffc30, parent +0x40, first child +0x44, next
 * +0x48, rectangle +0x4c..+0x58). Laid out for the whole screen's canvas, everything placed by a share (the
 * vehicle health at 50 %, the radar and hints at the right) landed outside a half's canvas: cut or missing. The
 * HUD is laid out again for the half's canvas when split screen starts drawing, and for the whole one when it
 * stops. The HUD frame is found from a HUD widget (split_hud_update). */
static BYTE *hud_root;
static DWORD hud_root_vt;

static void relayout(BYTE *e, int depth)
{
    if (!e || depth > 12 || IsBadReadPtr(e, 0x5c))
        return;
    ((void (TSCALL *)(void *))(*(void ***)e)[0xbc / 4])(e);
    for (BYTE *c = *(BYTE **)(e + 0x44); c && !IsBadReadPtr(c, 0x4c); c = *(BYTE **)(c + 0x48))
        relayout(c, depth + 1);
}

static void hud_relayout(const char *why)
{
    if (!hud_root || IsBadReadPtr(hud_root, 0x5c) || *(DWORD *)hud_root != hud_root_vt) {
        hud_root = NULL;
        return;
    }
    relayout(hud_root, 0);
    ts_log("split: HUD laid out again for %s (canvas %.1f wide)", why, CANVAS_W);
}
static BYTE *shown_node;        /* the other player's body node, shown for this view */
static DWORD shown_flags;
static BYTE *own_node;          /* the viewer's own body node, hidden for this view */
static DWORD own_flags;
static BYTE *body_of(BYTE *pl);
static void fx_apply_view(int k);
static void fx_restore(void);
static int p2_depth(void);
static void place_p2(BYTE *p1);
static void place_player(BYTE *pl, BYTE *partner);
static void p2_remove(const char *why);
static int net_mode(void);
static BYTE *rc;                /* the render camera: a copy of the view's camera with the half-screen fov */
static BYTE *cam2;              /* player 2's camera (stage 2: an offset copy of player 1's) */

static BYTE *p2;                /* player 2 */
static BYTE *p1_seen;           /* the player 1 object p2 was made next to (a new one: new level) */
static DWORD p1_since;
static BYTE *ctx2;              /* player 2's command context (a copy of GAME with its own entries) */
static BYTE *raw2;              /* player 2's raw input block */
static DWORD imf2_a[5], imf2_b[4];
static int p2_failed;
static DWORD p2_born;
static BYTE *p2_body;   /* (player 2's body at its spawn: a load gives it a new one) */
static LONG logged;

/* the picture of each view and the composite */
static IDirect3DTexture9 *pic[2];
static int pic_ok[2];
static UINT pic_w, pic_h;
static D3DFORMAT pic_fmt;
static int complaints;
typedef HRESULT (WINAPI *Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
static Reset_t real_Reset;

static void complain(const char *what, long hr)
{
    if (complaints++ < 8)
        ts_log("split: %s (0x%08lx)", what, (unsigned long)hr);
}

static int cinematic(void)
{
    BYTE *c = CINE, *ui = *(BYTE **)0x1095fc8;
    /* (UI +0xc0 bit 0x10: the level's opening scene of ts2_city_part01 (the suit's start-up, the scripted arrival)
     * runs with it, the game's camera unused at 0,0,0; it is 0 in play. Full screen then, as for cutscenes.) */
    if (ui && (*(DWORD *)(ui + 0xc0) & 0x10))
        return 1;
    if (!c)
        return 0;
    int i = *(int *)(c + 0x10);
    return i >= 0 && i < *(int *)(c + 8);
}

static void bb_size(int *w, int *h)
{
    BYTE *desc = DRV ? *(BYTE **)(DRV + 0x10c) : NULL;
    *w = desc ? *(int *)(desc + 8) : 0;
    *h = desc ? *(int *)(desc + 0xc) : 0;
}

/* side by side (1) or top and bottom (0) */
static int side_by_side(int w, int h)
{
    if (layout_cfg)
        return layout_cfg == 'v';
    return h > 0 && w * 2 > h * 3;
}

static void half_rect(int k, int w, int h, RECT *r)
{
    if (side_by_side(w, h))
        SetRect(r, k ? w / 2 : 0, 0, k ? w : w / 2, h);
    else
        SetRect(r, 0, k ? h / 2 : 0, w, k ? h : h / 2);
}

/* The view's field of view for a half screen: the vertical angle stays as the game set it for the whole
 * screen (+0x150, degrees), the horizontal one follows the half's shape (+0x14c), as a cut-out of the screen. */
static void fit_fov(BYTE *cam, int w, int h)
{
    RECT r;
    half_rect(0, w, h, &r);
    float hw = (float)(r.right - r.left), hh = (float)(r.bottom - r.top);
    float *f = (float *)cam;
    float vfov = f[0x150 / 4];
    if (!(vfov > 1 && vfov < 170) || hw < 1 || hh < 1)
        return;
    f[0x14c / 4] = 2.0f * atanf(tanf(vfov * 0.5f * 0.0174532925f) * hw / hh) * 57.2957795f;
    f[0x154 / 4] = hh / hw;
    cam_rebuild(cam);
}

/* Stage 2's stand-in for player 2: player 1's camera moved 1.5 m to its right and 0.5 m up. */
static void offset_camera(BYTE *dst, const BYTE *src)
{
    memcpy(dst, src, CAM_SIZE);
    const float *right = (const float *)src, *up = (const float *)(src + 0x10), *p = (const float *)(src + 0x30);
    float pos[3];
    for (int i = 0; i < 3; i++)
        pos[i] = p[i] + right[i] * 1.5f + up[i] * 0.5f;
    cam_set_pos(dst, pos);
}

/* In play: the in-game command context is active. The main menu runs a level with a `player` of its own,
 * loading screens and the pause menu use the UI context; they stay full screen. */
static int in_play(void)
{
    BYTE *im = IM, *c = im ? *(BYTE **)(im + 0x40) : NULL, *ui = *(BYTE **)0x1095fc8;
    /* (UI +0xc0: 0x1800 seen in the main menu, which keeps the GAME context active; 0 in play) */
    return c && *(DWORD *)c == VT_GAME && !(ui && (*(DWORD *)(ui + 0xc0) & 0x1800));
}

static int split_wanted(void)
{
    if (remote_host)
        return 0;
    if (!stage || !PLAYER_B || *(DWORD *)PLAYER_B != PLAYER_VTBL || !CAM || cinematic() || !in_play())
        return 0;
    if (CORES != 1) {
        static int told;
        if (!told++)
            ts_log("split: the frame runs on %d threads; split screen needs the single-thread frame", CORES);
        return 0;
    }
    return 1;
}

/* The first-person weapon in a half. The weapon pass (0x680730) draws it with its own vertical angle (console
 * WPN_FOV into camera +0x150) and the horizontal one from the picture's shape (+0x154, which fit_fov sets for
 * the half): in a side-by-side half the horizontal angle halved and the pistol at the lower right fell outside
 * (the rifle was cut). For a view of a split frame the weapon pass keeps the whole screen's horizontal angle and
 * gets the vertical angle that goes with it in the half: the weapon is drawn smaller, at its usual place. Done
 * at the pass's projection rebuild (the call 0x680917 -> 0x624690, camera in eax/ebx). */
static int fp_noscale;

__attribute__((used)) void __cdecl split_fp_proj(BYTE *cam)
{
    if (fp_noscale || !swapped || view < 0 || !cam)
        return;
    int w, h;
    bb_size(&w, &h);
    RECT r;
    half_rect(view, w, h, &r);
    float hw = (float)(r.right - r.left), hh = (float)(r.bottom - r.top);
    if (w <= 0 || h <= 0 || hw < 1 || hh < 1 || hw / hh >= (float)w / (float)h * 0.98f)
        return;   /* (a half as wide as the screen or wider: nothing falls out at the sides) */
    float *f = (float *)cam, v = f[0x150 / 4] * 0.0174532925f;
    if (!(v > 0.05f && v < 3.0f))
        return;
    float th = tanf(v * 0.5f) * (float)w / (float)h;   /* (tan of half the whole screen's horizontal angle) */
    f[0x14c / 4] = 2.0f * atanf(th) * 57.2957795f;
    f[0x150 / 4] = 2.0f * atanf(th * hh / hw) * 57.2957795f;
}

__attribute__((naked)) static void fp_proj_stub(void)
{
    __asm__("push %eax\n\t"
            "push %eax\n\t"
            "call _split_fp_proj\n\t"
            "add $4, %esp\n\t"
            "pop %eax\n\t"
            "mov $0x624690, %edx\n\t"
            "jmp *%edx");
}

static void restore_globals(void)
{
    if (!swapped)
        return;
    CAM = saved_cam;
    PLAYER_A = saved_pa;
    PLAYER_B = saved_pb;
    if (splitlog > 1 && n_geo2 > 4) {   /* (tests: the owner of one geometry at each frame end, the first frames) */
        static int n;
        if (n++ < 24)
            ts_log("split: frame end view %d geometry %p owner %08lx (p2 node %p, saved %08lx)", view, (void *)geo2[4].geo,
                   *(DWORD *)(geo2[4].geo + 0xbc), (void *)geo2[4].node2, geo2[4].saved);
    }
    fx_restore();
    if (geo_swapped)
        for (int i = 0; i < n_geo2; i++)
            *(DWORD *)(geo2[i].geo + 0xbc) = geo2[i].saved;
    for (int i = 0; i < n_fp_hidden; i++)   /* (bit 0 back as it was; the rest as the frame left it) */
        *(DWORD *)(fp_hidden[i].node + 4) = (*(DWORD *)(fp_hidden[i].node + 4) & ~1u) | (fp_hidden[i].saved & 1u);
    n_fp_hidden = 0;
    geo_swapped = 0;
    if (shown_node)
        *(DWORD *)(shown_node + 4) = (*(DWORD *)(shown_node + 4) & ~0x10000u) | (shown_flags & 0x10000u);
    shown_node = NULL;
    if (own_node)
        *(DWORD *)(own_node + 4) = (*(DWORD *)(own_node + 4) & ~0x10000u) | (own_flags & 0x10000u);
    own_node = NULL;
    if (hud_hidden)
        *(DWORD *)(hud_hidden + 0x18) = (*(DWORD *)(hud_hidden + 0x18) & ~2u) | (hud_hidden_flags & 2u);
    hud_hidden = NULL;
    if (extra_node)
        *(DWORD *)(extra_node + 4) = (*(DWORD *)(extra_node + 4) & ~0x10001u) | (extra_flags & 0x10001u);
    if (extra_node && extra_moved)
        memcpy(extra_node + 0x68, extra_pos, sizeof extra_pos);
    extra_moved = 0;
    extra_node = NULL;
    swapped = 0;
}

/* ---- renderer frame start / end ---- */
static void TSCALL hook_fstart(void *r)
{
    restore_globals();   /* (a frame without its end: never seen, but nothing must stay swapped) */
    view = -1;
    if (split_wanted()) {
        int w, h;
        bb_size(&w, &h);
        view = stage >= 2 ? (int)(frame_no++ & 1) : 0;
        BYTE *src = CAM;
        if (view == 1) {
            if (client_split && cl_have && !p2)
                client_cam(cam2);
            else if (stage == 2 || !p2)
                offset_camera(cam2, CAM);
            src = cam2;
        }
        memcpy(rc, src, CAM_SIZE);
        fit_fov(rc, w, h);
        saved_cam = CAM;
        saved_pa = PLAYER_A;
        saved_pb = PLAYER_B;
        saved_canvas = CANVAS_W;
        swapped = 1;
        CAM = rc;
        /* The HUD is laid out on a canvas of the half's shape: drawn over the whole picture and squeezed into
         * the half with it, it keeps its proportions (the UI update lays the frames out on the canvas every
         * frame, 0x671dcb). */
        RECT hr;
        half_rect(0, w, h, &hr);
        if (stage >= 2 && hr.bottom > hr.top) {
            float half = CANVAS_H * (float)(hr.right - hr.left) / (float)(hr.bottom - hr.top);
            int changed = !canvas_kept || half != canvas_half;
            if (!canvas_kept || CANVAS_W != canvas_half)   /* (the game's own value, or a new one it set) */
                canvas_full = CANVAS_W;
            canvas_kept = 1;
            CANVAS_W = canvas_half = half;
            if (changed)
                hud_relayout("a half");
        }
        if (view == 1 && p2) {
            PLAYER_A = PLAYER_B = p2;
            if (!no_geo_owner) {
                for (int i = 0; i < n_geo2; i++) {
                    geo2[i].saved = *(DWORD *)(geo2[i].geo + 0xbc);
                    *(DWORD *)(geo2[i].geo + 0xbc) = (DWORD)geo2[i].node2;
                }
                geo_swapped = 1;
            }
        }
        fx_apply_view(view);
        /* The other player's body is drawn in this view. The render prep skips nodes with +4 bit 0x10000 in the
         * main view (view entry flags 0, 0x9a1389): the local player's body has it, and both players are local
         * here. */
        if (p2) {
            BYTE *other = view == 0 ? (client_split ? NULL : p2) : saved_pb;
            BYTE *b = body_of(other), *n = b ? *(BYTE **)(b + 0x230) : NULL;
            shown_node = n;
            if (n) {
                shown_flags = *(DWORD *)(n + 4);
                *(DWORD *)(n + 4) &= ~0x10000u;
            }
            /* (and the viewer's own body hidden: the game marks only the local player's, and while player 2's
             * view is drawn the local player is player 1) */
            BYTE *mine = view == 0 ? saved_pb : p2;
            BYTE *ob = body_of(mine), *on = ob ? *(BYTE **)(ob + 0x230) : NULL;
            own_node = on != n ? on : NULL;
            if (own_node) {
                own_flags = *(DWORD *)(on + 4);
                *(DWORD *)(on + 4) |= 0x10000u;
            }
            if (splitlog > 1) {   /* (tests: both bodies' node flags in this view, every 2 s per view) */
                static DWORD last[2];
                DWORD t = GetTickCount();
                if (t - last[view] >= 2000) {
                    last[view] = t;
                    ts_log("split: view %d: own body %p node %p +4 %08lx +8 %08lx vis %d; partner body %p node %p +4 %08lx +8 %08lx vis %d",
                           view, (void *)ob, (void *)on, on ? *(DWORD *)(on + 4) : 0, on ? *(DWORD *)(on + 8) : 0, node_visible(on),
                           (void *)b, (void *)n, n ? *(DWORD *)(n + 4) : 0, n ? *(DWORD *)(n + 8) : 0, node_visible(n));
                }
            }
            n_fp_hidden = 0;
            BYTE **objs = view == 0 ? own2 : own1;
            int no = view == 0 ? n_own2 : n_own1;
            for (int i = 0; i < no && n_fp_hidden < 2 * MAX_OWN; i++) {
                BYTE *fn = objs[i] ? *(BYTE **)(objs[i] + 0x230) : NULL;
                if (!fn || IsBadReadPtr(fn, 8))
                    continue;
                fp_hidden[n_fp_hidden].node = fn;
                fp_hidden[n_fp_hidden++].saved = *(DWORD *)(fn + 4);
                *(DWORD *)(fn + 4) |= 1;
            }
            int ow = view == 0 ? 1 : 0;
            for (int i = 0; i < n_crs[ow] && n_fp_hidden < 2 * MAX_OWN; i++) {
                if (!crosshair_alive(ow, i))
                    continue;
                BYTE *fn = crs[ow][i].node;
                fp_hidden[n_fp_hidden].node = fn;
                fp_hidden[n_fp_hidden++].saved = *(DWORD *)(fn + 4);
                *(DWORD *)(fn + 4) |= 1;
            }
            if (client_split)
                client_extra(view);
        } else if (client_split)
            client_view(view);
    } else {
        pic_ok[0] = pic_ok[1] = 0;   /* (back to the whole screen: the old halves are stale) */
        if (canvas_kept) {
            if (CANVAS_W == canvas_half)
                CANVAS_W = canvas_full;
            canvas_kept = 0;
            hud_relayout("the whole screen");
        }
    }
    if (splitlog && view >= 0 && logged < 60) {
        logged++;
        ts_log("split: frame start view %d thread %lu cam %p pa %p pb %p p2 %p cores %d", view, GetCurrentThreadId(),
               (void *)CAM, (void *)PLAYER_A, (void *)PLAYER_B, (void *)p2, CORES);
    }
    {   /* (frames per second: all frames and the split ones, logged every 10 s) */
        static DWORD t0;
        static int nall, nsplit;
        DWORD now = GetTickCount();
        nall++, nsplit += view >= 0;
        if (!t0)
            t0 = now;
        else if (now - t0 >= 10000) {
            if (nsplit)
                ts_log("split: %.1f frames/s, %.1f split frames/s (each view %.1f/s)", nall * 1000.0f / (now - t0),
                       nsplit * 1000.0f / (now - t0), nsplit * 500.0f / (now - t0));
            t0 = now, nall = nsplit = 0;
        }
    }
    real_fstart(r);
}

/* ---- screen effects per view ----
 * The renderer's post-effect manager (0x1096f18, also [[0xfcacd8] +0x24]) holds 13 effect objects at +0x14f4
 * (vtable +4 start, +8 stop, +0x14 set the strength; +8 bit 0 of the object: on, drawn by 0x665e1e). The players'
 * code starts and sets them by index (0x665f70 and 0x504b40, stopped by 0x665fa0 / 0x504b80): the death blur (8),
 * the low-life tint (10, every frame from the player's update, 0x446600), the time powers and more. Both players'
 * calls land in the same objects. The mod keeps each player's last call per effect (player 2's: those made while
 * its swap is on), and from the frame start to the frame end of a view puts that view's player's state into the
 * objects (on/off and strength), then the last state back. */
#define N_FX 13
static struct { int seen, on; float v; } fx[2][N_FX];
static float fx_last[N_FX];
static int fx_view = -1;
static DWORD fx_saved_flags[N_FX];
static int fx_off;   /* TS_SPLIT_FXSHARED=1: effects as the game has them (shared) */
/* Per view only the player's own condition: the damage tint and blur (6), the death effect (8) and the low-life
 * tint (10). The time powers' effects stay as the game has them, in both halves: the world's time is one for
 * both players (TS_SPLIT_FXVIEW=6,8,10,... overrides the list). */
static DWORD fx_per_view = (1u << 6) | (1u << 8) | (1u << 10);

typedef void (TSCALL *fx_call_t)(void *);
typedef void (TSCALL *fx_set_t)(void *, float);

static BYTE *fx_obj(BYTE *pm, int i)
{
    return pm && i >= 0 && i < N_FX ? *(BYTE **)(pm + 0x14f4 + i * 4) : NULL;
}

__attribute__((used)) void __cdecl split_fx_start(BYTE *pm, int i, float v)
{
    BYTE *e = fx_obj(pm, i);
    if (!e)
        return;
    if (remote_host && p2_depth() && (fx_per_view >> i & 1)) {   /* (a client's player 2: its own condition is
                                                                    shown on that client, not on the host) */
        fx[1][i].seen = fx[1][i].on = 1;
        fx[1][i].v = v;
        return;
    }
    ((fx_call_t)(*(void ***)e)[1])(e);
    ((fx_set_t)(*(void ***)e)[5])(e, v);
    *(DWORD *)(pm + 0x2c) |= 0x10000;
    int who = p2_depth() ? 1 : 0;
    if (splitlog > 1 && !fx[who][i].seen)
        ts_log("split: effect %d first started by player %d (%.2f)", i, who + 1, v);
    fx[who][i].seen = fx[who][i].on = 1;
    fx[who][i].v = v;
    fx_last[i] = v;
}

__attribute__((used)) void __cdecl split_fx_stop(BYTE *pm, int i)
{
    BYTE *e = fx_obj(pm, i);
    if (!e)
        return;
    if (remote_host && p2_depth() && (fx_per_view >> i & 1)) {
        fx[1][i].seen = 1;
        fx[1][i].on = 0;
        return;
    }
    ((fx_call_t)(*(void ***)e)[2])(e);
    int who = p2_depth() ? 1 : 0;
    fx[who][i].seen = 1;
    fx[who][i].on = 0;
}

/* entries: 0x665f70 (esi = manager, edi = index, float on the stack, ret 4), 0x504b40 (edi = index, float,
 * ret 4), 0x665fa0 (eax = manager, ecx = index), 0x504b80 (eax = renderer, ecx = index) */
__attribute__((naked)) static void fx_start_a(void)
{
    __asm__("push 4(%esp)\n\t"
            "push %edi\n\t"
            "push %esi\n\t"
            "call _split_fx_start\n\t"
            "add $12, %esp\n\t"
            "ret $4");
}

__attribute__((naked)) static void fx_start_b(void)
{
    __asm__("push 4(%esp)\n\t"
            "push %edi\n\t"
            "mov 0xfcacd8, %eax\n\t"
            "push 0x24(%eax)\n\t"
            "call _split_fx_start\n\t"
            "add $12, %esp\n\t"
            "ret $4");
}

__attribute__((naked)) static void fx_stop_a(void)
{
    __asm__("push %ecx\n\t"
            "push %eax\n\t"
            "call _split_fx_stop\n\t"
            "add $8, %esp\n\t"
            "ret");
}

__attribute__((naked)) static void fx_stop_b(void)
{
    __asm__("push %ecx\n\t"
            "push 0x24(%eax)\n\t"
            "call _split_fx_stop\n\t"
            "add $8, %esp\n\t"
            "ret");
}

static int install_fx_hooks(void)
{
    static const struct { DWORD at; unsigned char want[5]; void *to; } h[4] = {
        {0x665f70, {0x8b, 0x8c, 0xbe, 0xf4, 0x14}, (void *)fx_start_a},
        {0x504b40, {0xa1, 0xd8, 0xac, 0xfc, 0x00}, (void *)fx_start_b},
        {0x665fa0, {0x8b, 0x8c, 0x88, 0xf4, 0x14}, (void *)fx_stop_a},
        {0x504b80, {0x8b, 0x40, 0x24, 0x8b, 0x8c}, (void *)fx_stop_b},
    };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char j[5] = {0xe9};
        *(DWORD *)(j + 1) = (DWORD)h[i].to - (h[i].at + 5);
        n += ts_patch_bytes(h[i].at, h[i].want, j, 5);
    }
    return n == 4;
}

static BYTE *fx_manager(void)
{
    BYTE *r = *(BYTE **)0xfcacd8;
    return r ? *(BYTE **)(r + 0x24) : NULL;
}

static void fx_apply_view(int k)
{
    BYTE *pm = fx_manager();
    if (fx_off || !pm || !p2)
        return;
    for (int i = 0; i < N_FX; i++) {
        BYTE *e = fx_obj(pm, i);
        if (!e)
            continue;
        fx_saved_flags[i] = *(DWORD *)(e + 8);
        if ((!fx[0][i].seen && !fx[1][i].seen) || !(fx_per_view >> i & 1))
            continue;
        int on = fx[k][i].seen && fx[k][i].on;
        *(DWORD *)(e + 8) = (*(DWORD *)(e + 8) & ~1u) | (on ? 1u : 0u);
        if (on)
            ((fx_set_t)(*(void ***)e)[5])(e, fx[k][i].v);
    }
    fx_view = k;
}

static void fx_restore(void)
{
    BYTE *pm = fx_manager();
    if (fx_view < 0 || !pm)
        return;
    for (int i = 0; i < N_FX; i++) {
        BYTE *e = fx_obj(pm, i);
        if (!e || (!fx[0][i].seen && !fx[1][i].seen) || !(fx_per_view >> i & 1))
            continue;
        *(DWORD *)(e + 8) = fx_saved_flags[i];
        if (fx_saved_flags[i] & 1)
            ((fx_set_t)(*(void ***)e)[5])(e, fx_last[i]);
    }
    fx_view = -1;
}

static void TSCALL hook_fend(void *r)
{
    real_fend(r);
    if (splitlog && view >= 0 && logged < 60) {
        logged++;
        ts_log("split: frame end view %d thread %lu", view, GetCurrentThreadId());
    }
    restore_globals();
}

/* ---- composite ---- */
static void release_pics(void)
{
    for (int i = 0; i < 2; i++) {
        if (pic[i])
            IDirect3DTexture9_Release(pic[i]);
        pic[i] = NULL;
        pic_ok[i] = 0;
    }
    pic_w = pic_h = 0;
}

static HRESULT WINAPI hook_Reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
{
    release_pics();   /* (default-pool textures must go before a reset) */
    return real_Reset(dev, pp);
}

static int make_pics(IDirect3DDevice9 *dev, const D3DSURFACE_DESC *d)
{
    if (pic[0] && pic_w == d->Width && pic_h == d->Height && pic_fmt == d->Format)
        return 1;
    release_pics();
    if (!real_Reset) {
        void *p = ts_patch_vtable(dev, 16, (void *)hook_Reset);
        if (p)
            real_Reset = (Reset_t)p;
    }
    for (int i = 0; i < 2; i++) {
        HRESULT hr = IDirect3DDevice9_CreateTexture(dev, d->Width, d->Height, 1, D3DUSAGE_RENDERTARGET, d->Format,
                                                    D3DPOOL_DEFAULT, &pic[i], NULL);
        if (FAILED(hr)) {
            complain("view picture not created", hr);
            release_pics();
            return 0;
        }
    }
    pic_w = d->Width, pic_h = d->Height, pic_fmt = d->Format;
    ts_log("split: view pictures %ux%u format %d", pic_w, pic_h, pic_fmt);
    return 1;
}

static void copy_surface(IDirect3DDevice9 *dev, IDirect3DSurface9 *from, const RECT *from_rect, IDirect3DTexture9 *to_tex,
                         const RECT *to_rect, IDirect3DSurface9 *to_surf, D3DTEXTUREFILTERTYPE f, const char *what)
{
    IDirect3DSurface9 *s = to_surf;
    if (!s && FAILED(IDirect3DTexture9_GetSurfaceLevel(to_tex, 0, &s)))
        return;
    HRESULT hr = IDirect3DDevice9_StretchRect(dev, from, from_rect, s, to_rect, f);
    if (FAILED(hr))
        complain(what, hr);
    if (!to_surf)
        IDirect3DSurface9_Release(s);
}

/* Replaces the full-screen quad (thiscall on the render driver; x0,y0,u0,v0, x1,y1,u1,v1, z, material). */
static void TSCALL hook_quad(void *rend, float x0, float y0, float u0, float v0, float x1, float y1, float u1, float v1,
                             float z, void *mat)
{
    int k = view;
    BYTE *pool = POOL_CUR >= 0 && POOL_CUR < 2 ? POOL(POOL_CUR) : NULL;
    IDirect3DTexture9 *tex = pool ? *(IDirect3DTexture9 **)(pool + 0x40) : NULL;
    IDirect3DDevice9 *dev = DRV ? *(IDirect3DDevice9 **)(DRV + 0x88c) : NULL;
    if (k < 0 || !tex || !dev || x1 < 2 || y1 < 2) {
        real_quad(rend, x0, y0, u0, v0, x1, y1, u1, v1, z, mat);
        return;
    }
    int w = (int)x1, h = (int)y1;
    RECT r[2];
    half_rect(0, w, h, &r[0]);
    half_rect(1, w, h, &r[1]);
    D3DSURFACE_DESC d;
    IDirect3DSurface9 *src = NULL;
    int ok = SUCCEEDED(IDirect3DTexture9_GetLevelDesc(tex, 0, &d)) && make_pics(dev, &d)
             && SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &src));
    /* The picture the game drew covers only the quad's texture coordinates of the frame texture (u1, v1 < 1 when
     * the back buffer is larger than the game's picture, as in a window bigger than the chosen resolution): only
     * that part is copied and put into the half; the rest of the texture is stale. */
    RECT used;
    SetRect(&used, (int)(u0 * d.Width + 0.5f), (int)(v0 * d.Height + 0.5f), (int)(u1 * d.Width + 0.5f),
            (int)(v1 * d.Height + 0.5f));
    if (used.left < 0 || used.top < 0 || used.right > (LONG)d.Width || used.bottom > (LONG)d.Height
        || used.right - used.left < 16 || used.bottom - used.top < 16)
        SetRect(&used, 0, 0, d.Width, d.Height);
    if (ok) {
        copy_surface(dev, src, &used, pic[k], &used, NULL, D3DTEXF_NONE, "view picture copy failed");
        pic_ok[k] = 1;
        IDirect3DSurface9_Release(src);
    }
    /* the engine's own quad into this view's half (its state set-up stays as the engine expects) */
    real_quad(rend, (float)r[k].left, (float)r[k].top, u0, v0, (float)r[k].right, (float)r[k].bottom, u1, v1, z, mat);
    if (!ok)
        return;
    /* then both pictures, filtered the same way, so the halves match (the engine's draw is immediate) */
    IDirect3DSurface9 *rt = NULL;
    if (FAILED(IDirect3DDevice9_GetRenderTarget(dev, 0, &rt)))
        return;
    for (int i = 0; i < 2; i++)
        if (pic_ok[i]) {
            IDirect3DSurface9 *s = NULL;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(pic[i], 0, &s))) {
                HRESULT hr = IDirect3DDevice9_StretchRect(dev, s, &used, rt, &r[i], D3DTEXF_LINEAR);
                if (FAILED(hr))
                    complain("half copy failed", hr);
                IDirect3DSurface9_Release(s);
            }
        }
    /* A back buffer larger than the game's picture (a window bigger than the chosen resolution, as the test
     * windows are) keeps an old picture in the strip outside it, which the halves made look like a repeated piece
     * of the right view: it is cleared. */
    D3DSURFACE_DESC rd;
    if (SUCCEEDED(IDirect3DSurface9_GetDesc(rt, &rd))) {
        RECT strip;
        if ((int)rd.Width > w) {
            SetRect(&strip, w, 0, rd.Width, rd.Height);
            IDirect3DDevice9_ColorFill(dev, rt, &strip, D3DCOLOR_XRGB(0, 0, 0));
        }
        if ((int)rd.Height > h) {
            SetRect(&strip, 0, h, rd.Width, rd.Height);
            IDirect3DDevice9_ColorFill(dev, rt, &strip, D3DCOLOR_XRGB(0, 0, 0));
        }
    }
    IDirect3DSurface9_Release(rt);
    if (splitlog && logged < 60) {
        logged++;
        ts_log("split: composite view %d into %ld,%ld-%ld,%ld (pool %d %ux%u fmt %d, used %ld,%ld-%ld,%ld uv %.3f,%.3f-%.3f,%.3f)"
               " thread %lu", k, r[k].left, r[k].top, r[k].right, r[k].bottom, POOL_CUR, d.Width, d.Height, d.Format,
               used.left, used.top, used.right, used.bottom, u0, v0, u1, v1, GetCurrentThreadId());
    }
}

/* ---- player 2 ---- */
static BYTE *game_context(BYTE *im)
{
    for (int i = 0; i < 8; i++) {
        BYTE *c = ((BYTE **)(im + 0x44))[i];
        if (c && *(DWORD *)c == VT_GAME)
            return c;
    }
    return NULL;
}

/* Player 2's command context: the GAME context (0x44 bytes) with its own copy of the entries (0x68 bytes per
 * command, bindings included: pad codes are what pass 2 feeds). */
static int make_context(void)
{
    BYTE *im = IM, *g = im ? game_context(im) : NULL;
    if (!g)
        return 0;
    int first = *(int *)(g + 8), end = *(int *)(g + 0xc);
    BYTE *e = *(BYTE **)(g + 4);
    if (!e || end <= first || end - first > 0x400)
        return 0;
    if (!ctx2)
        ctx2 = calloc(1, 0x44);
    BYTE *e2 = calloc(end - first, 0x68);
    if (!ctx2 || !e2)
        return 0;
    memcpy(ctx2, g, 0x44);
    memcpy(e2, e, (size_t)(end - first) * 0x68);
    *(BYTE **)(ctx2 + 4) = e2;   /* (the old copy of a previous level is left: a few KB) */
    BYTE *raw = *(BYTE **)(im + 0x3c);
    if (!raw2)
        raw2 = malloc(RAW_TO);
    if (!raw2)
        return 0;
    memcpy(raw2, raw, RAW_TO);   /* (calibration and layout as player 1's; the pass then makes it player 2's) */
    memset(raw2 + RAW_FROM, 0, 0x154 * 4);
    memset(raw2 + 0x6b4, 0, 0x154 * 4);
    memcpy(imf2_a, im + IMF_A, sizeof imf2_a);
    memcpy(imf2_b, im + IMF_B, sizeof imf2_b);
    return 1;
}

/* The engine's command evaluation, then a second pass for player 2 with its raw input, context and look state. */
static void TSCALL hook_eval(void *imv)
{
    real_eval(imv);
    BYTE *im = imv;
    BYTE *ctx1 = *(BYTE **)(im + 0x40);
    static int ran, skipped;
    static DWORD lastlog;
    if (!p2 || !ctx2 || !raw2 || !ctx1 || *(DWORD *)ctx1 != VT_GAME) {
        skipped++;
        if (splitlog && client_split && GetTickCount() - lastlog >= 3000) {   /* (tests: the second pass on a client) */
            lastlog = GetTickCount();
            ts_log("split: command pass for the copy skipped %d, run %d (p2 %p ctx2 %p raw2 %p ctx1 %p vt %08lx)", skipped, ran, (void *)p2,
                   (void *)ctx2, (void *)raw2, (void *)ctx1, ctx1 ? *(DWORD *)ctx1 : 0);
        }
        return;
    }
    ran++;
    if (splitlog && client_split && GetTickCount() - lastlog >= 3000) {
        lastlog = GetTickCount();
        ts_log("split: command pass for the copy skipped %d, run %d (context vt %08lx)", skipped, ran, *(DWORD *)ctx1);
    }
    BYTE *raw = *(BYTE **)(im + 0x3c);
    static BYTE raw1[RAW_TO];
    DWORD a1[5], b1[4];
    memcpy(raw1 + RAW_FROM, raw + RAW_FROM, RAW_TO - RAW_FROM);
    memcpy(a1, im + IMF_A, sizeof a1);
    memcpy(b1, im + IMF_B, sizeof b1);
    DWORD flags = *(DWORD *)(raw + 4);
    memcpy(raw + RAW_FROM, raw2 + RAW_FROM, RAW_TO - RAW_FROM);
    memcpy(im + IMF_A, imf2_a, sizeof a1);
    memcpy(im + IMF_B, imf2_b, sizeof b1);
    *(BYTE **)(im + 0x40) = ctx2;
    *(DWORD *)(raw + 4) = (flags & ~6u) | 1;   /* only the "keyboard" poll, where gamepad.c writes pad 2 */
    gamepad_poll_player2(1);
    eval_commands(im);
    gamepad_poll_player2(0);
    *(DWORD *)(raw + 4) = flags;
    *(BYTE **)(im + 0x40) = ctx1;
    memcpy(raw2 + RAW_FROM, raw + RAW_FROM, RAW_TO - RAW_FROM);
    memcpy(imf2_a, im + IMF_A, sizeof a1);
    memcpy(imf2_b, im + IMF_B, sizeof b1);
    memcpy(raw + RAW_FROM, raw1 + RAW_FROM, RAW_TO - RAW_FROM);
    memcpy(im + IMF_A, a1, sizeof a1);
    memcpy(im + IMF_B, b1, sizeof b1);
}

/* Player 2's update with the world seen as player 2's: player pointers, camera, commands and look state. */
/* The world as player 2 sees it: both player pointers, its camera, its command context and look state. Nested
 * calls (player 2's update sends messages to its weapons) swap once. */
static struct { int depth; BYTE *pa, *pb, *cam, *ctx1; int with_ctx; DWORD a1[5], b1[4]; } as2;

static void geo_owner_p2(int on);

/* A client's copy of player 2 runs its update as a single-player game's player (game mode 1: [0xfcab34] +0x3c):
 * in a network game's client mode the player's update takes the network's side of many checks (a move comes from
 * the server, the player waits for its mark, the motion is interpolated), and the copy, which the server does not
 * know, did not move with pad 2 (run 20261027-010554). The mode is put back at the end of the swap. TS_SPLIT_COPYMODE=1
 * turns it on: it made no clear difference in run 20261027-012413 (the copy moved about 1.3 m instead of walking), so it is off. */
static int copy_mode1, saved_mode = -1;

static void p2_enter(void)
{
    if (as2.depth++)
        return;
    if (client_split && copy_mode1) {
        BYTE *gm = *(BYTE **)0xfcab34;
        if (gm && *(int *)(gm + 0x3c) == 3)
            saved_mode = 3, *(int *)(gm + 0x3c) = 1;
    }
    geo_owner_p2(1);
    BYTE *im = IM;
    as2.pa = PLAYER_A, as2.pb = PLAYER_B, as2.cam = CAM;
    as2.ctx1 = im ? *(BYTE **)(im + 0x40) : NULL;
    as2.with_ctx = im && ctx2 && as2.ctx1 && *(DWORD *)as2.ctx1 == VT_GAME;
    PLAYER_A = PLAYER_B = p2;
    CAM = cam2;
    if (as2.with_ctx) {
        memcpy(as2.a1, im + IMF_A, sizeof as2.a1);
        memcpy(as2.b1, im + IMF_B, sizeof as2.b1);
        memcpy(im + IMF_A, imf2_a, sizeof as2.a1);
        memcpy(im + IMF_B, imf2_b, sizeof as2.b1);
        *(BYTE **)(im + 0x40) = ctx2;
    }
}

static int p2_depth(void)
{
    return as2.depth;
}

static void p2_leave(void)
{
    if (--as2.depth)
        return;
    if (saved_mode >= 0) {
        BYTE *gm = *(BYTE **)0xfcab34;
        if (gm)
            *(int *)(gm + 0x3c) = saved_mode;
        saved_mode = -1;
    }
    geo_owner_p2(0);
    BYTE *im = IM;
    if (as2.with_ctx && im) {
        *(BYTE **)(im + 0x40) = as2.ctx1;
        memcpy(imf2_a, im + IMF_A, sizeof as2.a1);
        memcpy(imf2_b, im + IMF_B, sizeof as2.b1);
        memcpy(im + IMF_A, as2.a1, sizeof as2.a1);
        memcpy(im + IMF_B, as2.b1, sizeof as2.b1);
    }
    PLAYER_A = as2.pa;
    PLAYER_B = as2.pb;
    CAM = as2.cam;
}

/* Player 2's own objects (first-person weapons, hands, holster), recorded at its spawn: their updates and
 * messages run as player 2 too (the weapons check the local player while they animate and fire). */

static int is_p2_object(BYTE *o)
{
    if (!p2 || !o)
        return 0;
    if (o == p2)
        return 1;
    for (int i = 0; i < n_own2; i++)
        if (own2[i] == o)
            return 1;
    return 0;
}

static void TSCALL hook_update(void *pl)
{
    if (!p2 || pl != p2 || !IM) {
        real_update(pl);
        return;
    }
    if (client_split)
        copy_updates++;
    p2_enter();
    real_update(pl);
    p2_leave();
}

/* The task scheduler's message dispatch (stdcall 0x946e40(task, msg, a, b), ret 0x10): every update and message
 * of player 2 and its objects runs inside the swap. Entered through a jump at its start; the trampoline holds the
 * displaced first 10 bytes. */
typedef int (__stdcall *dispatch_t)(BYTE *, int, int, int);
static dispatch_t dispatch_tramp;

static int msglog, p1_depth, allow_ff, p2_removing;
static int in_pickup;
int split_msg_owner(void)
{
    return as2.depth ? 2 : in_pickup ? in_pickup : p1_depth ? 1 : 0;
}
static BYTE *(TSCALL *find_task)(void *, int) = (void *)0x947c10;   /* scheduler: object by id */

static BYTE *p1_of(void)
{
    BYTE *p = as2.depth ? as2.pb : PLAYER_B;
    return p != p2 ? p : NULL;
}

static int is_p1_object(BYTE *o)
{
    BYTE *p1 = p1_of();
    if (!o || !p1)
        return 0;
    if (o == p1)
        return 1;
    for (int i = 0; i < n_own1; i++)
        if (own1[i] == o)
            return 1;
    return 0;
}

static int __stdcall hook_dispatch(BYTE *task, int msg, int a, int b)
{
    /* A level change: the host keeps its player 2 (a game object, as player 1), the clients load the new level
     * and get it again from the join list. Their player_mp of it (no owning client, +0x30 = -2) outlived the old
     * level, and the new level's player table pass (message 0x434, 0x5f78e0) looked up its model, missed and
     * crashed (0x5f7985, runs 20261026-201715, -204537). A client removes it itself at the end of the level.
     * (Removing player 2 on the host at the level end instead left it half removed: the host crashed, run
     * 20261026-205550.) */
    if (msg == 0x3f8 && task && *(DWORD *)task == 0xd74c28 && *(int *)(task + 0x30) == -2 && net_mode() == 3) {
        int r = dispatch_tramp(task, msg, a, b);
        DWORD fl = *(DWORD *)0xfcace8;
        *(DWORD *)0xfcace8 = fl | 4;
        dispatch_tramp(task, 2, 0, 0);
        *(DWORD *)0xfcace8 = (*(DWORD *)0xfcace8 & ~4u) | (fl & 4u);
        if (task == cp2)
            cp2 = NULL;
        ts_log("split: the host's player 2 (player_mp %p, net id %d) removed here at the level end", (void *)task, *(int *)(task + 0x18));
        return r;
    }
    if (msg == 1000 && n_p2_gone)
        for (int i = 0; i < n_p2_gone; i++)
            if (task == p2_gone[i].o && *(DWORD *)task == p2_gone[i].vt) {
                static int n;
                if (n++ < 5)
                    ts_log("split: update of removed player 2's object %p (vtable %08lx) skipped", (void *)task, p2_gone[i].vt);
                return 0;
            }
    if (msg == 1000 && (n_crs_gone || crs_scan)) {
        for (int i = 0; i < n_crs_gone; i++)
            if (task == crs_gone[i])
                return 0;   /* (a crosshair object of a removed player 2 that was not removed: not updated) */
        if (crs_scan && p2 && (int)(GetTickCount() - crs_scan) < 0 && !IsBadReadPtr(task, 0x234)) {
            BYTE *nd = *(BYTE **)(task + 0x230);
            for (int i = 0; nd && i < n_crs[1]; i++)
                if (crs[1][i].node == nd) {
                    int k = 0;
                    while (k < n_crs_obj && crs_obj[k].o != task)
                        k++;
                    if (k == n_crs_obj && n_crs_obj < MAX_CRS) {
                        crs_obj[n_crs_obj].o = task, crs_obj[n_crs_obj++].vt = *(DWORD *)task;
                        ts_log("split: player 2's crosshair object %p (vtable %08lx)", (void *)task, *(DWORD *)task);
                    }
                    break;
                }
        }
    }
    if (msglog && p2 && msg != 0x3e8 && msg != 0x3e9 && (task == p2 || task == PLAYER_B || is_p2_object(task))) {
        static int n;
        if (msg != 3 && msg != 0x415 && n++ < 400)
            ts_log("split: msg %#x to %s %p a=%08x b=%08x depth p2 %d p1 %d", msg, task == p2 ? "p2" : task == PLAYER_B ? "p1" : "p2obj",
                   (void *)task, a, b, as2.depth, p1_depth);
        if (msg == 0x4e3 && a && !IsBadReadPtr((void *)a, 64)) {
            DWORD *d = (DWORD *)a;
            ts_log("split:   %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", d[0],
                   d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        }
    }
    /* Friendly fire: message 0x4e3 is a hit on a player, delivered after the shot (outside the shooter's update);
     * its record (a) holds the shooter's scheduler id at +8 (0x947c10 finds the object). A hit by player 2 or its
     * weapons on player 1, or by player 1's on player 2, goes nowhere. */
    if (msg == 0x4e3 && p2 && a && splitlog && task != p2 && task != p1_of() && !IsBadReadPtr((void *)a, 12)) {
        BYTE *by = find_task((void *)0xecb7f8, ((int *)a)[2]);
        static int n;
        if (is_p2_object(by) && n++ < 30)   /* (tests: player 2's hits on others, with their net ids) */
            ts_log("split: player 2 hit %p (net id %d)", (void *)task, *(int *)(task + 0x18));
    }
    /* On a client the local copy of player 2 (client_split) shoots pictures only: its hits go nowhere (the host's
     * player 2 fires the same shots with authority; their damage comes from the host). */
    if (msg == 0x4e3 && client_split && p2 && task == p2) {   /* (a hit on the copy: its life comes from the host) */
        static int n;
        if (splitlog && n++ < 20)
            ts_log("split: a hit on our player 2's copy dropped");
        return 1;
    }
    if (msg == 0x4e3 && client_split && p2 && a && task != p2 && !IsBadReadPtr((void *)a, 12)
        && is_p2_object(find_task((void *)0xecb7f8, ((int *)a)[2]))) {
        static int n;
        if (splitlog && n++ < 20)
            ts_log("split: a hit of our player 2's copy on %p dropped (the host's player 2 hits)", (void *)task);
        return 1;
    }
    if (msg == 0x4e3 && p2 && a && (task == p2 || task == p1_of()) && !IsBadReadPtr((void *)a, 12)) {
        BYTE *by = find_task((void *)0xecb7f8, ((int *)a)[2]);
        if (!allow_ff && ((task == p2 && is_p1_object(by)) || (task != p2 && is_p2_object(by)))) {
            static int n;
            if (n++ < 8)
                ts_log("split: friendly hit on player %d by %p dropped", task == p2 ? 2 : 1, (void *)by);
            return 1;
        }
    }
    if (!is_p2_object(task)) {
        int mine = p2 && is_p1_object(task);
        p1_depth += mine;
        static BYTE before[0x620];
        int watch = msglog && msg == 0x4e3 && p2 && task == p1_of();   /* (tests: what a hit changes in player 1) */
        if (watch)
            memcpy(before, task, sizeof before);
        int r = dispatch_tramp(task, msg, a, b);
        if (watch) {
            static int n;
            for (int off = 0; off < 0x620 && n < 300; off += 4)
                if (*(DWORD *)(before + off) != *(DWORD *)(task + off)) {
                    n++;
                    ts_log("split: hit changed p1+%#x %08lx -> %08lx (%.3f -> %.3f)", off, *(DWORD *)(before + off),
                           *(DWORD *)(task + off), *(float *)(before + off), *(float *)(task + off));
                }
        }
        p1_depth -= mine;
        return r;
    }
    /* Removing player 2 (split screen turned off): the player's removal (message 2, vtable +0x144 = 0x4082f0)
     * stops the game on purpose unless bit 2 of the game flags (0xfcace8) is set, which single player never sets
     * (a player there lives as long as the game). For player 2's own removal the bit is set for the call. */
    int removing = task == p2 && msg == 2 && p2_removing;
    DWORD flags = *(DWORD *)0xfcace8;
    if (removing)
        *(DWORD *)0xfcace8 = flags | 4;
    p2_enter();
    int r = dispatch_tramp(task, msg, a, b);
    p2_leave();
    if (removing) {
        *(DWORD *)0xfcace8 = (*(DWORD *)0xfcace8 & ~4u) | (flags & 4u);
        ts_log("split: player 2 removal message handled");
    }
    return r;
}

static int install_dispatch_hook(void)
{
    static const unsigned char head[10] = {0x56, 0x8b, 0x74, 0x24, 0x08, 0x57, 0x8b, 0x7c, 0x24, 0x10};
    unsigned char *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t || memcmp((void *)0x946e40, head, 10) != 0)
        return 0;
    memcpy(t, head, 10);
    t[10] = 0xe9;
    *(DWORD *)(t + 11) = 0x946e4a - ((DWORD)t + 15);
    dispatch_tramp = (dispatch_t)t;
    unsigned char jmp[5] = {0xe9};
    *(DWORD *)(jmp + 1) = (DWORD)hook_dispatch - (0x946e40 + 5);
    return ts_patch_bytes(0x946e40, head, jmp, 5);
}

/* Player 2's death (vtable +0x2e8, 0x410cc0(this, int), ret 4; also reached from the script's Kill) would run the
 * single-player death: the "you died" screen and the restart for both players. Player 2 respawns next to player 1
 * instead, with its damage (+0x468) cleared. */
typedef void (TSCALL *die_t)(void *, int);
static die_t real_die;

#define HEALTH 0x234   /* the player's life, 100 at full; a rifle hit took 3 (measured: TS_SPLIT_MSGLOG, run 20261025-103150) */
static DWORD died_at[2];

static void TSCALL hook_die(void *pl, int arg)
{
    BYTE *p1 = p1_of();
    int who = pl == p2 ? 1 : pl == p1 ? 0 : -1;
    if (!p2 || who < 0) {
        real_die(pl, arg);
        return;
    }
    /* In a network game (co-op server) player 1 dies by the multiplayer rules (death screen, respawn timer, the
     * session's respawn points), as the clients do; player 2, which those rules do not know, is always respawned
     * next to player 1 (the game's death would leave it a dead object without HUD). */
    if (client_split && who == 1) {   /* (the client's copy: the host's player 2 dies and respawns, the copy follows) */
        *(float *)((BYTE *)pl + HEALTH) = 100.0f;
        ts_log("split: our player 2's copy would die: kept alive (the host's player 2 decides)");
        return;
    }
    BYTE *gm = *(BYTE **)0xfcab34;
    int net = gm && (*(int *)(gm + 0x3c) == 2 || *(int *)(gm + 0x3c) == 3);
    DWORD now = GetTickCount();
    if (net && who == 0) {
        died_at[0] = now;
        ts_log("split: player 1 died in a network game: the multiplayer death and respawn");
        real_die(pl, arg);
        return;
    }
    /* Down together (the partner died less than 5 s ago): the game's own death, the restart for both. */
    if (!net && died_at[!who] && now - died_at[!who] < 5000) {
        ts_log("split: player %d died while player %d was down: the game's death", who + 1, 2 - who);
        real_die(pl, arg);
        return;
    }
    died_at[who] = now;
    /* The script's Kill starts the dying first: vtable +0x370 sets +0x39c 0x1000, +0x3a4 (1, ...) sets 0x400000
     * and starts the death screen effect (0x43cb70: a renderer effect and flags in the globals [0xfcabac] +0x28,
     * [0x109649c] +0x28, and [0x109842c] vtable +0x20). The player's own counterparts end it: +0x3a4 (0, 0) and
     * +0x36c, called as that player. */
    {
        void **vt = *(void ***)pl;
        if (who)
            p2_enter();
        ((void (TSCALL *)(void *, int, int))vt[0x3a4 / 4])(pl, 0, 0);
        ((void (TSCALL *)(void *))vt[0x36c / 4])(pl);
        if (who)
            p2_leave();
    }
    *(DWORD *)((BYTE *)pl + 0x39c) &= ~(0x1000u | 0x400000u);
    *(float *)((BYTE *)pl + HEALTH) = 100.0f;
    *(float *)((BYTE *)pl + 0x468) = 0;
    if (who && client_split)
        ;   /* (our player 2's copy: the host's player 2 dies and respawns, the copy follows it, see shadow_sync) */
    else if (who && remote_host && remote_mp && *(BYTE **)(remote_mp + 0x230))
        place_player(pl, remote_mp);
    else
        place_player(pl, who ? p1 : p2);
    ts_log("split: player %d died: respawned next to player %d", who + 1, 2 - who);
}

/* The player's event handler (vtable +0x22c, 0x40b120(this, event*), ret 4; a switch on the event code, word +4):
 * hits on player 2 arrive here (player 1's come as message 0x4e3). TS_SPLIT_MSGLOG logs player 2's events. */
typedef void (TSCALL *event_t)(void *, BYTE *);
static event_t real_event;

static void TSCALL hook_event(void *pl, BYTE *ev)
{
    if (msglog && p2 && pl == p2 && ev && !IsBadReadPtr(ev, 32)) {
        static int n;
        DWORD *d = (DWORD *)ev;
        if (n++ < 200)
            ts_log("split: p2 event %d: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", *(short *)(ev + 4), d[0], d[1], d[2],
                   d[3], d[4], d[5], d[6], d[7]);
    }
    /* Friendly fire on player 2: a hit is event -98 with the shooter's scheduler id at +8. */
    if (p2 && pl == p2 && ev && *(short *)(ev + 4) == -98 && is_p1_object(find_task((void *)0xecb7f8, *(int *)(ev + 8)))) {
        static int n;
        if (n++ < 8)
            ts_log("split: friendly hit on player 2 dropped");
        return;
    }
    real_event(pl, ev);
}

/* The HUD message line (the widget at [0x1095ec8], its add method vtable +0xc: "Received: pistol", "ammo
 * full", ...) is one for both halves. A message added for one player (inside player 2's swap, inside player 1's
 * own objects, or during a pick-up by either) belongs to that player; the line is hidden in the other player's
 * half while it shows (the message box's timeout 3.5 s plus fading, here 6 s after the last such message).
 * Messages from elsewhere (level scripts) stay in both halves. */
static void (TSCALL *real_msg_add)(void *, void *, void *, DWORD);
static int msg_owner;          /* 0 shared, 1/2 player */
static int msg_next = 1;       /* (TS_SPLIT_MSGNEXT=0, tests: hide for the view being drawn instead of the next) */
static DWORD msg_time;
static BYTE *msg_hidden;
static DWORD msg_hidden_flags;

static int msg_forwarded;   /* (a client showing a message of its player 2 from the host) */
static int ns_msg_forward(const void *text);

static void TSCALL hook_msg_add(void *w, void *text, void *arg, DWORD t)
{
    if (w == *(void **)0x1095ec8 && p2 && !msg_forwarded) {
        int o = split_msg_owner();
        msg_owner = o;
        msg_time = GetTickCount();
        static int n;
        if (splitlog && n++ < 40)
            ts_log("split: HUD message for %s", o ? (o == 2 ? "player 2" : "player 1") : "both");
        if (o == 2 && remote_host) {   /* (a client's player 2: shown on that client, not here) */
            ns_msg_forward(text);
            return;
        }
        if (o == 2 && client_split)
            return;   /* (our player 2's copy: the host's real one sends its messages) */
    }
    real_msg_add(w, text, arg, t);
}

/* The message boxes (hintBox widgets "MessageBox"/"MessageBoxBig", class 0xda34f8, updated per view like the other
 * HUD widgets) are hidden after their update for the view of the other player while a player's own message
 * shows. */
static const char *elem_name(BYTE *e)
{
    BYTE *d = e ? *(BYTE **)(e + 8) : NULL, *n = d && !IsBadReadPtr(d, 0x14) ? *(BYTE **)(d + 0x10) : NULL;
    return n && !IsBadReadPtr(n, 0x20) ? (const char *)(n + 0xc) : "";
}

static void msg_box(BYTE *w, int v)
{
    static BYTE *hid[4];
    int k = 0;
    while (k < 4 && hid[k] && hid[k] != w)
        k++;
    if (k == 4)   /* (a free slot) */
        for (k = 0; k < 4 && hid[k]; k++)
            ;
    int hide = v >= 0 && msg_owner && msg_owner != v + 1 && GetTickCount() - msg_time < 10000;
    static int nl;
    if (splitlog && hide && nl++ < 10)
        ts_log("split: message box %p hidden for view %d (flags %08lx)", (void *)w, v, *(DWORD *)(w + 0x18));
    if (hide) {
        if (k < 4 && !hid[k] && !(*(DWORD *)(w + 0x18) & 2))
            hid[k] = w, *(DWORD *)(w + 0x18) |= 2;
        else if (k < 4 && hid[k] == w)
            *(DWORD *)(w + 0x18) |= 2;
    } else if (k < 4 && hid[k] == w) {
        *(DWORD *)(w + 0x18) &= ~2u;
        hid[k] = NULL;
    }
}

/* (after the HUD frame's update, class 0xda34f8, which updates its children: the message boxes among them) */
__attribute__((unused)) static void msg_after_update(BYTE *hud, int v)
{
    static int listed;
    for (BYTE *c = *(BYTE **)(hud + 0x44); c && !IsBadReadPtr(c, 0x4c); c = *(BYTE **)(c + 0x48)) {
        if (splitlog && !listed)
            ts_log("split: HUD child %p %s \"%s\" flags %08lx", (void *)c, elem_name(c),
                   *(EStr **)(c + 0x24) && !IsBadReadPtr(*(EStr **)(c + 0x24), 0x18) ? (*(EStr **)(c + 0x24))->s : "?", *(DWORD *)(c + 0x18));
        EStr *nm = *(EStr **)(c + 0x24);   /* (the element's name; elem_name gives its type) */
        if (nm && !IsBadReadPtr(nm, 0x18) && (strncmp(nm->s, "MessageBox", 10) == 0 || strcmp(nm->s, "MessageText") == 0))
            msg_box(c, v);
    }
    listed = 1;
}

/* The message lines are drawn by the HUD's console element "ConsoleText" (class 0xd9f300; the text draw
 * 0x706430 in slot +0x28, as for text elements, vtable 0xdafa10): its draw is skipped in the view of the other player while a player's own message shows. */
static void (TSCALL *real_text_draw)(void *, void *);
static int msg_hide_view(int v)
{
    return v >= 0 && p2 && msg_owner && msg_owner != v + 1 && GetTickCount() - msg_time < 10000;
}

static void TSCALL hook_text_draw(BYTE *e, void *arg)
{
    if (splitlog > 2 && view == 1) {   /* (tests: the names of the text elements drawn in player 2's view, once each) */
        static BYTE *seen[64];
        static int ns;
        int k = 0;
        while (k < ns && seen[k] != e)
            k++;
        if (k == ns && ns < 64) {
            seen[ns++] = e;
            EStr *nm = *(EStr **)(e + 0x24);
            BYTE *par = *(BYTE **)(e + 0x40);
            EStr *pn = par ? *(EStr **)(par + 0x24) : NULL;
            ts_log("split: view 1 draws text %p \"%s\" (parent \"%s\", flags %08lx)", (void *)e,
                   nm && !IsBadReadPtr(nm, 0x18) ? nm->s : "?", pn && !IsBadReadPtr(pn, 0x18) ? pn->s : "?", *(DWORD *)(e + 0x18));
        }
    }
    /* (the multiplayer texts, "mp*": player 1's death and respawn countdown, the server's warm-up; player 1's
     * alone in a network game, not drawn in player 2's half) */
    if (view == 1 && p2) {
        EStr *nm = *(EStr **)(e + 0x24);
        if (nm && !IsBadReadPtr(nm, 0x18) && nm->s[0] == 'm' && nm->s[1] == 'p')
            return;
    }
    if (msg_owner && view >= 0 && msg_hide_view(view)) {
        EStr *nm = *(EStr **)(e + 0x24);
        static int nn;
        if (splitlog > 1 && nn++ < 60) {
            BYTE *par = *(BYTE **)(e + 0x40);
            EStr *pn = par ? *(EStr **)(par + 0x24) : NULL;
            ts_log("split: text draw %p \"%s\" (parent \"%s\") in view %d", (void *)e, nm && !IsBadReadPtr(nm, 0x18) ? nm->s : "?",
                   pn && !IsBadReadPtr(pn, 0x18) ? pn->s : "?", view);
        }
        /* (the pick-up lines go to the HUD's console lines, "ConsoleText", class 0xd9f300) */
        if (nm && !IsBadReadPtr(nm, 0x18) && (strcmp(nm->s, "ConsoleText") == 0 || strcmp(nm->s, "MessageText") == 0)) {
            static int n;
            if (splitlog && n++ < 6)
                ts_log("split: message text not drawn in view %d", view);
            return;
        }
    }
    real_text_draw(e, arg);
}

/* Prompts that name a command's buttons ("Нажмите {PARAM}" with a command name: the respawn prompt, use hints):
 * a HUD element's naming method (vtable +0x110 = 0x709bf0, thiscall (element, int *count, int *codes, x)) names
 * the first one or two codes of the command's bindings, the keyboard/mouse ones first. When the player whose half
 * is drawn last used a pad, the pad codes (0x108-0x153) are put first, so the prompt names the pad button. */
int gamepad_pad_active(int player);   /* gamepad.c */
static void (TSCALL *real_name_codes)(void *, int *, int *, void *);
static void TSCALL hook_name_codes(void *e, int *count, int *codes, void *x)
{
    int v = view >= 0 && p2 ? view : 0;
    if (splitlog) {
        static int nl;
        if (nl++ < 20)
            ts_log("split: prompt naming from %p: count %d codes %x %x %x, x %p, pad %d", __builtin_return_address(0), count ? *count : -1,
                   count && codes && *count > 0 ? codes[0] : 0, count && codes && *count > 1 ? codes[1] : 0,
                   count && codes && *count > 2 ? codes[2] : 0, x, gamepad_pad_active(v));
    }
    /* (the prompt passes the command's first binding only (0x707e30 copies slot 0 of the command's entry in the
     * GAME context): the entry is found by that binding and its pad slot (slot 3 or 4, gamepad.c's) named instead) */
    BYTE *im = *(BYTE **)0xfcabb4;
    if (count && codes && *count >= 1 && *count <= 3 && !(codes[0] >= 0x108 && codes[0] < 0x154) && im && gamepad_pad_active(v)) {
        typedef struct { float value; struct { int n; int code[3]; } slot[5]; char unknown[0x10]; unsigned flags; } cmd_e;
        BYTE *ctx = ((BYTE *(TSCALL *)(void *, const char *))(*(void ***)im)[0x38 / 4])(im, "GAME");
        cmd_e *e = ctx ? *(cmd_e **)(ctx + 4) : NULL;
        int first = ctx ? *(int *)(ctx + 8) : 0, end = ctx ? *(int *)(ctx + 12) : 0;
        for (int i = 0; e && i < end - first && i < 512; i++) {
            if (e[i].slot[0].n != *count || memcmp(e[i].slot[0].code, codes, *count * 4))
                continue;
            for (int k = 3; k < 5; k++)
                if (e[i].slot[k].n >= 1 && e[i].slot[k].n <= 3 && e[i].slot[k].code[0] >= 0x108 && e[i].slot[k].code[0] < 0x154) {
                    static int nl;
                    if (splitlog && nl++ < 10)
                        ts_log("split: prompt in view %d names pad code %#x instead of %#x", v, e[i].slot[k].code[0], codes[0]);
                    *count = e[i].slot[k].n;
                    memcpy(codes, e[i].slot[k].code, *count * 4);
                    i = end;
                    break;
                }
        }
    }
    if (count && codes && *count > 1 && *count <= 8 && !IsBadReadPtr(codes, *count * 4) && gamepad_pad_active(v)) {
        int tmp[8], k = 0;
        for (int i = 0; i < *count; i++)
            if (codes[i] >= 0x108 && codes[i] < 0x154)
                tmp[k++] = codes[i];
        int pads = k;
        for (int i = 0; i < *count; i++)
            if (!(codes[i] >= 0x108 && codes[i] < 0x154))
                tmp[k++] = codes[i];
        if (pads && pads < *count) {
            static int n;
            if (splitlog && n++ < 10)
                ts_log("split: prompt in view %d names the pad button first (code %#x before %#x)", v, tmp[0], codes[0]);
            memcpy(codes, tmp, *count * 4);
        }
    }
    real_name_codes(e, count, codes, x);
}

static void install_name_hook(void)
{
    {   /* (the element classes that name a command's buttons, see hook_name_codes) */
        static const DWORD nv[] = {0xd9f2e8, 0xd9f410, 0xda0498, 0xda0870, 0xda0be8, 0xdafb20, 0xdb0578, 0xdb0968,
                                   0xdb2b58, 0xdb2c80, 0xdb2da8, 0xdb2ed0, 0xdb33b8, 0xdb3620, 0xdb3768};
        int nn = 0;
        for (int i = 0; i < (int)(sizeof nv / sizeof nv[0]); i++)
            if (*(DWORD *)nv[i] == 0x709bf0) {
                void *vt = (void *)nv[i];
                void *t = ts_patch_vtable(&vt, 0, (void *)hook_name_codes);
                if (t)
                    real_name_codes = t, nn++;
            }
        ts_log("split: %d prompt naming slots hooked (pad buttons named for pad players)", nn);
    }
}

static void install_msg_hook(void)
{
    static int done;
    BYTE *w = *(BYTE **)0x1095ec8;
    if (done || !w || IsBadReadPtr(w, 4))
        return;
    done = 1;
    static const DWORD vts[2] = {0xd9f300, 0xdafa10};
    for (int i = 0; i < 2; i++)
        if (*(DWORD *)(vts[i] + 0x28) == 0x706430) {
            void *vt = (void *)(vts[i] + 0x28);
            void *t = ts_patch_vtable(&vt, 0, (void *)hook_text_draw);
            if (t)
                real_text_draw = t;
        }
    void *o = ts_patch_vtable(w, 0xc / 4, (void *)hook_msg_add);
    if (o)
        real_msg_add = o;
    ts_log("split: HUD message line %p (vtable %08lx): %s", (void *)w, *(DWORD *)w, o ? "per player" : "not hooked");
}

/* Pick-ups: the item calls the toucher's vtable +0x17c (0x420470: the item's class checked, +0x2a4 decides,
 * +0x284 takes it). Logged with the player it went to. */
static int (TSCALL *real_pickup)(void *, void *);

static int TSCALL hook_pickup(void *pl, void *item)
{
    int was = in_pickup;
    in_pickup = pl == p2 ? 2 : 1;
    int r = real_pickup(pl, item);
    in_pickup = was;
    static int n;
    if (n++ < 200) {
        ts_log("split: pick-up %p by player %d (%p): %s", item, pl == p2 ? 2 : 1, pl, r ? "taken" : "not taken");
        if (splitlog > 2 && n < 4) {   /* (tests: the callers, code addresses on the stack) */
            DWORD *sp = (DWORD *)__builtin_frame_address(0);
            for (int k = 0; k < 64; k++)
                if (sp[k] > 0x401000 && sp[k] < 0xd11000)
                    ts_log("split:   pick-up stack +%#x %08lx", k * 4, sp[k]);
        }
    }
    return r;
}

/* The items' give method (0x8ad570, thiscall (item, toucher, x): item +0x208 check, toucher +0x17c take, item
 * +0x204 done), in the item classes' vtables. Tests: who calls it, for whom. */
static int (TSCALL *real_give)(void *, void *, int);
static int TSCALL hook_give(void *item, void *who, int x)
{
    int r = real_give(item, who, x);
    static int n;
    if (splitlog && n++ < 60) {
        ts_log("split: item %p give to %s %p (%d): %d, called from %p", item, who == p2 ? "player 2" : who == PLAYER_B ? "player 1" : "?",
               who, x, r, __builtin_return_address(0));
        if (splitlog > 2 && n < 6) {
            DWORD *sp = (DWORD *)__builtin_frame_address(0);
            for (int k = 0; k < 64; k++)
                if (sp[k] > 0x401000 && sp[k] < 0xd11000)
                    ts_log("split:   give stack +%#x %08lx", k * 4, sp[k]);
        }
    }
    return r;
}

/* Pick-ups for player 2. An item's update (0x4f5aa0, item vtable +0x13c) gives itself to the players of the
 * game's player list ([0xfcab34] +0x28, 0xa4-byte records, +4 the player: the network players' list, which in
 * single player holds player 1 only): for each, vtable +0x16c (0: in reach) and then +0x20c (0x8ad570: check,
 * the player's take +0x17c, done). Player 2 is not in that list, so no item ever reached it. After the item's
 * own update, the same is done for player 2 (as player 2), on the conditions of that loop (single player, the
 * item's node shown, item +0x270 bit 1 clear). */
static void (TSCALL *real_item_update)(void *);
static int host_items_off;   /* TS_SPLIT_HOSTITEMS=0: player 2 takes no items on a server */
/* A network game's item filter (item vtable +0x248, 0x4f5240) looks the taker up in the game's player table
 * ([0xfcab18] +0xfc, 0x84-byte records, +0x40 a handle of the player, 0x5fb6f0) and refuses a player without a
 * record (an empty record: "no player", 0x428f30). Player 2 has none (the table is the network players'), so a
 * server's items refused it. For player 2's take the handle of player 1's record names player 2 (the checks then
 * read player 2: alive, grenade count, energy) and is put back right after. Returns the handle, or NULL. */
static BYTE **p1_record_handle(void)
{
    BYTE *t = *(BYTE **)0xfcab18, *p1 = PLAYER_B != p2 ? PLAYER_B : (as2.depth ? as2.pb : NULL);
    if (!t || !p1 || IsBadReadPtr(t, 0x104))
        return NULL;
    BYTE *rec = *(BYTE **)(t + 0xfc);
    int n = *(int *)(t + 0x100);
    for (int i = 0; rec && i < n && i < 64; i++) {
        BYTE **h = *(BYTE ***)(rec + i * 0x84 + 0x40);
        if (h && !IsBadReadPtr(h, 4) && *h == p1)
            return h;
    }
    return NULL;
}

static BYTE *upd_items[64];   /* (tests: items seen updating, for TS_SPLIT_ITEMTEST=grab) */
static int n_upd_items;

static void TSCALL hook_item_update(BYTE *item)
{
    real_item_update(item);
    if (n_upd_items < 64) {
        int k = 0;
        while (k < n_upd_items && upd_items[k] != item)
            k++;
        if (k == n_upd_items)
            upd_items[n_upd_items++] = item;
    }
    BYTE *gm = *(BYTE **)0xfcab34, *node = *(BYTE **)(item + 0x230);
    int md = gm ? *(int *)(gm + 0x3c) : 0, kind = *(int *)(item + 0x14);
    /* (a server: the loop's network branch, items it has authority over (+0x2c bit 4) that are not of the kinds
     * 1-10 taken with the use key; a client never gives items) */
    int host_item = md == 2 && (*(DWORD *)(item + 0x2c) & 4) && !(kind >= 1 && kind <= 10);
    if (stage < 3 || !p2 || !gm || !(md == 1 || (host_item && !host_items_off)) || !node || (*(DWORD *)(node + 4) & 3)
        || (*(DWORD *)(item + 0x270) & 2) || as2.depth || *(DWORD *)p2 != PLAYER_VTBL)
        return;
    void **vt = *(void ***)item;
    if (((int (TSCALL *)(void *, void *))vt[0x16c / 4])(item, p2))
        return;   /* (not in reach) */
    BYTE **h = md == 2 ? p1_record_handle() : NULL, *h_was = h ? *h : NULL;
    if (h)
        *h = p2;
    p2_enter();
    int r = ((int (TSCALL *)(void *, void *, int))vt[0x20c / 4])(item, p2, 1);
    p2_leave();
    if (h)
        *h = h_was;
    static BYTE *last;   /* (logged once per item in a row) */
    if (r || item != last)
        ts_log("split: item %p (\"%.32s\", kind %d, game mode %d) in player 2's reach: %s", (void *)item,
               node && !IsBadReadPtr(*(char **)(node + 0x28), 1) ? *(char **)(node + 0x28) : "?", kind, md,
               r ? "taken by player 2" : "not taken");
    last = r ? NULL : item;
}

static int install_item_hooks(void)
{
    int n = 0;
    static const DWORD slots[2] = {0xd8c474, 0xd8c974};
    for (int i = 0; i < 2; i++)
        if (*(DWORD *)slots[i] == 0x4f5aa0) {
            void *vt = (void *)slots[i];
            void *o = ts_patch_vtable(&vt, 0, (void *)hook_item_update);
            if (o)
                real_item_update = o, n++;
        }
    static const DWORD calls[2] = {0x4f7463, 0x4f7813};   /* (subclass updates calling it) */
    for (int i = 0; i < 2 && real_item_update; i++) {
        unsigned char want[5] = {0xe8}, rep[5] = {0xe8};
        *(DWORD *)(want + 1) = 0x4f5aa0 - (calls[i] + 5);
        *(DWORD *)(rep + 1) = (DWORD)hook_item_update - (calls[i] + 5);
        n += !!ts_patch_bytes(calls[i], want, rep, 5);
    }
    return n;
}

static int install_give_hooks(void)
{
    static const DWORD slots[] = {0xd8c2f4, 0xd8c544, 0xd8c7d4, 0xd8ca44, 0xdc52fc};
    int n = 0;
    for (int i = 0; i < 5; i++) {
        if (*(DWORD *)slots[i] != 0x8ad570)
            continue;
        void *vt = (void *)slots[i];
        void *o = ts_patch_vtable(&vt, 0, (void *)hook_give);
        if (o)
            real_give = o, n++;
    }
    return n;
}

/* Tests (TS_SPLITLOG on a network client or host): the remote players (player_mp, vtable 0xd74c28, update slot
 * +0x13c = 0x42fa90) logged every 5 s each: net id, owner (+0x30), position, life, weapon slot. */
static void (TSCALL *real_mp_update)(void *);
static void TSCALL hook_mp_update(BYTE *o)
{
    real_mp_update(o);
    static struct { BYTE *o; DWORD t; } seen[8];
    DWORD now = GetTickCount();
    int k = 0;
    while (k < 8 && seen[k].o && seen[k].o != o)
        k++;
    if (k == 8)
        k = 0;
    if (seen[k].o == o && now - seen[k].t < 5000)
        return;
    seen[k].o = o, seen[k].t = now;
    BYTE *n = *(BYTE **)(o + 0x230);
    /* (tests on a client, TS_SPLIT_WATCH=1: the local player is put 4 m from the host's player 2 (the remote player
     * without an owner, -2) facing it, every 5 s, so the client's frames show it) */
    static int watch = -1;
    char wv[8];
    if (watch < 0)
        watch = ts_env("TS_SPLIT_WATCH", wv, sizeof wv) && wv[0] == '1';
    BYTE *me = PLAYER_B, *mn = me ? *(BYTE **)(me + 0x230) : NULL;
    if (watch && *(int *)(o + 0x30) == -2 && n && mn && *(DWORD *)me == PLAYER_VTBL) {
        float *q = (float *)(n + 0x68), pos[3] = {q[0] + 4.0f, q[1] + 0.3f, q[2]}, dir[3] = {-1, 0, 0};
        ((place_t)(*(void ***)me)[SLOT_PLACE])(me, pos, dir, 1);
        ts_log("split: watching the host's player 2 from 4 m");
    }
    float *p = n && !IsBadReadPtr(n, 0x74) ? (float *)(n + 0x68) : NULL;
    ts_log("split: remote player %p net id %d owner %d at %.2f %.2f %.2f life %.1f flags %08lx", (void *)o, *(int *)(o + 0x18),
           *(int *)(o + 0x30), p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, *(float *)(o + 0x234), *(DWORD *)(o + 0x39c));
}

/* The destructor clears both player pointers: player 2's must leave them on player 1. */
/* Removing player 2 in a network game (a client's player 2 ended on the host, run 20261025-213310) crashed the
 * next frame in a light's pass over the scene instances it lights (0x98b820): an instance of a removed node, still
 * in the list for that frame, has no node any more (+0xbc = 0) and was read at 0x98b840. Such an instance is
 * skipped (as one with +4 bit 0x4000 is). */
__attribute__((naked)) static void light_inst_guard(void)
{
    __asm__("mov 0xbc(%ecx), %esi\n\t"
            "test %esi, %esi\n\t"
            "jz 1f\n\t"
            "testl $0x4000, 4(%esi)\n\t"
            "jne 1f\n\t"
            "push $0x98b84d\n\t"
            "ret\n"
            "1:\n\t"
            "push $0x98b8e6\n\t"
            "ret");
}

static int install_light_guard(void)
{
    static const unsigned char want[13] = {0x8b, 0xb1, 0xbc, 0, 0, 0, 0xf7, 0x46, 0x04, 0, 0x40, 0, 0};
    unsigned char rep[13] = {0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    *(DWORD *)(rep + 1) = (DWORD)light_inst_guard - (0x98b83a + 5);
    return ts_patch_bytes(0x98b83a, want, rep, 13);
}

/* Player 2 removed (split screen off, a client's player 2 ended, a client's copy no longer needed). */
static void p2_remove(const char *why)
{
    char out[64];
    if (!p2)
        return;
    /* Its body (the third-person `player_ghost`, player +0x47c handle) and its crosshair objects (see
     * crs_obj) are removed first (message 2); the player then finds its handle empty. */
    BYTE *body = body_of(p2);
    if (body && !IsBadReadPtr(body, 0x234)) {
        ts_log("split: player 2's body %p (vtable %08lx) removed first", (void *)body, *(DWORD *)body);
        dispatch_tramp(body, 2, 0, 0);
    }
    for (int i = 0; i < n_crs_obj; i++)
        if (!IsBadReadPtr(crs_obj[i].o, 4) && *(DWORD *)crs_obj[i].o == crs_obj[i].vt) {
            ts_log("split: player 2's crosshair object %p (vtable %08lx) removed first", (void *)crs_obj[i].o, crs_obj[i].vt);
            dispatch_tramp(crs_obj[i].o, 2, 0, 0);
            if (n_crs_gone < MAX_CRS)
                crs_gone[n_crs_gone++] = crs_obj[i].o;
        }
    n_crs_obj = 0;
    /* (its weapons, holster and hands: not updated any more, see hook_dispatch; their nodes hidden) */
    n_p2_gone = 0;
    for (int i = 0; i < n_p2_objs; i++)
        if (p2_objs[i].o != body && !IsBadReadPtr(p2_objs[i].o, 0x234) && *(DWORD *)p2_objs[i].o == p2_objs[i].vt) {
            p2_gone[n_p2_gone++] = p2_objs[i];
            BYTE *nd = *(BYTE **)(p2_objs[i].o + 0x230);
            if (nd && !IsBadReadPtr(nd, 8))
                *(DWORD *)(nd + 4) |= 1;
        }
    ts_log("split: %d objects of player 2 (weapons, holster, hands) stopped", n_p2_gone);
    n_p2_objs = 0;
    p2_removing = 1;
    ts_script_run("$player2.Terminate()", out, sizeof out);
    ts_log("split: player 2 removed: %s (%s)", why, out);
}

/* The player-table message 0x434 (0x5f52f0: the record of a player looked up with 0x5fb6f0, then 0x5f78e0 with
 * that index) has no record for a host's player 2 on a client (the table holds the network players): the index
 * -1 read the record before the table and crashed (0x5f7985 / 0x5f7906, esi = -0x84; runs 20261026-201715 to
 * -210653, after a level change). A missing record is skipped (call at 0x5f53dd). */
__attribute__((naked)) static void stat434_stub(void)
{
    __asm__("test %eax, %eax\n\t"
            "js 1f\n\t"
            "push $0x5f78e0\n\t"
            "ret\n"
            "1:\n\t"
            "ret $4");
}

static void *TSCALL hook_dtor(void *pl, int flags)
{
    if (!p2 || pl != p2) {
        if (pl == p1_seen)
            p1_seen = NULL;
        return real_dtor(pl, flags);
    }
    BYTE *pa = PLAYER_A, *pb = PLAYER_B;
    p2 = NULL;
    p2_removing = 0;
    p2_body = NULL;
    p1_since = GetTickCount();   /* (a level change removes it: a new one after the delay in the next level) */
    p2_failed = 0;
    void *r = real_dtor(pl, flags);
    PLAYER_A = pa == pl ? NULL : pa;
    PLAYER_B = pb == pl ? NULL : pb;
    ts_log("split: player 2 destroyed");
    return r;
}

/* A scene node's visibility bit of this frame (world +0x50 bitmask, nodes 0x238 bytes from world +0x2ad0;
 * the render prep 0x67f9b0 draws and animates only nodes with it). -1: not a world node. */
static int node_visible(BYTE *n)
{
    BYTE *w = *(BYTE **)0xfcacdc;
    if (!w || !n)
        return -1;
    BYTE *base = *(BYTE **)(w + 0x2ad0);
    if (n < base || (n - base) % 0x238 || (n - base) / 0x238 >= 0xa8c)
        return -1;
    int i = (int)((n - base) / 0x238);
    return (((DWORD *)(w + 0x50))[i >> 5] >> (i & 31)) & 1;
}

/* The player's third-person body (player_ghost "player_3d"): player +0x47c holds a handle to it. */
static BYTE *body_of(BYTE *pl)
{
    BYTE **h = pl ? *(BYTE ***)(pl + 0x47c) : NULL;
    return h && !IsBadReadPtr(h, 4) ? *h : NULL;
}

/* Player 2 next to player 1, facing the same way (TS_SPLIT_P2FACE=1, tests: 2.5 m in front, facing player 1),
 * placed with the player's own vtable +0x210 as player 2. */
/* Respawn spots: each player's own trail of positions (every half second in play). A spot where the partner
 * stood a few seconds ago is ground it walked on; the partner's current place plus an offset is the fallback. */
#define TRAIL 24
static struct { float p[TRAIL][3]; int n, head; DWORD last; } trail[2];

static void trail_add(int who, BYTE *pl)
{
    BYTE *node = pl ? *(BYTE **)(pl + 0x230) : NULL;
    DWORD now = GetTickCount();
    if (!node || now - trail[who].last < 500)
        return;
    trail[who].last = now;
    memcpy(trail[who].p[trail[who].head], node + 0x68, 12);
    trail[who].head = (trail[who].head + 1) % TRAIL;
    if (trail[who].n < TRAIL)
        trail[who].n++;
}

/* The newest spot at least 4 samples (2 s) old, `away` m beside where the partner is now, not more than 12 m
 * (a level start can move the player after its first frames: in ts2_city_part03 player 1 stood 105 m lower
 * first, and player 2 was put there). */
static int trail_pick_away(int who, const float *now_pos, float *out, float away)
{
    for (int age = 4; age < trail[who].n; age++) {
        float *q = trail[who].p[(trail[who].head - 1 - age + 2 * TRAIL) % TRAIL];
        float dx = q[0] - now_pos[0], dy = q[1] - now_pos[1], dz = q[2] - now_pos[2], h2 = dx * dx + dz * dz;
        /* (beside, not above or below: in ts2_city_part03 a spot 2 m above player 1 put player 2 on its head) */
        if (h2 >= away * away && h2 <= 12.0f * 12.0f && dy * dy < 1.2f * 1.2f) {
            memcpy(out, q, 12);
            return 1;
        }
    }
    return 0;
}

static int trail_pick(int who, const float *now_pos, float *out)
{
    return trail_pick_away(who, now_pos, out, 0.8f);
}

/* The newest spot of one's own trail at least 2 s old (ground one stood on) within 25 m of the partner. */
static int trail_own(int who, const float *partner_pos, float *out)
{
    for (int age = 4; age < trail[who].n; age++) {
        float *q = trail[who].p[(trail[who].head - 1 - age + 2 * TRAIL) % TRAIL];
        float dx = q[0] - partner_pos[0], dy = q[1] - partner_pos[1], dz = q[2] - partner_pos[2], h2 = dx * dx + dz * dz;
        /* (beside the partner on its level, not on its head: a level start put player 2 2 m above player 1) */
        if (h2 <= 25.0f * 25.0f && h2 >= 1.0f && dy * dy < 1.2f * 1.2f) {
            memcpy(out, q, 12);
            return 1;
        }
    }
    return 0;
}

static void trail_clear(void)
{
    memset(trail, 0, sizeof trail);
}

/* Player `pl` (player 1 or 2) put next to its partner, facing the partner's way, with the player's own vtable
 * +0x210 as that player. Only spots someone stood on: the partner's trail (2 s old, 1.5 m away), else one's own
 * trail near the partner, else the partner's own spot (a fixed offset beside it could be inside a wall).
 * TS_SPLIT_P2FACE=1 (tests): player 2 2.5 m in front of player 1, facing it. */
static DWORD place_retry;
static BYTE *place_retry_who;

/* The placement (+0x210 = 0x409490, last argument: find the floor under the spot with a ray, filter 0x100100,
 * which does not see moving objects; 0 = put it there). */
static void put_player_f(BYTE *pl, float *pos, float *dir, int floor)
{
    if (pl == p2) {
        p2_enter();
        ((place_t)(*(void ***)p2)[SLOT_PLACE])(p2, pos, dir, floor);
        p2_leave();
    } else
        ((place_t)(*(void ***)pl)[SLOT_PLACE])(pl, pos, dir, floor);
}

static void put_player(BYTE *pl, float *pos, float *dir)
{
    put_player_f(pl, pos, dir, 1);
}

/* No floor under any spot beside the partner (it stands on something moving, which the floor ray does not see):
 * the player is put 1 m beside it at its height without the floor ray, and 1.5 s later it is checked that it
 * stayed there (on the partner's support); up to 3 tries per level, then the ground below and a new try every
 * 2 s. */
static DWORD support_check;
static BYTE *support_who;
static int support_tries;

static int put_on_support(BYTE *pl, const float *pp, float ax, float az, float *dir)
{
    static const float side[3][2] = {{-1, 0}, {0, 1}, {0, -1}};
    if (support_tries >= 3)
        return 0;
    int k = support_tries % 3;
    float pos[3] = {pp[0] + (ax * side[k][0] - az * side[k][1]) * 1.0f, pp[1] + 0.2f,
                    pp[2] + (az * side[k][0] + ax * side[k][1]) * 1.0f};
    float want = pos[1], first[3];
    memcpy(first, pos, sizeof first);
    put_player_f(pl, pos, dir, 0);
    BYTE *n = *(BYTE **)(pl + 0x230);
    if (n) {   /* (the placement raises the spot by the player's own offsets: asked again lower by what it added) */
        float got = ((float *)(n + 0x68))[1];
        first[1] = want - (got - want);
        put_player_f(pl, first, dir, 0);
    }
    support_tries++;
    support_check = GetTickCount() + 1500;
    support_who = pl;
    ts_log("split: player %d put 1 m beside the partner at its height (no floor under the spots beside it): %.2f %.2f %.2f,"
           " partner at %.2f %.2f %.2f; checked in 1.5 s", pl == p2 ? 2 : 1, first[0], n ? ((float *)(n + 0x68))[1] : first[1],
           first[2], pp[0], pp[1], pp[2]);
    return 1;
}

static void place_player(BYTE *pl, BYTE *partner)
{
    BYTE *node1 = partner ? *(BYTE **)(partner + 0x230) : NULL;
    if (!node1 || !pl)
        return;
    float *fw = (float *)(node1 + 0x58), *pp = (float *)(node1 + 0x68);
    /* (the last resort, when neither player has walked yet: 1.5 m ahead of the partner, in the open space the
     * partner looks into; a level starts its player facing into the playing area) */
    float fl = sqrtf(fw[0] * fw[0] + fw[2] * fw[2]);
    float ax = fl > 0.01f ? fw[0] / fl : 0, az = fl > 0.01f ? fw[2] / fl : 0;
    float pos[3] = {pp[0], pp[1] + 0.3f, pp[2]};
    float dir[3] = {fw[0], fw[1], fw[2]};
    const char *how = "ahead of the partner";
    char v[8];
    if (pl == p2 && ts_env("TS_SPLIT_P2FACE", v, sizeof v) && v[0] == '1') {
        pos[0] = pp[0] + fw[0] * 2.5f, pos[2] = pp[2] + fw[2] * 2.5f;
        dir[0] = -fw[0], dir[1] = -fw[1], dir[2] = -fw[2];
        how = "facing";
    } else if (*(DWORD *)partner == PLAYER_VTBL && trail_pick(partner == p2 ? 1 : 0, pp, pos))
        how = "on the partner's trail";
    else if (*(DWORD *)partner == PLAYER_VTBL && trail_own(pl == p2 ? 1 : 0, pp, pos))
        how = "on its own trail";
    else {
        /* Nobody walked yet: spots 1.6 m behind, beside or diagonally behind the partner, ahead only last. The
         * placement (+0x210) puts the player on the floor it finds under the spot and writes the spot back (in
         * `ts2_city_part02` a spot ahead came back 1 m up, on a crate in the partner's face); a spot is taken
         * when it comes back on the partner's floor level, not moved aside, and not inside the partner. */
        static const float cand[6][2] = {{-1, 0}, {0, 1}, {0, -1}, {-0.7f, 0.7f}, {-0.7f, -0.7f}, {1, 0}};
        static const char *cname[6] = {"behind the partner", "right of the partner", "left of the partner",
                                       "behind right of the partner", "behind left of the partner", "ahead of the partner"};
        float rx = -az, rz = ax;   /* (the partner's right on the ground) */
        /* (two rings, 1.6 m and 0.9 m. ts2_city_part03 starts player 1 100 m up on something moving (its height
         * swings by half a metre) with the ground below every spot around it: putting player 2 on player 1's own
         * spot pushed player 1 off and both died, so then player 2 stays on the ground below for now (the last
         * spot tried) and is placed again every 2 s until a spot beside player 1 holds) */
        int ok = 0;
        for (int ring = 0; ring < 2 && !ok; ring++)
            for (int c = 0; c < 6 && !ok; c++) {
                float r = ring ? 0.9f : 1.6f;
                float want[3] = {pp[0] + (ax * cand[c][0] + rx * cand[c][1]) * r, pp[1] + 0.3f,
                                 pp[2] + (az * cand[c][0] + rz * cand[c][1]) * r};
                memcpy(pos, want, sizeof pos);
                put_player(pl, pos, dir);
                float gx = pos[0] - want[0], gz = pos[2] - want[2], dx = pos[0] - pp[0], dz = pos[2] - pp[2];
                ok = fabsf(pos[1] - pp[1]) < 0.45f && gx * gx + gz * gz < 0.25f && dx * dx + dz * dz > r * r * 0.36f;
                if (splitlog)
                    ts_log("split: player %d spot %s (%.1f m): asked %.2f %.2f %.2f, got %.2f %.2f %.2f%s", pl == p2 ? 2 : 1, cname[c], r,
                       want[0], want[1], want[2], pos[0], pos[1], pos[2], ok ? "" : " (not taken)");
                how = cname[c];
            }
        int again = place_retry != 0;   /* (a retry: logged once) */
        if (!ok && put_on_support(pl, pp, ax, az, dir)) {
            place_retry = 0;
            return;
        }
        if (!ok) {
            how = "on the ground below the partner (no floor beside it; tried again every 2 s)";
            place_retry = GetTickCount() + 2000;
            place_retry_who = pl;
            if (again)
                return;
        } else
            place_retry = 0;
        ts_log("split: player %d placed %s at %.2f %.2f %.2f (partner at %.2f %.2f %.2f)", pl == p2 ? 2 : 1, how, pos[0],
               pos[1], pos[2], pp[0], pp[1], pp[2]);
        return;
    }
    put_player(pl, pos, dir);
    place_retry = 0;
    ts_log("split: player %d placed %s at %.2f %.2f %.2f (partner at %.2f %.2f %.2f)", pl == p2 ? 2 : 1, how, pos[0],
           pos[1], pos[2], pp[0], pp[1], pp[2]);
}

static void place_p2(BYTE *p1)
{
    if (client_split && cl_have) {   /* (the shadow: beside the host's player 2, whose player_mp stands there) */
        BYTE *mp = find_cp2();
        if (mp && *(BYTE **)(mp + 0x230))
            place_player(p2, mp);
        else {
            float pos[3] = {cl_pos[0] + cl_rows[0], cl_pos[1] - 1.7f, cl_pos[2] + cl_rows[2]}, dir[3] = {cl_rows[6], 0, cl_rows[8]};
            put_player(p2, pos, dir);   /* (1 m to its right) */
        }
        return;
    }
    if (remote_host && remote_mp && !IsBadReadPtr(remote_mp, 0x234) && *(BYTE **)(remote_mp + 0x230)) {
        place_player(p2, remote_mp);   /* (a client's player 2: next to that client's player) */
        return;
    }
    place_player(p2, p1);
}

static const char *script_name(BYTE *ent)
{
    BYTE *inst = *(BYTE **)(ent + 0x80);   /* (script instance: +4 name string, +0xc the entity) */
    if (!inst || IsBadReadPtr(inst, 0x10) || *(BYTE **)(inst + 0xc) != ent || !*(BYTE **)(inst + 4))
        return "?";
    return (const char *)(*(BYTE **)(inst + 4) + 0xc);
}

/* A script name for player 2. Its script instance (entity +0x80: +4 name, +0xc the entity) is made during the
 * spawn with the name of player 1's ("player"), which the script manager refuses ("already exist, new one added
 * as unnamed"). The manager keeps named objects in an array sorted by name (manager +0x14: items, +0x18 count;
 * byte-wise compare, 0x428b20), searched by `$name`; the instance gets the name and goes into the array as the
 * engine's own insert (0x8fb000) would put it. */
static void *(__cdecl *eng_alloc)(int, int, int) = (void *)0x7e4620;
static void (__cdecl *eng_free_)(void *) = (void *)0x7e4700;
static void (TSCALL *arr_insert)(void *, int, void *, int) = (void *)0x8fb000;

static int give_script_name(BYTE *ent, const char *name)
{
    BYTE *mgr = *(BYTE **)0xebedec, *inst = *(BYTE **)(ent + 0x80);
    if (!mgr || !inst || *(BYTE **)(inst + 0xc) != ent)
        return 0;
    EStr *old = *(EStr **)(inst + 4);
    if (old && old->s[0])
        return 0;   /* (named already) */
    BYTE **items = *(BYTE ***)(mgr + 0x14);
    int n = *(int *)(mgr + 0x18), lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        EStr *m = *(EStr **)(items[mid] + 4);
        int c = strcmp(m ? m->s : "", name);
        if (c == 0)
            return 0;   /* (taken) */
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    int len = (int)strlen(name);
    EStr *str = eng_alloc(len + 0x10, 0, 0);
    if (!str)
        return 0;
    str->refs = 1, str->len = len, str->cap = len;
    memcpy(str->s, name, len + 1);
    *(EStr **)(inst + 4) = str;
    if (old && --old->refs == 0)
        eng_free_(old);
    BYTE *item = inst;
    arr_insert(mgr + 0x14, lo, &item, 1);
    return 1;
}

static void spawn_p2(BYTE *p1)
{
    BYTE *pa = PLAYER_A, *pb = PLAYER_B, *cam = CAM;
    BYTE *node1 = *(BYTE **)(p1 + 0x230);
    /* (the object name names a placed object the new one is made from, found through [0xfcace4] vtable
     * +0x34: player 1's own "player" serves as the template; a name of nothing placed gives no object) */
    spawning_p2 = 1;
    spawn_partner = p1;
    n_own2 = 0;
    n_crs[1] = 0;
    n_geo2 = 0;
    n_p2_objs = 0;
    BYTE *e = spawn_entity(NULL, "player", "player", 0);
    spawning_p2 = 0;
    /* its constructor took both player pointers, its init may have used the camera */
    PLAYER_A = pa;
    PLAYER_B = pb;
    CAM = cam;
    if (!e || *(DWORD *)e != PLAYER_VTBL) {
        ts_log("split: player 2 not spawned (%p, vtable %08lx)", (void *)e, e ? *(DWORD *)e : 0);
        p2_failed++;
        p1_since = GetTickCount();   /* (tried again after the delay, three times per level) */
        return;
    }
    memcpy(cam2, cam, CAM_SIZE);
    if (!make_context())
        ts_log("split: no command context for player 2 (no pad input)");
    p2 = e;
    p2_born = GetTickCount();
    if (!(*(float *)(e + HEALTH) > 0))   /* (a spawned player starts without life: full, as player 1 began) */
        *(float *)(e + HEALTH) = 100.0f;
    ts_log("split: player 2 health %.1f (player 1 %.1f)", *(float *)(e + HEALTH), *(float *)(p1 + HEALTH));
    ts_log("split: player 2 spawned %p (script name \"%s\"; player 1 %p \"%s\")", (void *)e, script_name(e), (void *)p1,
           script_name(p1));
    /* (single player names the object "player" in its init: player 2 gets its own name back) */
    BYTE *node2 = *(BYTE **)(e + 0x230);
    if (node2)
        node_name(node2, "player2");
    if (!give_script_name(e, "player2"))
        ts_log("split: player 2 got no script name");
    ts_log("split: names now: player 1 \"%s\", player 2 \"%s\"", script_name(p1), script_name(e));
    place_p2(p1);
    p2_body = body_of(e);
    BYTE *gm = *(BYTE **)0xfcab34;
    if (!net_local && gm && *(int *)(gm + 0x3c) == 2) {
        unsigned r = *(unsigned *)(e + 0x2c);
        /* (the player's network start, vtable +0x44 = 0x420300 (kind, motion, 0): the server's motion writer
         * (+0x34) and the regular-update bits, which the server's player creation (0x697620) gives player 1;
         * without it the clients' player_mp of player 2 stood at its spawn spot) */
        ts_log("split: network start of player 2: player 1 motion %p kind %d, player 2 motion %p kind %d",
               *(void **)(p1 + 0x34), *(int *)(p1 + 0x38), *(void **)(e + 0x34), *(int *)(e + 0x38));
        if (!*(void **)(e + 0x34) && (DWORD)(*(void ***)e)[0x44 / 4] == 0x420300) {
            p2_enter();
            ((void (TSCALL *)(void *, int, int, int))(*(void ***)e)[0x44 / 4])(e, *(int *)(p1 + 0x38), 1, 0);
            p2_leave();
        }
        *(unsigned *)(e + 0x2c) |= 0x10u;
        ts_log("split: player 2 networked as a puppet for clients: net id %d, sync flags %08x, role %08x -> %08x (player 1 %08x)",
               *(int *)(e + 0x18), *(unsigned *)(e + 0x98), r, *(unsigned *)(e + 0x2c), *(unsigned *)(p1 + 0x2c));
    }
    if (gm && *(int *)(gm + 0x3c) == 3) {   /* (a client: the shadow of our player 2 is a local object) */
        int n = net_hide(e, "our player 2's copy") + net_hide(p2_body, "its body");
        for (int i = 0; i < n_own2; i++)
            n += net_hide(own2[i], own_cls[1][i]);
        if (*(int *)(e + 0x18) == -3)
            *(int *)(e + 0x18) = -1;   /* (not waiting for a server id) */
        ts_log("split: our player 2's local copy made (%d networked objects of it changed, net id %d)", n, *(int *)(e + 0x18));
    }
    if (net_local && gm && *(int *)(gm + 0x3c) == 2) {   /* (a server) */
        int n = net_hide(e, "player 2") + net_hide(p2_body, "player 2's body");
        for (int i = 0; i < n_own2; i++)
            n += net_hide(own2[i], own_cls[1][i]);
        ts_log("split: player 2 kept out of the network game (%d networked objects of it changed)", n);
    }
}

/* The menu asks whether a second pad is there (UI string CoopPad2 = "1"/"0"; the Co-op menu's "Split screen"
 * refuses to start without one). Called after each input poll (gamepad.c), which also runs in the menus where
 * the script tick stops; once a second. */
void gamepad_after_poll(void (*fn)(void));   /* gamepad.c */

static void publish_pad2(void)
{
    static DWORD last, logged;
    DWORD now = GetTickCount();
    if (now - last < 1000)
        return;
    last = now;
    char out[64], ssl[96];
    snprintf(ssl, sizeof ssl, "$ui.SetStr(\"CoopPad2\", \"%d\")", gamepad_slot(1) >= 0);
    ts_script_run(ssl, out, sizeof out);
    if (!logged++)
        ts_log("split: pad 2 %s (told to the menu from thread %lu)", gamepad_slot(1) >= 0 ? "there" : "missing",
               GetCurrentThreadId());
}

/* The player's choice (the Co-op menu's "Split screen"): the menu sets the UI string CoopSplit to "1" and
 * starts the level; the main menu clears it. Once a second. */
static int net_mode(void);

static void split_choice(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 1000)
        return;
    last = now;
    char out[64];
    if (!forced) {
        ts_script_run("return $ui.GetStr(\"CoopSplitJoin\")", out, sizeof out);
        client_choice = strcmp(out, "1") == 0;
    }
    if (forced || remote_host || client_split)
        return;
    ts_script_run("return $ui.GetStr(\"CoopSplit\")", out, sizeof out);
    /* (the Co-op menu's split screen (single player), or a co-op host with the Co-op frame's "Second player on
     * pad 2" and a second pad) */
    int host = net_mode() == 2 && client_choice && gamepad_slot(1) >= 0;
    int want = strcmp(out, "1") == 0 || host;
    if (want && !stage) {
        stage = 3;
        set_single_thread(1);
        ts_log("split: on (%s; player 2 on XInput pad %d)", host ? "a co-op host with \"Second player on pad 2\"" : "chosen in the Co-op menu",
               gamepad_slot(1) + 1);
    } else if (!want && stage) {
        if (p2)   /* (player 2 belongs to the game, not the level: removed when split screen ends) */
            p2_remove("split screen off");
        stage = 0;
        set_single_thread(0);
        restore_globals();
        pic_ok[0] = pic_ok[1] = 0;
        ts_log("split: off");
    }
}

/* The HUD widgets that show the local player (weapon panel with ammunition and grenades, time-power energy and
 * icons, life and armour, hit arrows, ...) read [0xfc8180] in their updates (widget vtable +0x34, thiscall
 * (widget, float dt)), which the UI update runs outside the frame's view span: both halves showed player 1's.
 * Each player has its own weapon set and ammunition. The views alternate per frame, so these widgets are updated
 * for the view the next frame draws, with that view's player. The table: the widget classes whose update reads
 * [0xfc8180] (vtables of the HUD classes 0x59e000-0x5e2000 found by their common base entries; slot +0x34). */
static const DWORD hud_vt[][2] = {
    {0xda02a0, 0x5a83d0}, {0xda09b8, 0x5ac390}, {0xda0f98, 0x5b0300}, {0xda10b8, 0x5b1a10}, {0xda1368, 0x5b2bf0},
    {0xda1490, 0x5b4630}, {0xda17b0, 0x5b5d90}, {0xda19f0, 0x5b8610}, {0xda1b10, 0x5ba250}, {0xda1d78, 0x5bac20},
    {0xda1f50, 0x5bbcd0}, {0xda21b0, 0x5bd8d0}, {0xda34f8, 0x5c8310}, {0xda4278, 0x5d46b0},
};
#define N_HUD (int)(sizeof hud_vt / sizeof hud_vt[0])
typedef void (TSCALL *hud_upd_t)(void *, float);
static hud_upd_t hud_real[N_HUD];
static int hudlog, hud_view1 = 1;   /* (frame_no & 1 at a widget update when the next frame draws view 1) */

__attribute__((used)) void __cdecl split_hud_update(int i, void *w, float dt)
{
    if (!hud_root) {   /* (the HUD frame: the top of a HUD widget's parents) */
        BYTE *e = w;
        for (int k = 0; e && k < 12 && *(BYTE **)(e + 0x40); k++)
            e = *(BYTE **)(e + 0x40);
        if (e && e != w) {
            hud_root = e;
            hud_root_vt = *(DWORD *)e;
            ts_log("split: HUD frame %p", (void *)e);
            if (canvas_kept)
                hud_relayout("a half");
        }
    }
    int next = (int)(frame_no & 1);
    if (hudlog && i == 10 && hudlog++ < 6)
        ts_log("split: HUD weapon panel update: frame %u, next view %d, view span %d", frame_no, next, swapped);
    int on = !(stage < 3 || !p2 || swapped || as2.depth || !PLAYER_B || PLAYER_B == p2 || !split_wanted());
    if (!on) {
        hud_real[i](w, dt);
        return;
    }
    int v = next == hud_view1 ? 1 : 0;
    /* The time-power bar (TimeGrenadeBar, class 0xda19f0; its ProgressTime bar = widget +0x298) puts the player's
     * energy (player +0x2ac) into the bar's target (+0x108); the bar's own update moves the shown value (+0x10c)
     * towards it over some frames. With one widget for both views the shown value ended between the two players'
     * energies (both bars went down while only player 2 used a power), so the shown value is set to this view's
     * player's energy at once. */
    BYTE *bar = hud_vt[i][0] == 0xda19f0 ? *(BYTE **)((BYTE *)w + 0x298) : NULL;
    if (bar && IsBadReadPtr(bar, 0x120))
        bar = NULL;
    BYTE *pa = PLAYER_A, *pb = PLAYER_B;
    if (v == 1)
        PLAYER_A = PLAYER_B = p2;
    hud_real[i](w, dt);
    PLAYER_A = pa;
    PLAYER_B = pb;
    if (bar) {
        *(float *)(bar + 0x10c) = *(float *)(bar + 0x108);
        *(float *)(bar + 0x110) = 0;
    }
}

/* One thunk per widget class: eax = its index, then the common part calls split_hud_update(i, this, dt). */
__attribute__((naked)) static void hud_common(void)
{
    __asm__("push 4(%esp)\n\t"   /* dt */
            "push %ecx\n\t"
            "push %eax\n\t"
            "call _split_hud_update\n\t"
            "add $12, %esp\n\t"
            "ret $4");
}

static int install_hud_hooks(void)
{
    unsigned char *t = VirtualAlloc(NULL, N_HUD * 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int n = 0;
    if (!t)
        return 0;
    for (int i = 0; i < N_HUD; i++) {
        DWORD slot = hud_vt[i][0] + 0x34;
        if (*(DWORD *)slot != hud_vt[i][1])
            continue;
        unsigned char *k = t + 16 * i;
        k[0] = 0xb8, *(DWORD *)(k + 1) = (DWORD)i;                                  /* mov eax, i */
        k[5] = 0xe9, *(DWORD *)(k + 6) = (DWORD)hud_common - (DWORD)(k + 10);       /* jmp hud_common */
        void *vt = (void *)slot;
        void *o = ts_patch_vtable(&vt, 0, k);
        if (o)
            hud_real[i] = (hud_upd_t)o, n++;
    }
    return n;
}

/* The HUD's ammunition source (0x5bbcd0): the player's vtable +0x248 gives its weapon set, whose virtual base's
 * vtable +0x4c (0 primary, 1 secondary) gives an ammunition record: +8 the total (float), +0xc the rounds in
 * the magazine (the HUD shows total - magazine). Tests: which record each player has. */
static BYTE *ammo_rec(BYTE *pl, int which, BYTE **set_out)
{
    if (!pl || IsBadReadPtr(pl, 4) || *(DWORD *)pl != PLAYER_VTBL)
        return NULL;
    BYTE *set = ((BYTE *(TSCALL *)(void *))(*(void ***)pl)[0x248 / 4])(pl);
    if (set_out)
        *set_out = set;
    if (!set || IsBadReadPtr(set, 8) || IsBadReadPtr(*(BYTE **)(set + 4), 8))
        return NULL;
    BYTE *vb = set + 4 + *(int *)(*(BYTE **)(set + 4) + 4);
    if (IsBadReadPtr(vb, 4) || IsBadReadPtr(*(void **)vb, 0x50))
        return NULL;
    BYTE *rec = ((BYTE *(TSCALL *)(void *, int))(*(void ***)vb)[0x4c / 4])(vb, which);
    return rec && !IsBadReadPtr(rec, 0x24) ? rec : NULL;
}

/* Player 2's weapons and hands made again (after a load took them), as at its spawn: the player's weapon set-up
 * (vtable +0x25c, thiscall, no arguments) run as player 2 with the spawn hooks in their player 2 mode. */
/* The player's weapon set-up (vtable +0x25c, 0x442840: weapons and hands). A single-player or server spawn runs it
 * inside the spawn; on a client the player's network start runs it later (0x41ec5f, the first update), outside
 * spawn_p2: player 2's first-person nodes were then refused (the template's geometry is player 1's) and the
 * engine wrote through the missing weapon (0x447a5c, run 20261025-213310). For player 2 the set-up always runs
 * as player 2's spawn. */
static int (TSCALL *real_wsetup)(void *);

static int TSCALL hook_wsetup(BYTE *pl)
{
    if (!spawning_p2 && (!p2 || pl != p2))
        return real_wsetup(pl);
    int was = spawning_p2;
    spawning_p2 = 1;
    int r = real_wsetup(pl);
    spawning_p2 = was;
    n_crs_obj = 0;
    crs_scan = GetTickCount() + 5000;
    if (!was)
        ts_log("split: player 2's weapon set-up outside its spawn: %s, %d first-person objects", r ? "ok" : "failed", n_own2);
    return r;
}

static void p2_rearm(void)
{
    DWORD ws = p2 ? (DWORD)(*(void ***)p2)[0x25c / 4] : 0;
    if (!p2 || (ws != 0x442840 && ws != (DWORD)hook_wsetup)) {
        ts_log("split: player 2's weapons not made again (other exe?)");
        return;
    }
    n_own2 = 0;
    n_crs[1] = 0;
    n_p2_objs = 0;   /* (the load removed the old ones; their addresses may be player 1's new objects) */
    spawning_p2 = 1;
    p2_enter();
    int ok = ((int (TSCALL *)(void *))(*(void ***)p2)[0x25c / 4])(p2);
    p2_leave();
    spawning_p2 = 0;
    BYTE *rec = ammo_rec(p2, 0, NULL);
    ts_log("split: player 2's weapons made again after the load: %s, %d objects, weapon %p total %.1f magazine %d",
           ok ? "ok" : "failed", n_own2, (void *)rec, rec ? *(float *)(rec + 8) : -1.0f, rec ? *(int *)(rec + 0xc) : -1);
}

/* Player 2 gets player 1's three weapon slots (player +0x3a8, +0x3ac, +0x3b0) when they differ. */
static void p2_loadout(BYTE *p1)
{
    if (!p1 || !p2)
        return;
    int *a = (int *)(p1 + 0x3a8), *b = (int *)(p2 + 0x3a8);
    ts_log("split: weapon slots at the level start: player 1 %d %d %d, player 2 %d %d %d", a[0], a[1], a[2], b[0], b[1], b[2]);
    if ((a[0] == b[0] && a[1] == b[1] && a[2] == b[2]) || !(a[0] | a[1] | a[2]))   /* (never takes player 2's away) */
        return;
    char ssl[96], out[64];
    snprintf(ssl, sizeof ssl, "$player2.SetupWeapon(%d, %d, %d)", a[0], a[1], a[2]);
    ts_script_run(ssl, out, sizeof out);
    ts_log("split: player 2 gets player 1's weapons %d %d %d (it had %d %d %d): %s", a[0], a[1], a[2], b[0], b[1], b[2], out);
}

/* Tests (TS_SPLIT_ITEMTEST=list,<s> or <name>,<s>): <s> seconds after player 2 came in, "list" logs the named
 * objects within 30 m of player 1 (script name table, entity node position); a name moves player 2 onto that
 * placed object (a pick-up of the level), to see who gets what. Each player's ammunition and life are logged
 * before and 3 s after. */
static BYTE *named_entity(const char *name)
{
    BYTE *mgr = *(BYTE **)0xebedec;
    if (!mgr)
        return NULL;
    BYTE **items = *(BYTE ***)(mgr + 0x14);
    int n = *(int *)(mgr + 0x18);
    for (int i = 0; i < n; i++) {
        EStr *m = *(EStr **)(items[i] + 4);
        if (m && strcmp(m->s, name) == 0)
            return *(BYTE **)(items[i] + 0xc);
    }
    return NULL;
}

static int item_next, item_step;
static int near_idx[8], n_near, near_at;

/* The next of the nearest recorded pick-up nodes (still existing: geometry unchanged) onto which player 2 goes. */
static int near_move(void)
{
    while (near_at < n_near) {
        int k = near_idx[near_at++];
        BYTE *n = items_seen[k].node;
        if (!n || IsBadReadPtr(n, 0x1c4) || *(BYTE **)(n + 0x1c0) != items_seen[k].geo)
            continue;
        float pos[3], dir[3] = {-0.89f, -0.45f, 0};
        memcpy(pos, n + 0x68, 12);
        pos[1] += 0.3f;
        put_player(p2, pos, dir);
        ts_log("split: item test: player 2 put on %s (node %p) at %.2f %.2f %.2f", items_seen[k].name, (void *)n, pos[0], pos[1],
               pos[2]);
        return 1;
    }
    return 0;
}

/* The next object of a "+"-separated list ("@name": onto it, "name": 1.2 m beside it facing it); 0: none left. */
static int item_move(const char *list, int *at)
{
    char tok[64];
    const char *t = list + *at;
    if (!*t)
        return 0;
    const char *end = strchr(t, '+');
    int len = end ? (int)(end - t) : (int)strlen(t);
    if (len <= 0 || len >= (int)sizeof tok)
        return 0;
    memcpy(tok, t, len);
    tok[len] = 0;
    *at += len + (end ? 1 : 0);
    int one = tok[0] == '!';   /* (!name: player 1 instead) */
    int exact = tok[one] == '@';
    BYTE *who = one ? (PLAYER_B != p2 ? PLAYER_B : NULL) : p2;
    if (!who)
        return 1;
    if (tok[one] == '#') {   /* (#x/y/z: a spot) */
        float pos[3], dir[3] = {-0.89f, -0.45f, 0};
        if (sscanf(tok + one + 1, "%f/%f/%f", &pos[0], &pos[1], &pos[2]) == 3) {
            put_player(who, pos, dir);
            ts_log("split: item test: player %d put on %.2f %.2f %.2f, got %.2f %.2f %.2f", one ? 1 : 2, pos[0], pos[1], pos[2],
                   ((float *)(*(BYTE **)(who + 0x230) + 0x68))[0], ((float *)(*(BYTE **)(who + 0x230) + 0x68))[1],
                   ((float *)(*(BYTE **)(who + 0x230) + 0x68))[2]);
        }
        return 1;
    }
    BYTE *e = named_entity(tok + one + exact), *en = e && !IsBadReadPtr(e, 0x234) ? *(BYTE **)(e + 0x230) : NULL;
    if (!en || IsBadReadPtr(en, 0x74)) {
        ts_log("split: item test: no object \"%s\"", tok);
        return 1;
    }
    /* (1.2 m from it, facing it and a bit down: some need the use button while looked at) */
    float pos[3], dir[3] = {-0.89f, -0.45f, 0};
    memcpy(pos, en + 0x68, 12);
    if (!exact)
        pos[0] += 1.2f;
    pos[1] += 0.3f;
    put_player(who, pos, dir);
    ts_log("split: item test: player %d put on \"%s\" at %.2f %.2f %.2f", one ? 1 : 2, tok, pos[0], pos[1], pos[2]);
    return 1;
}

static void item_test(BYTE *p1)
{
    static char what[256];
    static int secs = -1, stage_t;
    char v[300];
    if (secs == -1) {
        secs = -2;
        if (ts_env("TS_SPLIT_ITEMTEST", v, sizeof v)) {
            char *c = strchr(v, ',');
            if (c)
                *c = 0, snprintf(what, sizeof what, "%s", v), secs = atoi(c + 1);
        }
    }
    if (secs < 0 || !p2 || !p1 || stage_t > 1)
        return;
    int age = (int)(GetTickCount() - p2_born);
    BYTE *r1 = ammo_rec(p1, 0, NULL), *r2 = ammo_rec(p2, 0, NULL);
    if (stage_t == 0 && age >= secs * 1000) {
        stage_t = 1;
        char av[16];
        if (r2 && ts_env("TS_SPLIT_P2AMMO", av, sizeof av))   /* (tests: player 2's reserve set low, so ammunition is taken) */
            *(float *)(r2 + 8) = (float)atof(av);
        ts_log("split: item test before: player 1 ammo %.1f life %.1f slots %d %d %d, player 2 ammo %.1f life %.1f slots %d %d %d",
               r1 ? *(float *)(r1 + 8) : -1.0f, *(float *)(p1 + 0x234), *(int *)(p1 + 0x3a8), *(int *)(p1 + 0x3ac),
               *(int *)(p1 + 0x3b0), r2 ? *(float *)(r2 + 8) : -1.0f, *(float *)(p2 + 0x234), *(int *)(p2 + 0x3a8),
               *(int *)(p2 + 0x3ac), *(int *)(p2 + 0x3b0));
        BYTE *n1 = *(BYTE **)(p1 + 0x230);
        float *pp = n1 ? (float *)(n1 + 0x68) : NULL;
        if ((strcmp(what, "list") == 0 || strcmp(what, "listall") == 0) && pp) {
            float lim = what[4] ? 100000.0f : 30.0f;
            BYTE *mgr = *(BYTE **)0xebedec;
            BYTE **items = mgr ? *(BYTE ***)(mgr + 0x14) : NULL;
            int n = mgr ? *(int *)(mgr + 0x18) : 0;
            for (int i = 0; i < n; i++) {
                EStr *m = *(EStr **)(items[i] + 4);
                BYTE *e = *(BYTE **)(items[i] + 0xc), *en = e && !IsBadReadPtr(e, 0x234) ? *(BYTE **)(e + 0x230) : NULL;
                if (!m || !en || IsBadReadPtr(en, 0x74))
                    continue;
                float *q = (float *)(en + 0x68), dx = q[0] - pp[0], dy = q[1] - pp[1], dz = q[2] - pp[2];
                if (dx * dx + dy * dy + dz * dz < lim * lim)
                    ts_log("split: item test: \"%s\" %p at %.2f %.2f %.2f (%.1f m)", m->s, (void *)e, q[0], q[1], q[2],
                           sqrtf(dx * dx + dy * dy + dz * dz));
            }
            stage_t = 2;
            return;
        }
        item_next = 0;
        if (strcmp(what, "grab") == 0 && pp) {   /* (the nearest item seen updating, with its node shown) */
            BYTE *best = NULL;
            float bd = 1e9f;
            for (int i = 0; i < n_upd_items; i++) {
                BYTE *it = upd_items[i], *nd = !IsBadReadPtr(it, 0x234) ? *(BYTE **)(it + 0x230) : NULL;
                if (!nd || IsBadReadPtr(nd, 0x74) || (*(DWORD *)(nd + 4) & 3))
                    continue;
                float *q = (float *)(nd + 0x68), dx = q[0] - pp[0], dy = q[1] - pp[1], dz = q[2] - pp[2], d = dx * dx + dy * dy + dz * dz;
                if (d < bd)
                    bd = d, best = it;
            }
            if (best) {
                BYTE *nd = *(BYTE **)(best + 0x230);
                float pos[3], dir[3] = {-0.89f, -0.45f, 0};
                memcpy(pos, nd + 0x68, 12);
                pos[1] += 0.3f;
                put_player(p2, pos, dir);
                ts_log("split: item test: player 2 put on item %p (\"%.32s\", kind %d) at %.2f %.2f %.2f (%.1f m from player 1)", (void *)best,
                       !IsBadReadPtr(*(char **)(nd + 0x28), 1) ? *(char **)(nd + 0x28) : "?", *(int *)(best + 0x14), pos[0], pos[1],
                       pos[2], sqrtf(bd));
            } else
                ts_log("split: item test: no item seen (%d recorded)", n_upd_items);
            return;
        }
        if (strcmp(what, "nearitems") == 0 && pp) {
            /* (the world's scene nodes (world [0xfcacdc] +0x2ad0, 0xa8c nodes of 0x238 bytes, template at +0x1d0,
             * its name at +0x30): the pick-ups among them recorded too) */
            BYTE *w = *(BYTE **)0xfcacdc, *base = w ? *(BYTE **)(w + 0x2ad0) : NULL;
            for (int i = 0; base && i < 0xa8c && n_items_seen < MAX_ITEMS; i++) {
                BYTE *n = base + i * 0x238, *tpl = *(BYTE **)(n + 0x1d0);
                if (!tpl || IsBadReadPtr(tpl, 0x50))
                    continue;
                const char *tn = (const char *)(tpl + 0x30);
                float *q = (float *)(n + 0x68), dx = q[0] - pp[0], dz = q[2] - pp[2];
                if (splitlog > 2 && dx * dx + dz * dz < 40.0f * 40.0f)
                    ts_log("split: item test: world node %d \"%.40s\" at %.2f %.2f %.2f", i, tn, q[0], q[1], q[2]);
                if (strncmp(tn, "item_", 5) == 0 || strstr(tn, "ammo") || strstr(tn, "medkit") || strncmp(tn, "wpn_drop", 8) == 0) {
                    int dup = 0;
                    for (int k = 0; k < n_items_seen; k++)
                        dup |= items_seen[k].node == n;
                    if (!dup) {
                        items_seen[n_items_seen].node = n;
                        items_seen[n_items_seen].geo = *(BYTE **)(n + 0x1c0);
                        snprintf(items_seen[n_items_seen++].name, 28, "%s", tn);
                    }
                }
            }
            /* (the 8 nearest of the recorded pick-ups, nearest first) */
            float d[MAX_ITEMS];
            int alive = 0;
            for (int k = 0; k < n_items_seen; k++) {
                BYTE *n = items_seen[k].node;
                d[k] = 1e30f;
                if (!n || IsBadReadPtr(n, 0x1c4) || *(BYTE **)(n + 0x1c0) != items_seen[k].geo)
                    continue;
                float *q = (float *)(n + 0x68), dx = q[0] - pp[0], dy = q[1] - pp[1], dz = q[2] - pp[2];
                d[k] = dx * dx + dy * dy + dz * dz;
                alive++;
                ts_log("split: item test: pick-up %s at %.2f %.2f %.2f (%.1f m)", items_seen[k].name, q[0], q[1], q[2], sqrtf(d[k]));
            }
            n_near = 0;
            while (n_near < 8 && n_near < alive) {
                int best = -1;
                for (int k = 0; k < n_items_seen; k++)
                    if (d[k] < 1e29f && (best < 0 || d[k] < d[best]))
                        best = k;
                if (best < 0)
                    break;
                near_idx[n_near++] = best;
                d[best] = 1e30f;
            }
            near_at = 0;
            if (!near_move())
                stage_t = 2;
            return;
        }
        item_move(what, &item_next);
    } else if (stage_t == 1 && age >= (secs + 3 * (item_step + 1)) * 1000) {
        item_step++;
        ts_log("split: item test after: player 1 ammo %.1f life %.1f slots %d %d %d, player 2 ammo %.1f life %.1f slots %d %d %d",
               r1 ? *(float *)(r1 + 8) : -1.0f, *(float *)(p1 + 0x234), *(int *)(p1 + 0x3a8), *(int *)(p1 + 0x3ac),
               *(int *)(p1 + 0x3b0), r2 ? *(float *)(r2 + 8) : -1.0f, *(float *)(p2 + 0x234), *(int *)(p2 + 0x3a8),
               *(int *)(p2 + 0x3ac), *(int *)(p2 + 0x3b0));
        if (strcmp(what, "nearitems") == 0 ? !near_move() : !item_move(what, &item_next))
            stage_t = 2;
    }
}

/* ---- network split screen ----
 * Split screen on a client of a co-op game. The client keeps one player of its own (player 1, as every client);
 * its player 2 is a campaign `player` that the HOST runs, exactly as the host runs its own split-screen player 2:
 *   - the client sends its pad 2 to the host (mod message 60, {u8 version, u8 on, XINPUT_GAMEPAD}, at up to 30 Hz
 *     and on every change); the host feeds it to player 2's command context in place of a local pad 2
 *     (gamepad_set_player2_pad), so player 2 walks, aims, fires, uses powers and picks up items on the host, with
 *     the host's authority over hits, damage, deaths and items;
 *   - player 2 is replicated by the game to every machine as a `player_mp` (as the split host's player 2);
 *   - the host sends the client player 2's camera and state (message 61, {u8 version, u8 flags (1 alive),
 *     i32 net id, float right/up/forward[9], float position[3], float life} at 30 Hz);
 *   - the client renders its second half from that camera (position smoothed), hides the echoed player_mp of its
 *     player 2 in that half, shows its player 1's body there, and hides its player 1's first-person weapon,
 *     crosshair and HUD in that half.
 * One player 2 per host: a host with split screen of its own refuses clients' requests, and only the first client
 * asking is served. The client turns it on with TS_SPLIT=1 or the UI string CoopSplitJoin = "1" and needs a
 * second pad; TS_SPLIT_CLIENT=0 switches the whole feature off on either side. */
#define SUB_NS_PAD 60
#define SUB_NS_STATE 61
#define SUB_NS_MSG 62
#define NS_VERSION 2

#pragma pack(push, 1)
typedef struct { unsigned char ver, on; XINPUT_GAMEPAD pad; } NsPad;
typedef struct {
    unsigned char ver, flags;              /* flags: 1 alive */
    int netid;                             /* player 2 (player_mp on the clients) */
    float rows[9], pos[3], life;           /* its camera (right, up, forward, position), its life */
    float body[3], energy, ammo_total;     /* its body's position, time energy, primary ammunition record */
    int magazine, slots[3];                /* rounds in the magazine, the three weapon slots (player +0x3a8) */
    unsigned char fx_on;                   /* screen effects 6 damage, 8 death, 10 low life: bits 0-2 */
    float fx_v[3];
} NsState;
typedef struct { unsigned char ver, n; unsigned short text[100]; } NsMsg;   /* a HUD message of player 2 (UTF-16) */
#pragma pack(pop)
static const int ns_fx[3] = {6, 8, 10};
static int ns_msg_forward(const void *text);

int gamepad_read(int player, void *out);   /* gamepad.c */

static int net_mode(void)
{
    BYTE *gm = *(BYTE **)0xfcab34;
    return gm ? *(int *)(gm + 0x3c) : 0;
}

/* The player object of a client id in the game's player list ([0xfcab34] +0x28, 0xa4-byte records). */
static BYTE *player_of_client(int id)
{
    BYTE *gm = *(BYTE **)0xfcab34;
    if (!gm)
        return NULL;
    BYTE *slots = *(BYTE **)(gm + 0x28);
    int n = *(int *)(gm + 0x2c);
    for (int i = 0; slots && i < n && i < 32; i++)
        if (*(int *)(slots + i * 0xa4) == id)
            return *(BYTE **)(slots + i * 0xa4 + 4);
    return NULL;
}

static void remote_end(const char *why)
{
    if (p2)
        p2_remove("its client's player 2 ended");
    gamepad_set_player2_pad(NULL);
    ts_log("split: the player 2 of client %d ended (%s)", remote_peer, why);
    remote_host = 0;
    remote_peer = -1;
    remote_mp = NULL;
    stage = 0;
    set_single_thread(0);
}

/* The client's own copy of its player 2 (the "shadow"): a local `player` out of the network game, run with the
 * local pad 2 like the local split screen's player 2, so the second half has the game's own first-person weapon,
 * HUD, screen effects and immediate movement and aim. The host's player 2 stays the real one: the shadow takes
 * its life, time energy, ammunition, weapon slots and screen effects from message 61, and its position when it
 * drifted more than 0.75 m away (or after the host's player 2 died and respawned). Its shots and hits on the
 * client are pictures only (a client has no authority); the host's player 2 fires the same shots. */
static void shadow_sync(const NsState *st)
{
    if (!p2 || !client_split || *(DWORD *)p2 != PLAYER_VTBL)
        return;
    *(float *)(p2 + 0x234) = st->life > 0 ? st->life : 1.0f;
    *(float *)(p2 + 0x2ac) = st->energy;
    BYTE *rec = (int)(GetTickCount() - p2_born) > 3000 ? ammo_rec(p2, 0, NULL) : NULL;   /* (once its weapon exists) */
    if (rec) {
        *(float *)(rec + 8) = st->ammo_total;
        *(int *)(rec + 0xc) = st->magazine;
    }
    /* (the host's weapon slots: given to the copy once it exists for 2 s and whenever the host's change; compared
     * with the last given ones, not with the copy's slots (run 20261026-214145: the copy held no weapon)) */
    static int given[3] = {-1, -1, -1};
    static BYTE *given_to;
    if (given_to != p2)
        given_to = p2, given[0] = given[1] = given[2] = -1;
    if ((int)(GetTickCount() - p2_born) > 2000 && (st->slots[0] | st->slots[1] | st->slots[2])
        && (given[0] != st->slots[0] || given[1] != st->slots[1] || given[2] != st->slots[2])) {
        char ssl[96], out[64];
        memcpy(given, st->slots, sizeof given);
        /* (always, also when the slots read the same: the copy's weapon is not selected until then, its half had no
         * first-person weapon and an empty ammunition panel, run 20261027-005811) */
        snprintf(ssl, sizeof ssl, "$player2.SetupWeapon(%d, %d, %d)", st->slots[0], st->slots[1], st->slots[2]);
        ts_script_run(ssl, out, sizeof out);
        ts_script_run("$player2.SelectHolster(0, 1)", out, sizeof out);
        ts_log("split: our player 2's copy gets the host's weapons %d %d %d and selects the first (%s)", st->slots[0], st->slots[1],
               st->slots[2], out);
    }
    for (int k = 0; k < 3; k++) {
        int i = ns_fx[k];
        fx[1][i].seen = 1;
        fx[1][i].on = st->fx_on >> k & 1;
        fx[1][i].v = st->fx_v[k];
    }
    /* (its position: see client_slave) */
    memcpy(cl_body, st->body, sizeof cl_body);
    cl_body_ok = 1;
}

/* The copy is a slave of the host's player 2 (the copy did not walk with pad 2 in a client's game, run
 * 20261027-012413): every frame its position is moved to the host's player 2's (message 61), smoothed, with its
 * own look direction kept (pad 2's right stick is applied locally, so aim answers at once). Its own movement
 * still runs and is overwritten. A far jump (a respawn, a level change) is taken at once. */
static void client_slave(void)
{
    if (!client_split || !p2 || !cl_body_ok || copy_free || *(DWORD *)p2 != PLAYER_VTBL)
        return;
    BYTE *n = *(BYTE **)(p2 + 0x230);
    if (!n || IsBadReadPtr(n, 0x74))
        return;
    /* (0.6 m to the right of the host's player 2, not on it: on the same spot the host's player 2's replicated shots
     * hit the copy and the game stopped in the hit handler without a shooter, 0x576180, run 20261027-050946) */
    const float *rt = (const float *)cam2;
    float want[3] = {cl_body[0] + rt[0] * 0.6f, cl_body[1], cl_body[2] + rt[2] * 0.6f};
    float *q = (float *)(n + 0x68), d[3] = {want[0] - q[0], want[1] - q[1], want[2] - q[2]};
    float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], k = d2 > 36.0f ? 1.0f : 0.5f;
    if (d2 < 0.0004f)
        return;
    float pos[3] = {q[0] + d[0] * k, q[1] + d[1] * k, q[2] + d[2] * k}, dir[3] = {((float *)cam2)[8], 0, ((float *)cam2)[10]};
    static int nlog;
    if (splitlog && (d2 > 36.0f || nlog++ < 6))
        ts_log("split: our player 2's copy moved to the host's player 2 (%.2f m off, step %.2f)", sqrtf(d2), k);
    put_player_f(p2, pos, dir, 0);
}

/* A message of player 2 from the host, shown in its half (the message line, see hook_msg_add). The text goes in
 * as the game's own does: a holder of a counted wide string (refs, length, capacity, UTF-16 at +0xc; made by the
 * text lookup 0x68f560 -> 0x68f620). Ours are static, in a ring of 8, with a count that never runs out. */
static void client_show_msg(const unsigned short *t, int n)
{
    BYTE *w = *(BYTE **)0x1095ec8;
    static struct { LONG refs; int len, cap; unsigned short s[101]; } ring[8];
    static int next;
    if (!w || !real_msg_add || n <= 0 || n > 100)
        return;
    int i = next++ & 7;
    ring[i].refs = 1000000;
    ring[i].len = ring[i].cap = n;
    memcpy(ring[i].s, t, n * 2);
    ring[i].s[n] = 0;
    void *ws = &ring[i];
    static EStr *cat;   /* (the message channel "console", as the game's pick-up message passes it, 0x440720) */
    if (!cat) {
        cat = eng_alloc(7 + 0x10, 0, 0);
        if (!cat)
            return;
        cat->refs = 1000000, cat->len = cat->cap = 7;
        memcpy(cat->s, "console", 8);
    }
    msg_forwarded = 1;
    real_msg_add(w, &ws, &cat, *(DWORD *)0xd725bc);
    msg_forwarded = 0;
    msg_owner = 2;
    msg_time = GetTickCount();
}

static void ns_message(int peer, unsigned short sub, const unsigned char *data, int len)
{
    int m = net_mode();
    if (net_split_off)
        return;
    if (sub == SUB_NS_PAD && m == 2 && len >= (int)sizeof(NsPad) && data[0] == NS_VERSION) {
        const NsPad *pd = (const NsPad *)data;
        if (!pd->on) {
            if (remote_host && peer == remote_peer)
                remote_end("the client turned it off");
            return;
        }
        if (!remote_host) {
            if (stage) {   /* (this host has split screen of its own: one player 2 per host) */
                static int told;
                if (!told++)
                    ts_log("split: client %d asks for its player 2, but this host has split screen of its own", peer);
                return;
            }
            remote_host = 1;
            remote_peer = peer;
            stage = 3;
            set_single_thread(1);
            p1_since = GetTickCount() - (DWORD)p2_delay * 1000;   /* (player 2 comes in at once) */
            ts_log("split: client %d plays split screen: this host runs its player 2", peer);
        }
        if (peer != remote_peer)
            return;
        gamepad_set_player2_pad(&pd->pad);
        remote_seen = GetTickCount();
    } else if (sub == SUB_NS_STATE && m == 3 && len >= (int)sizeof(NsState) && data[0] == NS_VERSION && client_split) {
        const NsState *st = (const NsState *)data;
        if (st->netid != client_netid) {
            client_netid = st->netid;
            cp2 = NULL;
            ts_log("split: our player 2 is net id %d on the host", client_netid);
        }
        memcpy(cl_rows, st->rows, sizeof cl_rows);
        memcpy(cl_pos, st->pos, sizeof cl_pos);
        cl_life = st->life;
        cl_alive = st->flags & 1;
        if (!cl_have)
            memcpy(cl_spos, cl_pos, sizeof cl_spos);
        cl_have = 1;
        shadow_sync(st);
    } else if (sub == SUB_NS_MSG && m == 3 && len >= 2 && data[0] == NS_VERSION && client_split) {
        const NsMsg *mm = (const NsMsg *)data;
        if (mm->n && mm->n < 100 && len >= 2 + mm->n * 2)
            client_show_msg(mm->text, mm->n);
    }
}

/* A HUD message of the host's player 2 of a client to that client. Its text as the game passes it (0x440720): a
 * holder of a counted wide string (+4 length, UTF-16 at +0xc). 0 = not sent. */
static int ns_msg_forward(const void *text)
{
    BYTE *ws = text && !IsBadReadPtr(text, 4) ? *(BYTE **)text : NULL;
    NsMsg mm = {NS_VERSION, 0, {0}};
    if (!ws || IsBadReadPtr(ws, 0xc))
        return 0;
    int len = *(int *)(ws + 4);
    const unsigned short *t = (const unsigned short *)(ws + 0xc);
    if (len <= 0 || len > 4096 || IsBadReadPtr(t, len * 2))
        return 0;
    while (mm.n < 99 && mm.n < len && t[mm.n])
        mm.text[mm.n] = t[mm.n], mm.n++;
    if (!mm.n)
        return 0;
    BYTE *gm = *(BYTE **)0xfcab34, *slots = gm ? *(BYTE **)(gm + 0x28) : NULL;
    for (int i = 0; slots && i < *(int *)(gm + 0x2c) && i < 32; i++)
        if (*(int *)(slots + i * 0xa4) == remote_peer)
            return coopnet_send(i, SUB_NS_MSG, &mm, 2 + mm.n * 2) > 0;
    return 0;
}

/* Player 2's camera on a client: the host's camera of it, the position smoothed (messages come at 30 Hz, frames
 * faster), the angles of the client's own camera kept (field of view, near/far). */
static void client_cam(BYTE *dst)
{
    float d[3], dd = 0;
    for (int i = 0; i < 3; i++)
        d[i] = cl_pos[i] - cl_spos[i], dd += d[i] * d[i];
    for (int i = 0; i < 3; i++)
        cl_spos[i] = dd > 9.0f ? cl_pos[i] : cl_spos[i] + d[i] * 0.5f;
    memcpy(dst, CAM, CAM_SIZE);
    float *f = (float *)dst;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            f[r * 4 + k] = cl_rows[r * 3 + k];
    cam_set_pos(dst, cl_spos);
}

/* A client's view of player 2: its echoed player_mp hidden, player 1's body shown, player 1's first-person
 * weapon, crosshair and HUD hidden. */
/* The client's player_mp of the host's player 2 of this client (net id from message 61), or NULL. */
static BYTE *find_cp2(void)
{
    if (client_netid >= 0 && (!cp2 || IsBadReadPtr(cp2, 0x234) || *(int *)(cp2 + 0x18) != client_netid)) {
        BYTE *o = find_task((void *)0xecb7f8, client_netid);
        cp2 = o && !IsBadReadPtr(o, 0x234) && *(int *)(o + 0x18) == client_netid ? o : NULL;
    }
    return client_netid >= 0 ? cp2 : NULL;
}

static void client_view(int v)
{
    if (v != 1)
        return;
    find_cp2();
    BYTE *on = cp2 ? *(BYTE **)(cp2 + 0x230) : NULL;
    if (on && !IsBadReadPtr(on, 8)) {
        own_node = on;
        own_flags = *(DWORD *)(on + 4);
        *(DWORD *)(on + 4) |= 0x10000u;
    }
    BYTE *b = body_of(saved_pb), *n = b ? *(BYTE **)(b + 0x230) : NULL;
    if (n && !IsBadReadPtr(n, 8)) {
        shown_node = n;
        shown_flags = *(DWORD *)(n + 4);
        *(DWORD *)(n + 4) &= ~0x10000u;
    }
    n_fp_hidden = 0;
    for (int i = 0; i < n_own1 && n_fp_hidden < 2 * MAX_OWN; i++) {
        BYTE *fn = own1[i] ? *(BYTE **)(own1[i] + 0x230) : NULL;
        if (!fn || IsBadReadPtr(fn, 8))
            continue;
        fp_hidden[n_fp_hidden].node = fn;
        fp_hidden[n_fp_hidden++].saved = *(DWORD *)(fn + 4);
        *(DWORD *)(fn + 4) |= 1;
    }
    for (int i = 0; i < n_crs[0] && n_fp_hidden < 2 * MAX_OWN; i++) {
        if (!crosshair_alive(0, i))
            continue;
        BYTE *fn = crs[0][i].node;
        fp_hidden[n_fp_hidden].node = fn;
        fp_hidden[n_fp_hidden++].saved = *(DWORD *)(fn + 4);
        *(DWORD *)(fn + 4) |= 1;
    }
    if (hud_root && !IsBadReadPtr(hud_root, 0x1c)) {
        hud_hidden = hud_root;
        hud_hidden_flags = *(DWORD *)(hud_root + 0x18);
        *(DWORD *)(hud_root + 0x18) |= 2;
    }
}

/* With the shadow: player 1's view does not show it (the host's player 2 is there as a player_mp), player 2's
 * view does not show the player_mp of our player 2 (it would stand in the camera). */
static void client_extra(int v)
{
    BYTE *o = NULL;
    if (v == 0)
        o = body_of(p2);
    else {
        o = find_cp2();
        BYTE *ob = body_of(o);   /* (a player_mp draws through its body object too, if it has one) */
        if (ob && !IsBadReadPtr(ob, 0x234))
            o = ob;
    }
    BYTE *n = o ? *(BYTE **)(o + 0x230) : NULL;
    if (n && !IsBadReadPtr(n, 0x74)) {
        extra_node = n;
        extra_flags = *(DWORD *)(n + 4);
        *(DWORD *)(n + 4) |= v == 1 ? (1u | 0x10000u) : 1u;   /* (the render prep skips 0x10000 in the main view) */
        if (v == 1 && o == find_cp2()) {   /* (the host's puppet of player 2 is also moved far away for this view: the
                                              flags did not hide it, run 20261027-051718) */
            memcpy(extra_pos, n + 0x68, sizeof extra_pos);
            ((float *)(n + 0x68))[1] += 1000.0f;
            extra_moved = 1;
        }
    }
    static DWORD last[2];   /* (tests: what each view hides, every 3 s) */
    DWORD t = GetTickCount();
    if (splitlog && v >= 0 && v < 2 && t - last[v] >= 3000) {
        last[v] = t;
        BYTE *cn = p2 ? *(BYTE **)(p2 + 0x230) : NULL;
        float *cp = cn ? (float *)(cn + 0x68) : NULL;
        ts_log("split: client view %d hides %p node %p (+4 %08lx vis %d); copy at %.2f %.2f %.2f, cam2 at %.2f %.2f %.2f, netid %d cp2 %p,"
               " copy updates %d", v, (void *)o, (void *)n, n ? *(DWORD *)(n + 4) : 0, node_visible(n), cp ? cp[0] : 0, cp ? cp[1] : 0,
               cp ? cp[2] : 0, ((float *)(cam2 + 0x30))[0], ((float *)(cam2 + 0x30))[1], ((float *)(cam2 + 0x30))[2], client_netid,
               (void *)cp2, copy_updates);
    }
}

/* The client's copy of player 2 removed (split screen on this client ends). */
static void shadow_remove(const char *why)
{
    gamepad_mask_player2(0, 0);
    if (!p2 || !client_split)
        return;
    p2_remove(why);
}

static void net_split_tick(void)
{
    static int listening;
    if (net_split_off)
        return;
    if (!listening)
        listening = 1 + coopnet_listen(SUB_NS_PAD, SUB_NS_MSG, ns_message);
    int m = net_mode();
    DWORD now = GetTickCount();
    if (m == 3) {   /* client */
        int want = (forced > 0 || client_choice) && gamepad_slot(1) >= 0;
        if (want && !client_split) {
            client_split = 1;
            stage = 3;
            set_single_thread(1);
            ts_log("split: split screen on this client: player 2 (pad %d) is run by the host", gamepad_slot(1) + 1);
        } else if (!want && client_split) {
            shadow_remove("split screen off");
            NsPad off = {NS_VERSION, 0, {0}};
            coopnet_send(-1, SUB_NS_PAD, &off, sizeof off);
            client_split = cl_have = cl_body_ok = 0;
            cp2 = NULL;
            client_netid = -1;
            stage = 0;
            set_single_thread(0);
            restore_globals();
            pic_ok[0] = pic_ok[1] = 0;
            ts_log("split: split screen on this client off");
        }
        if (client_split) {   /* (also in menus and loading, with an idle pad: the host keeps player 2) */
            static NsPad last;
            static DWORD sent;
            NsPad pd = {NS_VERSION, 1, {0}};
            if (in_play())
                gamepad_read(1, &pd.pad);
            if ((memcmp(&pd, &last, sizeof pd) || now - sent >= 100) && now - sent >= 33
                && coopnet_send(-1, SUB_NS_PAD, &pd, sizeof pd) > 0)
                last = pd, sent = now;
        }
    } else if (m == 2) {   /* host */
        if (remote_host && now - remote_seen > 3000)
            remote_end("no pad from the client for 3 s");
        if (remote_host) {
            remote_mp = player_of_client(remote_peer);
            static DWORD sent;
            if (p2 && cam2 && now - sent >= 33) {
                NsState st = {NS_VERSION, 0, *(int *)(p2 + 0x18), {0}, {0}, *(float *)(p2 + 0x234)};
                const float *c = (const float *)cam2;
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++)
                        st.rows[r * 3 + k] = c[r * 4 + k];
                memcpy(st.pos, c + 12, sizeof st.pos);
                st.flags = st.life > 0;
                BYTE *bn = *(BYTE **)(p2 + 0x230);
                if (bn)
                    memcpy(st.body, bn + 0x68, sizeof st.body);
                st.energy = *(float *)(p2 + 0x2ac);
                BYTE *rec = ammo_rec(p2, 0, NULL);
                st.ammo_total = rec ? *(float *)(rec + 8) : 0;
                st.magazine = rec ? *(int *)(rec + 0xc) : 0;
                memcpy(st.slots, p2 + 0x3a8, sizeof st.slots);
                for (int k = 0; k < 3; k++) {
                    st.fx_on |= fx[1][ns_fx[k]].on ? 1u << k : 0;
                    st.fx_v[k] = fx[1][ns_fx[k]].v;
                }
                int slot = -1;
                BYTE *gm = *(BYTE **)0xfcab34, *slots = gm ? *(BYTE **)(gm + 0x28) : NULL;
                for (int i = 0; slots && i < *(int *)(gm + 0x2c) && i < 32; i++)
                    if (*(int *)(slots + i * 0xa4) == remote_peer)
                        slot = i;
                if (slot >= 0 && coopnet_send(slot, SUB_NS_STATE, &st, sizeof st) > 0)
                    sent = now;
            }
        }
    } else {
        if (remote_host)
            remote_end("the network game ended");
        if (client_split) {
            shadow_remove("the network game ended");
            client_split = cl_have = cl_body_ok = 0;
            cp2 = NULL;
            client_netid = -1;
            stage = forced > 0 ? 3 : 0;
            if (!stage)
                set_single_thread(0);
        }
    }
}

/* main thread, script tick */
static void split_tick(float dt)
{
    (void)dt;
    split_choice();
    if (splitlog) {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 2000) {
            last = now;
            BYTE *ui = *(BYTE **)0x1095fc8, *gm = *(BYTE **)0xfcab34;
            ts_log("split: state play=%d cine=%d mode=%d ui+c0=%08lx flags=%08lx p1=%p p2=%p cam=%p", in_play(), cinematic(),
                   gm ? *(int *)(gm + 0x3c) : -1, ui ? *(DWORD *)(ui + 0xc0) : 0, *(DWORD *)0xfcace8, (void *)PLAYER_B,
                   (void *)p2, (void *)CAM);
            for (int i = 0; i < 2; i++) {
                BYTE *pl = i ? p2 : (PLAYER_B != p2 ? PLAYER_B : NULL), *b = body_of(pl);
                BYTE *n = b ? *(BYTE **)(b + 0x230) : NULL, *pn = pl ? *(BYTE **)(pl + 0x230) : NULL;
                if (pl && splitlog > 1)   /* (TS_SPLITLOG=2: the players' own objects' scene nodes too) */
                    for (int k = 0; k < (i ? n_own2 : n_own1); k++) {
                        BYTE *o = i ? own2[k] : own1[k], *on = o ? *(BYTE **)(o + 0x230) : NULL;
                        if (on && IsBadReadPtr(on, 12))
                            on = NULL;
                        if (on && !(*(DWORD *)(on + 4) & 3))   /* (the shown ones: +4 bits 0/1 hide a node, 0x9a1331) */
                            ts_log("split:     p%d %-18s %p node %p +4 %08lx +8 %08lx vis %d at %.2f %.2f %.2f", i + 1,
                                   own_cls[i][k], (void *)o, (void *)on, *(DWORD *)(on + 4), *(DWORD *)(on + 8),
                                   node_visible(on), ((float *)(on + 0x68))[0], ((float *)(on + 0x68))[1],
                                   ((float *)(on + 0x68))[2]);
                        if (on && !(*(DWORD *)(on + 4) & 3)) {   /* (and its meshes: node +0x1c4 list, +0x1c8 count, 0x9a1070) */
                            BYTE **ml = *(BYTE ***)(on + 0x1c4);
                            int nm = *(int *)(on + 0x1c8);
                            ts_log("split:       geometry %p meshes %p x%d", *(void **)(on + 0x1c0), (void *)ml, nm);
                            for (int m = 0; (DWORD)ml > 0x10000 && !IsBadReadPtr(ml, 4) && m < nm && m < 3; m++) {
                                BYTE *me = ml[m];
                                if (me && !IsBadReadPtr(me, 0xc0))
                                    ts_log("split:       mesh %p +0 %08lx +4 %08lx +10 %08lx +28 %08lx +bc %08lx", (void *)me,
                                           *(DWORD *)me, *(DWORD *)(me + 4), *(DWORD *)(me + 0x10), *(DWORD *)(me + 0x28),
                                           *(DWORD *)(me + 0xbc));
                            }
                        }
                    }
                if (pl) {
                    BYTE *set = NULL, *rec = ammo_rec(pl, 0, &set);
                    ts_log("split:   player %d net id %d role %08x; body net id %d role %08x", i + 1, *(int *)(pl + 0x18),
                           *(unsigned *)(pl + 0x2c), b ? *(int *)(b + 0x18) : -9, b ? *(unsigned *)(b + 0x2c) : 0);
                    ts_log("split:   player %d weapon set %p ammo record %p total %.1f magazine %d", i + 1, (void *)set,
                           (void *)rec, rec ? *(float *)(rec + 8) : -1.0f, rec ? *(int *)(rec + 0xc) : -1);
                }
                if (pl) {
                    float *c = (float *)((i ? cam2 : (as2.depth ? as2.cam : (swapped ? saved_cam : CAM))) + 0x30);
                    ts_log("split:   player %d camera at %.2f %.2f %.2f", i + 1, c[0], c[1], c[2]);
                }
                if (pl)
                    ts_log("split:   player %d body %p node +4 %08lx +8 %08lx vis %d; player node +4 %08lx +8 %08lx health %.1f time energy %.2f",
                           i + 1, (void *)b, n ? *(DWORD *)(n + 4) : 0, n ? *(DWORD *)(n + 8) : 0, node_visible(n),
                           pn ? *(DWORD *)(pn + 4) : 0, pn ? *(DWORD *)(pn + 8) : 0, *(float *)(pl + 0x234),
                           *(float *)(pl + 0x2ac));   /* (+0x2ac: what the HUD's time-power bar shows, 0x5b5d90) */
            }
        }
    }
    {   /* (a level change: the objects of a removed player 2 are gone; their addresses may be reused) */
        static DWORD lt;
        DWORD t = GetTickCount();
        if (t - lt > 500)
            n_p2_gone = 0, n_crs_gone = 0;
        lt = t;
    }
    net_split_tick();
    if (stage < 3)
        return;
    install_msg_hook();
    /* (the client's copy of player 2 does not fire (both triggers, right stick click), throw grenades or use powers:
     * the host's player 2 does. A shot of the copy crashed the game when its hit found no shooter: 0x576180 with
     * esi 0, run 20261026-221127. Its pictures of the shots come from the host's.) */
    gamepad_mask_player2(client_split ? XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_DPAD_UP
                                            | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_THUMB : 0,
                         client_split);
    if (client_split && (net_noshadow || !cl_have))
        return;   /* (a client's player 2 is run by the host; its local copy only once the host runs it) */
    client_slave();
    {   /* In a network game the player's update shows "Waiting for server" (GAME_WAIT_FOR_SERVER, 0x40c5c6) while
         * its +0x39c bit 1 is clear: the server's "you may play" mark, which the game sets on player 1 only.
         * Player 2, updated as the local player, showed it all the time (in both halves). Player 2 gets player 1's
         * bit. */
        BYTE *gm = *(BYTE **)0xfcab34, *a = PLAYER_B;
        if (p2 && a && a != p2 && gm && (*(int *)(gm + 0x3c) == 2 || *(int *)(gm + 0x3c) == 3)) {
            DWORD m1 = *(DWORD *)(a + 0x39c) & 2, m2 = *(DWORD *)(p2 + 0x39c) & 2;
            static int logged;
            if (m1 != m2 && logged++ < 4)
                ts_log("split: player 2's server mark %lx set to player 1's %lx (+0x39c %08lx / %08lx)", m2, m1,
                       *(DWORD *)(p2 + 0x39c), *(DWORD *)(a + 0x39c));
            *(DWORD *)(p2 + 0x39c) = (*(DWORD *)(p2 + 0x39c) & ~2u) | m1;
        }
    }
    item_test(PLAYER_B != p2 ? PLAYER_B : NULL);
    {   /* (tests, TS_SPLIT_DROPAT=<s>: player 2 removed <s> seconds after it came in) */
        static int dropat = -1;
        char dv[16];
        if (dropat < 0)
            dropat = ts_env("TS_SPLIT_DROPAT", dv, sizeof dv) ? atoi(dv) : 0;
        if (dropat > 0 && p2 && (int)(GetTickCount() - p2_born) > dropat * 1000) {
            dropat = 0;
            p2_remove("test (TS_SPLIT_DROPAT)");
        }
    }
    if (support_check && (int)(GetTickCount() - support_check) >= 0 && p2 && PLAYER_B && PLAYER_B != p2) {
        support_check = 0;
        BYTE *who = support_who, *other = who == p2 ? PLAYER_B : p2;
        BYTE *n1 = *(BYTE **)(who + 0x230), *n2 = *(BYTE **)(other + 0x230);
        if (n1 && n2) {
            float dy = ((float *)(n1 + 0x68))[1] - ((float *)(n2 + 0x68))[1];
            if (dy * dy < 1.0f)
                ts_log("split: player %d stayed beside the partner (height difference %.2f m): on its support", who == p2 ? 2 : 1, dy);
            else {
                ts_log("split: player %d did not stay beside the partner (height difference %.2f m)", who == p2 ? 2 : 1, dy);
                place_player(who, other);
            }
        }
    }
    if (place_retry && (int)(GetTickCount() - place_retry) >= 0 && p2 && PLAYER_B && PLAYER_B != p2) {
        if (place_retry_who == p2)
            place_player(p2, PLAYER_B);
        else if (place_retry_who == PLAYER_B)
            place_player(PLAYER_B, p2);
        else
            place_retry = 0;
    }
    BYTE *p1 = PLAYER_B && *(DWORD *)PLAYER_B == PLAYER_VTBL && PLAYER_B != p2 ? PLAYER_B : NULL;
    if (p1 != p1_seen) {
        p1_seen = p1;
        p1_since = GetTickCount();
        p2_failed = 0;
        if (p2 && !p1)
            ts_log("split: player 1 gone, player 2 kept until the engine removes it");
    }
    /* Spawned only after p2_delay seconds of uninterrupted play (not during the level's opening frames). The tick
     * does not run in the main menu and while a level loads, and player 1 survives level changes (one object from
     * the start of the game), so a gap between ticks restarts the count. */
    static DWORD last_tick;
    DWORD now = GetTickCount();
    static int placed;
    int gap = now - last_tick > 500;   /* (a level load: the tick stopped) */
    static int apart;   /* (the players pulled apart at a level start) */
    if (gap)
        placed = 0, support_tries = 0, support_check = 0, place_retry = 0, apart = 0, hud_root = NULL, n_p2_gone = 0, n_crs_gone = 0;
    if (!in_play() || cinematic() || gap)
        p1_since = now;
    last_tick = now;
    if (gap || !p1)
        trail_clear();   /* (a level load: the old level's spots are gone) */
    /* A level start puts both players on the level's player start (both are players): two bodies in one spot
     * threw both 105 m up in ts2_city_part03 (they hung there, player 1's half red). Player 2 is moved beside
     * player 1 at the first tick of the level, before the bodies push each other apart. */
    if (!apart && p1 && p2 && !gap && (int)(now - p2_born) > 2000) {
        apart = 1;
        BYTE *a = *(BYTE **)(p1 + 0x230), *b = *(BYTE **)(p2 + 0x230);
        if (a && b) {
            float *pa = (float *)(a + 0x68), *pb = (float *)(b + 0x68), dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
            if (dx * dx + dz * dz < 0.25f && dy * dy < 4.0f) {
                ts_log("split: both players on one spot at the level start (%.2f m apart, %.2f m in height): player 2 moved"
                       " beside player 1", sqrtf(dx * dx + dz * dz), dy);
                place_player(p2, p1);
            }
        }
    }
    else if (in_play() && !cinematic()) {
        trail_add(0, p1);
        if (p2)
            trail_add(1, p2);
    }
    /* (player 2 comes in on ground player 1 walked on: once player 1 has moved 1.5 m from a spot it stood on
     * 2 s before; the same after a level change) */
    float spot[3], *p1pos = p1 && *(BYTE **)(p1 + 0x230) ? (float *)(*(BYTE **)(p1 + 0x230) + 0x68) : NULL;
    int walked = p1pos && trail_pick_away(0, p1pos, spot, 0.8f);
    /* (or after 3 s more without that: ahead of player 1, see place_player) */
    BYTE *gmode = *(BYTE **)0xfcab34;
    int on_client = gmode && *(int *)(gmode + 0x3c) == 3 && !client_split;   /* (on a client only the shadow of
                                                                                 client_split, see shadow_sync) */
    if (p1 && !p2 && !on_client && p2_failed < 3 && now - p1_since >= (DWORD)p2_delay * 1000
        && (walked || now - p1_since >= (DWORD)p2_delay * 1000 + 3000))
        spawn_p2(p1);
    /* Player 2 survives a level change like player 1 (both are objects of the game, not of the level): it is put
     * next to player 1 again once the new level is in play. */
    if (p1 && p2 && !placed && now - p1_since >= (DWORD)p2_delay * 1000
        && (walked || now - p1_since >= (DWORD)p2_delay * 1000 + 3000)) {
        placed = 1;
        /* Loading a save rebuilds the players' equipment from the save, which holds player 1's only: player 2
         * comes back without its weapons (an empty weapon, no first-person model; removing and remaking player 2
         * then hung the game, run 20261025-123537). Its weapons are made again with the player's own weapon set-up
         * (vtable +0x25c, 0x442840: weapons and hands, as at its spawn), as player 2. Then, at every level start,
         * player 2 gets player 1's three weapon slots (+0x3a8, +0x3ac, +0x3b0: what the level script or the save
         * gave player 1) through SetupWeapon when they differ. */
        BYTE *rec = ammo_rec(p2, 0, NULL);
        int unarmed = !rec || (*(float *)(rec + 8) <= 0 && *(int *)(rec + 0xc) <= 0);
        if ((int)(now - p2_born) > 2000) {
            if (unarmed)
                p2_rearm();
            place_p2(p1);
        }
        p2_loadout(p1);
    }
}

/* ---- install ---- */
static int patch_slot(DWORD vtbl, int slot, void *hook, void **orig)
{
    void *vt = (void *)vtbl;   /* (ts_patch_vtable takes an object: a pointer to the vtable pointer) */
    void *o = ts_patch_vtable(&vt, slot, hook);
    if (o)
        *orig = o;
    return o != NULL;
}

unsigned char *split_player2(void)
{
    return p2;
}

void split_install(void)
{
    char v[32];
    /* Split screen is a player feature (the Co-op menu's "Split screen": UI string CoopSplit = "1" while it is
     * chosen); the hooks are in place from the start and pass everything through while it is off.
     * TS_SPLIT=1 forces it on for tests (TS_SPLIT=0: never). */
    if (ts_env("TS_SPLIT", v, sizeof v)) {
        forced = v[0] == '1' ? 1 : -1;
        if (forced < 0) {
            ts_log("split: off (TS_SPLIT=0)");
            return;
        }
    }
    stage = forced > 0 ? 3 : 0;
    if (forced > 0 && ts_env("TS_SPLIT_STAGE", v, sizeof v) && v[0] >= '1' && v[0] <= '3')
        stage = v[0] - '0';
    if (ts_env("TS_SPLIT_LAYOUT", v, sizeof v) && (v[0] == 'v' || v[0] == 'h'))
        layout_cfg = v[0];
    keep_mt = ts_env("TS_SPLIT_MT", v, sizeof v) && v[0] == '1';
    no_geo_owner = ts_env("TS_SPLIT_NOGEO", v, sizeof v) && v[0] == '1';   /* (tests: without the owner swap) */
    fp_share = ts_env("TS_SPLIT_FPSHARE", v, sizeof v) && v[0] == '1';
    splitlog = ts_env("TS_SPLITLOG", v, sizeof v) ? atoi(v) : 0;
    msglog = ts_env("TS_SPLIT_MSGLOG", v, sizeof v) && v[0] == '1';
    allow_ff = ts_env("TS_SPLIT_FF", v, sizeof v) && v[0] == '1';   /* (tests: friendly fire on) */
    if (ts_env("TS_SPLIT_P2DELAY", v, sizeof v))
        p2_delay = atoi(v);
    void **rv = (void **)R_VTBL, **pv = (void **)PLAYER_VTBL, **iv = (void **)IM_VTBL;
    if ((DWORD)rv[SLOT_FSTART] != 0x504b30 || (DWORD)rv[SLOT_FEND] != 0x67f290 || (DWORD)pv[SLOT_UPDATE] != 0x4098c0
        || (DWORD)pv[SLOT_DTOR] != 0x406ab0 || (DWORD)iv[SLOT_EVAL] != 0x4f3190) {
        ts_log("split: renderer or player class not found (other exe), off");
        stage = 0;
        return;
    }
    rc = _aligned_malloc(CAM_SIZE, 16);
    cam2 = _aligned_malloc(CAM_SIZE, 16);
    if (!rc || !cam2) {
        stage = 0;
        return;
    }
    memset(cam2, 0, CAM_SIZE);
    unsigned char want[5] = {0xe8}, rep[5] = {0xe8};
    *(DWORD *)(want + 1) = QUAD_FN - (QUAD_CALL + 5);
    *(DWORD *)(rep + 1) = (DWORD)hook_quad - (QUAD_CALL + 5);
    int n = patch_slot(R_VTBL, SLOT_FSTART, (void *)hook_fstart, (void **)&real_fstart)
            + patch_slot(R_VTBL, SLOT_FEND, (void *)hook_fend, (void **)&real_fend)
            + ts_patch_bytes(QUAD_CALL, want, rep, 5);
    {
        unsigned char want[5] = {0xe8}, rep2[5] = {0xe8};
        *(DWORD *)(want + 1) = 0x698020 - (0x654bee + 5);
        *(DWORD *)(rep2 + 1) = (DWORD)create_msg_stub - (0x654bee + 5);
        if (!ts_patch_bytes(0x654bee, want, rep2, 5))
            ts_log("split: entity announcement hook not installed (other exe?)");
    }
    {
        unsigned char want[5] = {0xe8}, rep3[5] = {0xe8};
        *(DWORD *)(want + 1) = 0x5f78e0 - (0x5f53dd + 5);
        *(DWORD *)(rep3 + 1) = (DWORD)stat434_stub - (0x5f53dd + 5);
        if (!ts_patch_bytes(0x5f53dd, want, rep3, 5))
            ts_log("split: player table guard not installed (other exe?)");
    }
    if (!install_light_guard())
        ts_log("split: light pass guard not installed (other exe?)");
    if (stage >= 3 || !forced)
        n += patch_slot(PLAYER_VTBL, SLOT_UPDATE, (void *)hook_update, (void **)&real_update)
             + patch_slot(PLAYER_VTBL, SLOT_DTOR, (void *)hook_dtor, (void **)&real_dtor)
             + patch_slot(PLAYER_VTBL, 0x2e8 / 4, (void *)hook_die, (void **)&real_die)
             + patch_slot(PLAYER_VTBL, 0x25c / 4, (void *)hook_wsetup, (void **)&real_wsetup)
             + patch_slot(PLAYER_VTBL, 0x22c / 4, (void *)hook_event, (void **)&real_event)
             + (((DWORD *)PLAYER_VTBL)[0x17c / 4] == 0x420470
                && patch_slot(PLAYER_VTBL, 0x17c / 4, (void *)hook_pickup, (void **)&real_pickup))
             + patch_slot(IM_VTBL, SLOT_EVAL, (void *)hook_eval, (void **)&real_eval);
    if (stage >= 3 || !forced)
        for (int i = 0; i < 5; i++) {   /* (weapons: 0x447a2e, 0x447a55; hands 0x442868; body: 0x4074b4, 0x4074f9) */
            static const DWORD sites[5] = {0x447a2e, 0x447a55, 0x442868, 0x4074b4, 0x4074f9};
            DWORD at = sites[i];
            unsigned char w[5] = {0xe8}, r[5] = {0xe8};
            *(DWORD *)(w + 1) = 0x655460 - (at + 5);
            *(DWORD *)(r + 1) = (DWORD)spawn_trampoline - (at + 5);
            ts_patch_bytes(at, w, r, 5);
        }
    fx_off = ts_env("TS_SPLIT_FXSHARED", v, sizeof v) && v[0] == '1';
    hudlog = splitlog > 0;
    net_split_off = ts_env("TS_SPLIT_CLIENT", v, sizeof v) && v[0] == '0';
    net_noshadow = ts_env("TS_SPLIT_SHADOW", v, sizeof v) && v[0] == '0';
    copy_mode1 = ts_env("TS_SPLIT_COPYMODE", v, sizeof v) && v[0] == '1';
    copy_free = ts_env("TS_SPLIT_COPYFREE", v, sizeof v) && v[0] == '1';
    host_items_off = ts_env("TS_SPLIT_HOSTITEMS", v, sizeof v) && v[0] == '0';
    if (splitlog && *(DWORD *)(0xd74c28 + 0x13c) == 0x42fa90)
        patch_slot(0xd74c28, 0x13c / 4, (void *)hook_mp_update, (void **)&real_mp_update);
    install_name_hook();   /* (also without split screen: player 1 with a pad) */
    if (ts_env("TS_SPLIT_NETLOCAL", v, sizeof v))
        net_local = v[0] != '0';
    if (ts_env("TS_SPLIT_MSGNEXT", v, sizeof v))
        msg_next = v[0] != '0';
    fp_noscale = ts_env("TS_SPLIT_FPNOSCALE", v, sizeof v) && v[0] == '1';
    if (stage >= 3 || !forced) {   /* (the weapon pass's projection rebuild, see split_fp_proj) */
        static const unsigned char want[5] = {0xe8, 0x74, 0x3d, 0xfa, 0xff};
        unsigned char rep[5] = {0xe8};
        *(DWORD *)(rep + 1) = (DWORD)fp_proj_stub - (0x680917 + 5);
        if (!ts_patch_bytes(0x680917, want, rep, 5))
            ts_log("split: weapon pass not found (first-person weapons may be cut in a half)");
    }
    if ((stage >= 3 || !forced) && install_item_hooks() != 4)
        ts_log("split: item hooks not all set (pick-ups may not reach player 2)");
    if (splitlog > 1)
        ts_log("split: %d item give methods logged (tests)", install_give_hooks());
    if (stage >= 3 || !forced) {
        int nh = install_hud_hooks();
        if (nh != N_HUD)
            ts_log("split: %d of %d HUD widget updates hooked (the others show player 1 in both halves)", nh, N_HUD);
    }
    if (ts_env("TS_SPLIT_HUDNEXT", v, sizeof v))   /* (tests: which frame parity the HUD panel takes player 2 on) */
        hud_view1 = v[0] == '0' ? 0 : 1;
    if (ts_env("TS_SPLIT_FXVIEW", v, sizeof v)) {
        fx_per_view = 0;
        for (char *t = strtok(v, ","); t; t = strtok(NULL, ","))
            if (atoi(t) >= 0 && atoi(t) < N_FX)
                fx_per_view |= 1u << atoi(t);
    }
    if ((stage >= 3 || !forced) && !fx_off && !install_fx_hooks())
        ts_log("split: screen effect calls not found (effects shared by both views)");
    if ((stage >= 3 || !forced) && !install_dispatch_hook())
        ts_log("split: scheduler dispatch not found (player 2's weapons run as player 1)");
    if (stage >= 3 || !forced) {
        unsigned char w[5] = {0xe8}, r[5] = {0xe8};
        *(DWORD *)(w + 1) = 0x95dcf0 - (0x6551be + 5);
        *(DWORD *)(r + 1) = (DWORD)hook_mknode - (0x6551be + 5);
        if (!ts_patch_bytes(0x6551be, w, r, 5))
            ts_log("split: node creation call not found");
    }
    gamepad_after_poll(publish_pad2);
    if ((stage >= 3 || splitlog || !forced) && !ts_frame_add(split_tick))
        ts_log("split: no script tick, player 2 is never spawned");
    if (stage)
        set_single_thread(1);
    ts_log("split: %s, stage %d, layout %s, %d hooks, %s frame", forced > 0 ? "on (TS_SPLIT)" : "ready (Co-op menu)", stage, layout_cfg ? (layout_cfg == 'v' ? "side by side" : "top/bottom") : "auto",
           n, keep_mt ? "multi-thread" : "single-thread (while split screen is on)");
}
