/*
 * Direct3D 9Ex for the game (on by default; TS_D3D9EX=0 keeps plain Direct3D 9).
 *
 * Why: a plain Direct3D 9 device is "lost" when another program enters or leaves exclusive full screen
 * (or the display mode changes), and the game never resets it: its present step (0x79bf50) only skips
 * Present while TestCooperativeLevel says D3DERR_DEVICELOST and has no Reset path at all, so the picture
 * stays black for good (seen 2026-10-25, docs/multiplayer.md 6a). A Direct3D 9Ex device in a window is
 * never lost that way.
 *
 * How: Direct3DCreate9 returns an IDirect3D9Ex (Direct3DCreate9Ex) and CreateDevice becomes CreateDeviceEx.
 * 9Ex refuses D3DPOOL_MANAGED, which the game and D3DX use for every texture and most buffers:
 * - a managed texture (plain, cube, volume) is created in D3DPOOL_DEFAULT with a twin of the same shape in
 *   D3DPOOL_SYSTEMMEM; locks, dirty rects and the level surfaces/volumes the game asks for go to the twin,
 *   the texture is marked dirty, and SetTexture copies a dirty twin over with UpdateTexture first (the
 *   managed pool did the same copy behind the game's back). GetLevelDesc still reports D3DPOOL_MANAGED.
 *   A level surface handed out earlier and locked later is caught by the surface LockRect hook (its container
 *   is a twin).
 * - managed vertex and index buffers become D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC (lockable, kept on the GPU).
 * TS_D3DCENSUS=1 logs what the game creates (by kind and pool), also without 9Ex.
 *
 * Checked 2026-10-25 on the main PC (Intel UHD 630, windowed): with 9Ex the menus, a campaign level (co-op
 * host on ts2_city_part02) and its autosave look as before, and the game kept drawing while another program
 * (lab/fsgrab) held exclusive full screen on the other monitor for 3 s; without it the device was lost and
 * never came back (runs 20261025-101131, -101516, -101356). The game creates ~940 managed textures and a few
 * managed buffers by its main menu.
 */
#include <windows.h>
#include <d3d9.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

#define SLOT(vtbl, m) ((int)(offsetof(vtbl, m) / sizeof(void *)))

static int on, census;
static CRITICAL_SECTION lock;

/* ---- census ---- */
enum { K_TEX, K_CUBE, K_VOL, K_VB, K_IB, K_KINDS };
static const char *kind_name[K_KINDS] = {"texture", "cube", "volume", "vertex buffer", "index buffer"};
static LONG counts[K_KINDS][4];   /* by pool: default, managed, systemmem, scratch */
static LONG created;

static void count(int kind, D3DPOOL pool)
{
    if (!census)
        return;
    if ((unsigned)pool < 4)
        InterlockedIncrement(&counts[kind][pool]);
    LONG n = InterlockedIncrement(&created);
    if (n == 1 || n % 1000 == 0) {
        char line[512];
        int o = snprintf(line, sizeof line, "d3dex: %ld resources created:", (long)n);
        for (int k = 0; k < K_KINDS; k++)
            o += snprintf(line + o, sizeof line - o, " %s %ld/%ld/%ld/%ld", kind_name[k], (long)counts[k][0],
                          (long)counts[k][1], (long)counts[k][2], (long)counts[k][3]);
        ts_log("%s (default/managed/system/scratch)", line);
    }
}

/* ---- converted textures: owner (default pool) -> twin (system memory) ---- */
typedef struct {
    IDirect3DBaseTexture9 *owner, *twin;
    int kind;
    volatile LONG dirty;
} conv_t;

typedef struct {
    void **keys;
    conv_t **vals;
    unsigned cap, used;   /* (used counts tombstones too) */
} map_t;
static map_t by_owner, by_twin;
#define TOMB ((void *)1)

static unsigned hash_ptr(void *p, unsigned cap) { return ((unsigned)(UINT_PTR)p >> 4) * 2654435761u & (cap - 1); }

static void map_put(map_t *m, void *k, conv_t *v);

static void map_grow(map_t *m)
{
    map_t old = *m;
    m->cap = old.cap ? old.cap * 2 : 4096;
    m->keys = (void **)calloc(m->cap, sizeof(void *));
    m->vals = (conv_t **)calloc(m->cap, sizeof(conv_t *));
    m->used = 0;
    for (unsigned i = 0; i < old.cap; i++)
        if (old.keys[i] && old.keys[i] != TOMB)
            map_put(m, old.keys[i], old.vals[i]);
    free(old.keys);
    free(old.vals);
}

static void map_put(map_t *m, void *k, conv_t *v)
{
    if ((m->used + 1) * 2 > m->cap)
        map_grow(m);
    unsigned i = hash_ptr(k, m->cap);
    while (m->keys[i] && m->keys[i] != TOMB && m->keys[i] != k)
        i = (i + 1) & (m->cap - 1);
    if (!m->keys[i])
        m->used++;
    m->keys[i] = k;
    m->vals[i] = v;
}

static conv_t *map_get(map_t *m, void *k)
{
    if (!m->cap || !k)
        return NULL;
    unsigned i = hash_ptr(k, m->cap);
    while (m->keys[i]) {
        if (m->keys[i] == k)
            return m->vals[i];
        i = (i + 1) & (m->cap - 1);
    }
    return NULL;
}

static void map_del(map_t *m, void *k)
{
    if (!m->cap)
        return;
    unsigned i = hash_ptr(k, m->cap);
    while (m->keys[i]) {
        if (m->keys[i] == k) {
            m->keys[i] = TOMB;
            m->vals[i] = NULL;
            return;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

static conv_t *find_owner(void *p)
{
    EnterCriticalSection(&lock);
    conv_t *c = map_get(&by_owner, p);
    LeaveCriticalSection(&lock);
    return c;
}

/* ---- texture methods (one vtable per class, shared with every texture of that class: twins and textures that
 * were not converted pass straight through) ---- */
typedef ULONG (WINAPI *Release_t)(IUnknown *);
static Release_t real_TexRelease, real_CubeRelease, real_VolRelease;

static ULONG release_common(Release_t real, IUnknown *t)
{
    conv_t *c = find_owner(t);
    ULONG r = real(t);
    if (r == 0 && c) {
        EnterCriticalSection(&lock);
        map_del(&by_owner, c->owner);
        map_del(&by_twin, c->twin);
        LeaveCriticalSection(&lock);
        IDirect3DBaseTexture9_Release(c->twin);
        free(c);
    }
    return r;
}
static ULONG WINAPI hook_TexRelease(IUnknown *t) { return release_common(real_TexRelease, t); }
static ULONG WINAPI hook_CubeRelease(IUnknown *t) { return release_common(real_CubeRelease, t); }
static ULONG WINAPI hook_VolRelease(IUnknown *t) { return release_common(real_VolRelease, t); }

/* lock flags a system-memory twin accepts */
#define TWIN_LOCK(f) ((f) & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE | D3DLOCK_NOSYSLOCK))

typedef HRESULT (WINAPI *TexLevelDesc_t)(IDirect3DTexture9 *, UINT, D3DSURFACE_DESC *);
typedef HRESULT (WINAPI *TexSurf_t)(IDirect3DTexture9 *, UINT, IDirect3DSurface9 **);
typedef HRESULT (WINAPI *TexLock_t)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (WINAPI *TexUnlock_t)(IDirect3DTexture9 *, UINT);
typedef HRESULT (WINAPI *TexDirty_t)(IDirect3DTexture9 *, const RECT *);
static TexLevelDesc_t real_TexDesc;
static TexSurf_t real_TexSurf;
static TexLock_t real_TexLock;
static TexUnlock_t real_TexUnlock;
static TexDirty_t real_TexDirty;

static HRESULT WINAPI hook_TexDesc(IDirect3DTexture9 *t, UINT l, D3DSURFACE_DESC *d)
{
    HRESULT hr = real_TexDesc(t, l, d);
    if (SUCCEEDED(hr) && d && find_owner(t))
        d->Pool = D3DPOOL_MANAGED;
    return hr;
}

static HRESULT WINAPI hook_TexSurf(IDirect3DTexture9 *t, UINT l, IDirect3DSurface9 **s)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_TexSurf(t, l, s);
    c->dirty = 1;
    return IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)c->twin, l, s);
}

static HRESULT WINAPI hook_TexLock(IDirect3DTexture9 *t, UINT l, D3DLOCKED_RECT *lr, const RECT *r, DWORD f)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_TexLock(t, l, lr, r, f);
    if (!(f & D3DLOCK_READONLY))
        c->dirty = 1;
    return IDirect3DTexture9_LockRect((IDirect3DTexture9 *)c->twin, l, lr, r, TWIN_LOCK(f));
}

static HRESULT WINAPI hook_TexUnlock(IDirect3DTexture9 *t, UINT l)
{
    conv_t *c = find_owner(t);
    return c ? IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)c->twin, l) : real_TexUnlock(t, l);
}

static HRESULT WINAPI hook_TexDirty(IDirect3DTexture9 *t, const RECT *r)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_TexDirty(t, r);
    c->dirty = 1;
    return IDirect3DTexture9_AddDirtyRect((IDirect3DTexture9 *)c->twin, r);
}

typedef HRESULT (WINAPI *CubeLevelDesc_t)(IDirect3DCubeTexture9 *, UINT, D3DSURFACE_DESC *);
typedef HRESULT (WINAPI *CubeSurf_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, IDirect3DSurface9 **);
typedef HRESULT (WINAPI *CubeLock_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
typedef HRESULT (WINAPI *CubeUnlock_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT);
typedef HRESULT (WINAPI *CubeDirty_t)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, const RECT *);
static CubeLevelDesc_t real_CubeDesc;
static CubeSurf_t real_CubeSurf;
static CubeLock_t real_CubeLock;
static CubeUnlock_t real_CubeUnlock;
static CubeDirty_t real_CubeDirty;

static HRESULT WINAPI hook_CubeDesc(IDirect3DCubeTexture9 *t, UINT l, D3DSURFACE_DESC *d)
{
    HRESULT hr = real_CubeDesc(t, l, d);
    if (SUCCEEDED(hr) && d && find_owner(t))
        d->Pool = D3DPOOL_MANAGED;
    return hr;
}

static HRESULT WINAPI hook_CubeSurf(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES f, UINT l, IDirect3DSurface9 **s)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_CubeSurf(t, f, l, s);
    c->dirty = 1;
    return IDirect3DCubeTexture9_GetCubeMapSurface((IDirect3DCubeTexture9 *)c->twin, f, l, s);
}

static HRESULT WINAPI hook_CubeLock(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES f, UINT l, D3DLOCKED_RECT *lr,
                                    const RECT *r, DWORD fl)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_CubeLock(t, f, l, lr, r, fl);
    if (!(fl & D3DLOCK_READONLY))
        c->dirty = 1;
    return IDirect3DCubeTexture9_LockRect((IDirect3DCubeTexture9 *)c->twin, f, l, lr, r, TWIN_LOCK(fl));
}

static HRESULT WINAPI hook_CubeUnlock(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES f, UINT l)
{
    conv_t *c = find_owner(t);
    return c ? IDirect3DCubeTexture9_UnlockRect((IDirect3DCubeTexture9 *)c->twin, f, l) : real_CubeUnlock(t, f, l);
}

static HRESULT WINAPI hook_CubeDirty(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES f, const RECT *r)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_CubeDirty(t, f, r);
    c->dirty = 1;
    return IDirect3DCubeTexture9_AddDirtyRect((IDirect3DCubeTexture9 *)c->twin, f, r);
}

typedef HRESULT (WINAPI *VolLevelDesc_t)(IDirect3DVolumeTexture9 *, UINT, D3DVOLUME_DESC *);
typedef HRESULT (WINAPI *VolLevel_t)(IDirect3DVolumeTexture9 *, UINT, IDirect3DVolume9 **);
typedef HRESULT (WINAPI *VolLock_t)(IDirect3DVolumeTexture9 *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD);
typedef HRESULT (WINAPI *VolUnlock_t)(IDirect3DVolumeTexture9 *, UINT);
typedef HRESULT (WINAPI *VolDirty_t)(IDirect3DVolumeTexture9 *, const D3DBOX *);
static VolLevelDesc_t real_VolDesc;
static VolLevel_t real_VolLevel;
static VolLock_t real_VolLock;
static VolUnlock_t real_VolUnlock;
static VolDirty_t real_VolDirty;

static HRESULT WINAPI hook_VolDesc(IDirect3DVolumeTexture9 *t, UINT l, D3DVOLUME_DESC *d)
{
    HRESULT hr = real_VolDesc(t, l, d);
    if (SUCCEEDED(hr) && d && find_owner(t))
        d->Pool = D3DPOOL_MANAGED;
    return hr;
}

static HRESULT WINAPI hook_VolLevel(IDirect3DVolumeTexture9 *t, UINT l, IDirect3DVolume9 **v)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_VolLevel(t, l, v);
    c->dirty = 1;
    return IDirect3DVolumeTexture9_GetVolumeLevel((IDirect3DVolumeTexture9 *)c->twin, l, v);
}

static HRESULT WINAPI hook_VolLock(IDirect3DVolumeTexture9 *t, UINT l, D3DLOCKED_BOX *lb, const D3DBOX *b, DWORD f)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_VolLock(t, l, lb, b, f);
    if (!(f & D3DLOCK_READONLY))
        c->dirty = 1;
    return IDirect3DVolumeTexture9_LockBox((IDirect3DVolumeTexture9 *)c->twin, l, lb, b, TWIN_LOCK(f));
}

static HRESULT WINAPI hook_VolUnlock(IDirect3DVolumeTexture9 *t, UINT l)
{
    conv_t *c = find_owner(t);
    return c ? IDirect3DVolumeTexture9_UnlockBox((IDirect3DVolumeTexture9 *)c->twin, l) : real_VolUnlock(t, l);
}

static HRESULT WINAPI hook_VolDirty(IDirect3DVolumeTexture9 *t, const D3DBOX *b)
{
    conv_t *c = find_owner(t);
    if (!c)
        return real_VolDirty(t, b);
    c->dirty = 1;
    return IDirect3DVolumeTexture9_AddDirtyBox((IDirect3DVolumeTexture9 *)c->twin, b);
}

/* A level surface of a twin locked any time later: its texture is dirty again. */
typedef HRESULT (WINAPI *SurfLock_t)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
static SurfLock_t real_SurfLock;

static HRESULT WINAPI hook_SurfLock(IDirect3DSurface9 *s, D3DLOCKED_RECT *lr, const RECT *r, DWORD f)
{
    HRESULT hr = real_SurfLock(s, lr, r, f);
    if (SUCCEEDED(hr) && !(f & D3DLOCK_READONLY) && by_twin.cap) {
        IDirect3DBaseTexture9 *t = NULL;
        if (SUCCEEDED(IDirect3DSurface9_GetContainer(s, &IID_IDirect3DBaseTexture9, (void **)&t)) && t) {
            EnterCriticalSection(&lock);
            conv_t *c = map_get(&by_twin, t);
            if (c)
                c->dirty = 1;
            LeaveCriticalSection(&lock);
            IDirect3DBaseTexture9_Release(t);
        }
    }
    return hr;
}

static void hook_class(void *obj, int kind)
{
    static int done[3];
    void *p;
    if (done[kind])
        return;
    done[kind] = 1;
    switch (kind) {
    case K_TEX:
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DTexture9Vtbl, Release), (void *)hook_TexRelease)))
            real_TexRelease = (Release_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DTexture9Vtbl, GetLevelDesc), (void *)hook_TexDesc)))
            real_TexDesc = (TexLevelDesc_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DTexture9Vtbl, GetSurfaceLevel), (void *)hook_TexSurf)))
            real_TexSurf = (TexSurf_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DTexture9Vtbl, LockRect), (void *)hook_TexLock)))
            real_TexLock = (TexLock_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DTexture9Vtbl, UnlockRect), (void *)hook_TexUnlock)))
            real_TexUnlock = (TexUnlock_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DTexture9Vtbl, AddDirtyRect), (void *)hook_TexDirty)))
            real_TexDirty = (TexDirty_t)p;
        break;
    case K_CUBE:
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DCubeTexture9Vtbl, Release), (void *)hook_CubeRelease)))
            real_CubeRelease = (Release_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DCubeTexture9Vtbl, GetLevelDesc), (void *)hook_CubeDesc)))
            real_CubeDesc = (CubeLevelDesc_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DCubeTexture9Vtbl, GetCubeMapSurface), (void *)hook_CubeSurf)))
            real_CubeSurf = (CubeSurf_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DCubeTexture9Vtbl, LockRect), (void *)hook_CubeLock)))
            real_CubeLock = (CubeLock_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DCubeTexture9Vtbl, UnlockRect), (void *)hook_CubeUnlock)))
            real_CubeUnlock = (CubeUnlock_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DCubeTexture9Vtbl, AddDirtyRect), (void *)hook_CubeDirty)))
            real_CubeDirty = (CubeDirty_t)p;
        break;
    case K_VOL:
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DVolumeTexture9Vtbl, Release), (void *)hook_VolRelease)))
            real_VolRelease = (Release_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DVolumeTexture9Vtbl, GetLevelDesc), (void *)hook_VolDesc)))
            real_VolDesc = (VolLevelDesc_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DVolumeTexture9Vtbl, GetVolumeLevel), (void *)hook_VolLevel)))
            real_VolLevel = (VolLevel_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DVolumeTexture9Vtbl, LockBox), (void *)hook_VolLock)))
            real_VolLock = (VolLock_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DVolumeTexture9Vtbl, UnlockBox), (void *)hook_VolUnlock)))
            real_VolUnlock = (VolUnlock_t)p;
        if ((p = ts_patch_vtable(obj, SLOT(IDirect3DVolumeTexture9Vtbl, AddDirtyBox), (void *)hook_VolDirty)))
            real_VolDirty = (VolDirty_t)p;
        break;
    }
}

/* The surface class of a twin's levels gets the late-lock check. (A twin may be of another class than its
 * owner: the hooks above reach it through its own vtable, never through the owner class's real_* pointers.) */
static void hook_twin(void *twin, int kind)
{
    static int surf_done;
    if (kind == K_VOL || surf_done)
        return;
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = kind == K_TEX ? IDirect3DTexture9_GetSurfaceLevel((IDirect3DTexture9 *)twin, 0, &s)
                               : IDirect3DCubeTexture9_GetCubeMapSurface((IDirect3DCubeTexture9 *)twin,
                                                                         D3DCUBEMAP_FACE_POSITIVE_X, 0, &s);
    if (SUCCEEDED(hr) && s) {
        void *p = ts_patch_vtable(s, SLOT(IDirect3DSurface9Vtbl, LockRect), (void *)hook_SurfLock);
        if (p)
            real_SurfLock = (SurfLock_t)p;
        surf_done = 1;
        IDirect3DSurface9_Release(s);
    }
}

static void track(IDirect3DBaseTexture9 *owner, IDirect3DBaseTexture9 *twin, int kind)
{
    conv_t *c = (conv_t *)calloc(1, sizeof *c);
    c->owner = owner;
    c->twin = twin;
    c->kind = kind;
    EnterCriticalSection(&lock);
    map_put(&by_owner, owner, c);
    map_put(&by_twin, twin, c);
    LeaveCriticalSection(&lock);
}

/* ---- device ---- */
typedef HRESULT (WINAPI *CreateTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                          IDirect3DTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *CreateVolumeTexture_t)(IDirect3DDevice9 *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                IDirect3DVolumeTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *CreateCubeTexture_t)(IDirect3DDevice9 *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                              IDirect3DCubeTexture9 **, HANDLE *);
typedef HRESULT (WINAPI *CreateVB_t)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
typedef HRESULT (WINAPI *CreateIB_t)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
typedef HRESULT (WINAPI *SetTexture_t)(IDirect3DDevice9 *, DWORD, IDirect3DBaseTexture9 *);
typedef HRESULT (WINAPI *UpdateTexture_t)(IDirect3DDevice9 *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *);
static CreateTexture_t real_CreateTexture;
static CreateVolumeTexture_t real_CreateVolumeTexture;
static CreateCubeTexture_t real_CreateCubeTexture;
static CreateVB_t real_CreateVB;
static CreateIB_t real_CreateIB;
static SetTexture_t real_SetTexture;
static UpdateTexture_t real_UpdateTexture;
static LONG conv_fails;

static void conv_failed(const char *what, HRESULT hr)
{
    if (InterlockedIncrement(&conv_fails) <= 10)
        ts_log("d3dex: managed %s could not be made (hr=0x%08lx)", what, (unsigned long)hr);
}

static HRESULT WINAPI hook_CreateTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT lv, DWORD use, D3DFORMAT f,
                                         D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *sh)
{
    count(K_TEX, pool);
    if (!on || pool != D3DPOOL_MANAGED || !out)
        return real_CreateTexture(d, w, h, lv, use, f, pool, out, sh);
    IDirect3DTexture9 *t = NULL, *twin = NULL;
    HRESULT hr = real_CreateTexture(d, w, h, lv, use, f, D3DPOOL_DEFAULT, &t, sh);
    if (FAILED(hr)) {
        conv_failed("texture", hr);
        return hr;
    }
    /* (an autogen-mipmap texture takes only its top level from the twin) */
    UINT twin_levels = (use & D3DUSAGE_AUTOGENMIPMAP) ? 1 : IDirect3DTexture9_GetLevelCount(t);
    hr = real_CreateTexture(d, w, h, twin_levels, 0, f, D3DPOOL_SYSTEMMEM, &twin, NULL);
    if (FAILED(hr)) {
        IDirect3DTexture9_Release(t);
        conv_failed("texture twin", hr);
        return hr;
    }
    hook_class(t, K_TEX);
    hook_twin(twin, K_TEX);
    track((IDirect3DBaseTexture9 *)t, (IDirect3DBaseTexture9 *)twin, K_TEX);
    *out = t;
    return D3D_OK;
}

static HRESULT WINAPI hook_CreateCubeTexture(IDirect3DDevice9 *d, UINT e, UINT lv, DWORD use, D3DFORMAT f,
                                             D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *sh)
{
    count(K_CUBE, pool);
    if (!on || pool != D3DPOOL_MANAGED || !out)
        return real_CreateCubeTexture(d, e, lv, use, f, pool, out, sh);
    IDirect3DCubeTexture9 *t = NULL, *twin = NULL;
    HRESULT hr = real_CreateCubeTexture(d, e, lv, use, f, D3DPOOL_DEFAULT, &t, sh);
    if (FAILED(hr)) {
        conv_failed("cube texture", hr);
        return hr;
    }
    UINT twin_levels = (use & D3DUSAGE_AUTOGENMIPMAP) ? 1 : IDirect3DCubeTexture9_GetLevelCount(t);
    hr = real_CreateCubeTexture(d, e, twin_levels, 0, f, D3DPOOL_SYSTEMMEM, &twin, NULL);
    if (FAILED(hr)) {
        IDirect3DCubeTexture9_Release(t);
        conv_failed("cube texture twin", hr);
        return hr;
    }
    hook_class(t, K_CUBE);
    hook_twin(twin, K_CUBE);
    track((IDirect3DBaseTexture9 *)t, (IDirect3DBaseTexture9 *)twin, K_CUBE);
    *out = t;
    return D3D_OK;
}

static HRESULT WINAPI hook_CreateVolumeTexture(IDirect3DDevice9 *d, UINT w, UINT h, UINT dp, UINT lv, DWORD use,
                                               D3DFORMAT f, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *sh)
{
    count(K_VOL, pool);
    if (!on || pool != D3DPOOL_MANAGED || !out)
        return real_CreateVolumeTexture(d, w, h, dp, lv, use, f, pool, out, sh);
    IDirect3DVolumeTexture9 *t = NULL, *twin = NULL;
    HRESULT hr = real_CreateVolumeTexture(d, w, h, dp, lv, use, f, D3DPOOL_DEFAULT, &t, sh);
    if (FAILED(hr)) {
        conv_failed("volume texture", hr);
        return hr;
    }
    hr = real_CreateVolumeTexture(d, w, h, dp, IDirect3DVolumeTexture9_GetLevelCount(t), 0, f, D3DPOOL_SYSTEMMEM,
                                  &twin, NULL);
    if (FAILED(hr)) {
        IDirect3DVolumeTexture9_Release(t);
        conv_failed("volume texture twin", hr);
        return hr;
    }
    hook_class(t, K_VOL);
    hook_twin(twin, K_VOL);
    track((IDirect3DBaseTexture9 *)t, (IDirect3DBaseTexture9 *)twin, K_VOL);
    *out = t;
    return D3D_OK;
}

static HRESULT WINAPI hook_CreateVB(IDirect3DDevice9 *d, UINT len, DWORD use, DWORD fvf, D3DPOOL pool,
                                    IDirect3DVertexBuffer9 **out, HANDLE *sh)
{
    count(K_VB, pool);
    if (on && pool == D3DPOOL_MANAGED) {
        pool = D3DPOOL_DEFAULT;
        use |= D3DUSAGE_DYNAMIC;
    }
    HRESULT hr = real_CreateVB(d, len, use, fvf, pool, out, sh);
    if (FAILED(hr) && on)
        conv_failed("vertex buffer", hr);
    return hr;
}

static HRESULT WINAPI hook_CreateIB(IDirect3DDevice9 *d, UINT len, DWORD use, D3DFORMAT f, D3DPOOL pool,
                                    IDirect3DIndexBuffer9 **out, HANDLE *sh)
{
    count(K_IB, pool);
    if (on && pool == D3DPOOL_MANAGED) {
        pool = D3DPOOL_DEFAULT;
        use |= D3DUSAGE_DYNAMIC;
    }
    HRESULT hr = real_CreateIB(d, len, use, f, pool, out, sh);
    if (FAILED(hr) && on)
        conv_failed("index buffer", hr);
    return hr;
}

static HRESULT WINAPI hook_SetTexture(IDirect3DDevice9 *d, DWORD stage, IDirect3DBaseTexture9 *t)
{
    if (t) {
        conv_t *c = find_owner(t);
        if (c && c->dirty) {
            c->dirty = 0;
            HRESULT hr = real_UpdateTexture(d, c->twin, c->owner);
            static LONG fails;
            if (FAILED(hr) && InterlockedIncrement(&fails) <= 5)
                ts_log("d3dex: texture upload failed (hr=0x%08lx)", (unsigned long)hr);
        }
    }
    return real_SetTexture(d, stage, t);
}

static void hook_device(IDirect3DDevice9 *dev)
{
    void *p;
    if ((p = ts_patch_vtable(dev, SLOT(IDirect3DDevice9Vtbl, CreateTexture), (void *)hook_CreateTexture)))
        real_CreateTexture = (CreateTexture_t)p;
    if ((p = ts_patch_vtable(dev, SLOT(IDirect3DDevice9Vtbl, CreateVolumeTexture), (void *)hook_CreateVolumeTexture)))
        real_CreateVolumeTexture = (CreateVolumeTexture_t)p;
    if ((p = ts_patch_vtable(dev, SLOT(IDirect3DDevice9Vtbl, CreateCubeTexture), (void *)hook_CreateCubeTexture)))
        real_CreateCubeTexture = (CreateCubeTexture_t)p;
    if ((p = ts_patch_vtable(dev, SLOT(IDirect3DDevice9Vtbl, CreateVertexBuffer), (void *)hook_CreateVB)))
        real_CreateVB = (CreateVB_t)p;
    if ((p = ts_patch_vtable(dev, SLOT(IDirect3DDevice9Vtbl, CreateIndexBuffer), (void *)hook_CreateIB)))
        real_CreateIB = (CreateIB_t)p;
    if (on) {
        real_UpdateTexture = (UpdateTexture_t)dev->lpVtbl->UpdateTexture;
        if ((p = ts_patch_vtable(dev, SLOT(IDirect3DDevice9Vtbl, SetTexture), (void *)hook_SetTexture)))
            real_SetTexture = (SetTexture_t)p;
    }
}

/* ---- IDirect3D9 -> IDirect3D9Ex ---- */
typedef HRESULT (WINAPI *CreateDevice_t)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                         IDirect3DDevice9 **);
static CreateDevice_t real_CreateDevice;

static HRESULT WINAPI hook_CreateDevice(IDirect3D9 *d3d, UINT a, D3DDEVTYPE t, HWND w, DWORD fl,
                                        D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr;
    if (on) {
        D3DDISPLAYMODEEX mode, *pm = NULL;
        if (pp && !pp->Windowed) {   /* a full-screen device names its mode */
            memset(&mode, 0, sizeof mode);
            mode.Size = sizeof mode;
            mode.Width = pp->BackBufferWidth;
            mode.Height = pp->BackBufferHeight;
            mode.RefreshRate = pp->FullScreen_RefreshRateInHz;
            mode.Format = pp->BackBufferFormat;
            mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            pm = &mode;
        }
        hr = IDirect3D9Ex_CreateDeviceEx((IDirect3D9Ex *)d3d, a, t, w, fl, pp, pm, (IDirect3DDevice9Ex **)out);
        ts_log("d3dex: CreateDeviceEx windowed=%d -> hr=0x%08lx", pp ? pp->Windowed : -1, (unsigned long)hr);
    } else {
        hr = real_CreateDevice(d3d, a, t, w, fl, pp, out);
    }
    if (SUCCEEDED(hr) && out && *out)
        hook_device(*out);
    return hr;
}

typedef HRESULT (WINAPI *Create9Ex_t)(UINT, IDirect3D9Ex **);

IDirect3D9 *d3dex_create(UINT sdk, IDirect3D9 *(WINAPI *create9)(UINT))
{
    IDirect3D9 *d3d = NULL;
    if (on) {
        Create9Ex_t c = (Create9Ex_t)GetProcAddress(GetModuleHandleA("d3d9.dll"), "Direct3DCreate9Ex");
        IDirect3D9Ex *ex = NULL;
        HRESULT hr = c ? c(sdk, &ex) : E_NOTIMPL;
        if (SUCCEEDED(hr) && ex) {
            d3d = (IDirect3D9 *)ex;
            ts_log("d3dex: Direct3D 9Ex in use");
        } else {
            on = 0;
            ts_log("d3dex: Direct3DCreate9Ex failed (hr=0x%08lx): plain Direct3D 9", (unsigned long)hr);
        }
    }
    if (!d3d)
        d3d = create9(sdk);
    if (d3d && (on || census)) {
        void *p = ts_patch_vtable(d3d, SLOT(IDirect3D9Vtbl, CreateDevice), (void *)hook_CreateDevice);
        if (p)
            real_CreateDevice = (CreateDevice_t)p;
    }
    return d3d;
}

void d3dex_install(void)
{
    char v[8];
    InitializeCriticalSection(&lock);
    on = !(ts_env("TS_D3D9EX", v, sizeof v) && v[0] == '0');
    census = (ts_env("TS_D3DCENSUS", v, sizeof v) && v[0] == '1') || on;
    if (on)
        ts_log("d3dex: on (TS_D3D9EX=0 turns it off)");
}
