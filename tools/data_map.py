#!/usr/bin/env python3
"""data_map.py <map.json> <a.functions.json> <b.functions.json> <a.ELF> <b.ELF> ADDR...

Where a 1.00 data address is in 1.01. Code that forms the address is found
in build a -- `lis rX,hi` then `addi`/`addic`/a D-form load or store with
rX as base, or a load of a TOC (r2) slot holding it, or such a pair forming
an address up to 0x400 below it (a field of a struct at that base) -- and
the same instructions are decoded at the same offset of the matching
function in build b (tools/eboot_diff.py --map). Every site that forms it
is checked; the answer is the address they agree on, with the count.
"""
import sys, json, struct, bisect, collections, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eboot_diff as d

DFORM = set(range(32, 56)) | {58, 62}

def toc_of(fj):
    c = collections.Counter(f['toc'] for f in json.load(open(fj)))
    return int(c.most_common(1)[0][0], 16)

def reader(elf):
    b = open(elf, 'rb').read()
    phoff = struct.unpack_from('>Q', b, 0x20)[0]; n = struct.unpack_from('>H', b, 0x38)[0]
    segs = []
    for i in range(n):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('>IIQQQQQQ', b, phoff + i * 0x38)
        if t == 1 and fsz: segs.append((va, off, fsz))
    def r32(va):
        for v, off, fsz in segs:
            if v <= va and va + 4 <= v + fsz: return struct.unpack_from('>I', b, off + va - v)[0]
        return None
    return r32

def formed(ws, toc, r32):
    """(index, address) for every address the words form."""
    hi = {}
    out = []
    for i, w in enumerate(ws):
        op = w >> 26; rt = (w >> 21) & 31; ra = (w >> 16) & 31
        imm = w & 0xFFFF; simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 58: simm &= ~3
        if op == 15 and ra == 0:
            hi[rt] = imm << 16; continue
        if (op in (12, 14) or op in DFORM) and ra in hi:
            out.append((i, (hi[ra] + simm) & 0xFFFFFFFF))
        if op in (32, 58) and ra == 2:
            slot = (toc + simm) & 0xFFFFFFFF
            v = r32(slot + (4 if op == 58 else 0))
            if v is not None: out.append((i, v))
        if op in (14, 15, 12, 31, 21, 24, 25) and rt in hi and not (op == 15 and ra == 0):
            hi.pop(rt, None)
    return out

def main():
    m = {int(k, 16): int(v, 16) for k, v in json.load(open(sys.argv[1])).items()}
    A = {s: w for s, e, w in d.load(sys.argv[4], sys.argv[2])}
    B = {s: w for s, e, w in d.load(sys.argv[5], sys.argv[3])}
    ta, tb = toc_of(sys.argv[2]), toc_of(sys.argv[3])
    ra, rb = reader(sys.argv[4]), reader(sys.argv[5])
    index = collections.defaultdict(list)   # address -> [(function, word index)]
    for s, ws in A.items():
        if s not in m: continue
        for i, x in formed(ws, ta, ra):
            index[x].append((s, i))
    keys = sorted(index)
    for arg in sys.argv[6:]:
        x = int(arg, 16)
        votes = collections.Counter()
        j = bisect.bisect_right(keys, x) - 1
        while j >= 0 and x - keys[j] <= 0x400:
            base = keys[j]
            for s, i in index[base][:40]:
                wb = B[m[s]]
                got = dict(formed(wb, tb, rb))
                if i in got:
                    votes[got[i] + (x - base)] += 1
            if votes: break
            j -= 1
        if not votes:
            print('%08X: no code forms it' % x); continue
        (y, n), = votes.most_common(1)
        other = sum(votes.values()) - n
        print('%08X -> %08X  (%d sites agree%s)' % (x, y, n, ', %d disagree' % other if other else ''))

if __name__ == '__main__':
    main()
