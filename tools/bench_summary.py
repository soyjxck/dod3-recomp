#!/usr/bin/env python3
"""Summarise the [frametime] windows of one or more run logs.

    python tools/bench_summary.py out/bench/base3.log [more logs...] [--from 100] [--to 300]

Per log: the 5 s windows from t >= --from (default 100 s) to t <= --to, their
fps median and quartiles (robust to the cutscene and menu windows autoplay
sometimes wanders into), the mean, the share of windows at 60 fps or better,
the worst frame, and the share of frames over 34 ms. Also the audio-gap
totals from the [audio-rate] lines when present.
"""
import argparse
import re
import sys

FRAMETIME = re.compile(r"\[frametime\] t=(\d+)s ([\d.]+) fps, mean ([\d.]+) ms, worst ([\d.]+) ms, (\d+) of (\d+) frames")
AUDIO = re.compile(r"\[audio-rate\] gaps (\d+) \((\d+) blocks, ([\d.]+)% of the window\)")


def summarise(path, start, end):
    fps, worst, over, frames, gaps, gap_pct = [], 0.0, 0, 0, 0, []
    with open(path, errors="replace") as f:
        for line in f:
            m = FRAMETIME.search(line)
            if m:
                t = float(m.group(1))
                if t < start or (end is not None and t > end):
                    continue
                fps.append(float(m.group(2)))
                worst = max(worst, float(m.group(4)))
                over += int(m.group(5))
                frames += int(m.group(6))
                continue
            g = AUDIO.search(line)
            if g:
                gaps += int(g.group(1))
                gap_pct.append(float(g.group(3)))
    if not fps:
        return f"{path}: no windows past t={start:.0f}s"
    s = sorted(fps)
    n = len(s)
    med, q1, q3 = s[n // 2], s[n // 4], s[(3 * n) // 4]
    at60 = 100.0 * sum(1 for v in s if v >= 59.0) / n
    line = (f"{path}: {n} windows: median {med:.1f} fps (q1 {q1:.1f}, q3 {q3:.1f}), mean {sum(s) / n:.1f}, "
            f"{at60:.0f}% of windows at 60+, worst {worst:.0f} ms, {100.0 * over / frames:.2f}% of {frames} frames over 34 ms")
    if gap_pct:
        line += f"; audio gaps {gaps} ({sum(gap_pct) / len(gap_pct):.1f}% of blocks)"
    return line


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--from", dest="start", type=float, default=100.0, help="first second counted (default 100)")
    ap.add_argument("--to", dest="end", type=float, default=None, help="last second counted (default: the end)")
    args = ap.parse_args()
    for path in args.logs:
        print(summarise(path, args.start, args.end))
    return 0


if __name__ == "__main__":
    sys.exit(main())
