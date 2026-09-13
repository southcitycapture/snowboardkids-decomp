#!/bin/sh
# Wrap the cross-built executable into a Mac OS X .app bundle.
#
#   port/tools/make_bundle.sh [--with-rom [ROM]] [exe] ["out.app"]
#
# The bundle is self-contained: the PowerPC executable (SDL2 is linked in
# statically, so there is nothing else to install), the icon, and an
# Info.plist that names it for the Finder.
#
# No ROM goes in by default.  The player's own cartridge dumps live in the
# shared folder the launcher creates and points at,
#
#     ~/Library/Application Support/SnowboardKids/ROMs/
#
# which both bundles read and which survives replacing either app.  That is
# also the only arrangement that can be handed to someone else: the repository
# has never contained a ROM and neither does what it builds.
#
# --with-rom is for the user's personal build: it copies a dump into
# Contents/Resources as before, so one .app is the whole thing.  The shared
# folder still wins over it when both have a good copy.
#
# The executable inside is named `isle` so the isle-ppc-tools g4 console
# runner (which hard-codes that name) can launch it unchanged: on the G4,
# `ln -sfn "/Applications/Snowboard Kids.app" ~/SnowboardKids.app` and then
# `g4 use sbk` / `g4 run` / `g4 shot` all still work.
set -e
here=$(cd "$(dirname "$0")" && pwd)

with_rom=0
rom=
while [ $# -gt 0 ]; do
    case "$1" in
        --with-rom)
            with_rom=1
            shift
            case "$1" in
                ""|-*) ;;
                *.z64|*.n64|*.v64) rom=$1; shift ;;
            esac
            ;;
        --no-rom) with_rom=0; shift ;;
        --) shift; break ;;
        -*) echo "make_bundle.sh: unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done

exe=${1:-$here/../build-ppc-darwin/snowboardkids}
out=${2:-$here/../build-ppc-darwin/Snowboard Kids.app}
rom=${rom:-$here/../../snowboardkids.z64}

[ -f "$exe" ] || { echo "no executable at $exe (run port/build-ppc.sh)" >&2; exit 1; }
if [ "$with_rom" = 1 ] && [ ! -f "$rom" ]; then
    echo "--with-rom but no ROM at $rom" >&2
    exit 1
fi

rm -rf "$out"
mkdir -p "$out/Contents/MacOS" "$out/Contents/Resources"
cp "$exe" "$out/Contents/MacOS/isle"
chmod 755 "$out/Contents/MacOS/isle"
[ "$with_rom" = 1 ] && cp "$rom" "$out/Contents/Resources/snowboardkids.z64"
cp "$here/../resources/SnowboardKids.icns" "$out/Contents/Resources/SnowboardKids.icns"
cat > "$out/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>isle</string>
	<key>CFBundleIconFile</key><string>SnowboardKids</string>
	<key>CFBundleIdentifier</key><string>com.southcitycapture.snowboardkids</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>Snowboard Kids</string>
	<key>CFBundleDisplayName</key><string>Snowboard Kids</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleSignature</key><string>????</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>CFBundleGetInfoString</key><string>Snowboard Kids 1+2 PowerPC Edition</string>
	<key>LSMinimumSystemVersion</key><string>10.5</string>
	<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
	<key>NSHighResolutionCapable</key><false/>
</dict>
</plist>
PLIST
printf 'APPL????' > "$out/Contents/PkgInfo"
echo "bundle: $out"
[ "$with_rom" = 1 ] || echo "  (no ROM inside: put your cartridge dumps in ~/Library/Application Support/SnowboardKids/ROMs/)"
ls -la "$out/Contents/MacOS" "$out/Contents/Resources"
