#!/usr/bin/env python3
"""ppm2png.py in.ppm [out.png] [--scale N]: the benchmark's screenshots as PNG (no libraries needed)."""
import struct, sys, zlib


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:                       # magic, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    pos += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos:pos + w * h * 3]


def write_png(path, w, h, rgb, scale=1):
    if scale > 1:                               # nearest neighbour down-scaling
        nw, nh = w // scale, h // scale
        rows = []
        for y in range(nh):
            row = rgb[(y * scale) * w * 3:(y * scale + 1) * w * 3]
            rows.append(b"".join(row[x * scale * 3:x * scale * 3 + 3] for x in range(nw)))
        w, h, raw = nw, nh, b"".join(b"\0" + r for r in rows)
    else:
        raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    scale = int(sys.argv[sys.argv.index("--scale") + 1]) if "--scale" in sys.argv else 1
    if scale > 1:
        args = [a for a in args if a != str(scale)]
    src = args[0]
    dst = args[1] if len(args) > 1 else src.rsplit(".", 1)[0] + ".png"
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb, scale)
