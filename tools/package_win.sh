#!/bin/bash
# Package a Windows release: dist/dod3-recomp-<version>-win64.zip
#
#   bash tools/package_win.sh v0.1.0 [build folder]
#
# Contents: dod3.exe and the DLLs it needs that Windows 10/11 does not ship
# (zlib1.dll from the build; the Visual C++ runtime, app-local, as
# Microsoft's redistribution terms allow), the default dod3.ini, the player
# guide as README.txt, and the licences. Nothing from the game: players
# install their own files with dod3.exe's setup.
#
# Build first (tools\build_win.bat) and check build/dod3.exe is the one you
# mean to ship. The second argument picks another build folder.
set -euo pipefail
cd "$(dirname "$0")/.."
ver=${1:?usage: package_win.sh <version> [build folder]}
build=${2:-build}
name=dod3-recomp-$ver-win64
out=dist/$name
rm -rf "$out" "dist/$name.zip"
mkdir -p "$out"

cp "$build/dod3.exe" "$build/zlib1.dll" "$build/SDL2.dll" "$out/"
crt=$(ls -d /c/Program\ Files*/Microsoft\ Visual\ Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT 2>/dev/null | sort | tail -1)
[ -n "$crt" ] || { echo "package_win: the Visual C++ redistributable folder was not found"; exit 1; }
for dll in msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do cp "$crt/$dll" "$out/"; done
cp dod3.ini "$out/"
# The shader cache of a playthrough (cache/dxbc, bytecode keyed on the shader
# text this build generates): most pipelines are never compiled on a player's
# machine. The game reads it from cache/dxbc beside the executable.
if [ -d cache/dxbc ]; then mkdir -p "$out/cache"; cp -r cache/dxbc "$out/cache/"; fi
# Windows line endings, for Notepad.
sed 's/$/\r/' docs/PLAYING.md > "$out/README.txt"
sed 's/$/\r/' LICENSE > "$out/LICENSE.txt"
sed 's/$/\r/' THIRD_PARTY_NOTICES.md > "$out/THIRD_PARTY_NOTICES.txt"
sed 's/$/\r/' ps3recomp/LICENSE > "$out/LICENSE-ps3recomp.txt"
sed 's/$/\r/' third_party/imgui/LICENSE.txt > "$out/LICENSE-imgui.txt"

# Nothing the player supplies, and no build leftovers.
for bad in EBOOT.ELF EBOOT_101.ELF EBOOT.BIN flashMP3.pic PARAM.SFO keys.txt; do
  if find "$out" -iname "$bad" | grep -q .; then echo "package_win: $bad in the package"; exit 1; fi
done

powershell -NoProfile -Command "Compress-Archive -Path '$out' -DestinationPath 'dist/$name.zip' -CompressionLevel Optimal"
ls -la "dist/$name.zip"
(cd "$out" && sha256sum dod3.exe)
