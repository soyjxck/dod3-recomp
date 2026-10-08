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
- vcpkg with `zlib:x64-windows` (`VCPKG_ROOT`, or `C:\vcpkg`).
- Optional: the Vulkan SDK (`VULKAN_SDK`) for the Vulkan renderer.

macOS (Apple silicon):

- Xcode's command-line tools; from Homebrew: `cmake ninja python zlib sdl2
  glslang spirv-cross`.

Then, on either:

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
installer makes it from your disc and the update package, and lays out
`game/disc` at the same time. Any built `dod3` can do that into the
repository root before the first lift -- a release build, or a build from
another machine:

```sh
DOD3_INSTALL_BASE=. path/to/dod3 --install <disc .iso or folder> <update .pkg> [DLC .pkg ...]
```

Any other decrypted copy of the update's EBOOT.BIN with that hash works too
(RPCS3's *Utilities > Decrypt PS3 Binaries*, for one). `dod3 --check
<file>...` says what the installer makes of a file.

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
- `--nonvolatile-locals` keeps the guest registers in C locals between calls;
  the port is tuned for it.
- The three `--hook`s are required: those functions are supplied natively
  (the garbage collector and its reachability pass, `src/dod3_gc.cpp` and
  `src/dod3_gc_native.cpp`; the collision test's timing wrapper,
  `src/dod3_hot.cpp`) and the lifted bodies are emitted as
  `func_<addr>_lifted`. A lift without them fails to link.

The lift takes a few minutes and writes about 550 MB of C++ (14 translation
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

A clean build takes about 15 minutes, nearly all of it the lifted translation
units; a header every lifted unit includes (`ppu_recomp.h`, `ppu_vm_fast.h`)
rebuilds all of them. A build folder keeps `RECOMP_DIR` in its cache, so one
configured for the other version must be removed, or given `-DRECOMP_DIR=`
explicitly (the scripts do); CMake refuses a lift that is not the version's.
CMake options:

| Option | Default | |
|---|---|---|
| `DOD3_EBOOT` | 101 | the version the lift is from. 100 builds the disc version instead (`elf/EBOOT.ELF`, `recompiled/`, `src/dod3_eboot.h`'s other column); kept for comparison, not maintained |
| `RECOMP_DIR` | `recompiled_101` | the lifted PPU C++ |
| `DOD3_X86_LEVEL` | x86-64-v3 | the lifted code's x86 level (clang-cl); empty for the baseline. The default needs AVX2, FMA and BMI2, and the executable checks for them at start |
| `DOD3_INLINE_VM` | ON | inline the guest-memory fast path into the lifted code |
| `DOD3_SYMBOLS` | ON | Windows: `/Z7` on the port's and the runtime's sources, `/DEBUG` at link, for the profiler and the crash handler |
| `DOD3_CONSOLE` | OFF | Windows: link as a console program |
| `LIBSRE_PRX` | the submodule's | the module the SPURS queue paths are lifted from at build time (`tools/gen_libsre.py`) |

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

Both refuse to package anything of the game's.

## Regenerating the committed tables

| File | Tool |
|---|---|
| `src/dod3_menu_patch_data_101.h` | `python tools/menu_patch.py --eboot=101 --header=src/dod3_menu_patch_data_101.h` (`src/dod3_menu_patch_data.h` without `--eboot`, for 1.00) |
| `src/dod3_mp3dec_tables.h` | `python tools/gen_mp3_tables.py <minimp3.h> > src/dod3_mp3dec_tables.h` |
| `src/dod3_mp3_standin.h` | `tools/lift_spu.sh` (through `tools/make_spu_overlays.py standin`) |
| `assets/app.ico` | `python tools/make_icon.py assets/app.ico` |
