#!/usr/bin/env python3
"""Minimal P6 PPM -> PNG converter using only the stdlib (zlib + struct).

Usage: python ppm2png.py in.ppm out.png
"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", "not a P6 PPM"
    i = 2
    parts = []
    while len(parts) < 3:
        while data[i] in b" \t\r\n":
            i += 1
        if data[i] == ord("#"):
            while data[i] not in b"\r\n":
                i += 1
            continue
        j = i
        while data[j] not in b" \t\r\n":
            j += 1
        parts.append(int(data[i:j]))
        i = j + 1
    w, h, mx = parts
    assert mx == 255
    px = data[i:]
    assert len(px) >= w * h * 3, "truncated pixel data"
    return w, h, px[: w * h * 3]


def chunk(tag, payload):
    c = struct.pack(">I", len(payload)) + tag + payload
    return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)


def write_png(w, h, rgb, path):
    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    w, h, px = read_ppm(sys.argv[1])
    write_png(w, h, px, sys.argv[2])
    print("wrote %s (%dx%d)" % (sys.argv[2], w, h))
