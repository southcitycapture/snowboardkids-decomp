#!/bin/sh
# Build the whole thing: "Snowboard Kids 1+2 PowerPC Edition".
#
#   port/tools/make_package.sh [--with-rom] [--no-dmg] [--no-build] [-o DIR]
#
# The two games live in two repositories -- this one and, next to it,
# ../snowboardkids2-decomp -- because they are two decompilations.  They ship
# as one thing, though, so one script drives both: it cross-builds each
# executable, wraps each in its .app through that repository's own
# make_bundle.sh, and lays the pair out in a folder with a Read Me:
#
#   Snowboard Kids 1+2 PowerPC Edition/
#       Snowboard Kids.app
#       Snowboard Kids 2.app
#       Read Me.txt
#
# and nothing else.  Then it rolls that folder into a .dmg that Leopard can
# mount (HFS+, UDZO), which is the form to copy to the G4.
#
# No ROM goes into either bundle.  The player's own cartridge dumps go in
# ~/Library/Application Support/SnowboardKids/ROMs/, which both apps share and
# which outlives replacing either one -- see port/src/rom_scan.h.  --with-rom
# is the user's personal build: it puts each dump inside its own .app so the
# folder is self-contained, and is not the thing to hand anyone else.
set -e

here=$(cd "$(dirname "$0")" && pwd)
sbk1=$(cd "$here/.." && pwd)                       # .../snowboardkids-decomp/port
sbk2=$(cd "$sbk1/../../snowboardkids2-decomp/port" 2>/dev/null && pwd || true)

NAME="Snowboard Kids 1+2 PowerPC Edition"
with_rom=0
do_dmg=1
do_build=1
outdir=$sbk1/build-ppc-darwin

while [ $# -gt 0 ]; do
    case "$1" in
        --with-rom) with_rom=1; shift ;;
        --no-dmg)   do_dmg=0; shift ;;
        --no-build) do_build=0; shift ;;
        -o) outdir=$2; shift 2 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "make_package.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

[ -n "$sbk2" ] || { echo "no sequel repository at $sbk1/../../snowboardkids2-decomp" >&2; exit 1; }

pkg="$outdir/$NAME"

romopt=
[ "$with_rom" = 1 ] && romopt=--with-rom

# --- build ---------------------------------------------------------------
if [ "$do_build" = 1 ]; then
    echo "==> building Snowboard Kids"
    (cd "$sbk1/.." && port/build-ppc.sh)
    echo "==> building Snowboard Kids 2"
    (cd "$sbk2/.." && port/build-ppc.sh)
fi

# --- bundles -------------------------------------------------------------
echo "==> bundling"
rm -rf "$pkg"
mkdir -p "$pkg"
sh "$sbk1/tools/make_bundle.sh" $romopt "$sbk1/build-ppc-darwin/snowboardkids"  "$pkg/Snowboard Kids.app"   >/dev/null
sh "$sbk2/tools/make_bundle.sh" $romopt "$sbk2/build-ppc-darwin/snowboardkids2" "$pkg/Snowboard Kids 2.app" >/dev/null

# --- the Read Me ---------------------------------------------------------
cp "$sbk1/resources/ReadMe.txt" "$pkg/Read Me.txt"

# nothing else: no .DS_Store, no dot-underscore files out of a copy
find "$pkg" -name '.DS_Store' -delete 2>/dev/null || true
find "$pkg" -name '._*' -delete 2>/dev/null || true

echo "package: $pkg"
ls -1 "$pkg"

# --- the disk image ------------------------------------------------------
# HFS+ and UDZO: Leopard's DiskImages mounts both.  Not APFS (10.13+), not
# UDBZ (bzip2, 10.4+ but slower to make), not a sparsebundle.
if [ "$do_dmg" = 1 ]; then
    if command -v hdiutil >/dev/null 2>&1; then
        dmg="$outdir/$NAME.dmg"
        rm -f "$dmg"
        echo "==> disk image"
        hdiutil create -quiet -srcfolder "$pkg" -volname "$NAME" \
            -fs HFS+ -format UDZO -imagekey zlib-level=9 "$dmg"
        echo "dmg: $dmg"
        hdiutil imageinfo "$dmg" | sed -n 's/^Format: /  format: /p;s/^Checksum Type: /  checksum: /p'
    else
        echo "no hdiutil here; folder only" >&2
    fi
fi
