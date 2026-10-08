#!/usr/bin/env python3
"""hud_visible.py <frame.ppm>...: is the battle HUD on screen? Counts bright
green pixels where the health gauge's left end sits (x 210-300, y 84-97 of a
1280x720 frame); prints "<count> HUD|-" per frame, exit 0 if the last has it.
tools/autoplay.sh asks it. """
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm import read_ppm  # noqa: E402


def hud_pixels(w, h, px):
    n = 0
    for y in range(84 * h // 720, 97 * h // 720 + 1):
        for x in range(210 * w // 1280, 300 * w // 1280):
            o = (y * w + x) * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            if g > 170 and g > r + 50 and b > 100:
                n += 1
    return n


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    ok = False
    for f in sys.argv[1:]:
        n = hud_pixels(*read_ppm(f))
        ok = n >= 40
        print(n, "HUD" if ok else "-", f)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
