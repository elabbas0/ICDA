#!/usr/bin/env python3
"""Drive a headless ICDA GUI session in QEMU and verify what renders.

The QEMU guest must be started with:
    -qmp tcp:0.0.0.0:<port>,server,nowait
    -serial file:<log> -display none

Commands (all talk QMP; `screendump` needs a writable /workspace mount):
    scripts/gui-check.py <port> dump <ppm-path>
    scripts/gui-check.py <port> regions <ppm-path>
    scripts/gui-check.py <port> move <x> <y>
    scripts/gui-check.py <port> click <x> <y>
    scripts/gui-check.py <port> key <name>          # e.g. ret, up, t
"""
import json
import os
import socket
import sys
import time


CURSOR_FILE = "gui-cursor.txt"


def load_cursor():
    if os.path.exists(CURSOR_FILE):
        try:
            x, y = open(CURSOR_FILE).read().split()
            return (int(x), int(y))
        except (ValueError, OSError):
            pass
    return (512, 384)  # where the OS centers the pointer


def save_cursor(pos):
    try:
        with open(CURSOR_FILE, "w") as f:
            f.write("%d %d\n" % pos)
    except OSError:
        pass


class Qmp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("rwb", buffering=0)
        self.cursor = load_cursor()
        self._read_greeting()
        self.cmd("qmp_capabilities")

    def _read_greeting(self):
        line = self.f.readline()
        msg = json.loads(line)
        if "QMP" not in msg:
            raise SystemExit("not a QMP greeting: %r" % line[:80])

    def cmd(self, execute, arguments=None):
        obj = {"execute": execute}
        if arguments is not None:
            obj["arguments"] = arguments
        self.f.write((json.dumps(obj) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QMP connection closed")
            msg = json.loads(line)
            if "error" in msg:
                raise SystemExit("QMP error: %s" % msg["error"])
            if "return" in msg:
                return msg["return"]

    def hmp(self, command_line):
        return self.cmd("human-monitor-command",
                        {"command-line": command_line})

    def screendump(self, path):
        self.hmp("screendump %s" % path)
        time.sleep(0.4)

    def move(self, x, y, warmup=False):
        # The emulated PS/2 mouse takes relative motion only, and each
        # packet carries a signed 8-bit delta, so walk there in steps.
        # Send each axis as its own input-send-event: QEMU 7.2's PS/2
        # mouse silently drops the y event when x and y share a call.
        # QEMU can also lose the very first rel event after a fresh
        # boot, so the driver issues a throwaway warm-up move once.
        if warmup:
            self.cmd("input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": -5}},
            ]})
            time.sleep(1.0)
            self.cmd("input-send-event", {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 5}},
            ]})
            time.sleep(1.0)
        cur = self.cursor
        dx = x - cur[0]
        dy = y - cur[1]
        while dx or dy:
            sx = max(-127, min(127, dx))
            sy = max(-127, min(127, dy))
            if sx:
                self.cmd("input-send-event", {"events": [
                    {"type": "rel", "data": {"axis": "x", "value": sx}},
                ]})
                cur = (cur[0] + sx, cur[1])
                dx -= sx
                time.sleep(1.2)
            if sy:
                self.cmd("input-send-event", {"events": [
                    {"type": "rel", "data": {"axis": "y", "value": sy}},
                ]})
                cur = (cur[0], cur[1] + sy)
                dy -= sy
                time.sleep(1.2)
        self.cursor = (x, y)
        save_cursor(self.cursor)
        time.sleep(0.2)

    def click(self, x, y, warmup=False):
        self.move(x, y, warmup=warmup)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": True}},
        ]})
        time.sleep(0.1)
        self.cmd("input-send-event", {"events": [
            {"type": "btn", "data": {"button": "left", "down": False}},
        ]})
        time.sleep(0.3)

    def key(self, name):
        # QEMU 7.2 wants an object: {"type": "qcode", "data": name}
        self.cmd("input-send-event", {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": name}, "down": True}},
            {"type": "key", "data": {"key": {"type": "qcode", "data": name}, "down": False}},
        ]})
        time.sleep(0.2)

    def close(self):
        self.s.close()


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
    cmd = sys.argv[2]
    q = Qmp(port)
    try:
        if cmd == "dump":
            q.screendump(sys.argv[3])
            print("saved %s" % sys.argv[3])
        elif cmd == "regions":
            q.screendump(sys.argv[3])
            w, h, px = parse_ppm(sys.argv[3])
            print("framebuffer: %dx%d" % (w, h))
            region(w, h, px, 0, 0, w, h, "whole screen")
            region(w, h, px, 0, 0, w, int(h * 0.5), "sky (top half)")
            region(w, h, px, 0, int(h * 0.62), w, h - 42, "hills (lower)")
            region(w, h, px, 0, h - 42, w, h, "taskbar (bottom)")
            region(w, h, px, 120, 60, 900, 460, "desktop window area")
            region(w, h, px, 0, 0, 100, 40, "desktop icons")
        elif cmd == "move":
            q.move(int(sys.argv[3]), int(sys.argv[4]))
            print("moved mouse to %s,%s" % (sys.argv[3], sys.argv[4]))
        elif cmd == "click":
            q.click(int(sys.argv[3]), int(sys.argv[4]))
            print("clicked at %s,%s" % (sys.argv[3], sys.argv[4]))
        elif cmd == "key":
            q.key(sys.argv[3])
            print("pressed %s" % sys.argv[3])
        else:
            raise SystemExit("unknown command: %s" % cmd)
    finally:
        q.close()


if __name__ == "__main__":
    main()
