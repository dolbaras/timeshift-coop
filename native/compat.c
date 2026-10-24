/*
 * Start-up fixes for today's PCs, the same three changes as the community's "pre-patched" 1.2 exe
 * (Steam guide "[FIX] CPU and Startup loading crash", 2019), made in memory instead of in the exe:
 *
 * 1. More than 8 logical processors crash the engine at start (its per-thread tables hold 8: on a
 *    12-thread CPU the start crashed at 0x946499 reading a NULL entry of such a table). GetSystemInfo
 *    reports at most 8 processors.
 * 2. GetProcessAffinityMask reports only the first 8 processors, for the same reason (the engine
 *    pins its worker threads by these masks).
 * 3. The start-up adapter check compares the display driver's version against values of its time
 *    and, for today's driver numbers, calls a handler (0x4fcaa0) that fails the start; the check
 *    passes instead (je -> jmp at 0x7738e6). Only when the bytes match patch 1.2's exe.
 * 4. The same start-up check wants 100 MB of dedicated video memory (0x7734b6). Integrated GPUs report less
 *    (the N100's Intel UHD: the game ended right after GetDeviceCaps, ExitProcess(0)); the check is
 *    skipped as the game's own VIDEO_IgnoreVidMemCheck setting would (jae -> jmp).
 * 5. Vendors other than NVIDIA and ATI: see compat_on_d3d below.
 *
 * Checked 2026-10-25 on a 12-thread CPU (Windows 11): without 1-2 the start crashes at 0x946499 (as on
 * the owner's first launch); without 3 the game never creates its Direct3D device (stays without a
 * window); with all three it reaches the main menu.
 */
#include <windows.h>
#include <d3d9.h>
#include <wchar.h>

#include "ts.h"

typedef void (WINAPI *GetSystemInfo_t)(LPSYSTEM_INFO);
typedef BOOL (WINAPI *GetProcessAffinityMask_t)(HANDLE, PDWORD_PTR, PDWORD_PTR);
static GetSystemInfo_t real_GetSystemInfo;
static GetProcessAffinityMask_t real_GetProcessAffinityMask;

#define MAX_CPUS 8

static void WINAPI hook_GetSystemInfo(LPSYSTEM_INFO si)
{
    real_GetSystemInfo(si);
    if (si->dwNumberOfProcessors > MAX_CPUS)
        si->dwNumberOfProcessors = MAX_CPUS;
    si->dwActiveProcessorMask &= (1u << MAX_CPUS) - 1;
}

static BOOL WINAPI hook_GetProcessAffinityMask(HANDLE p, PDWORD_PTR proc, PDWORD_PTR sys)
{
    BOOL ok = real_GetProcessAffinityMask(p, proc, sys);
    if (ok) {
        if (proc)
            *proc &= (1u << MAX_CPUS) - 1;
        if (sys)
            *sys &= (1u << MAX_CPUS) - 1;
    }
    return ok;
}

/* The engine formats its start-up check messages ("Driver ... don't support ...", the driver version) into
 * a wide buffer and shows them nowhere a test can read: 0x7728b0 (buffer in edx, format and arguments
 * on the stack, vswprintf at 0xa61780) is replaced by the same call plus a log line. */
#define FMT_MSG 0x007728b0
#define FMT_VSWPRINTF 0x00a61780

void __cdecl compat_log_wide(const wchar_t *msg)
{
    char t[600];
    WideCharToMultiByte(CP_UTF8, 0, msg, -1, t, sizeof t, NULL, NULL);
    ts_log("engine: %s", t);
}

__attribute__((naked)) static void hook_fmt_msg(void)
{
    __asm__ volatile(
        ".intel_syntax noprefix\n"
        "push edx\n"                        /* the buffer, kept for the log */
        "mov ecx, [esp + 8]\n"              /* format */
        "lea eax, [esp + 12]\n"             /* va_list */
        "push eax\n"
        "push ecx\n"
        "push edx\n"
        "mov eax, 0x00a61780\n"
        "call eax\n"
        "add esp, 12\n"
        "push eax\n"
        "push dword ptr [esp + 4]\n"
        "call _compat_log_wide\n"
        "add esp, 4\n"
        "pop eax\n"
        "pop edx\n"
        "ret\n"
        ".att_syntax prefix\n");
}

/* 5. The engine has shader sets for NVIDIA and ATI only (pc_sm30_nv*, pc_sm30_ati*, table at 0xebecc0):
 *    it picks one from the adapter's vendor, and any other vendor ends the start with "exposes unsupported
 *    shader platform" (Intel UHD on the N100). Other vendors are reported as NVIDIA (0x10DE), whose SM3
 *    set the GPU runs like any other Direct3D 9 shader. */
typedef HRESULT (WINAPI *GetAdapterIdentifier_t)(IDirect3D9 *, UINT, DWORD, D3DADAPTER_IDENTIFIER9 *);
static GetAdapterIdentifier_t real_GetAdapterIdentifier;
static int vendor_off;

static HRESULT WINAPI hook_GetAdapterIdentifier(IDirect3D9 *d3d, UINT a, DWORD flags, D3DADAPTER_IDENTIFIER9 *id)
{
    HRESULT hr = real_GetAdapterIdentifier(d3d, a, flags, id);
    if (SUCCEEDED(hr) && id && id->VendorId != 0x10de && id->VendorId != 0x1002) {
        static LONG logged;
        if (InterlockedIncrement(&logged) == 1)
            ts_log("compat: adapter %u \"%s\" vendor 0x%04lx reported as NVIDIA (shader set)", a, id->Description,
                   id->VendorId);
        id->VendorId = 0x10de;
    }
    return hr;
}

void compat_on_d3d(void *d3dv)
{
    IDirect3D9 *d3d = (IDirect3D9 *)d3dv;
    if (vendor_off)
        return;
    void *p = ts_patch_vtable(d3d, 5, (void *)hook_GetAdapterIdentifier);
    if (p)
        real_GetAdapterIdentifier = (GetAdapterIdentifier_t)p;
}

#define DRIVER_CHECK_JE 0x007738e6
#define VIDMEM_CHECK_JAE 0x007734b6

void compat_install(void)
{
    SYSTEM_INFO si;
    char v[8];
    GetSystemInfo(&si);
    /* tests: TS_NOCOMPAT=1 the game's own start-up, =cpu or =driver without that fix only */
    ts_env("TS_NOCOMPAT", v, sizeof v);
    int no_cpu = v[0] == '1' || v[0] == 'c', no_drv = v[0] == '1' || v[0] == 'd';
    vendor_off = v[0] == '1' || v[0] == 'v';
    if (!no_cpu) {
        real_GetSystemInfo = (GetSystemInfo_t)ts_patch_iat("KERNEL32.dll", "GetSystemInfo", (void *)hook_GetSystemInfo);
        real_GetProcessAffinityMask = (GetProcessAffinityMask_t)ts_patch_iat("KERNEL32.dll", "GetProcessAffinityMask",
                                                                             (void *)hook_GetProcessAffinityMask);
    }
    static const unsigned char je[2] = {0x74, 0x0d}, jmp[2] = {0xeb, 0x0d};
    int drv = !no_drv && ts_patch_bytes(DRIVER_CHECK_JE, je, jmp, 2);
    static const unsigned char jae[2] = {0x73, 0x23}, jmp2[2] = {0xeb, 0x23};
    int vmem = !no_drv && ts_patch_bytes(VIDMEM_CHECK_JAE, jae, jmp2, 2);
    static const unsigned char fmt_start[5] = {0x8b, 0x4c, 0x24, 0x04, 0x8d};   /* mov ecx,[esp+4]; lea */
    unsigned char jmp5[5] = {0xe9};
    *(DWORD *)(jmp5 + 1) = (DWORD)hook_fmt_msg - (FMT_MSG + 5);
    if (!ts_patch_bytes(FMT_MSG, fmt_start, jmp5, 5))
        ts_log("compat: engine message formatter not found (other exe)");
    ts_log("compat: %lu processors reported as at most %d (%s), driver check %s, video memory check %s",
           si.dwNumberOfProcessors, MAX_CPUS, real_GetSystemInfo && real_GetProcessAffinityMask ? "hooked" : "NOT hooked",
           drv ? "passed" : no_drv ? "off (TS_NOCOMPAT)" : "not patched (other exe)",
           vmem ? "skipped" : no_drv ? "kept (TS_NOCOMPAT)" : "not patched (other exe)");
}
