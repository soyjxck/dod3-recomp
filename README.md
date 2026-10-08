# dod3-recomp

A static recompilation of **Drakengard 3** (Drag-On Dragoon 3, PS3, BLUS31197),
built on [ps3recomp](https://github.com/sp00nznet/ps3recomp), which is included
as a submodule.

## Layout

| Path | What it is |
|------|------------|
| `ps3recomp/` | Toolkit submodule (lifter, runtime, HLE libraries) |
| `elf/EBOOT.ELF` | Decrypted EBOOT (not committed, you supply it) |
| `game/disc/` | Extracted disc contents (not committed, you supply it) |
| `out/` | `ppu_loader.py` output: functions, imports, image manifest |
| `recompiled/` | `ppu_lifter.py` output: the lifted C++ |
| `main.cpp`, `stubs.cpp` | Port entry point and per-game HLE overrides |

## Pipeline

```bash
git submodule update --init
python3 -m venv .venv && .venv/bin/pip install -r ps3recomp/tools/requirements.txt

.venv/bin/python ps3recomp/tools/ppu_loader.py elf/EBOOT.ELF -o out/
.venv/bin/python ps3recomp/tools/ppu_lifter.py elf/EBOOT.ELF \
    --functions out/EBOOT.functions.json \
    --hle-stubs out/EBOOT.imports.json \
    --code-end 0x157e770 \
    --nonvolatile-locals \
    --hook 0x000C1E50 --hook 0x00EE6538 --hook 0x00272F58 \
    -o recompiled/

tools/lift_spu.sh     # SPU tasks, the ShaderPatching job, MultiStream DSP plugins

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
PS3_VFS_ROOT=game/disc ./build/dod3 elf/EBOOT.ELF
# or, for a bounded run with a log:  tools/run_timed.sh 120 out/run.log
```

`--code-end 0x157e770` is the end of the last executable section, so
`.rodata` in the R-X segment is never promoted to functions.

The 1.01 update's EBOOT (decrypted to `elf/EBOOT_101.ELF`) is lifted the same
way into `recompiled_101/`, with `--code-end 0x157fc00` and the hooks at
`0x000C1E50 0x00EE7728 0x00272E58`, and built with `-DDOD3_EBOOT=101`
(`src/dod3_eboot.h`; docs/WINDOWS_HANDOFF.md, section 16).

The three `--hook`s are required: those functions are supplied natively and
the lifted bodies are emitted as `func_<addr>_lifted` --
`src/dod3_gc.cpp` (the garbage collector, `func_000C1E50` and its
reachability pass `func_00EE6538`, which runs natively from
`src/dod3_gc_native.cpp`) and `src/dod3_hot.cpp` (a timing wrapper around
the collision test `func_00272F58`, `DOD3_HOT_LOG=1`). A lift without them
fails to link with duplicate symbols.

The build also lifts libsre's SPURS LFQueue push and SPURS queue paths out of
`ps3recomp/fw_spu/libsre.prx` (`tools/gen_libsre.py`, into
`build/gen/`). That output is firmware-derived and never committed; point
`-DLIBSRE_PRX=` at your own decrypted copy to use a different one.

## Title facts

- 30,134 unique functions from 38,848 OPD descriptors
- 252 firmware imports across 17 libraries (cellSpurs, sysPrxForUser,
  cellSysutil and cellGcmSys make up most of them)
- MultiStream audio (`cellMS*`) is linked statically and runs on the SPUs. Its
  mixer task loads three position-independent DSP plugins into its local store
  at 0x37000; `tools/make_spu_overlays.py` lifts them at that address
- One SPURS job (ShaderPatching) is a raw job binary inside the EBOOT at
  0x01785E00, not an ELF; `tools/lift_spu.sh` wraps and lifts it
- Package decompression runs on an SPU zlib task fed through an ANY2ANY SPURS
  LFQueue; `src/spurs_lfqueue.cpp` drives Sony's own push path for it
