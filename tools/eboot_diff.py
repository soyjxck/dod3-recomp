"""eboot_diff.py <a.ELF> <a.functions.json> <b.ELF> <b.functions.json> [--map out.json]

Which PPU functions differ between two builds of a title (BLUS31197 1.00 and
1.01), once the code that only moved is told apart from code that changed.

Functions come from ppu_loader.py's functions.json. Each body is compared as
its instruction words with the fields that change when code merely moves
masked out: branch targets (b/bl), and the 16-bit immediates of the
address-forming and load/store forms (addi/addic/addis/ori/oris, D- and DS-form
loads and stores), whose constants point into a .rodata that moved with the
code. Bodies are then aligned in address order (difflib over their hashes),
so an inserted or removed function does not shift every match after it.

--map writes the a->b start-address map of every matched function, the
table a port's address-specific hooks need to follow a relift.
"""
import sys, json, struct, hashlib, difflib, collections

def load(elf, fj):
    b = open(elf, 'rb').read()
    phoff = struct.unpack_from('>Q', b, 0x20)[0]; n = struct.unpack_from('>H', b, 0x38)[0]
    segs = []
    for i in range(n):
        t, fl, off, va, pa, fsz, msz, al = struct.unpack_from('>IIQQQQQQ', b, phoff + i * 0x38)
        if t == 1 and fsz: segs.append((va, off, fsz))
    def words(lo, hi):
        for va, off, fsz in segs:
            if va <= lo and hi <= va + fsz:
                return struct.unpack('>%dI' % ((hi - lo) // 4), b[off + lo - va:off + hi - va])
        return ()
    fns = []
    for f in json.load(open(fj)):
        s, e = int(f['start'], 16), int(f['end'], 16)
        fns.append((s, e, words(s, e)))
    fns.sort()
    return fns

IMM16 = {12, 13, 14, 15, 24, 25} | set(range(32, 48)) | {58, 62}
def norm(ws):
    out = []
    for w in ws:
        op = w >> 26
        if op == 18: w &= 0xFC000003          # b/bl: target
        elif op in IMM16: w &= 0xFFFF0000      # address-forming / load-store immediates
        out.append(w)
    return hashlib.sha1(struct.pack('>%dI' % len(out), *out)).digest()

def main():
    a = load(sys.argv[1], sys.argv[2]); b = load(sys.argv[3], sys.argv[4])
    ha = [norm(w) for _, _, w in a]; hb = [norm(w) for _, _, w in b]
    sm = difflib.SequenceMatcher(None, ha, hb, autojunk=False)
    amap = {}
    changed, added, removed = [], [], []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            for k in range(i2 - i1): amap[a[i1 + k][0]] = b[j1 + k][0]
        elif tag == 'replace' and i2 - i1 == j2 - j1:
            for k in range(i2 - i1):
                changed.append((a[i1 + k], b[j1 + k])); amap[a[i1 + k][0]] = b[j1 + k][0]
        else:
            removed += a[i1:i2]; added += b[j1:j2]
    exact = sum(1 for (s, e, w) in a if s in amap and w == dict((x[0], x[2]) for x in b).get(amap[s]))
    print('%s: %d functions; %s: %d' % (sys.argv[1], len(a), sys.argv[3], len(b)))
    print('matched %d (byte-identical %d, same once moves are masked %d), changed %d, only in a %d, only in b %d'
          % (len(amap), exact, len(amap) - len(changed) - exact, len(changed), len(removed), len(added)))
    deltas = collections.Counter(amap[s] - s for s in amap)
    print('address shifts (b - a):', ', '.join('%+#x x%d' % (d, c) for d, c in deltas.most_common(8)))
    for (sa, ea, _), (sb, eb, _) in changed:
        print('  changed  %08X (%5d bytes) -> %08X (%5d bytes)' % (sa, ea - sa, sb, eb - sb))
    for s, e, _ in removed: print('  only a   %08X (%5d bytes)' % (s, e - s))
    for s, e, _ in added: print('  only b   %08X (%5d bytes)' % (s, e - s))
    if '--map' in sys.argv:
        out = sys.argv[sys.argv.index('--map') + 1]
        json.dump({'%08X' % k: '%08X' % v for k, v in sorted(amap.items())}, open(out, 'w'), indent=0)
        print('map ->', out)

if __name__ == '__main__':
    main()
