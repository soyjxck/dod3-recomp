"""Write patched UE3 packages for DoD3 into an overlay tree.

replace_export(u, exps, idx, body) appends a new serialized body and points
the export table entry at it; save_full() writes a fully compressed package
(one zlib chunk stream, as the cooker stores the script packages) plus its
.UNCOMPRESSED_SIZE sidecar; toc_set() rewrites the package's PS3TOC.TXT line.
"""
import struct, zlib, os, sys
from . import upk

def block_size_of(path):
    d = open(path, 'rb').read(8)
    return struct.unpack('>II', d)[1]

def replace_export(u, exps, idx, body):
    """idx is 1-based. Returns the new package bytes."""
    e = exps[idx - 1]
    out = bytearray(u)
    off = len(out)
    out += body
    struct.pack_into('>ii', out, e['size_pos'], len(body), off)
    e['size'], e['off'] = len(body), off
    return bytes(out)

def compress_full(u, bs, level=6):
    blocks = [u[i:i + bs] for i in range(0, len(u), bs)]
    comp = [zlib.compress(b, level) for b in blocks]
    hdr = struct.pack('>IIII', upk.TAG, bs, sum(len(c) for c in comp), len(u))
    hdr += b''.join(struct.pack('>II', len(c), len(b)) for c, b in zip(comp, blocks))
    return hdr + b''.join(comp)

def save_full(u, out_path, bs):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    data = compress_full(u, bs)
    open(out_path, 'wb').write(data)
    open(out_path + '.UNCOMPRESSED_SIZE', 'w', newline='').write('%d' % len(u) + chr(13) + chr(10))
    return len(data)

SEP = chr(92)   # the TOC's path separator, a backslash

def toc_set(toc_in, toc_out, name, csize, usize):
    """Rewrite '<csize> <usize> <path> 0' lines whose path ends with name."""
    lines = open(toc_in, 'rb').read().decode('latin-1').split('\r\n')
    hit = 0
    for i, l in enumerate(lines):
        f = l.split(' ')
        if len(f) >= 4 and f[2].lower().endswith(SEP + name.lower()):
            f[0], f[1] = str(csize), str(usize); lines[i] = ' '.join(f); hit += 1
        elif len(f) >= 4 and f[2].lower().endswith((SEP + name + '.uncompressed_size').lower()):
            f[0] = str(len(str(usize)) + 2); lines[i] = ' '.join(f); hit += 1
    os.makedirs(os.path.dirname(toc_out), exist_ok=True)
    open(toc_out, 'wb').write('\r\n'.join(lines).encode('latin-1'))
    return hit

FIOS_DIR = 'game/BLES00000DATA/USRDIR/FIOS-UNREALENGINE3/SQEX03GAME'

def mirror_fios(overlay, rel_in_cooked):
    """The title reads its packages from the game-data install, not the disc:
    put the same file (and sidecar) at the install path in the overlay."""
    import shutil
    src = os.path.join(overlay, 'PS3_GAME/USRDIR/SQEX03GAME/COOKEDPS3', rel_in_cooked)
    dst = os.path.join(overlay, FIOS_DIR, 'COOKEDPS3', rel_in_cooked)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    for ext in ('', '.UNCOMPRESSED_SIZE'):
        if os.path.exists(src + ext):
            shutil.copyfile(src + ext, dst + ext)
