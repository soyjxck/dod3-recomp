#!/usr/bin/env python3
"""Compare replayed frames against a reference set (tools/replay_regress.sh).

    python tools/compare_frames.py <dir-with-replayed-ppms> [ref-dir] [--tol N]

Every <capture>.<present>.ppm in the reference directory is looked for in the
replay directory and compared pixel for pixel. One line per frame: how many
pixels differ, the worst channel difference, the mean absolute difference and
the share of pixels within --tol levels (default 8).

A frame passes when it is byte-identical, within the GPU-noise allowance (a
handful of pixels off by a few levels), or -- against a reference from
another GPU or API, where filtering and shader arithmetic round differently
-- when at least 99% of its pixels are within --tol levels. The single worst
pixel is not counted: a different API's point-vs-linear edges produce
outliers. Exit status 1 when any frame fails or is missing.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm import read_ppm  # noqa: E402

NOISE_PIXELS, NOISE_LEVELS = 16, 4


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("replay", help="the directory holding the replayed frames")
    ap.add_argument("ref", nargs="?", default="out/ref_clr", help="the reference frames (default out/ref_clr)")
    ap.add_argument("--tol", type=int, default=8, help="levels a pixel may differ by and count as within (default 8)")
    args = ap.parse_args()
    names = sorted(n for n in os.listdir(args.ref) if n.endswith(".ppm"))
    missing, same, close, bad = [], 0, 0, []
    for n in names:
        rp = os.path.join(args.replay, n)
        if not os.path.exists(rp):
            missing.append(n)
            continue
        w1, h1, a = read_ppm(os.path.join(args.ref, n))
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
            if d <= args.tol:
                within += 1
        mean = total / (3.0 * npx)
        share = within / npx
        if diff_px <= NOISE_PIXELS and worst <= NOISE_LEVELS:
            kind = "noise"
        elif share >= 0.99:
            kind = "close"
        else:
            kind = "DIFF"
        print(f"{n}: {kind} {diff_px} pixels differ, worst {worst}, mean {mean:.3f}, "
              f"{share * 100:.2f}% within {args.tol}")
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
