#!/bin/bash
# Package a macOS release: dist/dod3-recomp-<version>-macos-arm64.zip
#
#   tools/package_mac.sh v0.1.0
#
# Contents: "Drakengard 3 Recompiled.app" (the game and its installer, SDL2
# inside it, the default dod3.ini, an icon drawn by tools/make_icon.py), the
# player guide as README.txt and the licences. Nothing from the game: the
# player's own files are installed by the app's setup (src/ui/wizard.cpp)
# into ~/Library/Application Support.
#
# It builds its own Release tree, build-macos-release/ -- the 1.01 build, from
# recompiled_101/ and spu/ -- for Apple silicon and macOS
# $MACOSX_DEPLOYMENT_TARGET (default 13.0), against the libraries from
# tools/build_mac_deps.sh rather than Homebrew's, which only run on the macOS
# they were installed on. It needs this tree's lifted recompiled/ and spu/,
# as any build does. The app is signed ad hoc: without a Developer ID it is
# not notarized, so players open it the first time with right-click > Open.
set -euo pipefail
cd "$(dirname "$0")/.."
ver=${1:?usage: package_mac.sh <version>}
target=${MACOSX_DEPLOYMENT_TARGET:-13.0}
deps=$PWD/deps/macos-arm64
build=build-macos-release
product="Drakengard 3 Recompiled"
name=dod3-recomp-$ver-macos-arm64
out=dist/$name
app="$out/$product.app"
[ -d "$deps/lib" ] || { echo "package_mac: no $deps -- run tools/build_mac_deps.sh first"; exit 1; }

# ---- build ---------------------------------------------------------------------
# Homebrew is kept out of every search (CMake packages, pkg-config, plain
# find_library) so nothing in the binary needs a newer macOS than the target.
mkdir -p "$build"
PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig" PKG_CONFIG_PATH="" \
cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DDOD3_EBOOT=101 -DRECOMP_DIR="$PWD/recompiled_101" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET="$target" \
    -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
    -DCMAKE_FIND_FRAMEWORK=LAST > "$build/package-configure.log"
grep -E "SDL2:|Shaders:" "$build/package-configure.log" || true
cmake --build "$build"
bin="$build/dod3"
if otool -L "$bin" | grep -E "/opt/homebrew|/usr/local" ; then echo "package_mac: links Homebrew"; exit 1; fi
minos=$(otool -l "$bin" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
[ "$minos" = "$target" ] || { echo "package_mac: built for macOS $minos, not $target"; exit 1; }

# ---- the app ---------------------------------------------------------------------
rm -rf "$out" "dist/$name.zip"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks" "$app/Contents/Resources"
cp "$bin" "$app/Contents/MacOS/dod3"
cp "$deps/lib/libSDL2-2.0.0.dylib" "$app/Contents/Frameworks/"
install_name_tool -add_rpath @executable_path/../Frameworks "$app/Contents/MacOS/dod3"
cp dod3.ini "$app/Contents/Resources/dod3.ini"
# The shader cache of a playthrough (cache/msl: translated shaders keyed on
# their source; cache/pipelines.list: every pipeline built; the compiled
# archive is per machine and not shipped). At first launch the game warms
# every listed pipeline up from the MSL on all cores, before the title
# screen, instead of compiling each in play. Seeded into the data folder by
# the first-run setup (src/os/macos/setup_mac.mm) from Contents/Resources/cache.
if [ -d cache/msl ] && [ -f cache/pipelines.list ]; then
    mkdir -p "$app/Contents/Resources/cache"
    cp -r cache/msl "$app/Contents/Resources/cache/"
    cp cache/pipelines.list "$app/Contents/Resources/cache/"
fi

# The icon: tools/make_icon.py's drawing (an original), at every size an
# .icns holds.
iconset=$(mktemp -d)/AppIcon.iconset
mkdir -p "$iconset"
"${PYTHON:-python3}" -c "import sys; sys.path.insert(0, 'tools'); import make_icon; make_icon.render().save(sys.argv[1])" \
    "$iconset/icon_512x512@2x.png"
for s in 16 32 128 256 512; do
    sips -z $s $s "$iconset/icon_512x512@2x.png" --out "$iconset/icon_${s}x${s}.png" > /dev/null
    sips -z $((s * 2)) $((s * 2)) "$iconset/icon_512x512@2x.png" --out "$iconset/icon_${s}x${s}@2x.png" > /dev/null
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"

short=${ver#v}
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>                 <string>$product</string>
    <key>CFBundleDisplayName</key>          <string>$product</string>
    <key>CFBundleExecutable</key>           <string>dod3</string>
    <key>CFBundleIdentifier</key>           <string>io.github.soyjxck.dod3-recomp</string>
    <key>CFBundlePackageType</key>          <string>APPL</string>
    <key>CFBundleIconFile</key>             <string>AppIcon</string>
    <key>CFBundleShortVersionString</key>   <string>$short</string>
    <key>CFBundleVersion</key>              <string>$short</string>
    <key>LSMinimumSystemVersion</key>       <string>$target</string>
    <key>LSApplicationCategoryType</key>    <string>public.app-category.action-games</string>
    <key>NSHighResolutionCapable</key>      <true/>
    <key>GCSupportsControllerUserInteraction</key> <true/>
    <key>NSPrincipalClass</key>             <string>NSApplication</string>
</dict>
</plist>
EOF

# Ad-hoc signatures, inside out. No hardened runtime: its library validation
# refuses dylibs without a Team ID, which ad-hoc signing does not give.
codesign --force -s - "$app/Contents/Frameworks/libSDL2-2.0.0.dylib"
codesign --force -s - "$app"
codesign --verify --deep --strict "$app"

# ---- the rest of the package ------------------------------------------------------
cp docs/PLAYING.md "$out/README.txt"
cp LICENSE "$out/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$out/THIRD_PARTY_NOTICES.txt"
cp ps3recomp/LICENSE "$out/LICENSE-ps3recomp.txt"
cp third_party/imgui/LICENSE.txt "$out/LICENSE-imgui.txt"
{
  printf 'The libraries built into the app (tools/build_mac_deps.sh):\n'
  printf '\n\nSDL 2 (libSDL2-2.0.0.dylib, controllers and audio): zlib License\n\n'
  cat deps/src/SDL2/LICENSE.txt
  printf '\n\nglslang (shader translation)\n\n'
  cat deps/src/glslang/LICENSE.txt
  printf '\n\nSPIRV-Tools (shader translation): Apache License 2.0\n\n'
  cat deps/src/glslang/External/spirv-tools/LICENSE
  printf '\n\nSPIRV-Cross (shader translation): Apache License 2.0\n\n'
  cat deps/src/SPIRV-Cross/LICENSE
} > "$out/LICENSE-libraries.txt"

# Nothing the player supplies, and no build leftovers.
for bad in EBOOT.ELF EBOOT_101.ELF EBOOT.BIN flashMP3.pic PARAM.SFO keys.txt; do
  if find "$out" -iname "$bad" | grep -q .; then echo "package_mac: $bad in the package"; exit 1; fi
done

(cd dist && ditto -c -k --keepParent "$name" "$name.zip")
ls -la "dist/$name.zip"
shasum -a 256 "$app/Contents/MacOS/dod3"
