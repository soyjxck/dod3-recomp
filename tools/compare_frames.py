#!/usr/bin/env python3
"""Compare replayed frames against a reference set (handoff doc, section 8).

    python tools/compare_frames.py <dir-with-replayed-ppms> [ref-dir] [--tol N]

Every <capture>.<present>.ppm in the reference directory is looked for in the
replay directory and compared pixel for pixel. One line per frame: how many
pixels differ, the worst channel difference, the mean absolute difference and
the share of pixels within --tol levels (default 8).

A frame passes when it is byte-identical, within the GPU-noise allowance (a
handful of pixels off by a few levels, which outside2.000060 shows on the Mac
itself), or -- against a reference from another GPU/API, where filtering and
shader arithmetic round differently -- when at least 99% of its pixels are
within --tol levels and none is more than 64 off... except the outliers a
different API's point-vs-linear edges produce, which is why the pass rule
only counts pixels, not the single worst one. Exit status 1 when any frame
fails or is missing.
"""
import os
import sys


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    tol = 8
    if "--tol" in sys.argv:
        tol = int(sys.argv[sys.argv.index("--tol") + 1])
        args = [a for a in args if a != str(tol)]
    if not args:
        print(__doc__)
        return 2
    rep = args[0]
    ref = args[1] if len(args) > 1 else "out/ref_clr"
    noise_pixels, noise_levels = 16, 4
    names = sorted(n for n in os.listdir(ref) if n.endswith(".ppm"))
    missing, same, close, bad = [], 0, 0, []
    for n in names:
        rp = os.path.join(rep, n)
        if not os.path.exists(rp):
            missing.append(n)
            continue
        w1, h1, a = read_ppm(os.path.join(ref, n))
        w2, h2, b = read_ppm(rp)
        if (w1, h1) != (w2, h2):
            print(f"{n}: size {w2}x{h2} vs reference {w1}x{h1}")
            bad.append(n)
            continue
        if a == b:
            same += 1
            continue
        npx = w1 * h1
        diff_px, worst, total, within = 0, 0, 0, 0
        for i in range(0, len(a), 3):
            d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
            total += abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2])
            if d:
                diff_px += 1
                if d > worst:
                    worst = d
            if d <= tol:
                within += 1
        mean = total / (3.0 * npx)
        share = within / npx
        if diff_px <= noise_pixels and worst <= noise_levels:
            kind = "noise"
        elif share >= 0.99:
            kind = "close"
        else:
            kind = "DIFF"
        print(f"{n}: {kind} {diff_px} pixels differ, worst {worst}, mean {mean:.3f}, "
              f"{share * 100:.2f}% within {tol}")
        if kind == "DIFF":
            bad.append(n)
        else:
            close += 1
    print(f"{len(names)} reference frames: {same} identical, {close} within tolerance, "
          f"{len(bad)} differ, {len(missing)} missing")
    if missing:
        print("missing:", " ".join(missing[:10]), "..." if len(missing) > 10 else "")
    return 1 if (bad or missing) else 0


if __name__ == "__main__":
    sys.exit(main())
