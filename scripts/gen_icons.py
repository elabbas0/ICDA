#!/usr/bin/env python3
"""gen_icons.py - draw ICDA's own flat icon set (no external art).

Renders 16 distinct 32x32 RGBA icons with pure-stdlib raster ops and
writes two outputs consumed by the build:

  1. resources/icons/<name>.ico  - single-entry BMP .ico (32x32 32bpp
     BI_RGB) parsed by ic_ico_parse() into /usr/share/icons.
  2. userspace/icon_data.h       - builtin RGBA registry with the exact
     same struct layout the tree already uses
     (ic_builtin_icon_entry_t {name, w, h, rgba}).

Style: flat modern tiles - rounded-square hue tile + white glyph with
features >= 2px so icons survive nearest-neighbor downscales to the
22px taskbar and desktop sizes. Fully deterministic output.

Usage:  python3 scripts/gen_icons.py   (run from repo root)
"""
import math
import os
import struct
import sys

SIZE = 32
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(REPO, "resources", "icons")
HEADER_PATH = os.path.join(REPO, "userspace", "icon_data.h")

WHITE = (255, 255, 255, 255)


class Canvas:
    """Tiny 32x32 RGBA raster surface (top-left origin)."""

    def __init__(self):
        self.px = bytearray(SIZE * SIZE * 4)  # transparent

    def set(self, x, y, color):
        if 0 <= x < SIZE and 0 <= y < SIZE:
            r, g, b, a = color
            o = (y * SIZE + x) * 4
            if a == 255:
                self.px[o:o + 4] = bytes((r, g, b, a))
            else:
                # source-over blend onto destination
                dr, dg, db, da = self.px[o:o + 4]
                inv = 255 - a
                self.px[o:o + 4] = bytes((
                    (r * a + dr * inv) // 255,
                    (g * a + dg * inv) // 255,
                    (b * a + db * inv) // 255,
                    a + da * inv // 255,
                ))

    def rect(self, x0, y0, x1, y1, color):
        for y in range(max(0, y0), min(SIZE, y1)):
            for x in range(max(0, x0), min(SIZE, x1)):
                self.set(x, y, color)

    def hline(self, x0, x1, y, color):
        self.rect(x0, y, x1, y + 1, color)

    def vline(self, x, y0, y1, color):
        self.rect(x, y0, x + 1, y1, color)

    def line(self, x0, y0, x1, y1, w, color):
        """Thick line via square brush along Bresenham path."""
        dx = abs(x1 - x0)
        dy = -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        r = w // 2
        x, y = x0, y0
        while True:
            self.rect(x - r, y - r, x + r + (w % 2), y + r + (w % 2), color)
            if x == x1 and y == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x += sx
            if e2 <= dx:
                err += dx
                y += sy

    def circle(self, cx, cy, rad, color, fill=True):
        for y in range(cy - rad - 1, cy + rad + 2):
            for x in range(cx - rad - 1, cx + rad + 2):
                d = math.hypot(x - cx + 0.5, y - cy + 0.5)
                if fill and d <= rad + 0.5:
                    self.set(x, y, color)
                elif not fill and abs(d - rad) < 0.9:
                    self.set(x, y, color)

    def ellipse(self, cx, cy, rx, ry, color):
        for y in range(cy - ry - 1, cy + ry + 2):
            for x in range(cx - rx - 1, cx + rx + 2):
                if ((x - cx + 0.5) / (rx + 0.5)) ** 2 + \
                   ((y - cy + 0.5) / (ry + 0.5)) ** 2 <= 1.0:
                    self.set(x, y, color)

    def rrect(self, x0, y0, x1, y1, rad, color):
        self.rect(x0 + rad, y0, x1 - rad, y1, color)
        self.rect(x0, y0 + rad, x1, y1 - rad, color)
        for cx, cy in ((x0 + rad, y0 + rad), (x1 - rad - 1, y0 + rad),
                       (x0 + rad, y1 - rad - 1), (x1 - rad - 1, y1 - rad - 1)):
            self.circle(cx, cy, rad, color)

    def triangle(self, ax, ay, bx, by, cx, cy, color):
        xs = sorted((ax, bx, cx))
        for y in range(min(ay, by, cy), max(ay, by, cy) + 1):
            row = []
            for (px, py), (qx, qy) in (((ax, ay), (bx, by)),
                                       ((bx, by), (cx, cy)),
                                       ((cx, cy), (ax, ay))):
                if (py <= y < qy) or (qy <= y < py):
                    t = (y - py) / (qy - py)
                    row.append(px + t * (qx - px))
            if len(row) >= 2:
                self.hline(int(min(row)), int(max(row)) + 1, y, color)

    def polygon(self, pts, color):
        """Scanline fill for convex/concave polygons (cursor arrow)."""
        ys = [p[1] for p in pts]
        for y in range(min(ys), max(ys) + 1):
            row = []
            n = len(pts)
            for i in range(n):
                (px, py), (qx, qy) = pts[i], pts[(i + 1) % n]
                if (py <= y < qy) or (qy <= y < py):
                    t = (y - py) / (qy - py)
                    row.append(px + t * (qx - px))
            row.sort()
            for i in range(0, len(row) - 1, 2):
                self.hline(int(math.ceil(row[i])),
                           int(math.floor(row[i + 1])) + 1, y, color)

    def tile(self, rgb, radius=6, light=True):
        """Rounded-square app tile + subtle top light."""
        self.rrect(3, 3, 29, 29, radius, rgb + (255,))
        if light:
            r, g, b = rgb
            hi = (min(255, r + 28), min(255, g + 28), min(255, b + 28), 255)
            self.rect(3 + radius, 3, 29 - radius, 8, hi)


def shade(rgb, f):
    return tuple(max(0, min(255, int(c * f))) for c in rgb)


def draw_folder(c):
    c.tile((217, 147, 30))
    c.rrect(5, 9, 13, 13, 1, (217, 147, 30, 255))
    c.rrect(5, 13, 27, 25, 2, (245, 185, 63, 255))
    c.rect(5, 13, 27, 15, (255, 208, 107, 255))


def draw_terminal(c):
    c.tile((22, 32, 46))
    c.line(10, 13, 15, 17, 2, (74, 222, 128, 255))
    c.line(15, 17, 10, 21, 2, (74, 222, 128, 255))
    c.rect(17, 20, 23, 22, (74, 222, 128, 255))


def draw_shell(c):
    c.tile((11, 18, 32))
    y = (74, 222, 128, 255)
    c.rect(9, 12, 15, 14, y)      # upper bar
    c.rect(20, 12, 23, 23, y)     # right spine
    c.rect(9, 16, 23, 18, y)      # middle bar
    c.rect(9, 16, 12, 23, y)      # lower-left hook
    c.rect(9, 21, 15, 23, y)      # lower bar


def draw_file(c):
    c.rect(9, 5, 21, 27, (237, 242, 247, 255))
    c.triangle(21, 5, 21, 11, 15, 5, (148, 163, 184, 255))
    for yy in (15, 18, 21):
        c.rect(12, yy, 20, yy + 1, (100, 116, 139, 255))


def draw_editor(c):
    c.tile((20, 184, 166))
    c.line(20, 8, 11, 22, 3, WHITE)
    c.triangle(11, 22, 8, 26, 13, 24, (251, 191, 36, 255))


def draw_disk(c):
    c.tile((31, 41, 55))
    c.circle(16, 16, 9, (203, 213, 225, 255))
    c.circle(16, 16, 3, (31, 41, 55, 255))
    c.rect(14, 6, 18, 9, (245, 185, 63, 255))


def draw_gear(c):
    c.tile((51, 65, 85))
    for dx, dy in ((0, -9), (0, 9), (-9, 0), (9, 0),
                   (-6, -6), (6, -6), (-6, 6), (6, 6)):
        c.rect(16 + dx - 2, 16 + dy - 2, 16 + dx + 2, 16 + dy + 2,
               (226, 232, 240, 255))
    c.circle(16, 16, 7, (226, 232, 240, 255))
    c.circle(16, 16, 3, (51, 65, 85, 255))


def draw_app(c):
    c.tile((37, 99, 235))
    for bx, by in ((8, 8), (18, 8), (8, 18), (18, 18)):
        c.rrect(bx, by, bx + 7, by + 7, 2, WHITE)


def draw_audio(c):
    c.tile((124, 58, 237))
    c.rect(7, 13, 11, 20, WHITE)
    c.triangle(11, 10, 18, 16, 11, 22, WHITE)
    for rad in (4, 8):
        for deg in range(-50, 51, 4):
            a = math.radians(deg)
            c.set(int(18 + rad * math.cos(a)), int(16 + rad * math.sin(a)),
                  WHITE)


def draw_music(c):
    c.tile((219, 39, 119))
    c.rect(19, 7, 21, 21, WHITE)
    c.rect(11, 7, 21, 10, WHITE)
    c.ellipse(13, 21, 4, 3, WHITE)
    c.ellipse(22, 22, 4, 3, WHITE)


def draw_wav(c):
    c.tile((8, 145, 178))
    heights = (6, 12, 18, 11, 7, 14, 9)
    for i, hh in enumerate(heights):
        x = 6 + i * 3
        c.rect(x, 16 - hh // 2, x + 2, 16 + (hh + 1) // 2, WHITE)


def draw_desktop(c):
    c.rrect(5, 8, 27, 22, 2, (15, 23, 42, 255))
    c.rect(7, 10, 25, 19, (125, 211, 252, 255))
    c.rect(14, 22, 18, 25, (15, 23, 42, 255))
    c.rect(10, 25, 22, 27, (15, 23, 42, 255))


def draw_cursor(c):
    arrow = [(8, 4), (8, 24), (13, 19), (16, 25), (19, 24),
             (15, 18), (20, 18)]
    fat = [(x * 2 - 8, y * 2 - 4) for x, y in arrow]
    c.polygon([(x // 2, y // 2) for x, y in fat], (15, 23, 42, 255))
    c.polygon(arrow, WHITE)


def draw_close(c):
    c.tile((220, 38, 38))
    c.line(10, 10, 22, 22, 3, WHITE)
    c.line(22, 10, 10, 22, 3, WHITE)


def draw_min(c):
    c.tile((71, 85, 105))
    c.rect(10, 15, 22, 17, WHITE)


def draw_max(c):
    c.tile((71, 85, 105))
    c.rect(10, 10, 22, 12, WHITE)
    c.rect(10, 20, 22, 22, WHITE)
    c.rect(10, 10, 12, 22, WHITE)
    c.rect(20, 10, 22, 22, WHITE)


DRAWERS = {
    "app": draw_app,
    "audio": draw_audio,
    "close": draw_close,
    "cursor": draw_cursor,
    "desktop": draw_desktop,
    "disk": draw_disk,
    "editor": draw_editor,
    "file": draw_file,
    "folder": draw_folder,
    "gear": draw_gear,
    "max": draw_max,
    "min": draw_min,
    "music": draw_music,
    "shell": draw_shell,
    "terminal": draw_terminal,
    "wav": draw_wav,
}


def write_ico(path, canvas):
    """Single-entry 32x32 32bpp BI_RGB .ico (what ic_ico_parse reads)."""
    px = canvas.px
    assert len(px) == SIZE * SIZE * 4
    dib = struct.pack("<IIIHHIIIIII", 40, SIZE, SIZE * 2, 1, 32, 0,
                      SIZE * SIZE * 4, 0, 0, 0, 0)
    body = bytearray()
    for y in range(SIZE - 1, -1, -1):  # bottom-up, BGRA
        for x in range(SIZE):
            o = (y * SIZE + x) * 4
            r, g, b, a = px[o:o + 4]
            body += bytes((b, g, r, a))
    body += bytes(SIZE * SIZE // 8)  # AND mask: all opaque
    img = dib + bytes(body)
    hdr = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", SIZE, SIZE, 0, 0, 1, 32,
                        len(img), 6 + 16)
    with open(path, "wb") as f:
        f.write(hdr + entry + img)


def write_header(path, canvases):
    with open(path, "w") as f:
        f.write("/* Generated by scripts/gen_icons.py - do not edit by hand. */\n"
                "#ifndef USERSPACE_ICON_DATA_H\n"
                "#define USERSPACE_ICON_DATA_H\n\n"
                "#include <stdint.h>\n\n"
                "typedef struct {\n"
                "    const char *name;\n"
                "    uint16_t     w;\n"
                "    uint16_t     h;\n"
                "    const uint8_t *rgba;\n"
                "} ic_builtin_icon_entry_t;\n\n")
        for name in sorted(canvases):
            px = canvases[name].px
            f.write("static const uint8_t icon_%s_rgba[] = {\n    " % name)
            f.write(", ".join(str(b) for b in px))
            f.write("\n};\n\n")
        f.write("static const ic_builtin_icon_entry_t ic_builtin_icons[] = {\n")
        for name in sorted(canvases):
            f.write('    { "%s", 32, 32, icon_%s_rgba },\n' % (name, name))
        f.write("};\n\n#define IC_BUILTIN_ICON_COUNT %d\n\n"
                "#endif /* USERSPACE_ICON_DATA_H */\n" % len(canvases))


def main():
    os.makedirs(ICON_DIR, exist_ok=True)
    canvases = {}
    for name, fn in sorted(DRAWERS.items()):
        c = Canvas()
        fn(c)
        canvases[name] = c
        write_ico(os.path.join(ICON_DIR, name + ".ico"), c)
    write_header(HEADER_PATH, canvases)
    import hashlib
    digests = set()
    for n in canvases:
        with open(os.path.join(ICON_DIR, n + ".ico"), "rb") as f:
            digests.add(hashlib.md5(f.read()).digest())
    print("icons: %d, distinct .ico contents: %d, header: %s"
          % (len(canvases), len(digests), HEADER_PATH))
    if len(digests) != len(canvases):
        print("WARNING: duplicate .ico contents!", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
