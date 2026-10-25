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
static int fp_share;   /* TS_SPLIT_FPSHARE=1: player 2's first-person nodes share player 1's geometry (owner swaps) */

/* The crosshairs are scene nodes too (templates "crs_*", made with each player's weapons, kept in front of the
 * player's camera): in the other player's view a player's crosshair floats in front of that player's body
 * (player 2's showed at player 2's head in player 1's half). Each view hides the other player's crosshair nodes
 * like its first-person nodes. Recorded at their creation, with their geometry to know they still exist. */
#define MAX_CRS 32
static struct { BYTE *node, *geo; } crs[2][MAX_CRS];
static int n_crs[2];

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
static int view = -1;           /* the view this frame renders (-1: no split this frame) */
static unsigned frame_no;
static int swapped;
static BYTE *saved_cam, *saved_pa, *saved_pb;
static float saved_canvas;
static BYTE *shown_node;        /* the other player's body node, shown for this view */
static DWORD shown_flags;
static BYTE *body_of(BYTE *pl);
static void fx_apply_view(int k);
static void fx_restore(void);
static int p2_depth(void);
static void place_p2(BYTE *p1);
static void place_player(BYTE *pl, BYTE *partner);
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
    BYTE *c = CINE;
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

static void restore_globals(void)
{
    if (!swapped)
        return;
    CAM = saved_cam;
    PLAYER_A = saved_pa;
    PLAYER_B = saved_pb;
    CANVAS_W = saved_canvas;
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
            if (stage == 2 || !p2)
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
        if (stage >= 2 && hr.bottom > hr.top)
            CANVAS_W = CANVAS_H * (float)(hr.right - hr.left) / (float)(hr.bottom - hr.top);
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
            BYTE *other = view == 0 ? p2 : saved_pb;
            BYTE *b = body_of(other), *n = b ? *(BYTE **)(b + 0x230) : NULL;
            shown_node = n;
            if (n) {
                shown_flags = *(DWORD *)(n + 4);
                *(DWORD *)(n + 4) &= ~0x10000u;
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
        }
    } else
        pic_ok[0] = pic_ok[1] = 0;   /* (back to the whole screen: the old halves are stale) */
    if (splitlog && view >= 0 && logged < 60) {
        logged++;
        ts_log("split: frame start view %d thread %lu cam %p pa %p pb %p p2 %p cores %d", view, GetCurrentThreadId(),
               (void *)CAM, (void *)PLAYER_A, (void *)PLAYER_B, (void *)p2, CORES);
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
    if (!p2 || !ctx2 || !raw2 || !ctx1 || *(DWORD *)ctx1 != VT_GAME)
        return;
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

static void p2_enter(void)
{
    if (as2.depth++)
        return;
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
    /* Down together (the partner died less than 5 s ago): the game's own death, the restart for both. */
    DWORD now = GetTickCount();
    if (died_at[!who] && now - died_at[!who] < 5000) {
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

/* Pick-ups: the item calls the toucher's vtable +0x17c (0x420470: the item's class checked, +0x2a4 decides,
 * +0x284 takes it). Logged with the player it went to. */
static int (TSCALL *real_pickup)(void *, void *);
static int TSCALL hook_pickup(void *pl, void *item)
{
    int r = real_pickup(pl, item);
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

/* The destructor clears both player pointers: player 2's must leave them on player 1. */
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

static void put_player(BYTE *pl, float *pos, float *dir)
{
    if (pl == p2) {
        p2_enter();
        ((place_t)(*(void ***)p2)[SLOT_PLACE])(p2, pos, dir, 1);
        p2_leave();
    } else
        ((place_t)(*(void ***)pl)[SLOT_PLACE])(pl, pos, dir, 1);
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
    } else if (trail_pick(partner == p2 ? 1 : 0, pp, pos))
        how = "on the partner's trail";
    else if (trail_own(pl == p2 ? 1 : 0, pp, pos))
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
typedef struct { LONG refs; int len, cap; char s[1]; } EStr;
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
    n_own2 = 0;
    n_crs[1] = 0;
    n_geo2 = 0;
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
static void split_choice(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 1000)
        return;
    last = now;
    char out[64];
    if (forced)
        return;
    ts_script_run("return $ui.GetStr(\"CoopSplit\")", out, sizeof out);
    int want = strcmp(out, "1") == 0;
    if (want && !stage) {
        stage = 3;
        set_single_thread(1);
        ts_log("split: on (chosen in the Co-op menu; player 2 on XInput pad %d)", gamepad_slot(1) + 1);
    } else if (!want && stage) {
        if (p2) {   /* (player 2 belongs to the game, not the level: removed when split screen ends) */
            p2_removing = 1;
            ts_script_run("$player2.Terminate()", out, sizeof out);
            ts_log("split: player 2 removed (%s)", out);
        }
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
static void p2_rearm(void)
{
    if (!p2 || (DWORD)(*(void ***)p2)[0x25c / 4] != 0x442840) {
        ts_log("split: player 2's weapons not made again (other exe?)");
        return;
    }
    n_own2 = 0;
    n_crs[1] = 0;
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
    if (stage < 3)
        return;
    item_test(PLAYER_B != p2 ? PLAYER_B : NULL);
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
    if (gap)
        placed = 0;
    if (!in_play() || cinematic() || gap)
        p1_since = now;
    last_tick = now;
    if (gap || !p1)
        trail_clear();   /* (a level load: the old level's spots are gone) */
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
    if (p1 && !p2 && p2_failed < 3 && now - p1_since >= (DWORD)p2_delay * 1000
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
    if (stage >= 3 || !forced)
        n += patch_slot(PLAYER_VTBL, SLOT_UPDATE, (void *)hook_update, (void **)&real_update)
             + patch_slot(PLAYER_VTBL, SLOT_DTOR, (void *)hook_dtor, (void **)&real_dtor)
             + patch_slot(PLAYER_VTBL, 0x2e8 / 4, (void *)hook_die, (void **)&real_die)
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
