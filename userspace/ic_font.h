/*
 * ic_font.h - the ICDA type system.
 *
 * Text is set in named styles (IC_FONT_BODY, IC_FONT_HEADLINE, ...),
 * never in ad-hoc sizes, so every surface shares one typographic scale.
 * Faces are pre-rasterised by scripts/gen_fonts.py (Inter for UI,
 * JetBrains Mono for code) with fractional advances, GPOS kerning and
 * subpixel-positioned glyphs.
 *
 * Strings are UTF-8.  Printable ASCII plus a few typographic marks
 * (… • — – · © ° ’) are available; anything else renders as '?'.
 *
 * Vertical positions are baselines.  ic_text_draw_in() centres text
 * optically (on the cap height) inside a rect, which is what controls
 * and rows should use.
 */
#ifndef USERSPACE_IC_FONT_H
#define USERSPACE_IC_FONT_H

#include <stdint.h>
#include "ic_gfx.h"

typedef enum {
    IC_FONT_CAPTION = 0,   /* 11 regular  - secondary metadata */
    IC_FONT_CAPTION_EMPH,  /* 11 medium   - badges, section labels */
    IC_FONT_FOOTNOTE,      /* 12 regular  - hints, status text */
    IC_FONT_BODY,          /* 13 regular  - default UI text */
    IC_FONT_BODY_EMPH,     /* 13 medium   - menu items, emphasised body */
    IC_FONT_HEADLINE,      /* 13 semibold - window titles, row titles */
    IC_FONT_SUBHEAD,       /* 15 regular  - reading text */
    IC_FONT_TITLE3,        /* 15 semibold - group headers */
    IC_FONT_TITLE2,        /* 17 semibold - panel headers */
    IC_FONT_TITLE1,        /* 22 semibold - page titles */
    IC_FONT_LARGE_TITLE,   /* 28 bold     - hero numbers, splash */
    IC_FONT_MONO,          /* 13 mono     - terminal, editor */
    IC_FONT_MONO_SMALL,    /* 12 mono     - dense code */
    IC_FONT_STYLE_COUNT
} ic_font_style_t;

typedef struct {
    uint8_t  w, h;      /* coverage mask size */
    int8_t   ox, oy;    /* mask top-left relative to pen on baseline */
    uint32_t off;       /* offset into the face's alpha array */
} ic_fglyph_t;

typedef struct {
    uint8_t  l, r;      /* glyph indices */
    int16_t  units;     /* x adjustment in font units */
} ic_fkern_t;

typedef struct {
    uint8_t            px;        /* nominal size */
    uint8_t            ascent;
    uint8_t            descent;
    uint8_t            line_h;
    uint8_t            cap_h;
    uint8_t            x_h;
    uint16_t           upem;
    const uint16_t    *adv;       /* advance per glyph, 1/64 px */
    const ic_fglyph_t *glyphs;    /* glyph-major, IC_FONT_SUBPIXEL phases each */
    const uint8_t     *alpha;
    const ic_fkern_t  *kern;      /* sorted by (l, r) */
    uint16_t           nkern;
} ic_face_t;

typedef enum {
    IC_ALIGN_LEFT = 0,
    IC_ALIGN_CENTER,
    IC_ALIGN_RIGHT
} ic_align_t;

const ic_face_t *ic_font(ic_font_style_t style);

/* Advance width of s in pixels (rounded up). */
int  ic_text_measure(const ic_face_t *f, const char *s);
/* Measure only the first n bytes. */
int  ic_text_measure_n(const ic_face_t *f, const char *s, int n);
/* Bytes of s that fit in max_w pixels (whole characters). */
int  ic_text_fit(const ic_face_t *f, const char *s, int max_w);

/* Draw s with its baseline at y; returns the advance in pixels. */
int  ic_text_draw(ic_canvas_t *c, const ic_face_t *f, int x, int y, const char *s,
                  ic_color_t color);
int  ic_text_draw_n(ic_canvas_t *c, const ic_face_t *f, int x, int y, const char *s,
                    int n, ic_color_t color);

/* Baseline that optically centres a line of f inside [y, y + h). */
int  ic_text_center_baseline(const ic_face_t *f, int y, int h);

/* Single line inside r: vertically centred, aligned horizontally, and
 * truncated with an ellipsis when it does not fit. */
void ic_text_draw_in(ic_canvas_t *c, const ic_face_t *f, ic_rect_t r, const char *s,
                     ic_color_t color, ic_align_t align);

/* Word-wrapped paragraph starting with its first line box at r.y.
 * Draws at most as many lines as fit in r.h (r.h <= 0: unlimited) and
 * returns the height used.  Pass c == NULL to only measure. */
int  ic_text_draw_wrapped(ic_canvas_t *c, const ic_face_t *f, ic_rect_t r, const char *s,
                          ic_color_t color, int line_gap);

#endif /* USERSPACE_IC_FONT_H */
