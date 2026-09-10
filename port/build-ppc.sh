#!/bin/sh
# Cross-build the Snowboard Kids port for PowerPC Mac OS X (Leopard on the
# Quicksilver G4; binaries also run on Tiger) inside the
# gcc-powerpc-apple-darwin8 Docker image (GCC 14.2 + MacOSX10.4u SDK).
#
#   port/build-ppc.sh [make targets/vars...]     e.g.  port/build-ppc.sh -j8
#
# Mounts:
#   /work/sbk   -> this repository (read-write: build output goes to port/build-ppc)
#   /work/sdl2  -> panther-sdl2 Tiger prefix (libSDL2.a + SDL2 headers)
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
SDL2_PREFIX=${SDL2_PREFIX:-$HOME/Apps/panther-sdl2/build-tiger-joy/prefix}  # joystick+haptic build (isle-ppc-tools/tiger/build-sdl2-tiger-joy.sh)
IMAGE=${IMAGE:-ghcr.io/variantxyz/gcc-powerpc-apple-darwin8:build-gcc-14.2-MacOSXSDK10.4u}

[ -d "$SDL2_PREFIX/include/SDL2" ] || { echo "no SDL2 prefix at $SDL2_PREFIX (see isle-ppc-tools/tiger/build-sdl2-tiger.sh)" >&2; exit 1; }

mkdir -p "$here/build-ppc-darwin"
NM=${NM:-$HOME/.local/opt/mips64-elf/bin/mips64-elf-nm}
[ -x "$NM" ] && [ -f "$repo/build/snowboardkids.elf" ] && "$NM" -S "$repo/build/snowboardkids.elf" > "$here/build-ppc-darwin/n64syms.txt"

exec docker run --rm \
    -v "$repo":/work/sbk \
    -v "$SDL2_PREFIX":/work/sdl2:ro \
    -w /work/sbk/port \
    "$IMAGE" \
    make -f Makefile TARGET=ppc-darwin SDL2_PREFIX=/work/sdl2 "$@" 2>&1 | grep -v "requested image's platform"
