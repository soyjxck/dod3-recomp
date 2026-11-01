# Building

The port is built in three steps: the game's PPU code is lifted to C++, the
SPU programs are lifted, and the whole is compiled with CMake. Lifting needs
the game's executable, so you need your own copy of the game first.

## Prerequisites

Windows:

- Visual Studio 2022 or newer -- the Build Tools are enough -- with "Desktop
  development with C++" and the Windows SDK.
- LLVM with clang-cl (the toolchain used; MSVC's `cl` is not: the SPU
  dispatch relies on `musttail`, and the lifted code uses Clang builtins).
- CMake 3.20 or later, Ninja, Python 3.11 or later, Git for Windows (the
  `tools/*.sh` scripts run under Git Bash).
- vcpkg with `zlib:x64-windows` and `sdl2:x64-windows` (`VCPKG_ROOT`, or
  `C:\vcpkg`). SDL2 reads the controllers.
- Optional: the Vulkan SDK (`VULKAN_SDK`) for the Vulkan renderer.

macOS (Apple silicon):

- Xcode's command-line tools; from Homebrew: `cmake ninja python zlib sdl2
  glslang spirv-cross`.

Linux (x86-64):

- Clang 18 or later, CMake 3.20 or later, Ninja, Python 3.11 or later, and
  the development files of SDL2, zlib, the Vulkan headers and glslang built
  with SPIRV-Tools (the game's shaders go HLSL -> SPIR-V through it). On
  Ubuntu 24.04: `clang cmake ninja-build python3-venv libsdl2-dev
  zlib1g-dev libvulkan-dev glslang-dev glslang-tools spirv-tools`.
- A release is built in the oldest distribution it is for, since the
  executable runs on that glibc or newer: Ubuntu 24.04 (its build needs glibc 2.38) runs on
  SteamOS and on current distributions. On a Steam Deck, whose system is
  read-only and has no compiler, that is a distrobox:
  `distrobox create --name dod3-build --image docker.io/library/ubuntu:24.04`
  and the packages above in it; the build folder is in your home, so the
  result runs on the Deck itself.

Then, on any of them:

```sh
git clone --recurse-submodules https://github.com/soyjxck/dod3-recomp
cd dod3-recomp
python3 -m venv .venv                    # py -3 -m venv .venv on Windows
.venv/bin/pip install -r ps3recomp/tools/requirements.txt -r tools/requirements.txt
```

CMake uses the venv's Python (`.venv/bin/python` or `.venv\Scripts\python.exe`)
for the code it generates at build time.

## The game's files

The port is built from the game's 1.01 update: its executable, decrypted, as
`elf/EBOOT_101.ELF` (SHA-256 `ed89e80f2336a948…`, 26,872,424 bytes). The
installer makes it from the update package, and lays out `game/disc` from
your disc or the PSN release's game package at the same time. Any built `dod3` can do that into the
repository root before the first lift -- a release build, or a build from
another machine:

```sh
DOD3_INSTALL_BASE=. path/to/dod3 --install <disc .iso or folder, or PSN game .pkg> <update .pkg> [DLC .pkg ...]
```

Any other decrypted copy of the update's EBOOT.BIN with that hash works too
(RPCS3's *Utilities > Decrypt PS3 Binaries*, for one). `dod3 --check
<file>...` says what the installer makes of a file. On a platform with no
`dod3` yet (a first Linux build), RPCS3's decrypt is the way in: the update
package installed in RPCS3, then `rpcs3 --decrypt
<dev_hdd0>/game/BLUS31197/USRDIR/EBOOT.BIN`, saved as `elf/EBOOT_101.ELF`.

The build also lifts part of the console's SPURS library (the queue paths,
`tools/gen_libsre.py`), so it needs a decrypted `libsre.prx` at
`ps3recomp/fw_spu/libsre.prx` (git-ignored; `LIBSRE_PRX` points elsewhere).
It is the firmware's `dev_flash/sys/external/libsre.sprx`, decrypted: with
the firmware installed in RPCS3 (*File > Install Firmware*, the PS3UPDAT.PUP
from PlayStation's site), `rpcs3 --decrypt
<rpcs3>/dev_flash/sys/external/libsre.sprx` writes `libsre.prx` beside it.
Firmware 4.91 is known to work.

## Lifting the PPU code

```sh
.venv/bin/python ps3recomp/tools/ppu_loader.py elf/EBOOT_101.ELF -o out/v101/
.venv/bin/python ps3recomp/tools/ppu_lifter.py elf/EBOOT_101.ELF \
    --functions out/v101/EBOOT_101.functions.json \
    --hle-stubs out/v101/EBOOT_101.imports.json \
    --code-end 0x157fc00 \
    --nonvolatile-locals \
    --hook 0x000C1E50 --hook 0x00EE7728 --hook 0x00272E58 \
    -o recompiled_101/
```

- `--code-end` is the end of the last executable section, so `.rodata` in the
  R-X segment is never promoted to functions.
- `--nonvolatile-locals` keeps the guest registers in C locals, written back
  to the register file only where they changed and something reads them
  (`ps3recomp/docs/PPU_RECOMP.md`); the port is tuned for it.
- The three `--hook`s are required: those functions are supplied natively
  (the garbage collector and its reachability pass, `src/patches/gc.cpp` and
  `src/patches/gc_native.cpp`; the collision test's timing wrapper,
  `src/cpu/hot.cpp`) and the lifted bodies are emitted as
  `func_<addr>_lifted`. A lift without them fails to link.

The lift takes a few minutes and writes about 600 MB of C++ (14 translation
units). `recompiled_101/` is not in git.

## Lifting the SPU programs

```sh
tools/lift_spu.sh
```

extracts the SPU programs embedded in `elf/EBOOT_101.ELF` (the SPURS tasks,
the raw ShaderPatching job, MultiStream and its DSP plugins) and lifts them
into `spu/`, with the native fast paths hooked in (`--native-hook`). It also
writes the stand-in MP3 decoder image (`tools/make_spu_overlays.py`). Lifts
already in `spu/` are kept, so a second run only redoes what changed.

## Compiling

Windows (from a plain command prompt; the script finds Visual Studio, LLVM,
CMake and vcpkg):

```bat
tools\build_win.bat            rem configure + build into build
tools\build_win.bat build      rem build only
```

Or run CMake yourself from a "x64 Native Tools" prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
      -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

macOS:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linux (in the distrobox on a Deck: `distrobox enter dod3-build` first):

```sh
CC=clang CXX=clang++ cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j4
```

The renderer there is the draw engine over Vulkan 1.3 (ps3recomp's
`rsx_vulkan_engine.c`, which Windows runs with `RSX_BACKEND=vulkan`) in an
SDL2 window, and the setup's window is SDL2 too (`src/ui/ui_linux.cpp`); the
configure step stops if glslang is missing, as there would be no shaders.
Each lifted unit takes several GB of memory to compile at `-O3`: on a 16 GB
machine (a Steam Deck) build with `-j4` or fewer.

A clean build takes about 15 minutes, nearly all of it the lifted translation
units; a header every lifted unit includes (`ppu_recomp.h`, `ppu_vm_fast.h`)
rebuilds all of them. A build folder keeps `RECOMP_DIR` in its cache, so one
configured for the other version must be removed, or given `-DRECOMP_DIR=`
explicitly (the scripts do); CMake refuses a lift that is not the version's.
CMake options:

| Option | Default | |
|---|---|---|
| `DOD3_EBOOT` | 101 | the version the lift is from. 100 builds the disc version instead (`elf/EBOOT.ELF`, `recompiled/`, `src/cpu/eboot.h`'s other column); kept for comparison, not maintained |
| `RECOMP_DIR` | `recompiled_101` | the lifted PPU C++ |
| `DOD3_X86_LEVEL` | x86-64-v3 | the lifted code's x86 level (clang-cl, and clang on Linux); empty for the baseline. The default needs AVX2, FMA and BMI2, and the executable checks for them at start |
| `DOD3_INLINE_VM` | ON | inline the guest-memory fast path into the lifted code |
| `DOD3_SYMBOLS` | ON | Windows: `/Z7` on the port's and the runtime's sources, `/DEBUG` at link, for the profiler and the crash handler |
| `DOD3_CONSOLE` | OFF | Windows: link as a console program |
| `LIBSRE_PRX` | the submodule's | the module the SPURS queue paths are lifted from at build time (`tools/gen_libsre.py`) |
| `DOD3_PGO` | empty | profile-guided optimisation of the lifted code alone, in a build folder of its own: `generate` instruments it (the game then writes the profile named by `LLVM_PROFILE_FILE` every 30 s and on exit), `use:<profdata>` compiles it on a merged profile (`llvm-profdata merge -o lifted.profdata *.profraw`) |

The replay tool (`rsx_replay`, for the rendering regression) is not part of
the default target: `cmake --build build --target rsx_replay`, or
`tools\build_win.bat build rsx_replay`.

## Running

With no arguments, `dod3` runs the game installed next to it (the release
layout: `game/`, `elf/`, `dod3.ini`, `gamedata/`, `cache/` beside the
executable), and opens the installer when it is not installed yet.
`DOD3_INSTALL_BASE=<dir>` points that at another folder -- the repository
root, for a developer tree:

```sh
DOD3_INSTALL_BASE=. build/dod3
```

The developer form takes the executable to run, with the environment it
needs:

```sh
PS3_TITLE="Drakengard 3" PS3_VFS_ROOT=game/disc DOD3_FPS=60 build/dod3 elf/EBOOT_101.ELF
tools/run_timed.sh 120 out/run.log        # a bounded run with a log
```

`dod3.ini` is read from the executable's folder (CMake copies the
repository's once into the build folder) or the current one; a variable set
in the environment wins over it.

## Releases

- Windows: `bash tools/package_win.sh v0.1.0` zips `build/dod3.exe` with the
  DLLs it needs, `dod3.ini`, the player guide and the licences into `dist/`.
- macOS: `tools/build_mac_deps.sh` once (SDL2, glslang, SPIRV-Cross built for
  the deployment target), then `tools/package_mac.sh v0.1.0` builds its own
  release tree and makes the signed `.app`.
- Linux: `bash tools/package_linux.sh v0.1.0` packs `build-linux/dod3` with
  `dod3.ini`, the player guide and the licences into
  `dist/dod3-recomp-<version>-linux-x86_64.tar.gz`, and refuses an executable
  that needs a library beyond glibc, libstdc++, zlib and SDL2 (glslang and
  SPIRV-Tools are linked in), printing the oldest glibc it runs on.

All three refuse to package anything of the game's.

## Regenerating the committed tables

| File | Tool |
|---|---|
| `src/patches/menu_patch_data_101.h` | `python tools/menu_patch.py --eboot=101 --header=src/patches/menu_patch_data_101.h` (`src/patches/menu_patch_data.h` without `--eboot`, for 1.00) |
| `src/apu/mp3dec_tables.h` | `python tools/gen_mp3_tables.py <minimp3.h> > src/apu/mp3dec_tables.h` |
| `src/apu/mp3_standin.h` | `tools/lift_spu.sh` (through `tools/make_spu_overlays.py standin`) |
| `src/res/app.ico` | `python tools/make_icon.py src/res/app.ico` |
| `src/res/prompts/*.png` | `python tools/prompt_atlas.py --font Roboto-Bold.ttf` (the button-prompt atlases and the sheet of key caps the keyboard's is made from; the build embeds them as `gen/prompts_data.h`) |
| `src/patches/collision_native.cpp` | `python tools/lift_native.py <lift>/ppu_recomp_010.cpp func_00272E58 --name dod3_collision_native --inline-zero func_00252A70 -o src/patches/collision_native.cpp`, from a lift made with `PPU_NV_DIRTY=0` in the environment into a folder of its own (`-o <lift>/`): the tool reads the form in which every register is written back at every call, not the one the game is built from. The lifted body may be in another unit (`grep -l func_00272E58_lifted <lift>/*.cpp`) |
