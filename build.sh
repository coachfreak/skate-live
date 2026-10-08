#!/usr/bin/env bash
# Builds "Skate.live.app" (launcher + injected LAN library + server) and
# "Skate.live Server.app" (server only, on the Tailscale IP) into dist/.
# Needs Homebrew LLVM (brew install llvm), the Xcode Command Line Tools,
# skate3recomp 2.0.2 installed (SKATE3_APP, default /Applications/skate3recomp.app;
# its librexruntime.dylib is linked against) and a skate3recomp checkout with
# submodules for the headers (SKATE3RECOMP; v2.0.2 = rexglue-sdk 7eb0faf).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
: "${SKATE3RECOMP:?set SKATE3RECOMP to a skate3recomp checkout (git clone --recursive https://github.com/mchughalex/skate3recomp)}"
SDK="$SKATE3RECOMP/third_party/rexglue-sdk"
RUNTIME="${SKATE3_APP:-/Applications/skate3recomp.app}/Contents/MacOS/librexruntime.dylib"
BUILD="$HERE/build"
DIST="$HERE/dist"
APP="$DIST/Skate.live.app"
mkdir -p "$BUILD" "$DIST"

python3 "$HERE/server/skate3_lan_server.py" --test

/opt/homebrew/opt/llvm/bin/clang++ -std=gnu++23 -O2 -arch arm64 -mmacosx-version-min=12.0 \
  -dynamiclib -fvisibility=hidden -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -DREX_HAS_VULKAN=1 \
  -I"$SDK/include" -I"$SDK/thirdparty/simde" -I"$SDK/thirdparty/fmt/include" \
  -I"$SDK/thirdparty/spdlog/include" -isystem "$SDK/thirdparty/tomlplusplus/include" \
  -I"$SDK/thirdparty/vulkan-headers/include" -I"$SDK/thirdparty/volk" \
  "$HERE/dylib/lan.cpp" "$HERE/dylib/window.mm" -fobjc-arc -framework AppKit "$RUNTIME" -Wl,-rpath,@executable_path \
  -install_name @rpath/libskate3lan.dylib -o "$BUILD/libskate3lan.dylib"

swiftc -O -parse-as-library -target arm64-apple-macos14.0 "$HERE/launcher/Skate3LAN.swift" \
  "$HERE/launcher/SkateStyle.swift" -o "$BUILD/Skate3LAN"

# App icon: the LAN glyph in a paper-and-slate frame.
ICONSET="$BUILD/LANAppIcon.iconset"
if [ ! -f "$BUILD/LANAppIcon.icns" ] || [ "$HERE/launcher/make_glyph.swift" -nt "$BUILD/LANAppIcon.icns" ] ||
   [ "$HERE/launcher/make_icon.swift" -nt "$BUILD/LANAppIcon.icns" ]; then
  swift "$HERE/launcher/make_glyph.swift" "$HERE/launcher/LANIcon.png"
  swift "$HERE/launcher/make_icon.swift" "$HERE/launcher/LANIcon.png" "$BUILD/lan_icon.png"
  mkdir -p "$ICONSET"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$BUILD/lan_icon.png" --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$BUILD/lan_icon.png" --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
  done
  iconutil -c icns "$ICONSET" -o "$BUILD/LANAppIcon.icns"
fi

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$HERE/launcher/Info.plist" "$APP/Contents/"
cp "$BUILD/Skate3LAN" "$APP/Contents/MacOS/"
cp "$BUILD/libskate3lan.dylib" "$HERE/server/skate3_lan_server.py" "$HERE/launcher/LANIcon.png" "$APP/Contents/Resources/"
cp "$BUILD/LANAppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
codesign --force -s - "$APP/Contents/Resources/libskate3lan.dylib"
codesign --force -s - "$APP"
echo "Built: $APP"

# "Skate.live Server.app": only the server, on this Mac's Tailscale IP (server_app/).
SERVER_APP="$DIST/Skate.live Server.app"
rm -rf "$SERVER_APP"
mkdir -p "$SERVER_APP/Contents/MacOS" "$SERVER_APP/Contents/Resources"
cp "$HERE/server_app/Info.plist" "$SERVER_APP/Contents/"
cp "$HERE/server_app/run" "$SERVER_APP/Contents/MacOS/"
cp "$HERE/server_app/start_server.command" "$HERE/server/skate3_lan_server.py" "$SERVER_APP/Contents/Resources/"
cp "$BUILD/LANAppIcon.icns" "$SERVER_APP/Contents/Resources/AppIcon.icns"
chmod +x "$SERVER_APP/Contents/MacOS/run" "$SERVER_APP/Contents/Resources/start_server.command"
codesign --force -s - "$SERVER_APP"
echo "Built: $SERVER_APP"

# Release disk image: both apps plus an Applications shortcut
mkdir -p "$BUILD/dmg"
cp -R "$APP" "$SERVER_APP" "$BUILD/dmg/"
ln -s /Applications "$BUILD/dmg/Applications"
hdiutil create -volname "Skate.live" -srcfolder "$BUILD/dmg" -ov -format UDZO "$DIST/Skate.live.dmg" >/dev/null
rm -r "${BUILD:?}/dmg"
echo "Built: $DIST/Skate.live.dmg"
