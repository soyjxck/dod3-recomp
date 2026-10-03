#!/bin/sh
# Extract and lift every SPU program the port runs, into spu/ (git-ignored):
#   - the SPU ELFs embedded in the EBOOT (SPURS tasks, MultiStream, ...)
#   - the ShaderPatching SPURS job, a raw job binary at vaddr 0x01785E00
#   - the MultiStream DSP plugin block, rebuilt at the LS address it runs at
# then generate the workload and overlay registries the build compiles.
set -e
cd "$(dirname "$0")/.."
PY=.venv/bin/python
T=ps3recomp/tools

# Lifts already in spu/ are kept (build_spu_workloads.py skips them), so an
# unchanged image is not recompiled. The plugin block is rebuilt every time.
mkdir -p spu/images
rm -rf spu/spu_ovl_msdsp_37000
$PY $T/extract_spu_images.py elf/EBOOT.ELF --output spu/images

# ShaderPatching job: 34688 bytes at vaddr 0x01785E00 (file offset 0x01775E00).
# Named to sort after spu_*, so the task images keep their ids -- the runtime
# still special-cases some image ids for other titles.
dd if=elf/EBOOT.ELF of=spu/job_01785E00.bin bs=128 skip=$((0x01775E00 / 128)) \
   count=$((34688 / 128)) 2>/dev/null
$PY $T/wrap_spu_elf.py spu/job_01785E00.bin --base 0x0 --entry 0x0 \
    --out spu/images/spurs_job_01785E00.elf
rm spu/job_01785E00.bin

EXTRA=$($PY tools/make_spu_overlays.py wrap)
$PY $T/build_spu_workloads.py --images spu/images --lifted spu \
    --out spu/spu_workloads.c --register-fn dod3_spu_register_all \
    --constructor --title dod3 $EXTRA
$PY tools/make_spu_overlays.py register
