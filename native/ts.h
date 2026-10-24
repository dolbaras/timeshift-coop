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

/* compat.c: start-up fixes for today's PCs */
void compat_install(void);

/* testmode.c: window, render adapter and focus handling for test instances */
void testmode_install(void);
void testmode_wrap_dinput(void *dinput);

#endif
