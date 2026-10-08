#!/bin/bash
# build_linux.sh -- the native Linux build (relink stage R2b), in WSL Ubuntu (or any i386-multilib Linux with
# gcc/g++-multilib and libsdl2-dev:i386): every hook/*.cpp of hook\build_gcc.bat's list compiled with Linux GCC into
# out-linux/obj/, in parallel, then linked.
#
#   tools/build_linux.sh            compile (only what changed) + link out-linux/viperport-link-test and
#                                   out-linux/viperport (the game: loader/viperport_linux.cpp says how to run it)
#   tools/build_linux.sh clean      remove out-linux/
#   tools/build_linux.sh release    the build that's packaged (tools/package_linux.sh) -> out-linux-release/: run in the
#                                   Ubuntu 22.04 WSL distro (glibc 2.35, so older distros run it too) with g++-15 from the toolchain PPA (the same GCC 15.2 as the main Linux build: GCC 12 has no -fexcess-precision=standard for C++, and GCC 13's x87 code parts from the corpus) and
#                                   SDL2 built for i386 in /opt/sdl2-i386; libstdc++/libgcc linked in statically (Mesa's
#                                   drivers load into the process and need the system's own newer libstdc++, so an older
#                                   copy can't ship beside it), SDL2 found in ./lib beside the binary (RPATH $ORIGIN/lib)
#   VP_JOBS=n                       compilers at a time (default: nproc)
#
# The flags are build_gcc.bat's (the x87 rules of hook/compiler.h: -m32 -O2 -march=i686 -mfpmath=387
# -fexcess-precision=standard -fno-strict-aliasing -masm=intel -std=c++17 -fms-extensions), plus what makes Ubuntu's GCC
# generate the code mingw's does: no PIE/PIC (the GNU asm blocks address globals absolutely, as MSVC's do), no stack
# protector, no stack-clash probes, no CET endbr32, no _FORTIFY_SOURCE -- Ubuntu turns all of those on by default,
# mingw none.
#
# Linux has no <windows.h>: hook/linux_inc/ (on the include path here only) has windows.h, mmsystem.h, intrin.h,
# winsock.h, ddraw.h, d3d.h and dsound.h, which bring in hook/win32_compat.h (Windows' types, constants and functions at
# their exact 32-bit layouts) and hook/dx_compat.h (DirectX 5's) -- no source changes its #include, and the Windows
# builds never see them. hook/msvc_compat.h, force-included (-include), is what MSVC / mingw give every file without an
# #include: __stdcall & co., __declspec, the intrinsics, the MSVC C runtime names.
#
# The link test (out-linux/viperport-link-test): every object of the port plus a main() that references the stand-ins'
# table, proving everything resolves (a Linux path that can't be reached calls vp_r2b_todo, hook/linux_todo.cpp:
# prints its name and aborts). The real executable is out-linux/viperport: the ELF loader
# (loader/viperport_linux.cpp) and the same objects, linked with -Wl,--wrap=fopen (hook/vp_os_linux.h).
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
OUT=$ROOT/out-linux
RELEASE=0
if [ "${1:-}" = release ]; then RELEASE=1; OUT=$ROOT/out-linux-release; fi
if [ "${1:-}" = clean ]; then rm -rf "$OUT" "$ROOT/out-linux-release"; exit 0; fi
mkdir -p "$OUT/obj"

if [ $RELEASE = 1 ]; then
    CXX=${CXX:-g++-15}
    SDL_CONFIG=/opt/sdl2-i386/bin/sdl2-config
    [ -x "$SDL_CONFIG" ] || { echo "build_linux release: no $SDL_CONFIG (SDL2 2.32 built for i386, installed to /opt/sdl2-i386)"; exit 1; }
else
    CXX=${CXX:-g++}
    SDL_CONFIG=sdl2-config
fi
SDL_CFLAGS=$($SDL_CONFIG --cflags) || { echo "build_linux: no sdl2-config (apt install libsdl2-dev:i386)"; exit 1; }
CFLAGS="-m32 -O2 -march=i686 -mfpmath=387 -fexcess-precision=standard -fno-strict-aliasing -masm=intel -std=c++17 \
-fms-extensions -Wno-invalid-offsetof -fno-pie -fno-pic -fno-stack-protector -fno-stack-clash-protection \
-fcf-protection=none -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -mincoming-stack-boundary=2 -D_FILE_OFFSET_BITS=64 $SDL_CFLAGS \
-I$ROOT/hook/linux_inc -include $ROOT/hook/msvc_compat.h"
# -D_FILE_OFFSET_BITS=64: 32-bit stat()/readdir fail with EOVERFLOW on WSL's /mnt/c (drvfs inode numbers are 64-bit), so
# an install on a Windows drive would look missing.
# -mincoming-stack-boundary=2 (relink stage R2b's stack-alignment risk): race.exe's code calls the port with the stack
# aligned to 4 only (Win32's guarantee), and the i386 System V ABI that Linux GCC, glibc, SDL2 and Mesa assume is 16 at
# every call -- a misaligned movaps in a library the port calls would fault. With this, every port function that calls
# another (or keeps a 16-aligned local) realigns its own frame on entry (push ebp; mov ebp, esp; and esp, -16), so every
# call the port makes out of a game frame is 16-aligned again. Arguments stay ebp-relative, the return address where
# it was (_AddressOfReturnAddress), naked functions untouched, x87 arithmetic the same: frames only. (-mstackrealign,
# the obvious first choice, realigns only a function with an over-aligned local of its own, not a caller: a call into a
# library from a game frame would stay misaligned.)
# A change of flags rebuilds everything (out-linux/flags.txt).
# the port opens its own files with fopen and Windows paths: the wrapper (hook/vp_os_linux.cpp) takes them through the path layer
LDFLAGS="-m32 -no-pie -Wl,--wrap=fopen -Wl,--wrap=fopen64"
LIBS="-lSDL2 -lGL -lpthread -ldl -lrt"
if [ $RELEASE = 1 ]; then
    LDFLAGS="$LDFLAGS -static-libstdc++ -static-libgcc -L/opt/sdl2-i386/lib -Wl,-rpath,\$ORIGIN/lib -Wl,--enable-new-dtags"
fi

# build_gcc.bat's VP_SOURCES, in its order, plus the Linux-only files
PATTERNS="viperport.cpp port.cpp replay.cpp session.cpp standalone.cpp net_*.cpp crt_*.cpp edit_*.cpp phys_*.cpp \
wld_*.cpp krn_*.cpp gx_*.cpp snd_*.cpp ui_*.cpp menu_*.cpp root_*.cpp career_*.cpp paint_*.cpp w32_*.cpp vp_os.cpp \
platform.cpp gl_table.cpp gl_core.cpp gl_dxgi.cpp ddraw_gl.cpp dsound_sdl.cpp vp_os_linux.cpp linux_todo.cpp"
SOURCES=()
cd hook
for p in $PATTERNS; do
    for f in $(ls $p 2>/dev/null | LC_ALL=C sort); do SOURCES+=("$f"); done
done
cd ..

# compile: a source is rebuilt when it, or any header it read (gcc -MMD), is newer than its object
compile_one() {
    local s=$1 o="$OUT/obj/${1%.cpp}.o"
    if [ -f "$o" ] && [ -f "${o%.o}.d" ]; then
        local stale=0 dep
        for dep in $(sed -e 's/^[^:]*://' -e 's/\\$//' "${o%.o}.d"); do
            case $dep in /*) ;; *) dep=$ROOT/hook/$dep ;; esac        # (gcc ran in hook/)
            if [ "$dep" -nt "$o" ] || [ ! -e "$dep" ]; then stale=1; break; fi
        done
        [ $stale = 0 ] && return 0
    fi
    if (cd "$ROOT/hook" && $CXX $CFLAGS -MMD -MF "${o%.o}.d" -c "$s" -o "$o") >"$o.log" 2>&1; then
        [ -s "$o.log" ] && { echo "== $s"; cat "$o.log"; }
        return 0
    fi
    echo "== $s FAILED"; cat "$o.log"; rm -f "$o"
    return 1
}
export -f compile_one
export CXX CFLAGS OUT ROOT
if [ "$(cat "$OUT/flags.txt" 2>/dev/null)" != "$CXX $CFLAGS" ]; then     # (new flags: every object again)
    rm -f "$OUT"/obj/*.o
    printf '%s' "$CXX $CFLAGS" >"$OUT/flags.txt"
fi
JOBS=${VP_JOBS:-$(nproc)}
printf '%s\n' "${SOURCES[@]}" | xargs -P "$JOBS" -I{} bash -c 'compile_one "$@"' _ {}
failed=0
OBJS=()
for s in "${SOURCES[@]}"; do
    o="$OUT/obj/${s%.cpp}.o"
    if [ -f "$o" ]; then OBJS+=("$o"); else failed=$((failed + 1)); fi
done
if [ $failed != 0 ]; then echo "build_linux: $failed of ${#SOURCES[@]} sources failed"; exit 1; fi
echo "build_linux: ${#SOURCES[@]} sources compiled"

# the link test: everything the port is, plus a main that reaches it
cat >"$OUT/link_test.cpp" <<'EOF'
// generated by tools/build_linux.sh: references the port, so every object of it must resolve
#include <stdio.h>
#include "w32_table.h"
int main() {
    printf("viperport-link-test: %u stand-ins\n", (unsigned)viperport_stand_ins.count);
    return 0;
}
EOF
$CXX $CFLAGS -I"$ROOT/hook" -c "$OUT/link_test.cpp" -o "$OUT/link_test.o" || exit 1
$CXX $LDFLAGS -o "$OUT/viperport-link-test" "$OUT/link_test.o" "${OBJS[@]}" -Wl,-Map,"$OUT/viperport-link-test.map" $LIBS \
    || { echo "build_linux: link failed"; exit 1; }
echo "build_linux: ${OUT#$ROOT/}/viperport-link-test linked"

# the game: the ELF loader (loader/viperport_linux.cpp) and the whole port, one executable. Non-PIE, linked at ld's
# default 0x08048000, so race.exe's 0x400000-0x62c000 is free for the loader to map it.
$CXX $CFLAGS -I"$ROOT/hook" -c "$ROOT/loader/viperport_linux.cpp" -o "$OUT/viperport_linux.o" || exit 1
$CXX $LDFLAGS -o "$OUT/viperport" "$OUT/viperport_linux.o" "${OBJS[@]}" -Wl,-Map,"$OUT/viperport.map" $LIBS \
    || { echo "build_linux: link of viperport failed"; exit 1; }
echo "build_linux: ${OUT#$ROOT/}/viperport linked"
