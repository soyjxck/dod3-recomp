#!/bin/bash
# Package a Linux release: dist/dod3-recomp-<version>-linux-x86_64.tar.gz
#
#   bash tools/package_linux.sh v0.4.1 [build folder]
#
# Contents: dod3, the default dod3.ini, the player guide as README.md, and
# the licences. Nothing from the game: players install their own files with
# dod3's setup, into the folder the archive unpacks to.
#
# glslang and SPIRV-Tools are linked in statically (Ubuntu ships them so);
# what dod3 needs from the system is glibc, libstdc++, zlib and SDL2 (sdl2,
# or sdl2-compat as SteamOS has it), plus a Vulkan 1.3 driver at run time.
# The libstdc++ is the system's on purpose: a bundled one older than the
# system's breaks Mesa's drivers, which load into the same process. Build in
# the oldest distribution the release is for (tools: Ubuntu 24.04, glibc 2.39)
# and check build-linux/dod3 is the one you mean to ship. The second argument
# picks another build folder.
set -euo pipefail
cd "$(dirname "$0")/.."
ver=${1:?usage: package_linux.sh <version> [build folder]}
build=${2:-build-linux}
name=dod3-recomp-$ver-linux-x86_64
out=dist/$name
rm -rf "$out" "dist/$name.tar.gz"
mkdir -p "$out"

cp "$build/dod3" "$out/"
# the default settings as committed (a local dod3.ini may hold your own)
git show HEAD:dod3.ini > "$out/dod3.ini"
cp docs/PLAYING.md "$out/README.md"
cp LICENSE "$out/LICENSE.txt"
cp THIRD_PARTY_NOTICES.md "$out/THIRD_PARTY_NOTICES.md"
cp ps3recomp/LICENSE "$out/LICENSE-ps3recomp.txt"
cp third_party/imgui/LICENSE.txt "$out/LICENSE-imgui.txt"
cp third_party/fonts/inter/LICENSE.txt "$out/LICENSE-inter.txt"

# What the executable links: nothing outside this list may be needed, or the
# release only runs where that library happens to be installed.
allowed='^(libc\.so\.6|libm\.so\.6|libstdc\+\+\.so\.6|libgcc_s\.so\.1|libz\.so\.1|libSDL2-2\.0\.so\.0|ld-linux-x86-64\.so\.2|libdl\.so\.2|libpthread\.so\.0)$'
needed=$(readelf -d "$out/dod3" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
bad=$(echo "$needed" | grep -Ev "$allowed" || true)
if [ -n "$bad" ]; then echo "package_linux: dod3 needs libraries a player may not have:"; echo "$bad"; exit 1; fi
echo "package_linux: links $(echo $needed | tr '\n' ' ')"
# The newest glibc symbol version it asks for: the oldest glibc it runs on.
glibc=$(objdump -T "$out/dod3" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -t. -k2,2n -u | tail -1)
echo "package_linux: needs $glibc or newer"

# Nothing the player supplies, and no build leftovers.
for bad in EBOOT.ELF EBOOT_101.ELF EBOOT.BIN flashMP3.pic PARAM.SFO keys.txt guest_programs.bin libsre.prx; do
  if find "$out" -iname "$bad" | grep -q .; then echo "package_linux: $bad in the package"; exit 1; fi
done

tar -C dist -czf "dist/$name.tar.gz" "$name"
ls -la "dist/$name.tar.gz"
(cd "$out" && sha256sum dod3)
