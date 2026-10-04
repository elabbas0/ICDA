"""gen_boot_logo.py - render the boot wordmark into kernel/diag/boot_logo.h.

The kernel splash and the window manager's startup fade both draw this
alpha mask, so the handoff between them lines up pixel for pixel.

Requires Pillow:  python3 scripts/gen_boot_logo.py
"""
import os

from PIL import Image, ImageDraw, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(REPO, "resources", "fonts", "InterDisplay-Bold.ttf")
OUT = os.path.join(REPO, "kernel", "diag", "boot_logo.h")
TEXT = "ICDA"
SIZE = 54
TRACKING = 6
OVERSAMPLE = 8


def render():
    s = OVERSAMPLE
    font = ImageFont.truetype(FONT, SIZE * s)
    widths = [font.getlength(ch) for ch in TEXT]
    total = int(sum(widths) + TRACKING * s * (len(TEXT) - 1)) + 8 * s
    height = SIZE * 2 * s
    im = Image.new("L", (total, height), 0)
    draw = ImageDraw.Draw(im)
    x = 4 * s
    for ch, w in zip(TEXT, widths):
        draw.text((x, SIZE * s * 1.5), ch, font=font, fill=255, anchor="ls")
        x += w + TRACKING * s
    small = im.reduce(s)
    return small.crop(small.getbbox())


def main():
    mask = render()
    w, h = mask.size
    data = mask.tobytes()
    lines = [
        "#ifndef BOOT_LOGO_H",
        "#define BOOT_LOGO_H",
        "",
        "#define BOOT_LOGO_W %d" % w,
        "#define BOOT_LOGO_H_PX %d" % h,
        "",
        "static const unsigned char boot_logo_alpha[BOOT_LOGO_W * BOOT_LOGO_H_PX] = {",
    ]
    for i in range(0, len(data), 24):
        lines.append("    " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines += ["};", "", "#endif", ""]
    with open(OUT, "w") as f:
        f.write("\n".join(lines))
    print("wrote %s (%dx%d)" % (OUT, w, h))


if __name__ == "__main__":
    main()
