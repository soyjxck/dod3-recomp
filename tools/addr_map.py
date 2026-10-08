#!/usr/bin/env python3
"""addr_map.py <map.json> <a.functions.json> <b.functions.json> <a.ELF> <b.ELF> ADDR...

Where a 1.00 code address is in 1.01: the loader range that contains it, the
matching range in the other build (tools/eboot_diff.py --map), and the same
offset into it. The instruction words around the address are compared
(branch targets and address immediates masked, as in eboot_diff.py), so an
address inside a changed function is reported, not silently mapped.
"""
import bisect
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eboot_diff as d  # noqa: E402

def main():
    m = {int(k, 16): int(v, 16) for k, v in json.load(open(sys.argv[1])).items()}
    fa = sorted((int(f['start'], 16), int(f['end'], 16)) for f in json.load(open(sys.argv[2])))
    A = {s: w for s, e, w in d.load(sys.argv[4], sys.argv[2])}
    B = {s: w for s, e, w in d.load(sys.argv[5], sys.argv[3])}
    starts = [s for s, e in fa]
    for x in sys.argv[6:]:
        a = int(x, 16)
        i = bisect.bisect_right(starts, a) - 1
        if i < 0 or not (fa[i][0] <= a < fa[i][1]):
            print('%08X: not in any function range' % a); continue
        s = fa[i][0]
        if s not in m:
            print('%08X: in %08X, which has no match in b' % (a, s)); continue
        off = a - s
        k = off // 4
        wa, wb = A[s], B[m[s]]
        lo, hi = max(0, k - 8), k + 8
        same = len(wa) == len(wb) and d.norm(wa[lo:hi]) == d.norm(wb[lo:hi])
        print('%08X -> %08X  (in %08X -> %08X +0x%X)%s' % (a, m[s] + off, s, m[s], off,
              '' if same else '  !! the code around it differs'))

if __name__ == '__main__':
    main()
