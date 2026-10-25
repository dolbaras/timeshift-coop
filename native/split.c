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
 *   player 2: gamepad.c writes pad 2 there, the keyboard and mouse read as idle).
 * - Sound: the listener update runs in the game update, outside the render span, so it follows player 1.
 *
 *   TS_SPLIT=1           on (prototype; off unless set)
 *   TS_SPLIT_STAGE=n     1: the normal frame in one half only; 2: + a second camera (an offset copy of
 *                        player 1's) on alternate frames; 3 (default): + player 2
 *   TS_SPLIT_LAYOUT=     auto (default: side by side when the screen is wider than 3:2), v (side by side), h
 *   TS_SPLIT_MT=1        keep the engine's worker threads (default: single-thread frame in split mode)
 *   TS_SPLIT_P2DELAY=s   seconds after player 1 appears until player 2 is spawned (default 5)
 *   TS_SPLITLOG=1        frame phases, threads and globals of the first frames in the log
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

/* The player's weapon spawns (0x4478e0 calls 0x655460 at two places). A second `player` gets no object for most
 * first-person weapon classes (wpn_plr_pistol and on: the spawn returns NULL, and the engine then writes through
 * it at 0x447a5c); for player 2 the network players' class of the same weapon (wpn_mp_*) is tried then. */
static int spawning_p2;

__attribute__((used)) void *__cdecl split_weapon_spawn(void *xform, const char *name, const char *cls, int id)
{
    void *r = spawn_entity(xform, name, cls, id);
    const char *used = cls;
    char alt[64];
    if (!r && spawning_p2 && cls && strncmp(cls, "wpn_plr_", 8) == 0 && strlen(cls) < sizeof alt - 1) {
        snprintf(alt, sizeof alt, "wpn_mp_%s", cls + 8);
        r = spawn_entity(xform, name, alt, id);
        used = alt;
    }
    if (splitlog || (spawning_p2 && used != cls))
        ts_log("split: weapon spawn%s class \"%s\" -> %p", spawning_p2 ? " (player 2)" : "", used ? used : "(null)", r);
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
static BYTE *rc;                /* the render camera: a copy of the view's camera with the half-screen fov */
static BYTE *cam2;              /* player 2's camera (stage 2: an offset copy of player 1's) */

static BYTE *p2;                /* player 2 */
static BYTE *p1_seen;           /* the player 1 object p2 was made next to (a new one: new level) */
static DWORD p1_since;
static BYTE *ctx2;              /* player 2's command context (a copy of GAME with its own entries) */
static BYTE *raw2;              /* player 2's raw input block */
static DWORD imf2_a[5], imf2_b[4];
static int p2_failed;
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
        if (view == 1 && p2)
            PLAYER_A = PLAYER_B = p2;
    } else
        pic_ok[0] = pic_ok[1] = 0;   /* (back to the whole screen: the old halves are stale) */
    if (splitlog && view >= 0 && logged < 60) {
        logged++;
        ts_log("split: frame start view %d thread %lu cam %p pa %p pb %p p2 %p cores %d", view, GetCurrentThreadId(),
               (void *)CAM, (void *)PLAYER_A, (void *)PLAYER_B, (void *)p2, CORES);
    }
    real_fstart(r);
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

static void copy_surface(IDirect3DDevice9 *dev, IDirect3DSurface9 *from, IDirect3DTexture9 *to_tex, const RECT *to_rect,
                         IDirect3DSurface9 *to_surf, D3DTEXTUREFILTERTYPE f, const char *what)
{
    IDirect3DSurface9 *s = to_surf;
    if (!s && FAILED(IDirect3DTexture9_GetSurfaceLevel(to_tex, 0, &s)))
        return;
    HRESULT hr = IDirect3DDevice9_StretchRect(dev, from, NULL, s, to_rect, f);
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
    if (ok) {
        copy_surface(dev, src, pic[k], NULL, NULL, D3DTEXF_NONE, "view picture copy failed");
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
                HRESULT hr = IDirect3DDevice9_StretchRect(dev, s, NULL, rt, &r[i], D3DTEXF_LINEAR);
                if (FAILED(hr))
                    complain("half copy failed", hr);
                IDirect3DSurface9_Release(s);
            }
        }
    IDirect3DSurface9_Release(rt);
    if (splitlog && logged < 60) {
        logged++;
        ts_log("split: composite view %d into %ld,%ld-%ld,%ld (pool %d %ux%u fmt %d) thread %lu", k, r[k].left, r[k].top,
               r[k].right, r[k].bottom, POOL_CUR, d.Width, d.Height, d.Format, GetCurrentThreadId());
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
static void TSCALL hook_update(void *pl)
{
    if (!p2 || pl != p2 || !IM) {
        real_update(pl);
        return;
    }
    BYTE *im = IM, *pa = PLAYER_A, *pb = PLAYER_B, *cam = CAM, *ctx1 = *(BYTE **)(im + 0x40);
    int with_ctx = ctx2 && ctx1 && *(DWORD *)ctx1 == VT_GAME;
    DWORD a1[5], b1[4];
    PLAYER_A = PLAYER_B = p2;
    CAM = cam2;
    if (with_ctx) {
        memcpy(a1, im + IMF_A, sizeof a1);
        memcpy(b1, im + IMF_B, sizeof b1);
        memcpy(im + IMF_A, imf2_a, sizeof a1);
        memcpy(im + IMF_B, imf2_b, sizeof b1);
        *(BYTE **)(im + 0x40) = ctx2;
    }
    real_update(pl);
    if (with_ctx) {
        *(BYTE **)(im + 0x40) = ctx1;
        memcpy(imf2_a, im + IMF_A, sizeof a1);
        memcpy(imf2_b, im + IMF_B, sizeof b1);
        memcpy(im + IMF_A, a1, sizeof a1);
        memcpy(im + IMF_B, b1, sizeof b1);
    }
    PLAYER_A = pa;
    PLAYER_B = pb;
    CAM = cam;
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
    p1_since = GetTickCount();   /* (a level change removes it: a new one after the delay in the next level) */
    p2_failed = 0;
    void *r = real_dtor(pl, flags);
    PLAYER_A = pa == pl ? NULL : pa;
    PLAYER_B = pb == pl ? NULL : pb;
    ts_log("split: player 2 destroyed");
    return r;
}

static const char *script_name(BYTE *ent)
{
    BYTE *inst = *(BYTE **)(ent + 0x80);   /* (script instance: +4 name string, +0xc the entity) */
    if (!inst || IsBadReadPtr(inst, 0x10) || *(BYTE **)(inst + 0xc) != ent || !*(BYTE **)(inst + 4))
        return "?";
    return (const char *)(*(BYTE **)(inst + 4) + 0xc);
}

static void spawn_p2(BYTE *p1)
{
    BYTE *pa = PLAYER_A, *pb = PLAYER_B, *cam = CAM;
    BYTE *node1 = *(BYTE **)(p1 + 0x230);
    /* (the object name names a placed object the new one is made from, found through [0xfcace4] vtable
     * +0x34: player 1's own "player" serves as the template; a name of nothing placed gives no object) */
    spawning_p2 = 1;
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
    ts_log("split: player 2 spawned %p (script name \"%s\"; player 1 %p \"%s\")", (void *)e, script_name(e), (void *)p1,
           script_name(p1));
    /* (single player names the object "player" in its init: player 2 gets its own name back) */
    BYTE *node2 = *(BYTE **)(e + 0x230);
    if (node2)
        node_name(node2, "player2");
    ts_log("split: names now: player 1 \"%s\", player 2 \"%s\"", script_name(p1), script_name(e));
    /* next to player 1, facing the same way, placed as player 2 */
    if (node1) {
        float *fw = (float *)(node1 + 0x58), *pp = (float *)(node1 + 0x68);
        float pos[3] = {pp[0] + fw[2] * 1.5f, pp[1] + 0.3f, pp[2] - fw[0] * 1.5f};
        float dir[3] = {fw[0], fw[1], fw[2]};
        PLAYER_A = PLAYER_B = p2;
        CAM = cam2;
        ((place_t)(*(void ***)p2)[SLOT_PLACE])(p2, pos, dir, 1);
        PLAYER_A = pa;
        PLAYER_B = pb;
        CAM = cam;
        ts_log("split: player 1 at %.2f %.2f %.2f, player 2 placed at %.2f %.2f %.2f", pp[0], pp[1], pp[2], pos[0], pos[1],
               pos[2]);
    }
}

/* main thread, script tick */
static void split_tick(float dt)
{
    (void)dt;
    if (splitlog) {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 2000) {
            last = now;
            BYTE *ui = *(BYTE **)0x1095fc8, *gm = *(BYTE **)0xfcab34;
            ts_log("split: state play=%d cine=%d mode=%d ui+c0=%08lx flags=%08lx p1=%p p2=%p cam=%p", in_play(), cinematic(),
                   gm ? *(int *)(gm + 0x3c) : -1, ui ? *(DWORD *)(ui + 0xc0) : 0, *(DWORD *)0xfcace8, (void *)PLAYER_B,
                   (void *)p2, (void *)CAM);
        }
    }
    if (stage < 3)
        return;
    BYTE *p1 = PLAYER_B && *(DWORD *)PLAYER_B == PLAYER_VTBL && PLAYER_B != p2 ? PLAYER_B : NULL;
    if (p1 != p1_seen) {
        p1_seen = p1;
        p1_since = GetTickCount();
        p2_failed = 0;
        if (p2 && !p1)
            ts_log("split: player 1 gone, player 2 kept until the engine removes it");
    }
    /* Spawned only after p2_delay seconds of uninterrupted play. The tick does not run in the main menu and while a
     * level loads, and player 1 survives level changes (one object from the start of the game), so a gap between
     * ticks restarts the count: spawned at the first tick of a level, player 2's weapons were not made (the
     * spawn returned no object and the engine crashed, 0x447a5c). */
    static DWORD last_tick;
    DWORD now = GetTickCount();
    if (!in_play() || cinematic() || now - last_tick > 500)
        p1_since = now;
    last_tick = now;
    if (p1 && !p2 && p2_failed < 3 && now - p1_since >= (DWORD)p2_delay * 1000)
        spawn_p2(p1);
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
    if (!ts_env("TS_SPLIT", v, sizeof v) || v[0] != '1')
        return;
    stage = 3;
    if (ts_env("TS_SPLIT_STAGE", v, sizeof v) && v[0] >= '1' && v[0] <= '3')
        stage = v[0] - '0';
    if (ts_env("TS_SPLIT_LAYOUT", v, sizeof v) && (v[0] == 'v' || v[0] == 'h'))
        layout_cfg = v[0];
    keep_mt = ts_env("TS_SPLIT_MT", v, sizeof v) && v[0] == '1';
    splitlog = ts_env("TS_SPLITLOG", v, sizeof v) && v[0] == '1';
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
    int skip = ts_env("TS_SPLIT_SKIP", v, sizeof v) ? atoi(v) : 0;   /* (bisecting: 1 update, 2 dtor, 4 eval, 8 tick) */
    if (stage >= 3)
        n += (skip & 1 ? 0 : patch_slot(PLAYER_VTBL, SLOT_UPDATE, (void *)hook_update, (void **)&real_update))
             + (skip & 2 ? 0 : patch_slot(PLAYER_VTBL, SLOT_DTOR, (void *)hook_dtor, (void **)&real_dtor))
             + (skip & 4 ? 0 : patch_slot(IM_VTBL, SLOT_EVAL, (void *)hook_eval, (void **)&real_eval));
    if (skip & 8)
        stage = stage >= 3 ? 2 : stage;
    if (stage >= 3)
        for (DWORD at = 0x447a2e; at; at = at == 0x447a2e ? 0x447a55 : 0) {
            unsigned char w[5] = {0xe8}, r[5] = {0xe8};
            *(DWORD *)(w + 1) = 0x655460 - (at + 5);
            *(DWORD *)(r + 1) = (DWORD)spawn_trampoline - (at + 5);
            ts_patch_bytes(at, w, r, 5);
        }
    if ((stage >= 3 || splitlog) && !ts_frame_add(split_tick))
        ts_log("split: no script tick, player 2 is never spawned");
    if (!keep_mt) {   /* (as [CONFIG] DisableMultithreading = 1: the frame reads it once, into these) */
        MT_ONCE |= 1;
        MT_OFF = 1;
    }
    ts_log("split: on, stage %d, layout %s, %d hooks, %s frame", stage, layout_cfg ? (layout_cfg == 'v' ? "side by side" : "top/bottom") : "auto",
           n, keep_mt ? "multi-thread" : "single-thread");
}
