
"""gen_icons.py - draw ICDA's icon set (no external art).

Every icon is drawn at 4x on a 64 px grid and box-filtered down, so edges
are antialiased at every size the shell uses (22 px taskbar, 44-48 px
launcher and desktop, 64 px native).

The set follows one template so icons read as a family:
  * app icons sit on a 52 px rounded tile (radius 12) centred on the
    grid, filled with a top-lit vertical gradient, with a 1 px inner top
    highlight and a soft contact shadow beneath;
  * glyphs are white (or a single accent) with round caps, stroke 4 px
    at 64 px;
  * document/folder icons (used in file listings) are shaped objects,
    not tiles, with the same shadow.

Outputs consumed by the build:
  1. resources/icons/<name>.ico - single-entry BMP .ico (64x64 32bpp)
     parsed by ic_ico_parse() into /usr/share/icons.
  2. userspace/icon_data.h      - builtin RGBA registry
     (ic_builtin_icon_entry_t {name, w, h, rgba}).

Requires Pillow.  Usage:  python3 scripts/gen_icons.py
"""
import math
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFilter

SIZE = 64
S = 4                      
BIG = SIZE * S
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(REPO, "resources", "icons")
HEADER_PATH = os.path.join(REPO, "userspace", "icon_data.h")

WHITE = (255, 255, 255, 255)
TILE = (6, 5, 58, 57)      
TILE_R = 12
STROKE = 4


def sc(v):
    return int(round(v * S))


def box(x0, y0, x1, y1):
    return [sc(x0), sc(y0), sc(x1), sc(y1)]


def new_layer():
    return Image.new("RGBA", (BIG, BIG), (0, 0, 0, 0))


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def gradient(top, bottom, y0=0, y1=SIZE):
    """Full-canvas vertical gradient layer (RGBA)."""
    img = new_layer()
    px = img.load()
    for y in range(BIG):
        t = min(1.0, max(0.0, (y / S - y0) / max(1e-6, (y1 - y0))))
        c = lerp(top, bottom, t) + (255,)
        for x in range(BIG):
            px[x, y] = c
    return img


def mask_rrect(x0, y0, x1, y1, r):
    m = Image.new("L", (BIG, BIG), 0)
    ImageDraw.Draw(m).rounded_rectangle(box(x0, y0, x1, y1), radius=sc(r), fill=255)
    return m


def shadow(base, mask, dy=1.5, blur=2.0, alpha=0.32):
    """Soft contact shadow of `mask`, composited under everything."""
    sh = Image.new("L", (BIG, BIG), 0)
    sh.paste(mask, (0, sc(dy)))
    sh = sh.filter(ImageFilter.GaussianBlur(blur * S))
    sh = sh.point(lambda v: int(v * alpha))
    layer = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 255))
    layer.putalpha(sh)
    return Image.alpha_composite(base, layer)


def tile(top, bottom):
    """The shared app tile: gradient + inner highlight + contact shadow."""
    img = new_layer()
    m = mask_rrect(*TILE, TILE_R)
    img = shadow(img, m)
    fill = gradient(top, bottom, TILE[1], TILE[3])
    img.paste(fill, (0, 0), m)
    
    hl = new_layer()
    d = ImageDraw.Draw(hl)
    d.rounded_rectangle(box(TILE[0] + 0.5, TILE[1] + 0.5, TILE[2] - 0.5, TILE[3] - 0.5),
                        radius=sc(TILE_R - 0.5), outline=(255, 255, 255, 70), width=S)
    fade = Image.new("L", (BIG, BIG), 0)
    fp = fade.load()
    for y in range(BIG):
        v = int(255 * max(0.0, 1.0 - (y / S - TILE[1]) / 14.0))
        for x in range(BIG):
            fp[x, y] = v
    hl.putalpha(Image.composite(hl.getchannel("A"), Image.new("L", (BIG, BIG), 0), fade))
    img = Image.alpha_composite(img, hl)
    
    edge = new_layer()
    ImageDraw.Draw(edge).rounded_rectangle(box(*TILE), radius=sc(TILE_R),
                                           outline=(0, 0, 0, 38), width=max(1, S // 2))
    return Image.alpha_composite(img, edge)


def over(img, layer):
    return Image.alpha_composite(img, layer)


def stroke_lines(img, points, color=WHITE, width=STROKE, closed=False):
    layer = new_layer()
    d = ImageDraw.Draw(layer)
    pts = [(sc(x), sc(y)) for x, y in points]
    if closed:
        pts.append(pts[0])
    d.line(pts, fill=color, width=sc(width), joint="curve")
    r = sc(width) / 2
    for x, y in (pts if not closed else pts[:-1]):
        d.ellipse([x - r, y - r, x + r, y + r], fill=color)
    return over(img, layer)


def glyph_shadow(img, draw_fn, alpha=0.22):
    """Draw a glyph with a faint 1 px drop shadow for lift."""
    g = new_layer()
    draw_fn(ImageDraw.Draw(g))
    a = g.getchannel("A")
    sh = Image.new("L", (BIG, BIG), 0)
    sh.paste(a, (0, sc(1)))
    sh = sh.filter(ImageFilter.GaussianBlur(S)).point(lambda v: int(v * alpha))
    dark = Image.new("RGBA", (BIG, BIG), (0, 0, 0, 255))
    dark.putalpha(sh)
    return over(over(img, dark), g)




def draw_explorer():
    img = tile((92, 170, 255), (30, 110, 235))

    def g(d):
        d.rounded_rectangle(box(17, 22, 32, 30), radius=sc(2.5), fill=(214, 233, 255, 255))
        d.rounded_rectangle(box(17, 26, 47, 44), radius=sc(3.5), fill=WHITE)
        d.rectangle(box(17, 26, 47, 29), fill=(233, 243, 255, 255))
    return glyph_shadow(img, g)


def draw_terminal():
    img = tile((64, 64, 70), (28, 28, 32))
    inner = new_layer()
    ImageDraw.Draw(inner).rounded_rectangle(box(11, 10, 53, 52), radius=sc(8),
                                            outline=(255, 255, 255, 26), width=S)
    img = over(img, inner)
    img = stroke_lines(img, [(20, 24), (28, 31), (20, 38)], color=(245, 245, 247, 255), width=4)
    img = stroke_lines(img, [(32, 39), (43, 39)], color=(152, 152, 160, 255), width=4)
    return img


def draw_browser():
    img = tile((64, 200, 250), (24, 104, 232))
    cx, cy, r = 32, 31, 15

    def g(d):
        w = sc(3)
        d.ellipse(box(cx - r, cy - r, cx + r, cy + r), outline=WHITE, width=w)
        d.ellipse(box(cx - 6.5, cy - r, cx + 6.5, cy + r), outline=WHITE, width=w)
        d.line([sc(cx - r), sc(cy), sc(cx + r), sc(cy)], fill=WHITE, width=w)
        d.arc(box(cx - r * 1.5, cy - r - 13, cx + r * 1.5, cy - 4), 50, 130, fill=WHITE, width=w)
        d.arc(box(cx - r * 1.5, cy + 4, cx + r * 1.5, cy + r + 13), 230, 310, fill=WHITE, width=w)
    return glyph_shadow(img, g)


def draw_editor():
    img = tile((255, 255, 255), (228, 228, 234))
    lines = new_layer()
    d = ImageDraw.Draw(lines)
    for i, y in enumerate((20, 27, 34, 41)):
        w = (28, 30, 24, 17)[i]
        d.rounded_rectangle(box(15, y, 15 + w, y + 3), radius=sc(1.5), fill=(160, 160, 170, 255))
    img = over(img, lines)

    def pencil(d):
        
        ax, ay, bx, by = 47, 17, 29, 35
        d.line([sc(ax), sc(ay), sc(bx), sc(by)], fill=(255, 159, 10, 255), width=sc(7))
        d.line([sc(ax + 1.5), sc(ay - 1.5), sc(ax + 4), sc(ay - 4)], fill=(255, 105, 97, 255),
               width=sc(7))
        d.polygon([(sc(bx - 2.5), sc(by - 2.5)), (sc(bx + 2.5), sc(by + 2.5)),
                   (sc(bx - 5), sc(by + 5))], fill=(58, 58, 60, 255))
    return glyph_shadow(img, pencil, alpha=0.18)


def draw_music():
    img = tile((255, 104, 132), (240, 40, 82))

    def g(d):
        d.ellipse(box(15.5, 36, 26.5, 45), fill=WHITE)
        d.ellipse(box(35.5, 32, 46.5, 41), fill=WHITE)
        d.line([sc(25), sc(40), sc(25), sc(18)], fill=WHITE, width=sc(3.2))
        d.line([sc(45), sc(36), sc(45), sc(14)], fill=WHITE, width=sc(3.2))
        d.polygon([(sc(23.4), sc(16)), (sc(46.6), sc(11)), (sc(46.6), sc(18)), (sc(23.4), sc(23))],
                  fill=WHITE)
    return glyph_shadow(img, g)


def draw_disk():
    img = tile((120, 128, 146), (58, 64, 80))

    def g(d):
        d.rounded_rectangle(box(13, 20, 51, 43), radius=sc(5), fill=(236, 238, 242, 255))
        d.rectangle(box(13, 34, 51, 35), fill=(170, 174, 184, 255))
        d.ellipse(box(42, 37.5, 46, 41.5), fill=(52, 199, 89, 255))
        for x in (17, 21, 25):
            d.rounded_rectangle(box(x, 38, x + 2, 40), radius=sc(1), fill=(150, 154, 164, 255))
    return glyph_shadow(img, g)


def draw_taskman():
    img = tile((58, 58, 62), (22, 22, 26))
    grid = new_layer()
    d = ImageDraw.Draw(grid)
    for y in (20, 31, 42):
        d.line([sc(12), sc(y), sc(52), sc(y)], fill=(255, 255, 255, 20), width=S)
    img = over(img, grid)
    return stroke_lines(img, [(12, 33), (21, 33), (26, 20), (33, 44), (38, 29), (42, 33), (52, 33)],
                        color=(52, 211, 110, 255), width=3.6)


def gear_polygon(cx, cy, r_out, r_in, teeth, tooth_frac=0.46):
    pts = []
    n = teeth * 4
    for i in range(n):
        seg = i % 4
        a = (i // 4 + (0, tooth_frac * 0.5, 1 - tooth_frac * 0.5, 1)[seg] * 1.0) * 2 * math.pi / teeth
        r = r_out if seg in (1, 2) else r_in
        pts.append((sc(cx + r * math.cos(a)), sc(cy + r * math.sin(a))))
    return pts


def draw_settings():
    img = tile((214, 214, 220), (150, 150, 158))

    def g(d):
        d.polygon(gear_polygon(32, 31, 19, 15.5, 12), fill=(84, 84, 92, 255))
        d.ellipse(box(21, 20, 43, 42), fill=(84, 84, 92, 255))
        d.ellipse(box(24.5, 23.5, 39.5, 38.5), fill=(200, 200, 206, 255))
        d.ellipse(box(28, 27, 36, 35), fill=(84, 84, 92, 255))
    return glyph_shadow(img, g, alpha=0.3)


def draw_app():
    img = tile((170, 170, 178), (110, 110, 118))

    def g(d):
        for bx, by in ((18, 17), (34, 17), (18, 33), (34, 33)):
            d.rounded_rectangle(box(bx, by, bx + 12, by + 12), radius=sc(3.5), fill=WHITE)
    return glyph_shadow(img, g)




def draw_folder():
    img = new_layer()
    back = mask_rrect(8, 14, 56, 50, 5)
    tab = new_layer()
    d = ImageDraw.Draw(tab)
    d.rounded_rectangle(box(8, 11, 28, 20), radius=sc(3.5), fill=(53, 132, 228, 255))
    img = shadow(img, back, dy=1.2, blur=1.6, alpha=0.28)
    img = over(img, tab)
    bk = gradient((58, 140, 235), (43, 116, 214), 14, 50)
    img.paste(bk, (0, 0), back)
    front = mask_rrect(8, 20, 56, 51, 5)
    fr = gradient((117, 188, 255), (74, 156, 246), 20, 51)
    img.paste(fr, (0, 0), front)
    hl = new_layer()
    ImageDraw.Draw(hl).line([sc(12), sc(21), sc(52), sc(21)], fill=(255, 255, 255, 110), width=S)
    return over(img, hl)


def document(accent=None):
    img = new_layer()
    body = [(sc(14), sc(6)), (sc(40), sc(6)), (sc(50), sc(16)), (sc(50), sc(58)), (sc(14), sc(58))]
    m = Image.new("L", (BIG, BIG), 0)
    ImageDraw.Draw(m).polygon(body, fill=255)
    img = shadow(img, m, dy=1.0, blur=1.4, alpha=0.30)
    page = gradient((255, 255, 255), (242, 242, 246), 6, 58)
    img.paste(page, (0, 0), m)
    d = ImageDraw.Draw(img)
    d.polygon(body + [body[0]], outline=(0, 0, 0, 40))
    d.polygon([(sc(40), sc(6)), (sc(40), sc(16)), (sc(50), sc(16))], fill=(220, 220, 228, 255))
    return img


def draw_file():
    img = document()
    d = ImageDraw.Draw(img)
    for i, y in enumerate((26, 32, 38, 44)):
        w = (24, 26, 20, 14)[i]
        d.rounded_rectangle(box(20, y, 20 + w, y + 2.4), radius=sc(1.2), fill=(176, 176, 186, 255))
    return img


def draw_wav():
    img = document()
    d = ImageDraw.Draw(img)
    heights = (6, 12, 18, 10, 20, 14, 8, 16, 6)
    for i, h in enumerate(heights):
        x = 18 + i * 3.4
        d.rounded_rectangle(box(x, 37 - h / 2, x + 2.2, 37 + h / 2), radius=sc(1.1),
                            fill=lerp((255, 70, 110), (175, 82, 222), i / (len(heights) - 1)) + (255,))
    return img


DRAWERS = {
    "explorer": draw_explorer,
    "terminal": draw_terminal,
    "browser": draw_browser,
    "editor": draw_editor,
    "music": draw_music,
    "disk": draw_disk,
    "taskman": draw_taskman,
    "settings": draw_settings,
    "app": draw_app,
    "folder": draw_folder,
    "file": draw_file,
    "wav": draw_wav,
}


def render(fn):
    big = fn()
    return big.resize((SIZE, SIZE), Image.Resampling.BOX)


def write_ico(path, img):
    """Single-entry 64x64 32bpp BI_RGB .ico (what ic_ico_parse reads)."""
    px = img.tobytes()
    dib = struct.pack("<IIIHHIIIIII", 40, SIZE, SIZE * 2, 1, 32, 0,
                      SIZE * SIZE * 4, 0, 0, 0, 0)
    body = bytearray()
    for y in range(SIZE - 1, -1, -1):  
        for x in range(SIZE):
            o = (y * SIZE + x) * 4
            r, g, b, a = px[o:o + 4]
            body += bytes((b, g, r, a))
    body += bytes(SIZE * SIZE // 8)  
    data = dib + bytes(body)
    hdr = struct.pack("<HHH", 0, 1, 1)
    entry = struct.pack("<BBBBHHII", SIZE, SIZE, 0, 0, 1, 32, len(data), 6 + 16)
    with open(path, "wb") as f:
        f.write(hdr + entry + data)


def write_header(path, images):
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
        for name in sorted(images):
            px = images[name].tobytes()
            f.write("static const uint8_t icon_%s_rgba[] = {\n" % name)
            for i in range(0, len(px), 32):
                f.write("    " + ",".join(str(b) for b in px[i:i + 32]) + ",\n")
            f.write("};\n\n")
        f.write("static const ic_builtin_icon_entry_t ic_builtin_icons[] = {\n")
        for name in sorted(images):
            f.write('    { "%s", %d, %d, icon_%s_rgba },\n' % (name, SIZE, SIZE, name))
        f.write("};\n\n#define IC_BUILTIN_ICON_COUNT %d\n\n"
                "#endif /* USERSPACE_ICON_DATA_H */\n" % len(images))


def main():
    os.makedirs(ICON_DIR, exist_ok=True)
    images = {name: render(fn) for name, fn in sorted(DRAWERS.items())}
    keep = set(n + ".ico" for n in images)
    for stale in os.listdir(ICON_DIR):
        if stale.endswith(".ico") and stale not in keep:
            os.remove(os.path.join(ICON_DIR, stale))
    for name, img in images.items():
        write_ico(os.path.join(ICON_DIR, name + ".ico"), img)
    write_header(HEADER_PATH, images)
    if len(sys.argv) > 1 and sys.argv[1] == "--sheet":
        sheet = Image.new("RGBA", (SIZE * len(images) + 8 * (len(images) + 1), SIZE * 2 + 24),
                          (40, 40, 46, 255))
        light = Image.new("RGBA", (sheet.width, SIZE + 12), (236, 236, 240, 255))
        sheet.paste(light, (0, SIZE + 12))
        for i, name in enumerate(sorted(images)):
            x = 8 + i * (SIZE + 8)
            sheet.alpha_composite(images[name], (x, 6))
            sheet.alpha_composite(images[name], (x, SIZE + 18))
        sheet.save(sys.argv[2])
    print("icons: %d -> %s, %s" % (len(images), ICON_DIR, HEADER_PATH))
    return 0


if __name__ == "__main__":
    sys.exit(main())
