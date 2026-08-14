#!/usr/bin/env python3
"""Capture the ICDA GUI from a running QEMU HMP monitor and summarize the pixels.

Usage:
    scripts/gui-check.py <port> <ppm-out-path> [--regions]

Connects to the QEMU human monitor (HMP) on 127.0.0.1:<port>, issues a
`screendump`, and reports the average color + distinct color count for
regions of the framebuffer so a headless check can confirm the desktop
(and not a black screen or a fault screen) is actually rendering.
"""
import socket
import sys
import time


def monitor_cmd(port, cmd, settle=0.6):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    s.settimeout(10)
    out = b""
    s.sendall((cmd + "\n").encode())
    time.sleep(settle)
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
            if b"(qemu) " in out:
                break
    except socket.timeout:
        pass
    s.close()
    return out


def parse_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise SystemExit("bad PPM magic: %r" % data[:8])
    i = 2
    parts = []
    while len(parts) < 3:
        while i < len(data) and data[i:i + 1] in (b" \t\r\n"):
            i += 1
        j = i
        while i < len(data) and data[i:i + 1] not in (b" \t\r\n"):
            i += 1
        parts.append(int(data[j:i]))
    w, h, _ = parts
    while i < len(data) and data[i:i + 1] in (b" \t\r\n"):
        i += 1
    px = data[i:]
    if len(px) < w * h * 3:
        raise SystemExit("short pixel data: %d < %d" % (len(px), w * h * 3))
    return w, h, px


def region(w, h, px, x0, y0, x1, y1, label):
    x1 = min(x1, w)
    y1 = min(y1, h)
    if x0 >= x1 or y0 >= y1:
        print("  %-22s (empty)" % label)
        return
    rsum = gsum = bsum = 0
    colors = set()
    n = 0
    for y in range(y0, y1):
        row = y * w
        for x in range(x0, x1):
            o = (row + x) * 3
            r, g, b = px[o], px[o + 1], px[o + 2]
            rsum += r
            gsum += g
            bsum += b
            colors.add((r, g, b))
            n += 1
    print("  %-22s avg RGB(%3d,%3d,%3d) distinct=%d" % (
        label, rsum // n, gsum // n, bsum // n, len(colors)))


def main():
    port = int(sys.argv[1])
    ppm = sys.argv[2]
    if not monitor_cmd(port, "screendump %s" % ppm).strip():
        print("no monitor response")
        sys.exit(2)
    w, h, px = parse_ppm(ppm)
    print("framebuffer: %dx%d" % (w, h))
    if "--regions" in sys.argv:
        region(w, h, px, 0, 0, w, h, "whole screen")
        region(w, h, px, 0, 0, w, int(h * 0.5), "sky (top half)")
        region(w, h, px, 0, int(h * 0.62), w, h - 42, "hills (lower)")
        region(w, h, px, 0, h - 42, w, h, "taskbar (bottom)")
        region(w, h, px, 120, 60, 900, 460, "desktop window area")
        region(w, h, px, 0, 0, 100, 40, "desktop icons")


if __name__ == "__main__":
    main()
