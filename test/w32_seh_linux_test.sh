#!/bin/bash
# w32_seh_linux_test.sh -- build and run test/w32_seh_linux_test.cpp (hook/w32_seh.cpp's Linux SEH) in WSL / Linux,
# with tools/build_linux.sh's flags, into out-linux/test/. Exit code: the test's (the number of failures).
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
OUT=$ROOT/out-linux/test
mkdir -p "$OUT"
CFLAGS="-m32 -O2 -march=i686 -mfpmath=387 -fexcess-precision=standard -fno-strict-aliasing -masm=intel -std=c++17 \
-fms-extensions -Wno-invalid-offsetof -fno-pie -fno-pic -fno-stack-protector -fno-stack-clash-protection \
-fcf-protection=none -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -I$ROOT/hook/linux_inc -include $ROOT/hook/msvc_compat.h"
for s in hook/w32_seh hook/w32_thread hook/w32_handle test/w32_seh_linux_test; do
    g++ $CFLAGS -Wall -c "$s.cpp" -o "$OUT/$(basename $s).o" || exit 100
done
g++ -m32 -no-pie -o "$OUT/w32_seh_linux_test" "$OUT"/w32_seh.o "$OUT"/w32_thread.o "$OUT"/w32_handle.o \
    "$OUT"/w32_seh_linux_test.o -lpthread || exit 101
"$OUT/w32_seh_linux_test"
