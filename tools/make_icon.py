#!/usr/bin/env python3
"""Draw the application icon (an original design, no game artwork).

    python tools/make_icon.py assets/app.ico [preview.png]

A six-petal crimson flower on a dark rounded square, drawn at 1024 px and
scaled down, written as an .ico with 256, 128, 64, 48, 32 and 16 pixel sizes
(needs Pillow: tools/requirements.txt). app.rc embeds assets/app.ico as the
Windows executable's icon, which its windows load at run time;
tools/package_mac.sh renders the same drawing into the app's .icns.
"""
import math
import sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024


def petal(draw, cx, cy, angle, length, width, fill):
    """An almond-shaped petal from the centre outwards, as a polygon."""
    pts = []
    n = 48
    for i in range(n + 1):
        t = i / n                                   # 0 at the centre, 1 at the tip
        r = length * t
        half = width * math.sin(math.pi * t) ** 0.9 * (1.0 - 0.35 * t)
        pts.append((r, half))
    outline = pts + [(r, -h) for r, h in reversed(pts)]
    ca, sa = math.cos(angle), math.sin(angle)
    draw.polygon([(cx + r * ca - h * sa, cy + r * sa + h * ca) for r, h in outline], fill=fill)


def render():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((24, 24, S - 24, S - 24), radius=190, fill=(18, 14, 20, 255))
    cx = cy = S // 2
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((cx - 330, cy - 330, cx + 330, cy + 330), fill=(140, 10, 30, 110))
    im = Image.alpha_composite(im, glow.filter(ImageFilter.GaussianBlur(60)))
    d = ImageDraw.Draw(im)
    for k in range(6):                               # back petals, darker, offset
        petal(d, cx, cy, math.radians(30 + 60 * k), 380, 120, (120, 8, 28, 255))
    for k in range(6):                               # front petals
        petal(d, cx, cy, math.radians(60 * k - 90), 400, 125, (205, 22, 48, 255))
    for k in range(6):                               # a lighter vein down each
        petal(d, cx, cy, math.radians(60 * k - 90), 330, 26, (240, 90, 105, 255))
    d.ellipse((cx - 70, cy - 70, cx + 70, cy + 70), fill=(250, 238, 230, 255))
    d.ellipse((cx - 34, cy - 34, cx + 34, cy + 34), fill=(205, 22, 48, 255))
    return im


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    im = render()
    sizes = [(s, s) for s in (256, 128, 64, 48, 32, 16)]
    im.resize((256, 256), Image.LANCZOS).save(sys.argv[1], format="ICO", sizes=sizes)
    if len(sys.argv) > 2:
        im.resize((256, 256), Image.LANCZOS).save(sys.argv[2])
    print(f"{sys.argv[1]}: {', '.join(str(s[0]) for s in sizes)} px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
