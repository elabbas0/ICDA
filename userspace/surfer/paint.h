#ifndef SURFER_PAINT_H
#define SURFER_PAINT_H

#include <stdint.h>
#include "layout.h"

typedef struct {
    uint32_t *px;
    int       stride;          /* pixels per row */
    int       x, y, w, h;      /* destination rectangle (device px) */
    float     scale;           /* device px per CSS px */
    float     scroll_y;        /* CSS px */
    struct dom_node *focus;    /* focused form control, drawn with a ring and caret */
    int       caret_on;        /* caret phase (blinks) */
} paint_target_t;

/* Images are decoded by the browser; the painter only blits them. */
typedef struct {
    int       w, h;
    uint32_t *argb;
} image_t;

void paint_layout(layout_t *L, dom_doc_t *doc, const paint_target_t *t);

#endif
