
"""gen_fonts.py - build ICDA's UI font atlases (userspace/ic_fonts_gen.h).

The type system (see docs/DESIGN.md) uses Inter for UI text and
JetBrains Mono for code/terminal text; both are SIL OFL 1.1 and live in
resources/fonts with their licences.

Glyphs are rasterised *unhinted*: each glyph is drawn at OVERSAMPLE x the
target size and box-filtered down, which yields true outline coverage
(the soft, shape-faithful rendering macOS uses) instead of FreeType's
stem-snapped hinted output.  Every glyph is rendered at SUBPIXEL
horizontal phases so text can be laid out with fractional advances and
kerning without uneven letter spacing.

Kerning comes from the GPOS 'kern' feature (pair adjustment, formats 1
and 2) and is stored once per font file in font units; the runtime
scales it to each face's pixel size.

Requires Pillow (with FreeType) and fontTools:
    python3 -m pip install pillow fonttools
    python3 scripts/gen_fonts.py
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_DIR = os.path.join(REPO, "resources", "fonts")
OUT = os.path.join(REPO, "userspace", "ic_fonts_gen.h")

OVERSAMPLE = 8
SUBPIXEL = 2            
KERN_MIN_UNITS = 12     


EXTRA_CPS = [0x2026, 0x2022, 0x2014, 0x2013, 0x00B7, 0x00A9, 0x00B0, 0x2019]
CODEPOINTS = list(range(32, 127)) + EXTRA_CPS

FILES = {
    "inter_regular":  "Inter-Regular.ttf",
    "inter_medium":   "Inter-Medium.ttf",
    "inter_semibold": "Inter-SemiBold.ttf",
    "display_semibold": "InterDisplay-SemiBold.ttf",
    "display_bold":   "InterDisplay-Bold.ttf",
    "mono_regular":   "JetBrainsMono-Regular.ttf",
}


FACES = [
    ("CAPTION",     "inter_regular",    11),
    ("CAPTION_EMPH", "inter_medium",    11),
    ("FOOTNOTE",    "inter_regular",    12),
    ("BODY",        "inter_regular",    13),
    ("BODY_EMPH",   "inter_medium",     13),
    ("HEADLINE",    "inter_semibold",   13),
    ("SUBHEAD",     "inter_regular",    15),
    ("TITLE3",      "inter_semibold",   15),
    ("TITLE2",      "inter_semibold",   17),
    ("TITLE1",      "display_semibold", 22),
    ("LARGE_TITLE", "display_bold",     28),
    ("MONO",        "mono_regular",     13),
    ("MONO_SMALL",  "mono_regular",     12),
]


def cmap_glyph(tt, cp):
    return tt.getBestCmap().get(cp)


def value_xadv(v):
    return getattr(v, "XAdvance", 0) or 0 if v is not None else 0


def gpos_kern_pairs(tt, glyph_names):
    """{(left, right): units} for the given glyph set, first match wins."""
    if "GPOS" not in tt:
        return {}
    gpos = tt["GPOS"].table
    want = set(glyph_names)
    lookup_ids = []
    for fr in gpos.FeatureList.FeatureRecord:
        if fr.FeatureTag == "kern":
            for li in fr.Feature.LookupListIndex:
                if li not in lookup_ids:
                    lookup_ids.append(li)
    pairs = {}
    for li in sorted(lookup_ids):
        lookup = gpos.LookupList.Lookup[li]
        subtables = []
        for st in lookup.SubTable:
            if lookup.LookupType == 9:
                if st.ExtensionLookupType != 2:
                    continue
                st = st.ExtSubTable
            elif lookup.LookupType != 2:
                continue
            subtables.append(st)
        for st in subtables:
            cov = st.Coverage.glyphs
            if st.Format == 1:
                for i, left in enumerate(cov):
                    if left not in want:
                        continue
                    for pvr in st.PairSet[i].PairValueRecord:
                        right = pvr.SecondGlyph
                        if right in want and (left, right) not in pairs:
                            pairs[(left, right)] = value_xadv(pvr.Value1)
            elif st.Format == 2:
                cd1 = st.ClassDef1.classDefs if st.ClassDef1 else {}
                cd2 = st.ClassDef2.classDefs if st.ClassDef2 else {}
                covset = set(cov)
                for left in want:
                    if left not in covset:
                        continue
                    c1 = cd1.get(left, 0)
                    rec1 = st.Class1Record[c1]
                    for right in want:
                        if (left, right) in pairs:
                            continue
                        c2 = cd2.get(right, 0)
                        v = value_xadv(rec1.Class2Record[c2].Value1)
                        if v:
                            pairs[(left, right)] = v
    return pairs


def render_glyph(pil_font, ch, phase):
    """Coverage mask for ch at a horizontal phase (0..SUBPIXEL-1).

    Returns (w, h, ox, oy, bytes): ox/oy are the offsets of the mask's
    top-left pixel from the pen position on the baseline."""
    s = OVERSAMPLE
    size_px = pil_font.size // s
    pad = 4 * s
    width = (size_px * 3 + 8) * s
    height = (size_px * 3 + 8) * s
    base = pad + size_px * 2 * s
    shift = (phase * s) // SUBPIXEL
    im = Image.new("L", (width, height), 0)
    ImageDraw.Draw(im).text((pad + shift, base), ch, font=pil_font, fill=255, anchor="ls")
    small = im.reduce(s)
    bbox = small.getbbox()
    if not bbox:
        return 0, 0, 0, 0, b""
    x0, y0, x1, y1 = bbox
    crop = small.crop(bbox)
    return (x1 - x0, y1 - y0, x0 - pad // s, y0 - base // s, crop.tobytes())


def build():
    fonts = {}
    for key, fname in FILES.items():
        path = os.path.join(FONT_DIR, fname)
        if not os.path.exists(path):
            sys.exit("gen_fonts: missing %s" % path)
        fonts[key] = TTFont(path)

    out = []
    out.append("/* Generated by scripts/gen_fonts.py - do not edit. */")
    out.append("#ifndef USERSPACE_IC_FONTS_GEN_H")
    out.append("#define USERSPACE_IC_FONTS_GEN_H")
    out.append("")
    out.append('#include "ic_font.h"')
    out.append("")
    out.append("#define IC_FONT_SUBPIXEL %d" % SUBPIXEL)
    out.append("#define IC_FONT_GLYPHS %d" % len(CODEPOINTS))
    out.append("static const uint16_t ic_font_extra_cps[] = { %s };"
               % ", ".join("0x%04X" % c for c in EXTRA_CPS))
    out.append("#define IC_FONT_EXTRA_COUNT %d" % len(EXTRA_CPS))
    out.append("")

    
    kern_names = {}
    for key, tt in fonts.items():
        if key.startswith("mono"):
            continue
        names = [cmap_glyph(tt, cp) for cp in CODEPOINTS]
        index = {n: i for i, n in enumerate(names) if n}
        pairs = gpos_kern_pairs(tt, [n for n in names if n])
        rows = []
        for (l, r), v in sorted(pairs.items(), key=lambda kv: (index[kv[0][0]], index[kv[0][1]])):
            if abs(v) < KERN_MIN_UNITS:
                continue
            rows.append((index[l], index[r], v))
        sym = "ic_kern_%s" % key
        kern_names[key] = (sym, len(rows))
        out.append("static const ic_fkern_t %s[] = {" % sym)
        line = []
        for l, r, v in rows:
            line.append("{%d,%d,%d}" % (l, r, v))
            if len(line) == 8:
                out.append("    " + ",".join(line) + ",")
                line = []
        if line:
            out.append("    " + ",".join(line) + ",")
        if not rows:
            out.append("    {0,0,0}")
        out.append("};")
        out.append("")

    face_syms = []
    total_alpha = 0
    for enum, key, px in FACES:
        tt = fonts[key]
        upem = tt["head"].unitsPerEm
        hhea = tt["hhea"]
        os2 = tt["OS/2"]
        hmtx = tt["hmtx"]
        pil = ImageFont.truetype(os.path.join(FONT_DIR, FILES[key]), px * OVERSAMPLE)
        asc = round(hhea.ascent * px / upem)
        desc = round(-hhea.descent * px / upem)
        line_h = round((hhea.ascent - hhea.descent + hhea.lineGap) * px / upem)
        cap_h = round(os2.sCapHeight * px / upem)
        x_h = round(os2.sxHeight * px / upem)
        alpha = bytearray()
        glyph_rows = []
        adv_rows = []
        for cp in CODEPOINTS:
            gname = cmap_glyph(tt, cp)
            if gname is None:
                gname = cmap_glyph(tt, ord("?"))
                ch = "?"
            else:
                ch = chr(cp)
            adv_rows.append(round(hmtx[gname][0] * px * 64 / upem))
            for phase in range(SUBPIXEL):
                w, h, ox, oy, data = render_glyph(pil, ch, phase)
                glyph_rows.append("{%d,%d,%d,%d,%d}" % (w, h, ox, oy, len(alpha)))
                alpha += data
        total_alpha += len(alpha)
        low = enum.lower()
        out.append("static const uint8_t ic_face_%s_alpha[] = {" % low)
        for i in range(0, len(alpha), 24):
            out.append("    " + ",".join(str(b) for b in alpha[i:i + 24]) + ",")
        out.append("};")
        out.append("static const uint16_t ic_face_%s_adv[] = {" % low)
        for i in range(0, len(adv_rows), 16):
            out.append("    " + ",".join(str(a) for a in adv_rows[i:i + 16]) + ",")
        out.append("};")
        out.append("static const ic_fglyph_t ic_face_%s_glyphs[] = {" % low)
        for i in range(0, len(glyph_rows), 6):
            out.append("    " + ",".join(glyph_rows[i:i + 6]) + ",")
        out.append("};")
        ksym, kcount = kern_names.get(key, ("0", 0))
        out.append("static const ic_face_t ic_face_%s = {" % low)
        out.append("    %d, %d, %d, %d, %d, %d, %d, ic_face_%s_adv, ic_face_%s_glyphs, ic_face_%s_alpha, %s, %d" %
                   (px, asc, desc, line_h, cap_h, x_h, upem, low, low, low, ksym, kcount))
        out.append("};")
        out.append("")
        face_syms.append("&ic_face_%s" % low)

    out.append("static const ic_face_t *const ic_faces[IC_FONT_STYLE_COUNT] = {")
    for s in face_syms:
        out.append("    %s," % s)
    out.append("};")
    out.append("")
    out.append("#endif /* USERSPACE_IC_FONTS_GEN_H */")
    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s (%d faces, %d KiB glyph coverage)" % (OUT, len(FACES), total_alpha // 1024))



BLOB_OUT = os.path.join(REPO, "resources", "fonts", "ui-2x.icf")


def kern_rows(tt):
    names = [cmap_glyph(tt, cp) for cp in CODEPOINTS]
    index = {n: i for i, n in enumerate(names) if n}
    pairs = gpos_kern_pairs(tt, [n for n in names if n])
    rows = []
    for (l, r), v in sorted(pairs.items(), key=lambda kv: (index[kv[0][0]], index[kv[0][1]])):
        if abs(v) >= KERN_MIN_UNITS:
            rows.append((index[l], index[r], v))
    return rows


def build_blob(scale):
    """Write the scale-x atlases as one binary file (ui-2x.icf).

    Layout (little endian, every section 4-byte aligned):
      "ICF2", u32 version, u32 faces, u32 glyphs, u32 subpixel, u32 0
      faces x 32 bytes: u8 px, ascent, descent, line_h, cap_h, x_h,
        u16 upem, u16 nkern, u16 0, u32 adv, glyphs, kern, alpha offsets, u32 0
      then per face: u16 adv[glyphs], {u8 w,h; i8 ox,oy; u32 off}[glyphs*subpixel],
        {u8 l,r; i16 units}[nkern], u8 alpha[]
    The records match ic_fglyph_t / ic_fkern_t so the runtime points into
    the loaded file."""
    import struct
    fonts = {key: TTFont(os.path.join(FONT_DIR, fname)) for key, fname in FILES.items()}
    kerns = {key: ([] if key.startswith("mono") else kern_rows(tt)) for key, tt in fonts.items()}
    head = bytearray(b"ICF2" + struct.pack("<5I", 1, len(FACES), len(CODEPOINTS), SUBPIXEL, 0))
    table_off = len(head)
    body = bytearray()
    base = table_off + 32 * len(FACES)
    table = bytearray()

    def align():
        while (base + len(body)) % 4:
            body.append(0)

    for enum, key, px in FACES:
        tt = fonts[key]
        px *= scale
        upem = tt["head"].unitsPerEm
        hhea = tt["hhea"]
        os2 = tt["OS/2"]
        hmtx = tt["hmtx"]
        pil = ImageFont.truetype(os.path.join(FONT_DIR, FILES[key]), px * OVERSAMPLE)
        asc = round(hhea.ascent * px / upem)
        desc = round(-hhea.descent * px / upem)
        line_h = round((hhea.ascent - hhea.descent + hhea.lineGap) * px / upem)
        cap_h = round(os2.sCapHeight * px / upem)
        x_h = round(os2.sxHeight * px / upem)
        adv = bytearray()
        glyphs = bytearray()
        alpha = bytearray()
        for cp in CODEPOINTS:
            gname = cmap_glyph(tt, cp)
            ch = chr(cp)
            if gname is None:
                gname = cmap_glyph(tt, ord("?"))
                ch = "?"
            adv += struct.pack("<H", round(hmtx[gname][0] * px * 64 / upem))
            for phase in range(SUBPIXEL):
                w, h, ox, oy, data = render_glyph(pil, ch, phase)
                glyphs += struct.pack("<BBbbI", w, h, ox, oy, len(alpha))
                alpha += data
        kern = b"".join(struct.pack("<BBh", l, r, v) for l, r, v in kerns[key])
        offs = []
        for part in (adv, glyphs, kern, alpha):
            align()
            offs.append(base + len(body))
            body += part
        table += struct.pack("<6BHHH4II", px, asc, desc, line_h, cap_h, x_h, upem,
                             len(kerns[key]), 0, *offs, 0)
    with open(BLOB_OUT, "wb") as f:
        f.write(head + table + body)
    print("wrote %s (%d KiB)" % (BLOB_OUT, (len(head) + len(table) + len(body)) // 1024))


if __name__ == "__main__":
    if "--blob2x" in sys.argv:
        build_blob(2)
    else:
        build()
