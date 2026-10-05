#ifndef SURFER_FONT_H
#define SURFER_FONT_H

#include <stdint.h>
#include <stddef.h>

/* Scalable fonts for Surfer (stb_truetype): a few bundled faces, measured
 * with cached advance widths and drawn from a glyph bitmap cache. */

enum { FACE_SANS = 0, FACE_SANS_BOLD, FACE_MONO, FACE_COUNT };

typedef struct {
    int   face;
    float size;        /* CSS px */
    int   italic;
    float ascent, descent, line_gap;   /* CSS px, descent negative */
} font_t;

typedef struct {
    int      w, h, xoff, yoff;         /* device px */
    uint8_t *alpha;
} glyph_t;

int    font_init(void);
/* Picks a face for a CSS family / weight; never fails. */
font_t font_pick(const char *family, int weight, int italic, float size);
float  font_advance(const font_t *f, uint32_t cp);          /* CSS px */
float  font_text_width(const font_t *f, const char *s, size_t n);
/* Glyph rendered at size * scale device pixels. */
const glyph_t *font_glyph(const font_t *f, uint32_t cp, float scale);
uint32_t utf8_next(const char **s, const char *end);

#endif
