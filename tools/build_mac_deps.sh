#!/bin/bash
# The libraries the macOS release links, built for an older macOS than this
# machine's. Homebrew builds its bottles for the macOS they are installed on,
# so a binary linked against them needs that macOS too (glslang's: 27.0).
#
#   tools/build_mac_deps.sh        -> deps/macos-arm64 (git-ignored)
#
#   SDL2          pads and audio; a dylib, shipped inside the app
#   glslang       HLSL -> SPIR-V for the Metal backend's shader translation,
#                 with SPIRV-Tools for its HLSL legalization passes (static)
#   SPIRV-Cross   SPIR-V -> MSL (static, C API)
#
# MACOSX_DEPLOYMENT_TARGET (default 13.0) is the oldest macOS the release
# supports; nothing in the host code needs newer (the Metal backend falls
# back where macOS 26 added visibility accumulation). tools/package_mac.sh
# builds against this prefix.
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
PREFIX=$PWD/deps/macos-arm64
SRC=$PWD/deps/src
SDL_TAG=release-2.32.10
KHR_TAG=vulkan-sdk-1.4.363.0      # glslang, SPIRV-Tools, SPIRV-Headers, SPIRV-Cross

fetch() {   # repo dir tag
    [ -d "$2/.git" ] || git clone -q --depth 1 --branch "$3" "https://github.com/$1.git" "$2"
}
mkdir -p "$SRC" "$PREFIX"
fetch libsdl-org/SDL               "$SRC/SDL2"                                            "$SDL_TAG"
fetch KhronosGroup/glslang         "$SRC/glslang"                                         "$KHR_TAG"
fetch KhronosGroup/SPIRV-Tools     "$SRC/glslang/External/spirv-tools"                    "$KHR_TAG"
fetch KhronosGroup/SPIRV-Headers   "$SRC/glslang/External/spirv-tools/external/spirv-headers" "$KHR_TAG"
fetch KhronosGroup/SPIRV-Cross     "$SRC/SPIRV-Cross"                                     "$KHR_TAG"

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$TARGET" -DCMAKE_INSTALL_PREFIX="$PREFIX"
        -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_FIND_FRAMEWORK=LAST)
build() {   # name source-dir cmake-args...
    local name=$1 src=$2; shift 2
    echo "== $name"
    cmake -S "$src" -B "$SRC/build-$name" "${common[@]}" "$@" > "$SRC/build-$name.log"
    cmake --build "$SRC/build-$name" >> "$SRC/build-$name.log"
    cmake --install "$SRC/build-$name" >> "$SRC/build-$name.log"
}

build SDL2 "$SRC/SDL2" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_TESTS=OFF
build glslang "$SRC/glslang" -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=ON -DGLSLANG_TESTS=OFF \
      -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_ENABLE_INSTALL=ON -DSPIRV_SKIP_TESTS=ON \
      -DSPIRV_SKIP_EXECUTABLES=ON
build SPIRV-Cross "$SRC/SPIRV-Cross" -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON \
      -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_C_API=ON

echo "deps: $PREFIX (macOS $TARGET, arm64)"
