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
 *
 * Checked 2026-10-25 on a 12-thread CPU (Windows 11): without 1-2 the start crashes at 0x946499 (as on
 * the owner's first launch); without 3 the game never creates its Direct3D device (stays without a
 * window); with all three it reaches the main menu.
 */
#include <windows.h>

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

#define DRIVER_CHECK_JE 0x007738e6

void compat_install(void)
{
    SYSTEM_INFO si;
    char v[8];
    GetSystemInfo(&si);
    /* tests: TS_NOCOMPAT=1 the game's own start-up, =cpu or =driver without that fix only */
    ts_env("TS_NOCOMPAT", v, sizeof v);
    int no_cpu = v[0] == '1' || v[0] == 'c', no_drv = v[0] == '1' || v[0] == 'd';
    if (!no_cpu) {
        real_GetSystemInfo = (GetSystemInfo_t)ts_patch_iat("KERNEL32.dll", "GetSystemInfo", (void *)hook_GetSystemInfo);
        real_GetProcessAffinityMask = (GetProcessAffinityMask_t)ts_patch_iat("KERNEL32.dll", "GetProcessAffinityMask",
                                                                             (void *)hook_GetProcessAffinityMask);
    }
    static const unsigned char je[2] = {0x74, 0x0d}, jmp[2] = {0xeb, 0x0d};
    int drv = !no_drv && ts_patch_bytes(DRIVER_CHECK_JE, je, jmp, 2);
    ts_log("compat: %lu processors reported as at most %d (%s), driver check %s", si.dwNumberOfProcessors, MAX_CPUS,
           real_GetSystemInfo && real_GetProcessAffinityMask ? "hooked" : "NOT hooked",
           drv ? "passed" : no_drv ? "off (TS_NOCOMPAT)" : "not patched (other exe)");
}
