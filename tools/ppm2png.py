#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG with nothing but the standard library.

    python tools/ppm2png.py in.ppm out.png [--scale N]

--scale N downsamples by an integer factor (nearest), for a quick look.
"""
import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm import read_ppm  # noqa: E402


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ppm")
    ap.add_argument("png")
    ap.add_argument("--scale", type=int, default=1, help="downsample by this factor")
    args = ap.parse_args()
    w, h, rgb = read_ppm(args.ppm)
    if args.scale > 1:
        s = args.scale
        nw, nh = w // s, h // s
        out = bytearray()
        for y in range(nh):
            row = rgb[(y * s) * w * 3:(y * s + 1) * w * 3]
            for x in range(nw):
                out += row[x * s * 3:x * s * 3 + 3]
        w, h, rgb = nw, nh, bytes(out)
    write_png(args.png, w, h, rgb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
