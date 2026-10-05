/* Surfer painting: rasterizes a layout into an ARGB buffer at device
 * resolution, only touching the visible part of the page. */
#include "paint.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const paint_target_t *t;
    int   cx0, cy0, cx1, cy1;      /* current clip, device px */
    float alpha;                   /* opacity multiplier */
    float view_top, view_bottom;   /* visible document range */
} pctx_t;

typedef struct {
    box_t *box;
    float  ox, oy;
    int    z;
    int    order;
} deferred_t;

static deferred_t *deferred;
static int ndeferred, capdeferred;

/* ---- pixels ------------------------------------------------------------- */

static uint32_t blend(uint32_t dst, uint32_t src, uint32_t a) {
    uint32_t inv = 255 - a;
    uint32_t rb = (((src & 0xFF00FF) * a + (dst & 0xFF00FF) * inv) >> 8) & 0xFF00FF;
    uint32_t g = (((src & 0x00FF00) * a + (dst & 0x00FF00) * inv) >> 8) & 0x00FF00;
    return 0xFF000000U | rb | g;
}

static void fill(pctx_t *c, float fx, float fy, float fw, float fh, uint32_t color) {
    const paint_target_t *t = c->t;
    int x0 = t->x + (int)(fx * t->scale + 0.5f);
    int y0 = t->y + (int)((fy - t->scroll_y) * t->scale + 0.5f);
    int x1 = t->x + (int)((fx + fw) * t->scale + 0.5f);
    int y1 = t->y + (int)((fy + fh - t->scroll_y) * t->scale + 0.5f);
    uint32_t a = (uint32_t)((color >> 24) * c->alpha);
    if (a == 0) return;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = t->px + (size_t)y * (size_t)t->stride;
        if (a >= 255) {
            uint32_t v = 0xFF000000U | color;
            for (int x = x0; x < x1; x++) row[x] = v;
        } else {
            for (int x = x0; x < x1; x++) row[x] = blend(row[x], color, a);
        }
    }
}

/* Filled rectangle with rounded corners (radius in CSS px). */
static void fill_round(pctx_t *c, float fx, float fy, float fw, float fh, float r, uint32_t color) {
    const paint_target_t *t = c->t;
    float rd;
    int x0, y0, x1, y1;
    uint32_t a0 = (uint32_t)((color >> 24) * c->alpha);
    if (r <= 0.5f) {
        fill(c, fx, fy, fw, fh, color);
        return;
    }
    if (r > fw / 2) r = fw / 2;
    if (r > fh / 2) r = fh / 2;
    rd = r * t->scale;
    x0 = t->x + (int)(fx * t->scale + 0.5f);
    y0 = t->y + (int)((fy - t->scroll_y) * t->scale + 0.5f);
    x1 = t->x + (int)((fx + fw) * t->scale + 0.5f);
    y1 = t->y + (int)((fy + fh - t->scroll_y) * t->scale + 0.5f);
    for (int y = y0 > c->cy0 ? y0 : c->cy0; y < y1 && y < c->cy1; y++) {
        uint32_t *row = t->px + (size_t)y * (size_t)t->stride;
        float py = y + 0.5f, dy = 0;
        if (py < y0 + rd) dy = y0 + rd - py;
        else if (py > y1 - rd) dy = py - (y1 - rd);
        for (int x = x0 > c->cx0 ? x0 : c->cx0; x < x1 && x < c->cx1; x++) {
            float px = x + 0.5f, dx = 0, cover = 1;
            if (px < x0 + rd) dx = x0 + rd - px;
            else if (px > x1 - rd) dx = px - (x1 - rd);
            if (dx > 0 && dy > 0) {
                float d2 = dx * dx + dy * dy, edge = rd - 0.5f;
                if (d2 > (rd + 0.5f) * (rd + 0.5f)) continue;
                if (d2 > edge * edge) {
                    /* one-pixel ramp: cheap anti-aliasing */
                    float d = (d2 - edge * edge) / ((rd + 0.5f) * (rd + 0.5f) - edge * edge);
                    cover = 1 - d;
                }
            }
            row[x] = blend(row[x], color, (uint32_t)(a0 * cover));
        }
    }
}

static void draw_text(pctx_t *c, const font_t *f, float x, float baseline, const char *s, size_t n, uint32_t color) {
    const paint_target_t *t = c->t;
    const char *end = s + n;
    float pen = x * t->scale + t->x;
    int by = t->y + (int)((baseline - t->scroll_y) * t->scale + 0.5f);
    uint32_t a = (uint32_t)((color >> 24) * c->alpha);
    if (!a) return;
    while (s < end) {
        uint32_t cp = utf8_next(&s, end);
        const glyph_t *g;
        float adv = font_advance(f, cp) * t->scale;
        if (cp == ' ' || cp == 0xA0) {
            pen += adv;
            continue;
        }
        g = font_glyph(f, cp, t->scale);
        if (g && g->alpha) {
            int gx = (int)(pen + 0.5f) + g->xoff, gy = by + g->yoff;
            for (int yy = 0; yy < g->h; yy++) {
                int py = gy + yy;
                uint32_t *row;
                const uint8_t *src;
                if (py < c->cy0 || py >= c->cy1) continue;
                row = t->px + (size_t)py * (size_t)t->stride;
                src = g->alpha + (size_t)yy * (size_t)g->w;
                for (int xx = 0; xx < g->w; xx++) {
                    int px = gx + xx;
                    uint32_t ga = src[xx];
                    if (!ga || px < c->cx0 || px >= c->cx1) continue;
                    row[px] = blend(row[px], color, ga * a / 255);
                }
            }
        }
        pen += adv;
        if (pen > c->cx1 + 50) break;
    }
}

static void draw_image(pctx_t *c, const image_t *img, float fx, float fy, float fw, float fh) {
    const paint_target_t *t = c->t;
    int x0 = t->x + (int)(fx * t->scale + 0.5f), y0 = t->y + (int)((fy - t->scroll_y) * t->scale + 0.5f);
    int w = (int)(fw * t->scale + 0.5f), h = (int)(fh * t->scale + 0.5f);
    if (!img || !img->argb || w <= 0 || h <= 0) return;
    for (int y = y0 > c->cy0 ? y0 : c->cy0; y < y0 + h && y < c->cy1; y++) {
        uint32_t *row = t->px + (size_t)y * (size_t)t->stride;
        int sy = (int)((int64_t)(y - y0) * img->h / h);
        const uint32_t *src = img->argb + (size_t)sy * (size_t)img->w;
        for (int x = x0 > c->cx0 ? x0 : c->cx0; x < x0 + w && x < c->cx1; x++) {
            uint32_t p = src[(int64_t)(x - x0) * img->w / w];
            uint32_t a = (uint32_t)((p >> 24) * c->alpha);
            if (a >= 255) row[x] = p | 0xFF000000U;
            else if (a) row[x] = blend(row[x], p, a);
        }
    }
}

/* ---- boxes -------------------------------------------------------------- */

static float hframe_get(const box_t *b) {
    return b->bl + b->br + b->pl + b->pr;
}

static float vframe_get(const box_t *b) {
    return b->bt + b->bb + b->pt + b->pb;
}

static void defer(box_t *b, float ox, float oy) {
    if (ndeferred == capdeferred) {
        capdeferred = capdeferred ? capdeferred * 2 : 64;
        deferred = (deferred_t *)realloc(deferred, sizeof(deferred_t) * (size_t)capdeferred);
        if (!deferred) {
            ndeferred = capdeferred = 0;
            return;
        }
    }
    deferred[ndeferred].box = b;
    deferred[ndeferred].ox = ox;
    deferred[ndeferred].oy = oy;
    deferred[ndeferred].z = b->st && !b->st->z_auto ? b->st->z_index : 0;
    deferred[ndeferred].order = ndeferred;
    ndeferred++;
}

static void paint_borders(pctx_t *c, box_t *b, float x, float y) {
    const css_style_t *s = b->st;
    float w = b->w, h = b->h;
    if (b->bt > 0) fill(c, x, y, w, b->bt, s->border_color[0]);
    if (b->bb > 0) fill(c, x, y + h - b->bb, w, b->bb, s->border_color[2]);
    if (b->bl > 0) fill(c, x, y + b->bt, b->bl, h - b->bt - b->bb, s->border_color[3]);
    if (b->br > 0) fill(c, x + w - b->br, y + b->bt, b->br, h - b->bt - b->bb, s->border_color[1]);
}

static void paint_control(pctx_t *c, box_t *b, float x, float y) {
    dom_node_t *n = b->node;
    const css_style_t *s = b->st;
    const char *label = 0;
    font_t f = font_pick(s->font_family, s->font_weight, 0, s->font_size);
    uint32_t color = s->color;
    if (n->tag == T_INPUT) {
        const char *type = dom_attr(n, "type");
        if (type && (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0)) {
            fill_round(c, x, y, b->w, b->h, strcmp(type, "radio") == 0 ? b->w / 2 : 3, 0xFFFFFFFFU);
            if (dom_attr(n, "checked")) fill_round(c, x + 3, y + 3, b->w - 6, b->h - 6, strcmp(type, "radio") == 0 ? b->w / 2 : 2, 0xFF0B57D0U);
            return;
        }
        label = dom_attr(n, "value");
        if (!label || !*label) {
            label = dom_attr(n, "placeholder");
            color = 0xFF888888U;
        }
        if (type && (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0) && !label) label = "Submit";
        if (type && strcmp(type, "password") == 0 && label && color != 0xFF888888U) label = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
    } else if (n->tag == T_SELECT) {
        for (dom_node_t *o = n->first; o; o = o->next) {
            if (o->type == N_ELEMENT && o->tag == T_OPTION && o->first && o->first->type == N_TEXT) {
                label = o->first->text;
                break;
            }
        }
    } else if (n->tag == T_TEXTAREA) {
        label = n->first && n->first->type == N_TEXT ? n->first->text : dom_attr(n, "placeholder");
    }
    if (label) {
        size_t len = strlen(label);
        const char *nl = memchr(label, '\n', len);
        if (nl) len = (size_t)(nl - label);
        if (len > 200) len = 200;
        draw_text(c, &f, x + b->bl + 4, y + b->h / 2 + (f.ascent + f.descent) / 2, label, len, color);
    }
}

static void paint_box(pctx_t *c, box_t *b, float ox, float oy, int in_deferred);

static void paint_children(pctx_t *c, box_t *b, float ox, float oy) {
    for (box_t *k = b->first; k; k = k->next) paint_box(c, k, ox, oy, 0);
    for (frag_t *f = b->frags; f; f = f->next) {
        float fy = f->y + oy;
        if (fy > c->view_bottom || fy + f->h < c->view_top) continue;
        if (f->inline_bg && (f->inline_bg->bg_color >> 24)) {
            float pad = f->inline_bg->padding[1].unit == U_PX ? f->inline_bg->padding[1].v : 0;
            fill_round(c, f->x + ox - pad * 0.5f, fy - 1, f->w + pad, f->h + 2, f->inline_bg->radius > 0 ? f->inline_bg->radius : 0,
                       f->inline_bg->bg_color);
        }
        draw_text(c, &f->font, f->x + ox, f->baseline + oy, f->text, f->len, f->color);
        if (f->decoration & TD_UNDERLINE) {
            float th = f->font.size / 14 > 1 ? f->font.size / 14 : 1;
            fill(c, f->x + ox, f->baseline + oy + f->font.size * 0.12f, f->w, th, f->color);
        }
        if (f->decoration & TD_LINE_THROUGH) {
            fill(c, f->x + ox, f->baseline + oy - f->font.size * 0.3f, f->w, f->font.size / 14 > 1 ? f->font.size / 14 : 1, f->color);
        }
    }
}

static void paint_box(pctx_t *c, box_t *b, float ox, float oy, int in_deferred) {
    const css_style_t *s = b->st;
    float x, y;
    int saved[4];
    float saved_alpha = c->alpha;
    if (b->positioned && !in_deferred) {
        defer(b, ox, oy);
        return;
    }
    if (!s) {
        paint_children(c, b, ox, oy);
        return;
    }
    if (s->opacity <= 0.01f) return;
    ox += b->rel_x;
    oy += b->rel_y;
    x = b->x + ox;
    y = b->y + oy;
    c->alpha *= s->opacity;
    if (s->visibility && y < c->view_bottom && y + b->h > c->view_top) {
        if ((s->bg_color >> 24) && b->node && b->node->tag != T_HTML && b->node->tag != T_BODY) {
            fill_round(c, x, y, b->w, b->h, s->radius >= 0 ? s->radius : b->w * -s->radius / 100, s->bg_color);
        }
        if (b->image && b->kind == BX_REPLACED) {
            draw_image(c, (const image_t *)b->image, x + b->bl + b->pl, y + b->bt + b->pt, b->w - hframe_get(b), b->h - vframe_get(b));
        }
        paint_borders(c, b, x, y);
        if (b->kind == BX_REPLACED && b->node && (b->node->tag == T_INPUT || b->node->tag == T_SELECT ||
                                                  b->node->tag == T_TEXTAREA)) {
            paint_control(c, b, x, y);
        }
    }
    memcpy(saved, &c->cx0, sizeof(saved));
    if (s->overflow != OV_VISIBLE && b->node && b->node->tag != T_HTML && b->node->tag != T_BODY) {
        const paint_target_t *t = c->t;
        int x0 = t->x + (int)((x + b->bl) * t->scale), y0 = t->y + (int)((y + b->bt - t->scroll_y) * t->scale);
        int x1 = t->x + (int)((x + b->w - b->br) * t->scale), y1 = t->y + (int)((y + b->h - b->bb - t->scroll_y) * t->scale);
        if (x0 > c->cx0) c->cx0 = x0;
        if (y0 > c->cy0) c->cy0 = y0;
        if (x1 < c->cx1) c->cx1 = x1;
        if (y1 < c->cy1) c->cy1 = y1;
    }
    if (c->cx0 < c->cx1 && c->cy0 < c->cy1) paint_children(c, b, ox, oy);
    memcpy(&c->cx0, saved, sizeof(saved));
    c->alpha = saved_alpha;
}

static int deferred_cmp(const void *a, const void *b) {
    const deferred_t *x = (const deferred_t *)a, *y = (const deferred_t *)b;
    if (x->z != y->z) return x->z < y->z ? -1 : 1;
    return x->order - y->order;
}

/* The page background comes from <html>, or <body> when html has none. */
static uint32_t canvas_color(dom_doc_t *doc) {
    if (doc->html && doc->html->style && (doc->html->style->bg_color >> 24)) return doc->html->style->bg_color;
    if (doc->body && doc->body->style && (doc->body->style->bg_color >> 24)) return doc->body->style->bg_color;
    return 0xFFFFFFFFU;
}

void paint_layout(layout_t *L, dom_doc_t *doc, const paint_target_t *t) {
    pctx_t c;
    uint32_t bg = canvas_color(doc) | 0xFF000000U;
    memset(&c, 0, sizeof(c));
    c.t = t;
    c.cx0 = t->x;
    c.cy0 = t->y;
    c.cx1 = t->x + t->w;
    c.cy1 = t->y + t->h;
    c.alpha = 1;
    c.view_top = t->scroll_y;
    c.view_bottom = t->scroll_y + t->h / t->scale;
    for (int y = c.cy0; y < c.cy1; y++) {
        uint32_t *row = t->px + (size_t)y * (size_t)t->stride;
        for (int x = c.cx0; x < c.cx1; x++) row[x] = bg;
    }
    if (!L || !L->root) return;
    ndeferred = 0;
    paint_box(&c, L->root, 0, 0, 0);
    /* positioned boxes on top, by z-index; fixed ones stay put while scrolling */
    for (int pass = 0; pass < 4 && ndeferred; pass++) {
        int n = ndeferred;
        deferred_t *list = (deferred_t *)malloc(sizeof(deferred_t) * (size_t)n);
        if (!list) break;
        memcpy(list, deferred, sizeof(deferred_t) * (size_t)n);
        ndeferred = 0;
        qsort(list, (size_t)n, sizeof(deferred_t), deferred_cmp);
        for (int i = 0; i < n; i++) {
            float oy = list[i].oy;
            if (list[i].box->positioned == 2) oy += t->scroll_y;
            {
                float vt = c.view_top, vb = c.view_bottom;
                paint_box(&c, list[i].box, list[i].ox, oy, 1);
                c.view_top = vt;
                c.view_bottom = vb;
            }
        }
        free(list);
    }
}
