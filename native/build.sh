#!/bin/sh
# Build the 32-bit dinput8.dll proxy for TimeShift with Zig's clang.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p build
"$ZIG" cc -target x86-windows-gnu -O2 -fno-omit-frame-pointer -shared -o build/dinput8.dll \
  -DWIN32 proxy.c compat.c testmode.c console.c coopnet.c netfix.c gamepad.c dinput8.def \
  -luser32 -lkernel32 -lws2_32 -liphlpapi
