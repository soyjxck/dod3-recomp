#!/usr/bin/env python3
"""Build the Windows application icon from a PNG.

    .venv/Scripts/python tools/make_icon.py Drakengard_3_boxart.png assets/app.ico

Square-crops the image, then writes an .ico with 256, 128, 64, 48, 32 and
16 pixel sizes (needs Pillow: .venv/Scripts/pip install pillow). app.rc
embeds assets/app.ico as the executable's icon; the D3D12 window class loads
it at run time.
"""
import sys
from PIL import Image


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGBA")
    w, h = im.size
    side = min(w, h)
    im = im.crop(((w - side) // 2, (h - side) // 2, (w - side) // 2 + side, (h - side) // 2 + side))
    sizes = [(s, s) for s in (256, 128, 64, 48, 32, 16) if s <= side or s == 16]
    im.save(dst, format="ICO", sizes=sizes)
    print(f"{dst}: {', '.join(str(s[0]) for s in sizes)} px from {src} ({w}x{h})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
