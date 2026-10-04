#!/bin/sh
# Extract and lift every SPU program the port runs, into spu/ (git-ignored):
#   - the SPU ELFs embedded in the EBOOT (SPURS tasks, MultiStream, ...)
#   - raw SPURS job binaries embedded in the EBOOT (ShaderPatching and others)
#   - the MultiStream DSP plugin block, rebuilt at the LS address it runs at
# then generate the workload and overlay registries the build compiles.
set -e
cd "$(dirname "$0")/.."
PY=.venv/bin/python
T=ps3recomp/tools

# Lifts already in spu/ are kept (build_spu_workloads.py skips them), so an
# unchanged image is not recompiled. The overlays (the DSP plugin block and
# the firmware's MP3 decoder, see tools/make_spu_overlays.py) are rebuilt
# every time; the MP3 one needs fw/dev_flash from tools/extract_dev_flash.py.
mkdir -p spu/images
rm -rf spu/spu_ovl_msdsp_37000 spu/spu_ovl_mp3_1A900
$PY $T/extract_spu_images.py elf/EBOOT.ELF --output spu/images

# Raw SPURS job binaries in the EBOOT (not ELFs, so extract_spu_images.py
# misses them): "<file offset> <bytes>". The vaddr is offset + 0x10000.
# Named to sort after spu_*, so the task images keep their ids -- the runtime
# still special-cases some image ids for other titles.
#   0x01775E00  ShaderPatching job chain
#   0x0177E580  job dispatched as the title loads (fp D6E964B601AB14C0)
for job in "0x01775E00 34688" "0x0177E580 528"; do
    set -- $job
    va=$(printf '%08X' $(( $1 + 0x10000 )))
    dd if=elf/EBOOT.ELF of=spu/job.bin bs=1 skip=$(( $1 )) count=$2 2>/dev/null
    $PY $T/wrap_spu_elf.py spu/job.bin --base 0x0 --entry 0x0 \
        --out spu/images/spurs_job_$va.elf
    rm spu/job.bin
done

EXTRA=$($PY tools/make_spu_overlays.py wrap)
$PY $T/build_spu_workloads.py --images spu/images --lifted spu \
    --out spu/spu_workloads.c --register-fn dod3_spu_register_all \
    --constructor --title dod3 $EXTRA
$PY tools/make_spu_overlays.py register
