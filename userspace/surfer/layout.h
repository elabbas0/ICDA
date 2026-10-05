#ifndef SURFER_LAYOUT_H
#define SURFER_LAYOUT_H

#include <stdint.h>
#include "dom.h"
#include "css.h"
#include "font.h"

/* Layout: turns the styled DOM into positioned boxes and text fragments,
 * in CSS pixels with document coordinates (y grows down the page). */

typedef struct box box_t;

typedef struct frag {
    float        x, y, w, h;       /* text: y is the line top, baseline = y + ascent_off */
    float        baseline;
    const char  *text;
    uint16_t     len;
    font_t       font;
    uint32_t     color;
    uint8_t      decoration;
    uint8_t      kind;              /* 0 text, 1 marker */
    dom_node_t  *link;              /* enclosing <a href>, for clicks */
    const css_style_t *inline_bg;   /* innermost inline ancestor with a background */
    struct frag *next;
} frag_t;

enum { BX_BLOCK = 0, BX_INLINE_BLOCK, BX_REPLACED, BX_TABLE, BX_FLEX, BX_GRID, BX_ROOT };

struct box {
    uint8_t      kind;
    uint8_t      positioned;        /* absolute / fixed, painted in a later pass */
    dom_node_t  *node;
    const css_style_t *st;
    float        x, y, w, h;        /* border box, document coordinates */
    float        mt, mr, mb, ml;    /* used margins */
    float        bt, br, bb, bl;    /* borders */
    float        pt, pr, pb, pl;    /* padding */
    float        rel_x, rel_y;      /* position: relative offset (applied when painting) */
    box_t       *parent, *first, *last, *next;
    frag_t      *frags, *frags_tail;
    dom_node_t  *link;
    void        *image;             /* decoded image for <img> / background */
};

typedef struct {
    arena_t   arena;
    box_t    *root;
    float     width, height;        /* document size */
    float     viewport_w, viewport_h;
} layout_t;

/* Callback the layout uses to ask for an image's intrinsic size (0 if not
 * loaded yet); images are fetched by the browser. */
typedef int (*image_size_fn)(dom_node_t *img, void *ctx, int *w, int *h, void **image);

layout_t *layout_document(dom_doc_t *doc, float viewport_w, float viewport_h, image_size_fn img, void *img_ctx);
void      layout_free(layout_t *l);
/* The innermost link at document point (x, y), or 0. */
dom_node_t *layout_hit_link(layout_t *l, float x, float y);

#endif
