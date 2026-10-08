"""UE3 package reader for DoD3 (BLUS31197, version 860, big-endian PS3 cook).

upk.decompress(path) -> bytes of the whole uncompressed package.
Handles both layouts on the disc: chunk-compressed packages (summary with a
CompressedChunks table, zlib) and fully compressed ones (a .UNCOMPRESSED_SIZE
sidecar; the file is one compressed chunk stream).
"""
import struct, zlib, os, sys

TAG = 0x9E2A83C1

def _chunk_stream(d, off):
    tag, bs, csz, usz = struct.unpack_from('>IIII', d, off)
    assert tag == TAG, hex(tag)
    n = (usz + bs - 1) // bs
    blocks = [struct.unpack_from('>II', d, off + 16 + 8 * i) for i in range(n)]
    p = off + 16 + 8 * n
    out = []
    for c, u in blocks:
        out.append(zlib.decompress(d[p:p + c]))
        p += c
    r = b''.join(out)
    assert len(r) == usz, (len(r), usz)
    return r

def _fstring(d, p):
    n = struct.unpack_from('>i', d, p)[0]; p += 4
    if n >= 0:
        s = d[p:p + n].rstrip(b'\0').decode('latin-1'); p += n
    else:
        s = d[p:p - 2 * n].decode('utf-16-be').rstrip('\0'); p += -2 * n
    return s, p

def summary(d):
    """Parse the package summary; returns dict incl. 'chunks' and end offset."""
    s = {}
    p = 4
    s['ver'], s['hdr'] = struct.unpack_from('>II', d, p); p += 8
    s['folder'], p = _fstring(d, p)
    (s['flags'], s['name_n'], s['name_off'], s['exp_n'], s['exp_off'],
     s['imp_n'], s['imp_off'], s['depends_off']) = struct.unpack_from('>8I', d, p); p += 32
    # v860: ImportExportGuidsOffset, ImportGuidsCount, ExportGuidsCount, ThumbnailTableOffset
    p += 16
    p += 16                     # Guid
    gen_n = struct.unpack_from('>I', d, p)[0]; p += 4
    p += 12 * gen_n             # generations: exports, names, netobjects
    s['engine'], s['cooker'] = struct.unpack_from('>II', d, p); p += 8
    s['comp'], cn = struct.unpack_from('>II', d, p); p += 8
    s['chunks'] = [struct.unpack_from('>IIII', d, p + 16 * i) for i in range(cn)]
    p += 16 * cn
    s['end'] = p
    return s

def decompress(path):
    d = open(path, 'rb').read()
    if os.path.exists(path + '.UNCOMPRESSED_SIZE'):
        return _chunk_stream(d, 0)
    s = summary(d)
    if not s['chunks']:
        return d
    first_u = min(c[0] for c in s['chunks'])
    out = bytearray(d[:first_u])
    for uoff, usz, coff, csz in sorted(s['chunks']):
        assert len(out) == uoff, (len(out), uoff)
        out += _chunk_stream(d, coff)
    return bytes(out)

def tables(u):
    """names, imports, exports of an uncompressed package."""
    s = summary(u)
    names = []
    p = s['name_off']
    for _ in range(s['name_n']):
        n, p = _fstring(u, p); p += 8   # flags (u64)
        names.append(n)
    def nm(i, num=0):
        return names[i] + ('_%d' % (num - 1) if num else '')
    imps = []
    p = s['imp_off']
    for _ in range(s['imp_n']):
        cp, cpn, cls, clsn, outer, on, onn = struct.unpack_from('>IIIIiII', u, p); p += 28
        imps.append((nm(cp, cpn), nm(cls, clsn), outer, nm(on, onn)))
    exps = []
    p = s['exp_off']
    for _ in range(s['exp_n']):
        tpos = p
        cls, sup, outer, on, onn, arch = struct.unpack_from('>iiiIIi', u, p); p += 24
        oflags = struct.unpack_from('>Q', u, p)[0]; p += 8
        size, off, eflags = struct.unpack_from('>iiI', u, p); p += 12
        nn = struct.unpack_from('>I', u, p)[0]; p += 4 + 4 * nn   # generation net object counts
        p += 16 + 4                                            # package guid, package flags
        exps.append(dict(cls=cls, sup=sup, outer=outer, name=nm(on, onn), size=size, off=off,
                         size_pos=tpos + 32))
    return s, names, imps, exps

def objname(i, imps, exps):
    if i > 0: return exps[i - 1]['name']
    if i < 0: return imps[-i - 1][3]
    return 'None'

if __name__ == '__main__':
    u = decompress(sys.argv[1])
    s, names, imps, exps = tables(u)
    print('ver', s['ver'], 'names', len(names), 'imports', len(imps), 'exports', len(exps), 'size', len(u))
    for i, e in enumerate(exps[:int(sys.argv[2]) if len(sys.argv) > 2 else 40]):
        print(i + 1, objname(e['cls'], imps, exps), e['name'], 'outer', objname(e['outer'], imps, exps), e['size'], hex(e['off']))
