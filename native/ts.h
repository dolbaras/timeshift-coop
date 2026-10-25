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

/* netfix.c: the game's own multiplayer on today's networks */
void netfix_install(void);

/* gamepad.c: XInput pad through the engine's own joystick codes and command bindings */
void gamepad_install(void);
int gamepad_slot(int player);   /* XInput pad index of local player 0/1, -1 = none */

/* testmode.c: window, render adapter and focus handling for test instances */
void testmode_install(void);
void testmode_wrap_dinput(void *dinput);

#endif
