#!/usr/bin/env python3
"""gen_mp3_tables.py -- the constant tables of src/apu/mp3dec.c.

    tools/gen_mp3_tables.py <minimp3.h> > src/apu/mp3dec_tables.h

An MPEG audio Layer III decoder needs the standard's (ISO/IEC 11172-3 and
13818-3) constant tables: the Huffman code tables, the scale factor band
widths and the synthesis window. Every decoder carries the same ones. This
script takes them from minimp3 (github.com/lieff/minimp3, public domain, CC0
1.0) and writes them in the form apu/mp3dec.c uses:

  * Huffman codes: minimp3 packs each table into a multi-level lookup; this
    walks every entry back out to its (x, y, length, code) and writes a plain
    binary decoding tree per table.
  * Band widths: minimp3's g_scf_long / g_scf_short rows as they are.
  * Synthesis window: minimp3 stores it pre-arranged for its own filterbank,
    so it was recovered once by measuring that filterbank's impulse response
    in every subband and solving for the window under the standard's
    matrixing (V[i] = sum_k cos((16+i)(2k+1)pi/64) S[k]). Every value came out
    within 0.04 of a multiple of 1/65536, as the standard's table is, and is
    kept below in those units. Entries 16+64n multiply V[16], which is always
    0, and are left 0.

Everything else (the IMDCT, matrixing and window cosines, the antialias and
stereo coefficients) is computed here from the standard's formulas.
"""
import math
import re
import sys

D_WINDOW_X65536 = [
    0,-1,-1,-1,-1,-1,-1,-2,-2,-2,-2,-3,-3,-4,-4,-5,0,-6,-7,-7,-8,-9,-10,-11,-13,-14,-16,-17,-19,-21,-24,-26,
    -29,-31,-35,-38,-41,-45,-49,-53,-58,-63,-68,-73,-79,-85,-91,-97,-104,-111,-117,-125,-132,-139,-147,-154,-161,-169,-176,-183,-190,-196,-202,-208,
    213,218,222,225,227,228,228,227,224,221,215,208,200,189,177,163,0,127,106,83,57,29,-2,-36,-72,-111,-153,-197,-244,-294,-347,-401,
    -459,-519,-581,-645,-711,-779,-848,-919,-991,-1064,-1137,-1210,-1283,-1356,-1428,-1498,-1567,-1634,-1698,-1759,-1817,-1870,-1919,-1962,-2001,-2032,-2057,-2075,-2085,-2087,-2080,-2063,
    2037,2000,1952,1893,1822,1739,1644,1535,1414,1280,1131,970,794,605,402,185,0,-288,-545,-814,-1095,-1388,-1692,-2006,-2330,-2663,-3004,-3351,-3705,-4063,-4425,-4788,
    -5153,-5517,-5879,-6237,-6589,-6935,-7271,-7597,-7910,-8209,-8491,-8755,-8998,-9219,-9416,-9585,-9727,-9838,-9916,-9959,-9966,-9935,-9863,-9750,-9592,-9389,-9139,-8840,-8492,-8092,-7640,-7134,
    6574,5959,5288,4561,3776,2935,2037,1082,70,-998,-2122,-3300,-4533,-5818,-7154,-8540,0,-11455,-12980,-14548,-16155,-17799,-19478,-21189,-22929,-24694,-26482,-28289,-30112,-31947,-33791,-35640,
    -37489,-39336,-41176,-43006,-44821,-46617,-48390,-50137,-51853,-53534,-55178,-56778,-58333,-59838,-61289,-62684,-64019,-65290,-66494,-67629,-68692,-69679,-70590,-71420,-72169,-72835,-73415,-73908,-74313,-74630,-74856,-74992,
    75038,74992,74856,74630,74313,73908,73415,72835,72169,71420,70590,69679,68692,67629,66494,65290,0,62684,61289,59838,58333,56778,55178,53534,51853,50137,48390,46617,44821,43006,41176,39336,
    37489,35640,33791,31947,30112,28289,26482,24694,22929,21189,19478,17799,16155,14548,12980,11455,9975,8540,7154,5818,4533,3300,2122,998,-70,-1082,-2037,-2935,-3776,-4561,-5288,-5959,
    6574,7134,7640,8092,8492,8840,9139,9389,9592,9750,9863,9935,9966,9959,9916,9838,0,9585,9416,9219,8998,8755,8491,8209,7910,7597,7271,6935,6589,6237,5879,5517,
    5153,4788,4425,4063,3705,3351,3004,2663,2330,2006,1692,1388,1095,814,545,288,45,-185,-402,-605,-794,-970,-1131,-1280,-1414,-1535,-1644,-1739,-1822,-1893,-1952,-2000,
    2037,2063,2080,2087,2085,2075,2057,2032,2001,1962,1919,1870,1817,1759,1698,1634,0,1498,1428,1356,1283,1210,1137,1064,991,919,848,779,711,645,581,519,
    459,401,347,294,244,197,153,111,72,36,2,-29,-57,-83,-106,-127,-146,-163,-177,-189,-200,-208,-215,-221,-224,-227,-228,-228,-227,-225,-222,-218,
    213,208,202,196,190,183,176,169,161,154,147,139,132,125,117,111,0,97,91,85,79,73,68,63,58,53,49,45,41,38,35,31,
    29,26,24,21,19,17,16,14,13,11,10,9,8,7,7,6,5,5,4,4,3,3,2,2,2,2,1,1,1,1,1,1,
]


def arrays(src):
    def arr(name):
        m = re.search(r"static const (?:int16_t|uint8_t) " + re.escape(name) + r"(\[[^\]]*\])+ *= *\{(.*?)\};", src, re.S)
        if not m:
            sys.exit(f"gen_mp3_tables: {name} not found")
        return [int(x) for x in re.findall(r"-?\d+", m.group(2))]
    return {n: arr(n) for n in ("tabs", "tab32", "tab33", "tabindex", "g_scf_long", "g_scf_short")}


def huffman_codes(a, t):
    """{(code, length): (x, y)} of big-value table t."""
    tabs, base0 = a["tabs"], a["tabindex"][t]
    out = {}
    def walk(base, w, prefix, plen):
        for idx in range(1 << w):
            leaf = tabs[base0 + base + idx]
            if leaf < 0:
                if idx & ((1 << w) - 1) != idx:
                    continue
                walk(-(leaf >> 3), leaf & 7, (prefix << w) | idx, plen + w)
            else:
                n = leaf >> 8
                code = (prefix << n) | (idx >> (w - n))
                out[(code, plen + n)] = (leaf & 15, (leaf >> 4) & 15)
    walk(0, 5, 0, 0)
    return out


def count1_codes(tab):
    out = {}
    for pat in range(64):
        leaf = tab[pat >> 2]
        if not leaf & 8:
            leaf = tab[(leaf >> 3) + ((pat & 3) >> (2 - (leaf & 3)))]
        n = leaf & 7
        out[(pat >> (6 - n), n)] = ((leaf >> 4) & 15, 0)
    return out


def tree(codes):
    """Binary decoding tree: node i's children at [2i], [2i+1]; a child is
    a node index, or 0x8000 | x << 4 | y for a leaf."""
    # Kraft: a complete prefix code
    if abs(sum(2.0 ** -n for (_c, n) in codes) - 1.0) > 1e-12:
        sys.exit("gen_mp3_tables: incomplete code")
    nodes = [[None, None]]
    for (code, n), (x, y) in sorted(codes.items(), key=lambda kv: kv[0][1]):
        cur = 0
        for b in range(n - 1, 0, -1):
            bit = (code >> b) & 1
            if nodes[cur][bit] is None:
                nodes.append([None, None])
                nodes[cur][bit] = len(nodes) - 1
            cur = nodes[cur][bit]
        bit = code & 1
        if nodes[cur][bit] is not None:
            sys.exit("gen_mp3_tables: not a prefix code")
        nodes[cur][bit] = 0x8000 | x << 4 | y
    return [c for nd in nodes for c in nd]


def flit(v):
    t = f"{v:.9g}"
    if not any(ch in t for ch in ".en"):
        t += ".0"
    return t + "f"


def floats(name, vals, per=8):
    s = f"static const float {name}[{len(vals)}] = {{\n"
    for i in range(0, len(vals), per):
        s += "    " + ", ".join(flit(v) for v in vals[i:i + per]) + ",\n"
    return s + "};\n"


def ints(ctype, name, vals, per=16, dims=None):
    """dims "[r][c]": one brace-enclosed row per line (per = c)."""
    s = f"static const {ctype} {name}{dims or f'[{len(vals)}]'} = {{\n"
    rows = dims and dims.count("[") == 2
    for i in range(0, len(vals), per):
        body = ",".join(str(v) for v in vals[i:i + per])
        s += f"    {{ {body} }},\n" if rows else f"    {body},\n"
    return s + "};\n"


def main():
    a = arrays(open(sys.argv[1]).read())
    out = ["/* Generated by tools/gen_mp3_tables.py -- do not edit. The constant tables\n"
           " * of ISO/IEC 11172-3 / 13818-3 Layer III for src/apu/mp3dec.c. */\n"
           "#include <stdint.h>\n\n"]
    # Huffman trees: tables 1-24 (16-23 share 16's codes, 24-31 share 24's)
    # and the two count1 tables (32: A, 33: B).
    trees, offs = [], {}
    for t in [1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 24, 32, 33]:
        codes = huffman_codes(a, t) if t < 32 else count1_codes(a["tab32"] if t == 32 else a["tab33"])
        want = {1: 4, 2: 9, 3: 9, 5: 16, 6: 16, 7: 36, 8: 36, 9: 36, 10: 64, 11: 64, 12: 64,
                13: 256, 15: 256, 16: 256, 24: 256, 32: 16, 33: 16}[t]
        if len(codes) != want:
            sys.exit(f"gen_mp3_tables: table {t} has {len(codes)} codes, not {want}")
        offs[t] = len(trees)
        trees += tree(codes)
    out.append(ints("uint16_t", "k_huff_tree", trees))
    start = [offs.get(16 if 16 <= t < 24 else 24 if t >= 24 else t, 0xFFFF) for t in range(32)]
    out.append(ints("uint16_t", "k_huff_start", start + [offs[32], offs[33]], dims="[34]"))
    out.append(ints("uint8_t", "k_huff_linbits", [0] * 16 + [1, 2, 3, 4, 6, 8, 10, 13, 4, 5, 6, 7, 8, 9, 11, 13]))
    # Scale factor band widths: 8 rows (11.025/12, 8, 22.05, 24, 16, 44.1, 48,
    # 32 kHz), long 22 bands; short 13 bands, each width once.
    lo, sh = a["g_scf_long"], a["g_scf_short"]
    out.append(ints("uint8_t", "k_sfb_long", [v for r in range(8) for v in lo[r * 23:r * 23 + 22]], per=22, dims="[8][22]"))
    out.append(ints("uint8_t", "k_sfb_short", [sh[r * 40 + 3 * i] for r in range(8) for i in range(13)], per=13, dims="[8][13]"))
    # The window as used: D/65536 scaled by 32768 for 16-bit output, i.e. /2
    # (exact in float).
    out.append(floats("k_synth_window", [v / 2 for v in D_WINDOW_X65536], 8))
    # |v|^(4/3) for the common small values; 2^(q/4) for q = 0..3.
    out.append(floats("k_pow43", [v ** (4.0 / 3.0) for v in range(256)], 8))
    out.append(floats("k_pow2_quarter", [2.0 ** (q / 4.0) for q in range(4)], 4))
    # IMDCT: 36 outputs from 18 inputs, 12 from 6; the four block windows.
    out.append(floats("k_imdct36", [math.cos(math.pi / 72 * (2 * i + 1 + 18) * (2 * k + 1)) for i in range(36) for k in range(18)], 18))
    out.append(floats("k_imdct12", [math.cos(math.pi / 24 * (2 * i + 1 + 6) * (2 * k + 1)) for i in range(12) for k in range(6)], 6))
    w = [[0.0] * 36 for _ in range(4)]
    for i in range(36):
        w[0][i] = math.sin(math.pi / 36 * (i + 0.5))
    for i in range(18):
        w[1][i] = w[0][i]
    for i in range(18, 24):
        w[1][i] = 1.0
    for i in range(24, 30):
        w[1][i] = math.sin(math.pi / 12 * (i - 18 + 0.5))
    for i in range(6, 12):
        w[3][i] = math.sin(math.pi / 12 * (i - 6 + 0.5))
    for i in range(12, 18):
        w[3][i] = 1.0
    for i in range(18, 36):
        w[3][i] = w[0][i]
    for i in range(12):
        w[2][i] = math.sin(math.pi / 12 * (i + 0.5))
    out.append(floats("k_imdct_window", [v for r in w for v in r], 12))
    # Matrixing: X[m] = sum_k S[k] cos(m(2k+1)pi/64), m = 0..31.
    out.append(floats("k_matrix", [math.cos(m * (2 * k + 1) * math.pi / 64) for m in range(32) for k in range(32)], 8))
    # Antialias butterflies.
    c = [-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037]
    out.append(floats("k_aa_cs", [1 / math.sqrt(1 + x * x) for x in c]))
    out.append(floats("k_aa_ca", [x / math.sqrt(1 + x * x) for x in c]))
    # MPEG-1 intensity stereo: is_pos 0..6 -> left, right factors.
    l, r = [], []
    for p in range(7):
        if p == 6:
            l.append(1.0); r.append(0.0)
        else:
            t = math.tan(p * math.pi / 12)
            l.append(t / (1 + t)); r.append(1 / (1 + t))
    out.append(floats("k_is_left", l))
    out.append(floats("k_is_right", r))
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
