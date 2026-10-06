#!/usr/bin/env python3
"""Summarise the [frametime] windows of one or more run logs.

    python tools/bench_summary.py out/bench/base3.log [more logs...] [--from 100]

Per log: the 5 s windows from t >= --from (default 100 s), their fps median
and quartiles (robust to the cutscene and menu windows autoplay sometimes
wanders into), the mean, the share of windows at 60 fps or better, the
worst frame, and the share of frames over 34 ms. Also the audio-gap totals
from the [audio-rate] lines when present.
"""
import re
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    start = 100.0
    if "--from" in sys.argv:
        start = float(sys.argv[sys.argv.index("--from") + 1])
        args = [a for a in args if a != str(int(start)) and a != str(start)]
    if not args:
        print(__doc__)
        return 2
    rx = re.compile(r"\[frametime\] t=(\d+)s ([\d.]+) fps, mean ([\d.]+) ms, worst ([\d.]+) ms, (\d+) of (\d+) frames")
    gx = re.compile(r"\[audio-rate\] gaps (\d+) \((\d+) blocks, ([\d.]+)% of the window\)")
    for path in args:
        fps, worst, over, frames, gaps, gap_pct = [], 0.0, 0, 0, 0, []
        with open(path, errors="replace") as f:
            for line in f:
                m = rx.search(line)
                if m:
                    t = float(m.group(1))
                    if t < start:
                        continue
                    fps.append(float(m.group(2)))
                    worst = max(worst, float(m.group(4)))
                    over += int(m.group(5)); frames += int(m.group(6))
                    continue
                g = gx.search(line)
                if g:
                    gaps += int(g.group(1)); gap_pct.append(float(g.group(3)))
        if not fps:
            print(f"{path}: no windows past t={start:.0f}s")
            continue
        s = sorted(fps)
        n = len(s)
        med = s[n // 2]; q1 = s[n // 4]; q3 = s[(3 * n) // 4]
        at60 = 100.0 * sum(1 for v in s if v >= 59.0) / n
        line = (f"{path}: {n} windows: median {med:.1f} fps (q1 {q1:.1f}, q3 {q3:.1f}), mean {sum(s)/n:.1f}, "
                f"{at60:.0f}% of windows at 60+, worst {worst:.0f} ms, {100.0*over/frames:.2f}% of {frames} frames over 34 ms")
        if gap_pct:
            line += f"; audio gaps {gaps} ({sum(gap_pct)/len(gap_pct):.1f}% of blocks)"
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
