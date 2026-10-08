#!/bin/bash
# Package a Windows release: dist/dod3-recomp-<version>-win64.zip
#
#   bash tools/package_win.sh v0.1.0
#
# Contents: dod3.exe and the DLLs it needs that Windows 10/11 does not ship
# (zlib1.dll from the build; the Visual C++ runtime, app-local, as
# Microsoft's redistribution terms allow), the default dod3.ini, the player
# guide as README.txt, and the licence notices. Nothing from the game or the
# PS3 firmware: the player's own files are installed by dod3.exe's setup.
#
# Build first (tools\build_win.bat build), and check build/dod3.exe is the
# one you mean to ship.
set -euo pipefail
cd "$(dirname "$0")/.."
ver=${1:?usage: package_win.sh <version>}
name=dod3-recomp-$ver-win64
out=dist/$name
rm -rf "$out" "dist/$name.zip"
mkdir -p "$out"

cp build/dod3.exe build/zlib1.dll "$out/"
crt=$(ls -d "/c/Program Files (x86)/Microsoft Visual Studio/2022/"*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT 2>/dev/null | sort | tail -1)
[ -n "$crt" ] || { echo "package_win: Visual C++ redistributable folder not found"; exit 1; }
for dll in msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do cp "$crt/$dll" "$out/"; done
cp dod3.ini "$out/"
# Windows line endings for Notepad.
sed 's/$/\r/' docs/PLAYING.md > "$out/README.txt"
{
  printf 'Third-party components\r\n\r\n'
  printf 'ps3recomp (the recompilation toolkit and runtime): MIT License, see https://github.com/sp00nznet/ps3recomp\r\n\r\n'
  sed 's/$/\r/' ps3recomp/LICENSE
  printf '\r\nzlib (zlib1.dll): zlib License, Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler\r\n'
  printf '\r\nMicrosoft Visual C++ runtime (msvcp140.dll, vcruntime140*.dll): redistributed under the Visual Studio license terms\r\n'
} > "$out/THIRD-PARTY.txt"
[ -f LICENSE ] && sed 's/$/\r/' LICENSE > "$out/LICENSE.txt"

# Nothing the player supplies, and no build leftovers.
for bad in EBOOT.ELF EBOOT.BIN flashMP3.pic PARAM.SFO; do
  if find "$out" -iname "$bad" | grep -q .; then echo "package_win: $bad in the package"; exit 1; fi
done

powershell -NoProfile -Command "Compress-Archive -Path '$out' -DestinationPath 'dist/$name.zip' -CompressionLevel Optimal"
ls -la "dist/$name.zip"
(cd "$out" && sha256sum dod3.exe)
