#ifndef TS_H
#define TS_H
#include <windows.h>

#define TS_VERSION "0.0.1-dev"

/* proxy.c */
void ts_log(const char *fmt, ...);
int ts_env(const char *name, char *out, int size);
void **ts_find_iat_slot(const char *dll, const char *func);
void *ts_patch_iat(const char *dll, const char *func, void *hook);
void *ts_patch_vtable(void *obj, int index, void *hook);
int ts_patch_bytes(DWORD addr, const void *expect, const void *repl, int len);
void *ts_patch_ws2(const char *func, void *hook);   /* by-ordinal ws2_32 imports; chains */

/* compat.c: start-up fixes for today's PCs */
void compat_install(void);
void compat_on_d3d(void *d3d);   /* IDirect3D9 from Direct3DCreate9 */

/* console.c: script runtime access (test console TS_CONSOLE=<port>, script error log) */
void console_install(void);
const char *ts_script_run(const char *text, char *out, int size);
int ts_tick_install(void);   /* the per-frame script tick hook; 1 = in place */
typedef void (*ts_ssl_done)(int tag, const char *text, const char *result);
int ts_ssl_queue(const char *text, ts_ssl_done done, int tag);   /* any thread; runs in the script tick */
typedef void (*ts_frame_fn)(float dt);
int ts_frame_add(ts_frame_fn fn);   /* called every frame in the script tick (main thread) */

/* coopnet.c: the mod's own messages over the game's network messages */
void coopnet_install(void);
int coopnet_send(int target, unsigned short sub, const void *data, int len);   /* main thread */
int coopnet_console(const char *text, char *out, int size);   /* "@all/@peers/@srv <ssl>"; 0 = not ours */
typedef void (*coopnet_recv)(int peer, unsigned short sub, const unsigned char *data, int len);
int coopnet_listen(unsigned short first, unsigned short last, coopnet_recv fn);   /* subs >= 10; 1 = hooks in */

/* coopsession.c: the co-op session (network game on a campaign level), its settings, the version check */
void coopsession_install(void);   /* before timesync_install: sets the TS_TIMESYNC default */
int ts_coop_active(void);         /* 1 = co-op session now (any thread; reads game state only) */
int ts_coop_feature(int flag);    /* COOP_F_* of this session: the host's settings (clients get them on join) */
#define COOP_F_ACTIVE 1
#define COOP_F_TIMESYNC 2
#define COOP_F_STORYSYNC 4
#define COOP_F_FRIENDLY_FIRE 8           /* (stock friendly fire rule kept) */
#define COOP_F_MP_RULES 16               /* (stock warm-up and ready system kept) */

/* timesync.c: players' time powers in co-op (one world power for everyone, TS_TIMESYNC) */
void timesync_install(void);

/* storysync.c: the campaign story in co-op (host runs it, clients replay it; exits, checkpoints; TS_STORYSYNC) */
void storysync_install(void);
int storysync_console(const char *text, char *out, int size);   /* "!ss ..." test commands; 0 = not ours */

/* aisync.c: AI soldiers on clients of a campaign level */
void aisync_install(void);
int aisync_console(const char *text, char *out, int size);   /* "!ai ..." test commands; 0 = not ours */

/* netfix.c: the game's own multiplayer on today's networks */
void netfix_install(void);

/* gamepad.c: XInput pad through the engine's own joystick codes and command bindings */
void gamepad_install(void);
int gamepad_slot(int player);   /* XInput pad index of local player 0/1, -1 = none */
void gamepad_poll_player2(int on);   /* split.c: the next input polls read pad 2 for local player 2 */

/* split.c: local split screen, player 2 on pad 2 (TS_SPLIT=1, prototype) */
void split_install(void);

/* d3dex.c: Direct3D 9Ex instead of 9 (default; TS_D3D9EX=0 off: no lost device in a window), resource census (TS_D3DCENSUS=1) */
void d3dex_install(void);
struct IDirect3D9 *d3dex_create(UINT sdk, struct IDirect3D9 *(WINAPI *create9)(UINT));   /* testmode.c's Direct3DCreate9 hook */

/* display.c: screen mode from the Video options (profile key COOP_DisplayMode: borderless / window) */
void display_install(void);
void display_on_d3d(void *d3d);

/* testmode.c: window, render adapter and focus handling for test instances */
void testmode_install(void);
void testmode_wrap_dinput(void *dinput);

#endif
