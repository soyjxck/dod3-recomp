#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG with nothing but the standard library.

    python tools/ppm2png.py in.ppm out.png [--scale N]

--scale N downsamples by an integer factor (nearest), for a quick look.
"""
import struct
import sys
import zlib


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
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    scale = 1
    if "--scale" in sys.argv:
        scale = int(sys.argv[sys.argv.index("--scale") + 1])
        args = [a for a in args if a != str(scale)]
    if len(args) < 2:
        print(__doc__)
        return 2
    w, h, rgb = read_ppm(args[0])
    if scale > 1:
        nw, nh = w // scale, h // scale
        out = bytearray()
        for y in range(nh):
            row = rgb[(y * scale) * w * 3:(y * scale + 1) * w * 3]
            for x in range(nw):
                out += row[x * scale * 3:x * scale * 3 + 3]
        w, h, rgb = nw, nh, bytes(out)
    write_png(args[1], w, h, rgb)
    return 0


if __name__ == "__main__":
    sys.exit(main())
