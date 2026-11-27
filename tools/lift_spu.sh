#!/bin/sh
# Extract and lift every SPU program the port runs, into spu/ (git-ignored):
#   - the SPU ELFs embedded in the EBOOT (SPURS tasks, MultiStream, ...)
#   - raw SPURS job binaries embedded in the EBOOT (ShaderPatching and others)
#   - the MultiStream DSP plugin block, rebuilt at the LS address it runs at
# then generate the workload and overlay registries the build compiles.
#
# From the 1.01 update's EBOOT, elf/EBOOT_101.ELF (or DOD3_ELF=<path>): the
# images are named by their address in it, and the hooks below name them.
set -e
cd "$(dirname "$0")/.."
PY=.venv/bin/python
[ -x "$PY" ] || PY=.venv/Scripts/python.exe   # a Windows venv (run this from Git Bash)
T=ps3recomp/tools
ELF=${DOD3_ELF:-elf/EBOOT_101.ELF}
[ -f "$ELF" ] || { echo "lift_spu: no $ELF (docs/BUILDING.md)"; exit 1; }
[ "$(wc -c < "$ELF" | tr -d ' ')" = 26872424 ] || { echo "lift_spu: $ELF is not the 1.01 update's EBOOT"; exit 1; }

# Lifts already in spu/ are kept (build_spu_workloads.py skips them), so an
# unchanged image is not recompiled; one made before a function entry below
# was listed is made again (seed). The overlays (the DSP plugin block and our
# stand-in MP3 decoder: see tools/make_spu_overlays.py) are rebuilt every
# time. The console's own MP3 decoder is lifted beside ours only when asked
# for, DOD3_FW_MP3=1 (from fw/dev_flash, or FW_DEV_FLASH=<a dev_flash
# folder>): it is firmware code, for DOD3_MP3_NATIVE=0 and the MP3 check
# modes on a developer's machine, and the packaging scripts refuse a build
# that holds it.
mkdir -p spu/images
rm -rf spu/spu_ovl_msdsp_37000 spu/spu_ovl_mp3_1A900 spu/spu_ovl_mp3native_1A900
$PY $T/extract_spu_images.py "$ELF" --output spu/images

# Raw SPURS job binaries in the EBOOT (not ELFs, so extract_spu_images.py
# misses them): "<file offset> <bytes>". The vaddr is offset + 0x10000.
# Named to sort after spu_*, so the task images keep their ids -- the runtime
# still special-cases some image ids for other titles.
#   0x01777480  ShaderPatching job chain
#   0x0177FC00  job dispatched as the title loads (fp D6E964B601AB14C0)
for job in "0x01777480 34688" "0x0177FC00 528"; do
    set -- $job
    va=$(printf '%08X' $(( $1 + 0x10000 )))
    dd if="$ELF" of=spu/job.bin bs=1 skip=$(( $1 )) count=$2 2>/dev/null
    $PY $T/wrap_spu_elf.py spu/job.bin --base 0x0 --entry 0x0 \
        --out spu/images/spurs_job_$va.elf
    rm spu/job.bin
done

EXTRA=$($PY tools/make_spu_overlays.py wrap)
# seed <image> <address,address...>: function entries the lifter is told of.
# A lift already in spu/ that has no function at one of them was made before
# it was listed, and is removed so that it is made again.
seed() {
    EXTRA="$EXTRA --extra-funcs $1=$2"
    [ -f "spu/$1/spu_recomp.c" ] || return 0
    for a in $(echo "$2" | tr ',' ' '); do
        if ! grep -qF "$(printf 'spu_func_%08X(' $(( a )))" "spu/$1/spu_recomp.c"; then
            echo "lift_spu: $1 was lifted without an entry at $a -- lifting it again"
            rm -rf "spu/${1:?}"
            return 0
        fi
    done
}
# Function-pointer targets the lifter's table scans miss (words in a task's
# data segment that point at code): PhysX's spu_0018 called 0x170E0 through
# one, hit unlifted local store, died, and the chapter load hung. Found by
# scanning each image's data for 4-aligned text addresses that start a
# plausible instruction and are not already a lifted function.
seed spu_0018_at_0186C400 0x3288,0x3298,0xF9E8,0x10000,0x10008,0x170E0,0x17420
seed spu_0015_at_01842000 0x10000
seed spu_0000_at_01782D80 0x505C
# PhysX's contact task (spu_0020) keeps one of its tables of functions among
# the constants in its code, and 0xDF90 is named nowhere else. With no
# function there the runtime interpreted it, instruction by instruction, up
# to the next lifted address: 15 times a second in a battle. The other three
# are named by words in its data and have not been seen called.
seed spu_0020_at_018A7B80 0x31B8,0x31C8,0xDF90,0xED58
# Native fast paths (src/patches/spu_hooks.c): ShaderPatching's LZF copy loops.
EXTRA="$EXTRA --native-hook spurs_job_01787480=0x528:dod3_spu_lzf_literal_hook"
EXTRA="$EXTRA --native-hook spurs_job_01787480=0x638:dod3_spu_lzf_match_hook"
EXTRA="$EXTRA --native-hook spurs_job_01787480=0x560:dod3_spu_lzf_token_hook"
EXTRA="$EXTRA --native-hook spurs_job_01787480=0x6A0:dod3_spu_patch_loop_hook"
# MultiStream's DSP plugin block (src/apu/msdsp_hooks.c): the two biquad
# loops that are half of the mixer task's time.
EXTRA="$EXTRA --native-hook spu_ovl_msdsp_37000=0x39350:dod3_msdsp_biquad_a_hook"
EXTRA="$EXTRA --native-hook spu_ovl_msdsp_37000=0x39630:dod3_msdsp_biquad_b_hook"
# The MP3 decoder, natively (src/apu/mp3_native.c): the stand-in
# flashMP3.pic's entry, and the console decoder's decodeFrame when it is
# lifted (DOD3_FW_MP3=1).
EXTRA="$EXTRA --native-hook spu_ovl_mp3native_1A900=0x1A910:dod3_mp3_standin_hook"
EXTRA="$EXTRA --native-hook spu_ovl_mp3_1A900=0x21D20:dod3_mp3_decode_hook"
$PY $T/build_spu_workloads.py --images spu/images --lifted spu \
    --out spu/spu_workloads.c --register-fn dod3_spu_register_all \
    --constructor --title dod3 $EXTRA
$PY tools/make_spu_overlays.py register
