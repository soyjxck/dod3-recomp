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
    -o recompiled/

.venv/bin/python ps3recomp/tools/extract_spu_images.py elf/EBOOT.ELF --output spu/images
.venv/bin/python ps3recomp/tools/build_spu_workloads.py --images spu/images --lifted spu \
    --out spu/spu_workloads.c --register-fn dod3_spu_register_all --constructor --title dod3

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
PS3_VFS_ROOT=game/disc ./build/dod3 elf/EBOOT.ELF
```

`--code-end 0x157e770` is the end of the last executable section, so
`.rodata` in the R-X segment is never promoted to functions.

## Title facts

- 30,134 unique functions from 38,848 OPD descriptors
- 252 firmware imports across 17 libraries (cellSpurs, sysPrxForUser,
  cellSysutil and cellGcmSys make up most of them)
- MultiStream audio (`cellMS*`) is linked statically and runs on the SPUs
