#!/bin/bash
# package_linux.sh -- the Linux release tarball, in the Ubuntu 22.04 WSL distro:
#
#   wsl -d Ubuntu-22.04 -- tools/package_linux.sh
#
# builds tools/build_linux.sh release (glibc 2.35, libstdc++/libgcc static, SDL2 from /opt/sdl2-i386) and packs
#   out-linux-release/viperport-linux-<version>.tar.gz  (+ .sha256)
#     viperport-linux-<version>/viperport, viperport.sh, README-linux.txt, lib/libSDL2-2.0.so.0,
#     LICENSES/SDL2-LICENSE.txt, LICENSES/GCC-RUNTIME-EXCEPTION.txt
# <version> = the commit's date and short hash (e.g. 2026.10.07-7153cf3). The tarball unpacks into a folder of that name;
# the README says to extract it with --strip-components=1 straight into the game folder, beside race.exe.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
tools/build_linux.sh release
OUT=$ROOT/out-linux-release
VERSION=$(git log -1 --date=format:%Y.%m.%d --format=%cd-%h)
# (a Windows checkout seen from WSL: its CRLF files and modes aren't changes)
[ -z "$(git -c core.autocrlf=true -c core.filemode=false status --porcelain -- hook loader tools)" ] || VERSION=$VERSION-dirty
NAME=viperport-linux-$VERSION
STAGE=$OUT/stage/$NAME
rm -rf "$OUT/stage" && mkdir -p "$STAGE/lib" "$STAGE/LICENSES"

cp "$OUT/viperport" "$STAGE/"
strip "$STAGE/viperport"
cp -L /opt/sdl2-i386/lib/libSDL2-2.0.so.0 "$STAGE/lib/"
strip --strip-unneeded "$STAGE/lib/libSDL2-2.0.so.0"
install -m 755 tools/linux_pkg/viperport.sh "$STAGE/"

# the dependency lines, one place: what the binary and SDL2 load from the system (32-bit glibc, Mesa GL, X11, audio,
# udev -- without it SDL's joystick driver can't start: the game then runs without controllers)
APT="libc6:i386 libgl1:i386 libgl1-mesa-dri:i386 libx11-6:i386 libxext6:i386 libxcursor1:i386 libxi6:i386 libxrandr2:i386 libxss1:i386 libpulse0:i386 libudev1:i386"
DNF="glibc.i686 mesa-libGL.i686 mesa-dri-drivers.i686 libX11.i686 libXext.i686 libXcursor.i686 libXi.i686 libXrandr.i686 libXScrnSaver.i686 pulseaudio-libs.i686 systemd-libs.i686"
PACMAN="lib32-glibc lib32-mesa lib32-libx11 lib32-libxext lib32-libxcursor lib32-libxi lib32-libxrandr lib32-libxss lib32-libpulse lib32-systemd"
sed -e "s/@VERSION@/$VERSION/" -e "s/@APT_PACKAGES@/$APT/" -e "s/@DNF_PACKAGES@/$DNF/" -e "s/@PACMAN_PACKAGES@/$PACMAN/" \
    tools/linux_pkg/README-linux.txt > "$STAGE/README-linux.txt"
if grep -q $'\r' "$STAGE/README-linux.txt" "$STAGE/viperport.sh"; then
    echo "package_linux: CR line ends in the README or viperport.sh (a Windows edit): the install line would break"; exit 1
fi

SDL_SRC=/opt/src/SDL2-2.32.10
cp "$SDL_SRC/LICENSE.txt" "$STAGE/LICENSES/SDL2-LICENSE.txt"
sed -n '/^GCC RUNTIME LIBRARY EXCEPTION/,/requirements of the license of GCC\./p' /usr/share/doc/gcc-13-base/copyright \
    > "$STAGE/LICENSES/GCC-RUNTIME-EXCEPTION.txt"
[ -s "$STAGE/LICENSES/GCC-RUNTIME-EXCEPTION.txt" ] || { echo "package_linux: no GCC runtime exception text"; exit 1; }

# the binary's floor, checked: nothing newer than glibc 2.35, nothing but SDL2 beyond glibc
max=$(objdump -T "$STAGE/viperport" "$STAGE/lib/libSDL2-2.0.so.0" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
[ "$(printf '%s\nGLIBC_2.35\n' "$max" | sort -V | tail -1)" = GLIBC_2.35 ] || { echo "package_linux: needs $max (> 2.35)"; exit 1; }
needed=$(readelf -d "$STAGE/viperport" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
[ "$needed" = "ld-linux.so.2 libSDL2-2.0.so.0 libc.so.6 libm.so.6 " ] || { echo "package_linux: unexpected libraries: $needed"; exit 1; }

tar -C "$OUT/stage" --owner=0 --group=0 -czf "$OUT/$NAME.tar.gz" "$NAME"
(cd "$OUT" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
echo "package_linux: out-linux-release/$NAME.tar.gz ($(du -h "$OUT/$NAME.tar.gz" | cut -f1); glibc floor $max)"
