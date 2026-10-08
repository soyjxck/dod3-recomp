#!/bin/sh
# Extract and lift every SPU program the port runs, into spu/ (git-ignored):
#   - the SPU ELFs embedded in the EBOOT (SPURS tasks, MultiStream, ...)
#   - raw SPURS job binaries embedded in the EBOOT (ShaderPatching and others)
#   - the MultiStream DSP plugin block, rebuilt at the LS address it runs at
# then generate the workload and overlay registries the build compiles.
set -e
cd "$(dirname "$0")/.."
PY=.venv/bin/python
[ -x "$PY" ] || PY=.venv/Scripts/python.exe   # a Windows venv (run this from Git Bash)
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
# Function-pointer targets the lifter's table scans miss (words in a task's
# data segment that point at code): PhysX's spu_0018 called 0x170E0 through
# one, hit unlifted local store, died, and the chapter load hung. Found by
# scanning each image's data for 4-aligned text addresses that start a
# plausible instruction and are not already a lifted function.
EXTRA="$EXTRA --extra-funcs spu_0018_at_0186AD80=0x3288,0x3298,0xF9E8,0x10000,0x10008,0x170E0,0x17420"
EXTRA="$EXTRA --extra-funcs spu_0015_at_01840980=0x10000"
EXTRA="$EXTRA --extra-funcs spu_0000_at_01781700=0x505C"
# Native fast paths (src/dod3_spu_hooks.c): ShaderPatching's LZF copy loops.
EXTRA="$EXTRA --native-hook spurs_job_01785E00=0x528:dod3_spu_lzf_literal_hook"
EXTRA="$EXTRA --native-hook spurs_job_01785E00=0x638:dod3_spu_lzf_match_hook"
EXTRA="$EXTRA --native-hook spurs_job_01785E00=0x560:dod3_spu_lzf_token_hook"
EXTRA="$EXTRA --native-hook spurs_job_01785E00=0x6A0:dod3_spu_patch_loop_hook"
# MultiStream's DSP plugin block (src/dod3_msdsp_hooks.c): the two biquad
# loops that were half of the mixer task's time.
EXTRA="$EXTRA --native-hook spu_ovl_msdsp_37000=0x39350:dod3_msdsp_biquad_a_hook"
EXTRA="$EXTRA --native-hook spu_ovl_msdsp_37000=0x39630:dod3_msdsp_biquad_b_hook"
$PY $T/build_spu_workloads.py --images spu/images --lifted spu \
    --out spu/spu_workloads.c --register-fn dod3_spu_register_all \
    --constructor --title dod3 $EXTRA
$PY tools/make_spu_overlays.py register
