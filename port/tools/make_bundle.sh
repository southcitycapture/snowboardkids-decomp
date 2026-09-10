#!/bin/sh
# Wrap the cross-built executable and the ROM into a Mac OS X .app bundle.
#
#   port/tools/make_bundle.sh [exe] [rom] [out.app]
#
# The executable inside is named `isle` so the isle-ppc-tools g4 console
# runner (which hard-codes that name) can launch it unchanged: on the G4,
# `ln -sfn SnowboardKids.app isle.app` and `g4 run` / `g4 shot` just work.
set -e
here=$(cd "$(dirname "$0")" && pwd)
exe=${1:-$here/../build-ppc-darwin/snowboardkids}
rom=${2:-$here/../../snowboardkids.z64}
out=${3:-$here/../build-ppc-darwin/SnowboardKids.app}

[ -f "$exe" ] || { echo "no executable at $exe (run port/build-ppc.sh)" >&2; exit 1; }
[ -f "$rom" ] || { echo "no ROM at $rom" >&2; exit 1; }

rm -rf "$out"
mkdir -p "$out/Contents/MacOS" "$out/Contents/Resources"
cp "$exe" "$out/Contents/MacOS/isle"
chmod 755 "$out/Contents/MacOS/isle"
cp "$rom" "$out/Contents/Resources/snowboardkids.z64"
cat > "$out/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>isle</string>
	<key>CFBundleIdentifier</key><string>com.southcitycapture.snowboardkids</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>Snowboard Kids</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>0.1</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>10.4</string>
	<key>NSHighResolutionCapable</key><false/>
</dict>
</plist>
EOF
printf 'APPL????' > "$out/Contents/PkgInfo"
echo "bundle: $out"
ls -la "$out/Contents/MacOS" "$out/Contents/Resources"
