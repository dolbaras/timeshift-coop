#!/bin/sh
# Offline checks of the native code (host build, no game needed). Run from anywhere.
set -e
cd "$(dirname "$0")"
ZIG="${ZIG:-../../../../../tools/zig/zig-x86_64-windows-0.16.0/zig.exe}"
mkdir -p ../build
"$ZIG" cc -O1 -o ../build/axes_test.exe axes_test.c
../build/axes_test.exe
"$ZIG" cc -O1 -o ../build/latejoin_test.exe latejoin_test.c
../build/latejoin_test.exe
"$ZIG" cc -O1 -o ../build/rewindfollow_test.exe rewindfollow_test.c
../build/rewindfollow_test.exe
"$ZIG" cc -O1 -o ../build/readyroom_test.exe readyroom_test.c
../build/readyroom_test.exe
