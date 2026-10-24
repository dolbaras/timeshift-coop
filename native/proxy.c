/*
 * dinput8.dll proxy for TimeShift (2007, 32-bit, patch 1.2).
 *
 * Placed next to bin\TimeShift.exe, it forwards DirectInput8Create to the system DLL and installs
 * the mod's hooks: start-up fixes for today's PCs (compat.c), test-mode window handling
 * (testmode.c) and a crash log. The exe on disk is never modified.
 *
 *   TS_LOG=<file>     the mod's log (default: ts_coop.log next to the exe)
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

typedef HRESULT (WINAPI *DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
static DirectInput8Create_t real_DirectInput8Create;
static char log_path[MAX_PATH];
static CRITICAL_SECTION log_lock;

void ts_log(const char *fmt, ...)
{
    if (!log_path[0])
        return;
    EnterCriticalSection(&log_lock);
    FILE *f = fopen(log_path, "a");
    if (f) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&log_lock);
}

int ts_env(const char *name, char *out, int size)
{
    DWORD n = GetEnvironmentVariableA(name, out, (DWORD)size);
    if (n == 0 || n >= (DWORD)size) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

/* ---- import table patching ---- */
void **ts_find_iat_slot(const char *dll, const char *func)
{
    BYTE *base = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll) != 0)
            continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        void **slots = (void **)(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; i++) {
            if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal))
                continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) == 0)
                return &slots[i];
        }
    }
    return NULL;
}

void *ts_patch_iat(const char *dll, const char *func, void *hook)
{
    void **slot = ts_find_iat_slot(dll, func);
    DWORD old;
    if (!slot || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) {
        ts_log("import %s!%s not found", dll, func);
        return NULL;
    }
    void *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return orig;
}

void *ts_patch_vtable(void *obj, int index, void *hook)
{
    void **vt = *(void ***)obj;
    DWORD old;
    if (vt[index] == hook || !VirtualProtect(&vt[index], sizeof(void *), PAGE_READWRITE, &old))
        return NULL;
    void *orig = vt[index];
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void *), old, &old);
    return orig;
}

/* Replace `len` bytes at `addr` when they still are `expect` (the exe this was found in). */
int ts_patch_bytes(DWORD addr, const void *expect, const void *repl, int len)
{
    unsigned char *p = (unsigned char *)addr;
    DWORD old;
    if (memcmp(p, expect, len) != 0)
        return 0;
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(p, repl, len);
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    return 1;
}

/* ---- crash log ---- */
/* An access violation (first chance): where, what, registers and likely return addresses (a
 * dword pointing into the exe's code right after a call). The first few only: the engine
 * catches some of its own. */
static LONG CALLBACK log_access_violation(EXCEPTION_POINTERS *x)
{
    static LONG seen;
    EXCEPTION_RECORD *r = x->ExceptionRecord;
    CONTEXT *c = x->ContextRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || InterlockedIncrement(&seen) > 4)
        return EXCEPTION_CONTINUE_SEARCH;
    ts_log("AccessViolation at %p (%s %p) eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx",
           r->ExceptionAddress, r->ExceptionInformation[0] ? "write" : "read", (void *)r->ExceptionInformation[1],
           c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD lo = (DWORD)exe, hi = lo + nt->OptionalHeader.SizeOfImage;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    int n = 0;
    for (DWORD *q = (DWORD *)c->Esp; q < (DWORD *)tib->StackBase && n < 24; q++) {
        DWORD a = *q;
        const unsigned char *p = (const unsigned char *)a;
        if (a < lo + 0x1006 || a >= hi)
            continue;
        if (p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
            || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)) {
            ts_log("  [esp+%04x] %08lx", (unsigned)((BYTE *)q - (BYTE *)c->Esp), a);
            n++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- hang watchdog (tests): TS_WATCHDOG=<s> logs where the main thread is every <s> seconds ---- */
static HANDLE main_thread;
static DWORD watchdog_secs;

/* Likely return addresses on a stack: dwords pointing into the exe's code right after a call. Fills
 * at most `max` (no allocation: also used while the main thread is suspended). */
static int stack_returns(DWORD esp, DWORD stack_top, DWORD *out, DWORD *offs, int max)
{
    HMODULE exe = GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((BYTE *)exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    DWORD lo = (DWORD)exe, hi = lo + nt->OptionalHeader.SizeOfImage;
    int n = 0;
    for (DWORD *q = (DWORD *)esp; q < (DWORD *)stack_top && n < max; q++) {
        DWORD a = *q;
        const unsigned char *p = (const unsigned char *)a;
        if (a < lo + 0x1006 || a >= hi)
            continue;
        if (p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
            || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)) {
            out[n] = a;
            offs[n++] = (DWORD)((BYTE *)q - (BYTE *)esp);
        }
    }
    return n;
}

/* The main thread is suspended only while its registers and stack are copied (it may hold the
 * heap lock, so nothing here allocates until it runs again). */
static DWORD WINAPI watchdog(LPVOID arg)
{
    DWORD stack_top = (DWORD)arg;
    for (;;) {
        Sleep(watchdog_secs * 1000);
        CONTEXT c;
        DWORD ret[24], off[24];
        memset(&c, 0, sizeof c);
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(main_thread) == (DWORD)-1)
            return 0;
        int n = GetThreadContext(main_thread, &c) ? stack_returns(c.Esp, stack_top, ret, off, 24) : -1;
        ResumeThread(main_thread);
        ts_log("watchdog: main thread at %08lx esp=%08lx", c.Eip, c.Esp);
        for (int i = 0; i < n; i++)
            ts_log("  [esp+%04lx] %08lx", off[i], ret[i]);
    }
}

/* The game's own exits (ExitProcess, TerminateProcess on itself, FatalAppExitA): code and caller
 * stack in the log, so a start that ends without a word shows where it decided to. */
typedef void (WINAPI *ExitProcess_t)(UINT);
typedef BOOL (WINAPI *TerminateProcess_t)(HANDLE, UINT);
typedef void (WINAPI *FatalAppExitA_t)(UINT, LPCSTR);
static ExitProcess_t real_ExitProcess;
static TerminateProcess_t real_TerminateProcess;
static FatalAppExitA_t real_FatalAppExitA;

static void log_exit(const char *what, UINT code, DWORD esp)
{
    DWORD ret[24], off[24];
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    int n = stack_returns(esp, (DWORD)tib->StackBase, ret, off, 24);
    ts_log("%s(%u) on thread %lu", what, code, GetCurrentThreadId());
    for (int i = 0; i < n; i++)
        ts_log("  [esp+%04lx] %08lx", off[i], ret[i]);
}

static void WINAPI hook_ExitProcess(UINT code)
{
    log_exit("ExitProcess", code, (DWORD)&code);
    real_ExitProcess(code);
}

static BOOL WINAPI hook_TerminateProcess(HANDLE p, UINT code)
{
    if (p == GetCurrentProcess() || GetProcessId(p) == GetCurrentProcessId())
        log_exit("TerminateProcess", code, (DWORD)&code);
    return real_TerminateProcess(p, code);
}

static void WINAPI hook_FatalAppExitA(UINT action, LPCSTR msg)
{
    ts_log("FatalAppExitA: %s", msg ? msg : "");
    log_exit("FatalAppExitA", action, (DWORD)&action);
    real_FatalAppExitA(action, msg);
}

static void load_real_dinput8(void)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n + 13 >= MAX_PATH)
        return;
    strcpy(path + n, "\\dinput8.dll");
    HMODULE h = LoadLibraryA(path);
    if (h)
        real_DirectInput8Create = (DirectInput8Create_t)GetProcAddress(h, "DirectInput8Create");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&log_lock);
        if (!ts_env("TS_LOG", log_path, MAX_PATH)) {
            DWORD n = GetModuleFileNameA(NULL, log_path, MAX_PATH);
            char *slash = strrchr(log_path, '\\');
            if (n && slash && slash - log_path + 13 < MAX_PATH)
                strcpy(slash + 1, "ts_coop.log");
            else
                log_path[0] = 0;
        }
        ts_log("TimeShift co-op proxy %s loaded", TS_VERSION);
        load_real_dinput8();
        AddVectoredExceptionHandler(1, log_access_violation);
        real_ExitProcess = (ExitProcess_t)ts_patch_iat("KERNEL32.dll", "ExitProcess", (void *)hook_ExitProcess);
        real_TerminateProcess = (TerminateProcess_t)ts_patch_iat("KERNEL32.dll", "TerminateProcess",
                                                                 (void *)hook_TerminateProcess);
        real_FatalAppExitA = (FatalAppExitA_t)ts_patch_iat("KERNEL32.dll", "FatalAppExitA", (void *)hook_FatalAppExitA);
        compat_install();
        testmode_install();
        console_install();
        char v[16];
        if (ts_env("TS_WATCHDOG", v, sizeof v) && (watchdog_secs = (DWORD)atoi(v)) > 0) {
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &main_thread,
                            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
            NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
            CreateThread(NULL, 0, watchdog, tib->StackBase, 0, NULL);
        }
    }
    return TRUE;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid,
                                                        LPVOID *out, LPUNKNOWN outer)
{
    if (!real_DirectInput8Create)
        return E_FAIL;
    HRESULT hr = real_DirectInput8Create(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out)
        testmode_wrap_dinput(*out);
    return hr;
}
