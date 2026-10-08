#!/usr/bin/env python3
"""make_spu_overlays.py -- MultiStream's SPU overlays, lifted where they actually run.

The MultiStream mixer task (spu_0000, "msngSPURS_MP3") streams two kinds of
code into its own local store at run time:

  * DSP plugins -- small position-independent SPU programs linked at 0x80
    (spu_0027/0029/0037 in the EBOOT). MultiStream copies their text, packed
    back to back, into one heap block and GETs the whole block into the
    mixer's plugin area at LS 0x37000.
  * The MP3 decoder -- the firmware's /dev_flash/sys/external/flashMP3.pic,
    an SPU ELF linked at va 0x80. The PPU builds a 128-byte descriptor from
    its section table ({+4 body EA, +8 lowest sh_addr, +12 size, +16 e_entry});
    the mixer parses that, then GETs the body (the file from va 0x80) to LS
    0x1A900 and calls 0x1A900 + (e_entry - 0x80): va 0x80 is LS 0x1A900.
    Without the file the slot stays empty and the task dies on the first MP3
    stream (opening movie, new game).

Lifted code is only right at the address it was lifted for (a PIC module
derives its data addresses from its own return address, and the lifter turns
that into a constant), so each image is rebuilt here exactly as the mixer lays
it out, lifted once at that address, and registered as an overlay recognised
by the first 16 bytes of the body the mixer GETs -- the heap copy's address
differs from run to run.

  wrap     write spu/images/<overlay>.elf for each overlay and print the
           extra function seeds build_spu_workloads.py needs (--extra-funcs)
  register write spu/spu_overlays.c with the image ids build_spu_workloads.py
           assigned (run after it)

The DSP block layout comes from a dump of that GET (SPU_DUMP_OVL=<dir>,0x37000).
A different plugin set or order would arrive with a different signature and
miss, and the runtime would report the branch into unlifted LS.
"""
import os
import re
import struct
import sys
from pathlib import Path

MSDSP_LSA = 0x37000
MSDSP_NAME = "spu_ovl_msdsp_37000"
# (source SPU ELF, offset of its text in the block)
PLUGINS = [
    ("spu_0027_at_018F7C00", 0x0000),
    ("spu_0029_at_018FC300", 0x2000),
    ("spu_0037_at_01904F80", 0x2900),
]

MP3_LSA = 0x1A900
MP3_NAME = "spu_ovl_mp3_1A900"
# The firmware's dev_flash: FW_DEV_FLASH, else fw/dev_flash. Install the PS3
# firmware in RPCS3 (File > Install Firmware) and point FW_DEV_FLASH at its
# dev_flash folder, or copy that folder to fw/dev_flash.
MP3_PIC = Path("sys/external/flashMP3.pic")
MP3_HEADER = 0x80          # the PPU's descriptor; the ELF leaves va 0..0x7F free for it


def elf_segments(elf):
    entry, phoff = struct.unpack(">II", elf[0x18:0x20])
    phn, = struct.unpack(">H", elf[0x2C:0x2E])
    segs = []
    for i in range(phn):
        o = phoff + i * 32
        p_type, off, va, _pa, fsz, msz, flags, _al = struct.unpack(">8I", elf[o:o + 32])
        if p_type == 1:
            segs.append((va, fsz, msz, flags, elf[off:off + fsz]))
    return entry, segs


def text_segment(elf):
    entry, segs = elf_segments(elf)
    for va, _fsz, _msz, flags, code in segs:
        if flags & 1:
            return entry, va, code
    sys.exit("make_spu_overlays: no executable PT_LOAD")


def elf_functions(elf):
    """STT_FUNC symbols (va, name), when the ELF kept its symbol table."""
    shoff, = struct.unpack(">I", elf[0x20:0x24])
    shn, = struct.unpack(">H", elf[0x30:0x32])
    if not shoff or not shn:
        return []
    shs = [struct.unpack(">10I", elf[shoff + i * 40:shoff + (i + 1) * 40]) for i in range(shn)]
    out = []
    for s in shs:
        if s[1] != 2:          # SHT_SYMTAB
            continue
        stro = shs[s[6]][4]
        for i in range(s[5] // 16):
            nm, val, _sz, info, _oth, _shn = struct.unpack(">IIIBBH", elf[s[4] + i * 16:s[4] + (i + 1) * 16])
            if info & 0xF == 2:
                out.append((val, elf[stro + nm:stro + nm + 64].split(b"\0")[0].decode(errors="replace")))
    return out


BI_R0, NOP, LNOP = 0x35000000, 0x4020007F, 0x00200000


def after_returns(code):
    """Offsets of the first real instruction after every `bi $r0`: functions
    laid out back to back, whose callers reach them only through tables."""
    words = struct.unpack(f">{len(code) // 4}I", code[:len(code) // 4 * 4])
    out = []
    for i, w in enumerate(words):
        if w != BI_R0:
            continue
        j = i + 1
        while j < len(words) and words[j] in (NOP, LNOP):
            j += 1
        if j < len(words) and words[j] != 0:
            out.append(j * 4)
    return out


def build_msdsp(root):
    """-> (image at MSDSP_LSA, entry seeds, signature, span)."""
    images = root / "spu/images"
    block, entries = bytearray(), []
    for module, at in PLUGINS:
        entry, va, code = text_segment((images / f"{module}.elf").read_bytes())
        if len(block) > at:
            sys.exit(f"make_spu_overlays: {module} overlaps the previous plugin")
        block += bytes(at - len(block)) + code
        entries.append(MSDSP_LSA + at + (entry - va))
        # Every plugin opens with a 16-byte signature and then the entry the
        # host calls through, at +0x10. Nothing inside the module branches
        # there, so the lifter only finds it as a seed.
        entries.append(MSDSP_LSA + at + 0x10)
        # The rest of a plugin's interface is a table of callbacks it hands the
        # mixer, computed position-independently, so no branch in the image
        # names them. Seed the first real instruction after every return.
        entries += [MSDSP_LSA + at + o for o in after_returns(code)]
    return bytes(block), entries, bytes(block[:16]), len(block)


def build_mp3(root):
    """-> (image at MP3_LSA, entry seeds, signature, span). The image is the
    module's own address space from va 0: descriptor area, text, rodata, data.
    The signature is what leads the body GET: the 16 bytes at va 0x80."""
    dev_flash = Path(os.environ.get("FW_DEV_FLASH") or (root / "fw" / "dev_flash"))
    pic = dev_flash / MP3_PIC
    if not pic.exists():
        sys.exit(f"make_spu_overlays: {pic} missing -- install the PS3 firmware in RPCS3 and set "
                 f"FW_DEV_FLASH to its dev_flash folder (or copy that folder to fw/dev_flash)")
    elf = pic.read_bytes()
    entry, segs = elf_segments(elf)
    end = max(va + fsz for va, fsz, _m, _f, _c in segs)
    image = bytearray(end)
    for va, fsz, _msz, _flags, data in segs:
        image[va:va + fsz] = data
    if any(image[:MP3_HEADER]):
        sys.exit("make_spu_overlays: flashMP3.pic uses va 0..0x7F, where the descriptor goes")
    # The mixer prefetches the 128-byte descriptor to parse it, then DMAs the
    # body -- the file from its lowest alloc section (va 0x80) -- to LS
    # 0x1A900 itself, and calls 0x1A900 + (e_entry - 0x80). So va 0x80 is LS
    # 0x1A900 (verified from a local-store dump: 0x1A900 holds file offset
    # 0x100, the entry slot reads 0x21D20). Lift the body at that address.
    _e, tva, text = text_segment(elf)
    body = bytes(image[MP3_HEADER:])
    entries = [MP3_LSA + entry - MP3_HEADER]
    entries += [MP3_LSA + va - MP3_HEADER for va, _n in elf_functions(elf)]
    entries += [MP3_LSA + tva - MP3_HEADER + o for o in after_returns(text)]
    return body, entries, body[:16], len(body)


OVERLAYS = [
    (MSDSP_NAME, MSDSP_LSA, build_msdsp),
    (MP3_NAME, MP3_LSA, build_mp3),
]


def wrap(root):
    sys.path.insert(0, str(root / "ps3recomp/tools"))
    from wrap_spu_elf import wrap as wrap_elf
    images = root / "spu/images"
    args = []
    for name, lsa, build in OVERLAYS:
        image, entries, _sig, _span = build(root)
        (images / f"{name}.elf").write_bytes(wrap_elf(image, base=lsa, entry=entries[0]))
        args.append(f"--extra-funcs {name}=" + ",".join(f"0x{e:X}" for e in sorted(set(entries[1:]))))
    print(" ".join(args))


def register(root):
    workloads = (root / "spu/spu_workloads.c").read_text()
    body, notes = [], []
    for name, _lsa, build in OVERLAYS:
        m = re.search(r"spu_begin_image\((\d+)\); " + re.escape(name) + r"_spu_recomp_register", workloads)
        if not m:
            sys.exit(f"make_spu_overlays: {name} is not in spu_workloads.c -- run build_spu_workloads.py first")
        _image, _entries, sig, span = build(root)
        sig_c = ", ".join(f"0x{b:02X}" for b in sig)
        body.append(f"""    {{ static const uint8_t sig[16] = {{ {sig_c} }};
      spu_overlay_register_sig_region(sig, 0x{span:X}u, {m.group(1)}); }}  /* {name} */""")
        notes.append(f"{name} = image {m.group(1)}")
    (root / "spu/spu_overlays.c").write_text(f"""\
/* spu_overlays.c - GENERATED by tools/make_spu_overlays.py. */
#include <stdint.h>

extern void spu_overlay_register_sig_region(const uint8_t sig[16], uint32_t span, int image_id);

__attribute__((constructor)) static void dod3_spu_overlays_register(void)
{{
{chr(10).join(body)}
}}
""")
    print("make_spu_overlays: wrote spu/spu_overlays.c (" + "; ".join(notes) + ")")


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in ("wrap", "register"):
        sys.exit(__doc__)
    root = Path(__file__).resolve().parent.parent
    {"wrap": wrap, "register": register}[sys.argv[1]](root)
