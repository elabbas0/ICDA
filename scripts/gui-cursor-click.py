#!/usr/bin/env python3
"""Locate the WM cursor tip in a screendump and click a target reliably.

The emulated PS/2 mouse takes relative motion only and QEMU drops events
sometimes, so the OS cursor drifts from any externally-tracked position.
This finds the actual cursor (white 45-deg diagonal + black outline) in a
screendump and walks it to the target, verifying after each axis.

Usage: scripts/gui-cursor-click.py <qmp-port> <x> <y>
"""
import json
import socket
import sys
import time


class Qmp:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("rwb", buffering=0)
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
        return self.cmd("human-monitor-command", {"command-line": command_line})

    def screendump(self, path):
        self.hmp("screendump %s" % path)
        time.sleep(0.4)

    def rel(self, axis, value):
        self.cmd("input-send-event", {
            "events": [{"type": "rel", "data": {"axis": axis, "value": value}}]})
        time.sleep(0.6)

    def button(self, down):
        self.cmd("input-send-event", {
            "events": [{"type": "btn", "data": {"button": "left",
                                               "down": down}}]})
        time.sleep(0.2)


def load_px(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = data[3 + len(parts[2]):]
    return w, h, px


def find_cursor(w, h, px):
    """Return (mx, my) tip of the WM cursor, or None.

    Cursor is a white diagonal (tip at (mx,my)) with a black outline ring.
    """
    def at(x, y):
        o = (y * w + x) * 3
        return px[o], px[o + 1], px[o + 2]

    cands = []
    for my in range(1, h - 20):
        for mx in range(1, w - 12):
            ok = True
            for k in range(10):
                if at(mx + k, my + k) != (255, 255, 255):
                    ok = False
                    break
            if not ok:
                continue
            # black outline corner above-left of the tip
            if all(max(at(mx + dx, my + dy)) < 60 for dx, dy in
                   ((-1, -1), (-1, 0), (0, -1), (1, -1), (-1, 1))):
                cands.append((mx, my))
    # the true tip is the uppermost-leftmost candidate; drop clusters that
    # are just the diagonal body of another candidate
    cands.sort()
    return cands[0] if cands else None


def main():
    port = int(sys.argv[1])
    tx, ty = int(sys.argv[2]), int(sys.argv[3])
    dump_path = "cursor-click.ppm"
    q = Qmp(port)
    q.screendump(dump_path)
    w, h, px = load_px(dump_path)
    cur = find_cursor(w, h, px)
    if cur is None:
        raise SystemExit("cursor not found in dump")
    print("cursor at %s, target %s" % (cur, (tx, ty)))
    cx, cy = cur
    for attempt in range(8):
        dx = tx - cx
        dy = ty - cy
        if dx == 0 and dy == 0:
            break
        if dx:
            q.rel("x", max(-127, min(127, dx)))
        if dy:
            q.rel("y", max(-127, min(127, dy)))
        q.screendump(dump_path)
        w, h, px = load_px(dump_path)
        nxt = find_cursor(w, h, px)
        if nxt is None:
            raise SystemExit("cursor vanished after move")
        cx, cy = nxt
        print("  moved, cursor now %s" % ((cx, cy),))
    if cx != tx or cy != ty:
        print("WARN: cursor at %s not target %s" % ((cx, cy), (tx, ty)))
    # click: press + release at current spot
    q.button(True)
    q.button(False)
    print("clicked at %s" % ((cx, cy),))


if __name__ == "__main__":
    main()
