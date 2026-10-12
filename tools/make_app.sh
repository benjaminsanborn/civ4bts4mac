#!/bin/sh
# Package the built game as an app (and a disk image) for testers:
#   tools/make_app.sh [--sign "Developer ID Application: ..."] [--notarize]
# Output: build/release/Civ4 BtS Native.app and build/release/Civ4-BtS-Native-VERSION.dmg
#
# The app has no game files of its own: its launcher (tools/setup/Setup.swift)
# asks for the tester's own installation of the Aspyr Mac port, BtS 3.19, and
# the game reads its data from there (CIV4_GAME). It needs a finished build
# (tools/build.sh, tools/link_civ4.sh).
#
# --notarize uses the App Store Connect API key (NOTARY_KEY, NOTARY_KEY_ID,
# NOTARY_ISSUER; defaults below).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
VERSION=0.2.0
NAME="Civ4 BtS Native"
BUNDLE_ID=com.benjaminsanborn.civ4btsnative
NOTARY_KEY=${NOTARY_KEY:-$HOME/.appstoreconnect/private_keys/AuthKey_7J89CUHS7F.p8}
NOTARY_KEY_ID=${NOTARY_KEY_ID:-7J89CUHS7F}
NOTARY_ISSUER=${NOTARY_ISSUER:-38ce5b0b-6ef3-474c-a382-f22b96084c67}

SIGN=- NOTARIZE=
while [ $# -gt 0 ]; do
    case $1 in
        --sign) SIGN=$2; shift 2;;
        --notarize) NOTARIZE=1; shift;;
        *) echo "usage: $0 [--sign IDENTITY] [--notarize]" >&2; exit 2;;
    esac
done

OUT=build/release
APP="$OUT/$NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/gen"

echo "== launcher"
swiftc -parse-as-library -O -target arm64-apple-macos13.0 tools/setup/Setup.swift tools/setup/BugMod.swift tools/setup/Movies.swift \
    -o "$APP/Contents/MacOS/Civ4BtS"

echo "== movie converter"
tools/build_ffmpeg.sh
cp build/ffmpeg/ffmpeg "$APP/Contents/MacOS/ffmpeg"

echo "== game"
cp build/civ4 "$APP/Contents/MacOS/civ4"
strip -S "$APP/Contents/MacOS/civ4" 2>/dev/null || true
cp build/gen/world.img build/gen/exports.txt build/gen/symbols.txt "$APP/Contents/Resources/gen/"
rsync -a --exclude '*.pyc' build/pylib "$APP/Contents/Resources/"

echo "== icon"
# The game's own icon (Aspyr's app), which it only has at 48x48: scaled up for the larger sizes.
ICONSET="$OUT/AppIcon.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
sips -s format png "build/game/Beyond the Sword/Civilization IV Beyond the Sword.app/Contents/Resources/AppIcon.icns" \
    --out "$OUT/icon48.png" >/dev/null
for size in 16 32 128 256 512; do
    sips -z $size $size "$OUT/icon48.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
    sips -z $((size * 2)) $((size * 2)) "$OUT/icon48.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET" "$OUT/icon48.png"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>Civ4BtS</string>
    <key>CFBundleIdentifier</key><string>$BUNDLE_ID</string>
    <key>CFBundleName</key><string>$NAME</string>
    <key>CFBundleDisplayName</key><string>$NAME</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>LSArchitecturePriority</key><array><string>arm64</string></array>
    <key>LSApplicationCategoryType</key><string>public.app-category.strategy-games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHumanReadableCopyright</key><string>Runs your own copy of Civilization IV: Beyond the Sword (Aspyr Mac port). Not affiliated with Firaxis, 2K or Aspyr.</string>
</dict>
</plist>
EOF
cp "$ROOT/tools/release/README.txt" "$APP/Contents/Resources/README.txt"
{
    echo "Third-party software in this app"
    echo "================================"
    echo
    echo "- CPython 2.6.9 (recompiled; the standard library is in Contents/Resources/pylib): the"
    echo "  Python Software Foundation License, below."
    echo "- GNU libstdc++ (Apple libstdcxx-52, GCC 4.2.1) and libsupc++/unwinder (GCC 4.2.1),"
    echo "  recompiled: GPL version 2 with the libstdc++ runtime exception, which allows use in"
    echo "  programs under any license. Sources: https://github.com/apple-oss-distributions/libstdcxx"
    echo "  and https://ftp.gnu.org/gnu/gcc/gcc-4.2.1/"
    echo "- dlmalloc (Doug Lea, 2023): MIT No Attribution-style permissive license."
    echo "- FFmpeg 7.1 (Contents/MacOS/ffmpeg, converts the game's Sorenson Video movies): GNU Lesser"
    echo "  General Public License 2.1 or later, built without GPL parts by tools/build_ffmpeg.sh in"
    echo "  https://github.com/benjaminsanborn/civ4bts4mac from https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz"
    echo "  (unmodified). Its license is at the end of this file."
    echo
    echo "------------------------------------------------------------------------------"
    cat third_party/Python-2.6.9/LICENSE
    echo
    echo "------------------------------------------------------------------------------"
    cat third_party/ffmpeg-7.1/COPYING.LGPLv2.1
} > "$APP/Contents/Resources/LICENSES.txt"

echo "== sign ($SIGN)"
OPTS="--force --options runtime --timestamp"
[ "$SIGN" = - ] && OPTS="--force --options runtime"
codesign $OPTS --sign "$SIGN" "$APP/Contents/MacOS/civ4"
codesign $OPTS --sign "$SIGN" "$APP/Contents/MacOS/ffmpeg"
codesign $OPTS --sign "$SIGN" "$APP/Contents/MacOS/Civ4BtS"
codesign $OPTS --sign "$SIGN" "$APP"
codesign --verify --deep --strict "$APP"

notarize() { # notarize FILE: submit, wait, staple
    xcrun notarytool submit "$1" --key "$NOTARY_KEY" --key-id "$NOTARY_KEY_ID" --issuer "$NOTARY_ISSUER" --wait
}
if [ -n "$NOTARIZE" ]; then
    echo "== notarize app"
    ditto -c -k --keepParent "$APP" "$OUT/app.zip"
    notarize "$OUT/app.zip"
    rm "$OUT/app.zip"
    xcrun stapler staple "$APP"
    spctl --assess --type execute -v "$APP"
fi

echo "== disk image"
DMG="$OUT/Civ4-BtS-Native-$VERSION.dmg"
STAGE="$OUT/dmg"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
cp tools/release/README.txt "$STAGE/Read Me.txt"
ln -s /Applications "$STAGE/Applications"
hdiutil create -quiet -volname "$NAME $VERSION" -srcfolder "$STAGE" -format UDZO -fs HFS+ "$DMG"
rm -rf "$STAGE"
[ "$SIGN" = - ] || codesign --force --timestamp --sign "$SIGN" "$DMG"

if [ -n "$NOTARIZE" ]; then
    echo "== notarize disk image"
    notarize "$DMG"
    xcrun stapler staple "$DMG"
    xcrun stapler validate "$DMG"
    spctl --assess --type open --context context:primary-signature -v "$DMG"
fi
ls -la "$OUT"
