/*
 * Script runtime access (patch 1.2 exe only; addresses from the static analysis in the private notes):
 *
 * - Test console, TS_CONSOLE=<port>: UDP on 127.0.0.1 only. Each datagram is SSL statements; they are
 *   compiled as `func __console__ : * ... end` into a throwaway object and run on the game's main
 *   thread inside the engine's own per-frame script tick (the call to the delayed-call update at
 *   0x6fb683), the way the retail-removed "Saber3d console" did. The reply is the result as text
 *   ("Ok" for no value, `return <expr>` for one) or the error. Client: tools/sslcon.py.
 *   In a network game, `@all <ssl>` / `@peers <ssl>` / `@srv <ssl>` send the text to the other machines
 *   (coopnet.c); the same queue runs SSL text received from them.
 * - Script errors and log lines: the engine's log callback [0xecb100] is a bare `ret` in retail; the
 *   proxy puts its own there, so every script error ("unknown object", type and parse errors) lands in
 *   the mod's log as "ssl: ...". With TS_CONSOLE or TS_SSLLOG=1 until checked in the game.
 */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts.h"

#define TSCALL __attribute__((thiscall))
typedef struct { int refs, len, cap; char s[1]; } StrHdr;
typedef StrHdr *String;
typedef struct { int code; String msg; } Status;
typedef struct { void **type; unsigned data; } Variant;
typedef void *ObjPtr;

#define MGR (*(void **)0xebedec)
static void (__cdecl *eng_free)(void *) = (void *)0x7e4700;
static ObjPtr *(TSCALL *mgr_create_class)(void *, ObjPtr *, ObjPtr) = (void *)0x8fc2d0;
static Status *(TSCALL *cls_parse)(ObjPtr *, Status *, const char *, const char *) = (void *)0x8f9150;
static ObjPtr *(TSCALL *mgr_create_obj)(void *, ObjPtr *, const char *, ObjPtr, int) = (void *)0x8fd2f0;
static void (TSCALL *mgr_remove_obj)(void *, ObjPtr) = (void *)0x8fd7d0;
static int *(TSCALL *sym_ctor)(int *, const char *, char) = (void *)0x9c7530;
static Status *(TSCALL *inst_call)(ObjPtr *, Status *, int, int, Variant *, Variant *, ObjPtr) = (void *)0x8f9460;
static void (TSCALL *cls_release)(ObjPtr *) = (void *)0x8f7f10;
static void (TSCALL *inst_release)(ObjPtr *) = (void *)0x8f8050;
static Variant *(TSCALL *var_ctor)(Variant *) = (void *)0x9c9260;
static void (TSCALL *var_dtor)(Variant *) = (void *)0x9c8690;
static void *(*type_null)(void) = (void *)0x9c8bf0;
typedef String *(TSCALL *ToString_t)(void *type, String *out, const Variant *v);

static void str_release(String s)
{
    if (s && --s->refs == 0)
        eng_free(s);
}

static ObjPtr addref(ObjPtr p)
{
    if (p)
        InterlockedIncrement((LONG *)p);
    return p;
}

/* Compile and run SSL statements; main thread only (inside the script tick). */
const char *ts_script_run(const char *text, char *out, int size)
{
    void *mgr = MGR;
    ObjPtr cls = 0, inst = 0;
    Status st;
    Variant res;
    int sym;
    size_t n = strlen(text) + 32;
    char *src = malloc(n);
    if (!src)
        return snprintf(out, size, "out of memory"), out;
    snprintf(src, n, "func __console__ : *\n%s\nend\n", text);
    mgr_create_class(mgr, &cls, 0);
    cls_parse(&cls, &st, src, "");
    free(src);
    if (st.code) {
        snprintf(out, size, "parse error %d: %s", st.code, st.msg ? st.msg->s : "");
        str_release(st.msg);
        goto done;
    }
    str_release(st.msg);
    mgr_create_obj(mgr, &inst, "__console__", addref(cls), 0);   /* (the callee releases the by-value class) */
    if (!inst) {
        snprintf(out, size, "create failed %d", *(int *)mgr);
        goto done;
    }
    var_ctor(&res);
    sym_ctor(&sym, "__console__", 1);
    inst_call(&inst, &st, sym, 0, 0, &res, 0);   /* (a script function: argv may be NULL here, not for natives) */
    if (st.code)
        snprintf(out, size, "error %d: %s", st.code, st.msg ? st.msg->s : "");
    else if (res.type == type_null())
        snprintf(out, size, "Ok");
    else {
        String s = 0;
        ToString_t to_string = (ToString_t)(*(void ***)res.type)[1];
        to_string(res.type, &s, &res);
        snprintf(out, size, "%s", s ? s->s : "");
        str_release(s);
    }
    str_release(st.msg);
    mgr_remove_obj(mgr, addref(inst));
    var_dtor(&res);
done:
    if (inst)
        inst_release(&inst);
    if (cls)
        cls_release(&cls);
    return out;
}

/* ---- main-thread queue: the UDP console and coopnet.c add SSL text, the script tick runs it ----
 * Entries with `done == NULL` are console datagrams (the reply goes back to the sender); the others call
 * `done` with the result. Producers are the console's socket thread and the engine's network pump. */
#define QMAX 64
static SOCKET con_sock = INVALID_SOCKET;
static CRITICAL_SECTION q_lock;
static int tick_installed;
static struct { char *text; struct sockaddr_in from; ts_ssl_done done; int tag; } queue[QMAX];
static int q_count;
static LONG q_dropped;

static int queue_add(const char *text, const struct sockaddr_in *from, ts_ssl_done done, int tag)
{
    int ok = 0;
    char *copy = _strdup(text);
    if (!copy)
        return 0;
    EnterCriticalSection(&q_lock);
    if (q_count < QMAX) {
        queue[q_count].text = copy;
        if (from)
            queue[q_count].from = *from;
        queue[q_count].done = done;
        queue[q_count].tag = tag;
        q_count++;
        ok = 1;
    }
    LeaveCriticalSection(&q_lock);
    if (!ok) {
        free(copy);
        if (InterlockedIncrement(&q_dropped) <= 4)
            ts_log("console: queue full, dropped: %.60s", text);
    }
    return ok;
}

int ts_ssl_queue(const char *text, ts_ssl_done done, int tag)
{
    return tick_installed && done ? queue_add(text, NULL, done, tag) : 0;
}

static DWORD WINAPI console_thread(LPVOID arg)
{
    (void)arg;
    static char buf[16384];
    for (;;) {
        struct sockaddr_in from;
        int fl = sizeof from;
        int n = recvfrom(con_sock, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0)
            continue;
        buf[n] = 0;
        queue_add(buf, &from, NULL, 0);
    }
}

static void console_pump(void)
{
    if (!q_count)   /* (an unlocked peek: a datagram that just arrived waits one frame) */
        return;
    EnterCriticalSection(&q_lock);
    int n = q_count;
    char *texts[QMAX];
    struct sockaddr_in from[QMAX];
    ts_ssl_done done[QMAX];
    int tag[QMAX];
    for (int i = 0; i < n; i++)
        texts[i] = queue[i].text, from[i] = queue[i].from, done[i] = queue[i].done, tag[i] = queue[i].tag;
    q_count = 0;
    LeaveCriticalSection(&q_lock);
    for (int i = 0; i < n; i++) {
        static char out[16384];
        if (done[i])
            done[i](tag[i], texts[i], ts_script_run(texts[i], out, sizeof out));
        else {
            /* "@all ", "@peers ", "@srv ": send to the other machines of a network game (coopnet.c) */
            if (!coopnet_console(texts[i], out, sizeof out))
                ts_script_run(texts[i], out, sizeof out);
            if (con_sock != INVALID_SOCKET)
                sendto(con_sock, out, (int)strlen(out), 0, (struct sockaddr *)&from[i], sizeof from[i]);
        }
        free(texts[i]);
    }
}

/* The engine's delayed-call update, called once per frame from the script task (0x6fb683). */
static void (TSCALL *orig_update_delayed)(void *, float) = (void *)0x8fbf70;
static LONG tick_logged;

static void TSCALL hook_update_delayed(void *mgr, float dt)
{
    orig_update_delayed(mgr, dt);
    if (InterlockedIncrement(&tick_logged) == 1)
        ts_log("console: first script tick on thread %lu", GetCurrentThreadId());
    console_pump();
}

/* ---- script log ---- */
static void __cdecl script_log(int level, const char *msg)
{
    ts_log("ssl%s: %s", level ? " error" : "", msg ? msg : "");
}

#define SCRIPT_LOG_CB 0x00ecb100
#define SCRIPT_LOG_RET 0x006dedd0   /* retail's callback: a bare ret */
#define DELAYED_CALL 0x006fb683

/* Called once the engine has set its own callback (from the first script tick). */
static void install_script_log(void)
{
    void **cb = (void **)SCRIPT_LOG_CB;
    if (*cb == (void *)SCRIPT_LOG_RET || *cb == NULL)
        *cb = (void *)script_log;
}

static void TSCALL hook_update_first(void *mgr, float dt);

/* The script tick hook (also wanted by coopnet.c for remote SSL); once. */
int ts_tick_install(void)
{
    if (tick_installed)
        return 1;
    static const unsigned char want[5] = {0xe8, 0xe8, 0x08, 0x20, 0x00};
    unsigned char rep[5] = {0xe8};
    *(DWORD *)(rep + 1) = (DWORD)hook_update_first - (DELAYED_CALL + 5);
    InitializeCriticalSection(&q_lock);   /* (before the hook can run) */
    if (!ts_patch_bytes(DELAYED_CALL, want, rep, 5)) {
        ts_log("console: script tick not found (other exe)");
        DeleteCriticalSection(&q_lock);
        return 0;
    }
    tick_installed = 1;
    return 1;
}

void console_install(void)
{
    char v[16];
    /* (until checked in the game: only with TS_CONSOLE or TS_SSLLOG=1) */
    if (!ts_env("TS_CONSOLE", v, sizeof v) && !(ts_env("TS_SSLLOG", v, sizeof v) && v[0] == '1'))
        return;
    if (!ts_tick_install())
        return;
    if (!ts_env("TS_CONSOLE", v, sizeof v))
        return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    con_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)atoi(v));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   /* this PC only */
    if (con_sock == INVALID_SOCKET || bind(con_sock, (struct sockaddr *)&a, sizeof a) != 0) {
        ts_log("console: cannot listen on 127.0.0.1:%s", v);
        con_sock = INVALID_SOCKET;
        return;
    }
    CreateThread(NULL, 0, console_thread, NULL, 0, NULL);
    ts_log("console: listening on 127.0.0.1:%s", v);
}

/* First tick: put the log callback in (the engine sets its own during start-up), then the plain hook. */
static void TSCALL hook_update_first(void *mgr, float dt)
{
    static int done;
    if (!done) {
        done = 1;
        install_script_log();
    }
    hook_update_delayed(mgr, dt);
}
