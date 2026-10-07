/* Surfer layout: block and inline formatting with floats, relative /
 * absolute / fixed positioning, flexbox, a simple grid, auto-layout tables,
 * replaced elements and list markers.  Works directly from the styled DOM and
 * produces boxes plus text fragments in document coordinates. */
#include "layout.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Children in layout order: the ::before box, the DOM children, the ::after
 * box.  The pseudo-element boxes are synthetic nodes the cascade hangs off
 * their element (dom_node_t.gen); scripts never see them. */
static dom_node_t *lfirst(const dom_node_t *n) {
    return n->gen[0] ? n->gen[0] : n->first ? n->first : n->gen[1];
}

static dom_node_t *lnext(const dom_node_t *k) {
    const dom_node_t *p = k->parent;
    if (!p) return k->next;
    if (k == p->gen[0]) return p->first ? p->first : p->gen[1];
    if (k == p->gen[1]) return 0;
    return k->next ? k->next : p->gen[1];
}

#define MAX_FLOATS 128
#define MAX_ABS    256

typedef struct {
    float l, r, top, bottom;   /* float margin box */
    int   right;
} flt_t;

typedef struct {
    flt_t f[MAX_FLOATS];
    int   n;
} bfc_t;

typedef struct {
    box_t      *box;
    dom_node_t *node;
    float       static_x, static_y;
} abs_t;

typedef struct {
    layout_t     *L;
    image_size_fn img;
    void         *img_ctx;
    abs_t         abs[MAX_ABS];
    int           nabs;
    int           depth;
    float         pct_h;   /* definite content height of the containing block, -1 if none */
} lctx_t;

/* ---- helpers ------------------------------------------------------------ */

static float res(css_len_t l, float base, float dflt) {
    if (l.unit == U_PX) return l.v;
    if (l.unit == U_PCT) return base >= 0 ? base * l.v / 100 : dflt;
    return dflt;
}

static int is_auto(css_len_t l) {
    return l.unit == U_AUTO;
}

static float clampf(float v, float lo, float hi) {
    if (hi >= 0 && v > hi) v = hi;
    if (v < lo) v = lo;
    return v;
}

static box_t *new_box(lctx_t *c, dom_node_t *n, const css_style_t *st, int kind) {
    box_t *b = (box_t *)arena_alloc(&c->L->arena, sizeof(box_t));
    if (!b) return 0;
    b->kind = (uint8_t)kind;
    b->node = n;
    b->st = st;
    if (st && st->bg_image && c->img && n && n->tag != T_IMG && n->tag != T_SVG) {
        /* for any other element the image callback answers with its background */
        int w = 0, h = 0;
        if (c->img(n, c->img_ctx, &w, &h, &b->bg)) {
            b->bg_w = (float)w;
            b->bg_h = (float)h;
        }
    }
    return b;
}

static void add_child(box_t *parent, box_t *child) {
    child->parent = parent;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static dom_node_t *link_of(dom_node_t *n) {
    for (; n; n = n->parent) {
        if (n->type == N_ELEMENT && n->tag == T_A && dom_attr(n, "href")) return n;
    }
    return 0;
}

static int is_block_level(const css_style_t *st) {
    switch (st->display) {
    case D_BLOCK: case D_LIST_ITEM: case D_TABLE: case D_FLEX: case D_GRID: case D_TABLE_ROW:
    case D_TABLE_ROW_GROUP: case D_TABLE_HEADER_GROUP: case D_TABLE_FOOTER_GROUP: case D_TABLE_CAPTION:
    case D_TABLE_CELL:
        return 1;
    default:
        return 0;
    }
}

static int is_replaced(dom_node_t *n) {
    return n->tag == T_IMG || n->tag == T_INPUT || n->tag == T_TEXTAREA || n->tag == T_SELECT ||
           n->tag == T_BUTTON || n->tag == T_IFRAME || n->tag == T_VIDEO || n->tag == T_CANVAS || n->tag == T_SVG ||
           n->tag == T_HR || n->tag == T_EMBED || n->tag == T_OBJECT;
}

static void edges(box_t *b, float cb_w) {
    const css_style_t *s = b->st;
    b->bt = s->border_w[0];
    b->br = s->border_w[1];
    b->bb = s->border_w[2];
    b->bl = s->border_w[3];
    b->pt = res(s->padding[0], cb_w, 0);
    b->pr = res(s->padding[1], cb_w, 0);
    b->pb = res(s->padding[2], cb_w, 0);
    b->pl = res(s->padding[3], cb_w, 0);
    b->mt = res(s->margin[0], cb_w, 0);
    b->mr = res(s->margin[1], cb_w, 0);
    b->mb = res(s->margin[2], cb_w, 0);
    b->ml = res(s->margin[3], cb_w, 0);
}

static float hframe(const box_t *b) {
    return b->bl + b->br + b->pl + b->pr;
}

static float vframe(const box_t *b) {
    return b->bt + b->bb + b->pt + b->pb;
}

/* ---- floats ------------------------------------------------------------- */

static void line_span(const bfc_t *f, float y, float h, float left, float right, float *ol, float *or_) {
    float l = left, r = right;
    for (int i = 0; f && i < f->n; i++) {
        const flt_t *x = &f->f[i];
        if (x->bottom <= y || x->top >= y + h) continue;
        if (!x->right && x->r > l) l = x->r;
        if (x->right && x->l < r) r = x->l;
    }
    *ol = l;
    *or_ = r;
}

static float clear_y(const bfc_t *f, float y, int which) {
    for (int i = 0; f && i < f->n; i++) {
        const flt_t *x = &f->f[i];
        if (((which & 1) && !x->right) || ((which & 2) && x->right)) {
            if (x->bottom > y) y = x->bottom;
        }
    }
    return y;
}

static float floats_bottom(const bfc_t *f, float from) {
    float b = from;
    for (int i = 0; f && i < f->n; i++) {
        if (f->f[i].bottom > b) b = f->f[i].bottom;
    }
    return b;
}

/* ---- intrinsic sizes ---------------------------------------------------- */

static void text_intrinsic(const char *s, size_t n, const css_style_t *st, float *mn, float *mx) {
    font_t f = font_pick(st->font_family, st->font_weight, st->font_italic, st->font_size);
    float word = 0, line = 0, space = font_advance(&f, ' ');
    const char *end = s + n;
    int in_space = 0, pre = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP;
    int nowrap = st->white_space == WS_NOWRAP || st->white_space == WS_PRE;
    while (s < end) {
        char ch = *s;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (ch == '\n' && pre) {
                if (line > *mx) *mx = line;
                line = 0;
            } else if (!in_space || pre) {
                line += space;
            }
            if (!nowrap) {
                if (word > *mn) *mn = word;
                word = 0;
            } else {
                word += space;
            }
            in_space = 1;
            s++;
            continue;
        }
        in_space = 0;
        {
            float a = font_advance(&f, utf8_next(&s, end));
            word += a;
            line += a;
        }
    }
    if (word > *mn) *mn = word;
    if (line > *mx) *mx = line;
}

static void intrinsic(lctx_t *c, dom_node_t *n, const css_style_t *st, float *mn, float *mx);

static void replaced_size(lctx_t *c, dom_node_t *n, const css_style_t *st, float cb_w, float *w, float *h) {
    int iw = 0, ih = 0;
    void *image = 0;
    float cw = res(st->width, cb_w, -1), ch = res(st->height, c->pct_h, -1);
    if ((n->tag == T_IMG || n->tag == T_SVG) && c->img) c->img(n, c->img_ctx, &iw, &ih, &image);
    if (n->tag == T_INPUT) {
        const char *type = dom_attr(n, "type");
        if (type && (strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0)) { iw = 13; ih = 13; }
        else if (type && (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0 || strcmp(type, "reset") == 0)) {
            const char *v = dom_attr(n, "value");
            font_t f = font_pick(st->font_family, st->font_weight, 0, st->font_size);
            iw = (int)font_text_width(&f, v ? v : "Submit", strlen(v ? v : "Submit")) + 16;
            ih = (int)(st->font_size * 1.4f) + 4;
        } else {
            iw = 160;
            ih = (int)(st->font_size * 1.4f) + 4;
        }
    } else if (n->tag == T_TEXTAREA) { iw = 300; ih = 60; }
    else if (n->tag == T_SELECT) { iw = 120; ih = (int)(st->font_size * 1.4f) + 4; }
    else if (n->tag == T_BUTTON) {
        float mn = 0, mx = 0;
        intrinsic(c, n, st, &mn, &mx);
        iw = (int)mx + 16;
        ih = (int)(st->line_height) + 6;
    } else if (n->tag == T_IFRAME || n->tag == T_VIDEO || n->tag == T_CANVAS || n->tag == T_EMBED || n->tag == T_OBJECT) {
        iw = 300;
        ih = 150;
    } else if (n->tag == T_SVG && !image) {
        iw = 24;
        ih = 24;
    } else if (n->tag == T_HR) {
        iw = (int)(cb_w > 0 ? cb_w : 0);
        ih = 0;
    }
    if (cw >= 0 && ch >= 0) { *w = cw; *h = ch; }
    else if (cw >= 0) { *w = cw; *h = iw > 0 ? cw * ih / iw : ch >= 0 ? ch : (float)ih; }
    else if (ch >= 0) { *h = ch; *w = ih > 0 ? ch * iw / ih : (float)iw; }
    else { *w = (float)iw; *h = (float)ih; }
    {
        float maxw = res(st->max_w, cb_w, -1);
        if (maxw >= 0 && *w > maxw) {
            if (*w > 0) *h = *h * maxw / *w;
            *w = maxw;
        }
    }
    if (st->box_sizing && cw >= 0) {
        float fr = st->padding[1].v + st->padding[3].v + st->border_w[1] + st->border_w[3];
        *w -= fr;
        if (*w < 0) *w = 0;
    }
}

static void intrinsic(lctx_t *c, dom_node_t *n, const css_style_t *st, float *mn, float *mx) {
    float fw = 0;
    *mn = *mx = 0;
    if (st->width.unit == U_PX) {
        float w = st->width.v + (st->box_sizing ? 0 : st->padding[1].v * (st->padding[1].unit == U_PX) +
                                                    st->padding[3].v * (st->padding[3].unit == U_PX) +
                                                    st->border_w[1] + st->border_w[3]);
        *mn = *mx = w;
        return;
    }
    if (is_replaced(n) && n->tag != T_BUTTON) {
        float w, h;
        replaced_size(c, n, st, -1, &w, &h);
        *mn = *mx = w;
        return;
    }
    fw = st->border_w[1] + st->border_w[3] + (st->padding[1].unit == U_PX ? st->padding[1].v : 0) +
         (st->padding[3].unit == U_PX ? st->padding[3].v : 0);
    {
        float line_max = 0, line_min = 0, block_max = 0, block_min = 0;
        int row = st->display == D_FLEX && (st->flex_dir == FD_ROW || st->flex_dir == FD_ROW_REVERSE);
        for (dom_node_t *k = lfirst(n); k; k = lnext(k)) {
            float a = 0, b = 0;
            if (k->type == N_TEXT) {
                text_intrinsic(k->text, k->text_len, st, &a, &b);
                line_max += b;
                if (a > line_min) line_min = a;
                continue;
            }
            if (k->type != N_ELEMENT || !k->style || k->style->display == D_NONE) continue;
            if (k->style->position == P_ABSOLUTE || k->style->position == P_FIXED) continue;
            intrinsic(c, k, k->style, &a, &b);
            a += res(k->style->margin[1], -1, 0) + res(k->style->margin[3], -1, 0);
            b += res(k->style->margin[1], -1, 0) + res(k->style->margin[3], -1, 0);
            if (row) {
                line_max += b;
                if (a > line_min) line_min = a;
            } else if (is_block_level(k->style) && !k->style->float_) {
                if (line_max > block_max) block_max = line_max;
                if (line_min > block_min) block_min = line_min;
                line_max = line_min = 0;
                if (b > block_max) block_max = b;
                if (a > block_min) block_min = a;
            } else {
                line_max += b;
                if (a > line_min) line_min = a;
            }
        }
        if (line_max > block_max) block_max = line_max;
        if (line_min > block_min) block_min = line_min;
        *mn = block_min + fw;
        *mx = block_max + fw;
    }
    if (st->min_w.unit == U_PX && *mn < st->min_w.v) *mn = st->min_w.v;
    if (st->max_w.unit == U_PX && *mx > st->max_w.v) *mx = st->max_w.v;
    if (*mn > *mx) *mx = *mn;
}

/* ---- inline formatting -------------------------------------------------- */

enum { IT_TEXT = 0, IT_ATOMIC, IT_BR, IT_SPACE_OPEN, IT_SPACE_CLOSE };

typedef struct {
    uint8_t            type;
    const char        *s;
    size_t             n;
    const css_style_t *st;
    const css_style_t *bg;
    dom_node_t        *link;
    box_t             *atomic;
    float              space;     /* IT_SPACE_*: inline padding/border/margin */
    dom_node_t        *node;      /* source node: text node for text, element otherwise */
} item_t;

typedef struct {
    item_t *it;
    int     n, cap;
} items_t;

static void push_item(items_t *v, item_t it) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->it = (item_t *)realloc(v->it, sizeof(item_t) * (size_t)v->cap);
        if (!v->it) {
            v->n = v->cap = 0;
            return;
        }
    }
    v->it[v->n++] = it;
}

static box_t *layout_atomic(lctx_t *c, dom_node_t *n, const css_style_t *st, float cb_w);

static void collect_inline(lctx_t *c, items_t *v, dom_node_t *n, const css_style_t *st, const css_style_t *bg,
                           dom_node_t *link, float cb_w) {
    if (n->type == N_TEXT) {
        item_t it = { IT_TEXT, n->text, n->text_len, st, bg, link, 0, 0, n };
        it.node = n;
        if (n->text_len) push_item(v, it);
        return;
    }
    if (n->type != N_ELEMENT || !n->style || n->style->display == D_NONE) return;
    st = n->style;
    if (n->tag == T_BR) {
        item_t it = { IT_BR, 0, 0, st, bg, link, 0, 0, n };
        push_item(v, it);
        return;
    }
    if (n->tag == T_A && dom_attr(n, "href")) link = n;
    if (st->display != D_INLINE || is_replaced(n)) {
        item_t it = { IT_ATOMIC, 0, 0, st, bg, link, 0, 0, n };
        it.atomic = layout_atomic(c, n, st, cb_w);
        if (it.atomic) {
            it.atomic->link = link;
            push_item(v, it);
        }
        return;
    }
    if ((st->bg_color >> 24) || st->border_w[0] > 0) bg = st;
    {
        float open = res(st->margin[3], cb_w, 0) + res(st->padding[3], cb_w, 0) + st->border_w[3];
        float close = res(st->margin[1], cb_w, 0) + res(st->padding[1], cb_w, 0) + st->border_w[1];
        if (open > 0) {
            item_t it = { IT_SPACE_OPEN, 0, 0, st, bg, link, 0, open, n };
            push_item(v, it);
        }
        for (dom_node_t *k = lfirst(n); k; k = lnext(k)) collect_inline(c, v, k, st, bg, link, cb_w);
        if (close > 0) {
            item_t it = { IT_SPACE_CLOSE, 0, 0, st, bg, link, 0, close, n };
            push_item(v, it);
        }
    }
}

typedef struct {
    float  x, w;          /* relative to line start */
    float  above, below;  /* extent around the baseline */
    item_t *item;
    frag_t *frag;
} piece_t;

typedef struct {
    lctx_t  *c;
    box_t   *container;
    bfc_t   *bfc;
    const css_style_t *cst;   /* container style (strut, text-align) */
    float    left, right;     /* content edges */
    float    y;               /* top of the current line */
    float    line_left, line_right;
    piece_t  p[512];
    int      np;
    float    used;
    float    pending_space;
    int      first_line;
    float    first_baseline;
    /* text accumulation for the open fragment */
    char     buf[2048];
    int      blen;
    piece_t *open;
} line_t;

static void strut(const css_style_t *st, float *above, float *below) {
    font_t f = font_pick(st->font_family, st->font_weight, st->font_italic, st->font_size);
    float content = f.ascent - f.descent;
    float half = (st->line_height - content) / 2;
    *above = f.ascent + half;
    *below = -f.descent + half;
}

static void close_frag(line_t *ln) {
    if (ln->open && ln->blen) {
        frag_t *f = ln->open->frag;
        char *t = arena_strndup(&ln->c->L->arena, ln->buf, (size_t)ln->blen);
        f->text = t;
        f->len = (uint16_t)ln->blen;
    }
    ln->open = 0;
    ln->blen = 0;
}

static void begin_line(line_t *ln) {
    float strut_a, strut_b;
    strut(ln->cst, &strut_a, &strut_b);
    line_span(ln->bfc, ln->y, strut_a + strut_b, ln->left, ln->right, &ln->line_left, &ln->line_right);
    /* skip past floats that leave no room */
    while (ln->line_right - ln->line_left < 20 && ln->bfc && ln->bfc->n) {
        float next = 1e30f;
        for (int i = 0; i < ln->bfc->n; i++) {
            if (ln->bfc->f[i].bottom > ln->y && ln->bfc->f[i].bottom < next) next = ln->bfc->f[i].bottom;
        }
        if (next >= 1e29f) break;
        ln->y = next;
        line_span(ln->bfc, ln->y, strut_a + strut_b, ln->left, ln->right, &ln->line_left, &ln->line_right);
    }
    ln->np = 0;
    ln->used = ln->first_line ? res(ln->cst->text_indent, ln->right - ln->left, 0) : 0;
    ln->pending_space = 0;
}

static void finish_line(line_t *ln, int last) {
    float strut_a, strut_b, above, below, shift = 0, avail;
    close_frag(ln);
    if (ln->np == 0) return;
    strut(ln->cst, &strut_a, &strut_b);
    above = strut_a;
    below = strut_b;
    for (int i = 0; i < ln->np; i++) {
        if (ln->p[i].above > above) above = ln->p[i].above;
        if (ln->p[i].below > below) below = ln->p[i].below;
    }
    avail = ln->line_right - ln->line_left;
    if (ln->cst->text_align == TA_CENTER) shift = (avail - ln->used) / 2;
    else if (ln->cst->text_align == TA_RIGHT) shift = avail - ln->used;
    if (shift < 0) shift = 0;
    (void)last;
    for (int i = 0; i < ln->np; i++) {
        piece_t *p = &ln->p[i];
        float x = ln->line_left + shift + p->x;
        if (p->frag) {
            p->frag->x = x;
            p->frag->w = p->w;
            p->frag->baseline = ln->y + above;
            p->frag->y = p->frag->baseline - p->frag->font.ascent;
            p->frag->h = p->frag->font.ascent - p->frag->font.descent;
            if (!p->frag->next && ln->container->frags_tail != p->frag) {
                if (ln->container->frags_tail) ln->container->frags_tail->next = p->frag;
                else ln->container->frags = p->frag;
                ln->container->frags_tail = p->frag;
            }
        } else if (p->item && p->item->atomic) {
            box_t *b = p->item->atomic;
            float dx = x + b->ml - b->x, dy;
            uint8_t va = b->st->vertical_align;
            float top = va == VA_TOP ? ln->y : va == VA_MIDDLE ? ln->y + (above + below - b->h) / 2 : ln->y + above - (b->h + b->mt + b->mb) + b->mt;
            dy = top - b->y;
            /* move the atomic box (and its subtree) into place */
            {
                box_t *stack[256];
                int sp = 0;
                stack[sp++] = b;
                while (sp) {
                    box_t *q = stack[--sp];
                    q->x += dx;
                    q->y += dy;
                    for (frag_t *f = q->frags; f; f = f->next) {
                        f->x += dx;
                        f->y += dy;
                        f->baseline += dy;
                    }
                    for (box_t *k = q->first; k && sp < 256; k = k->next) stack[sp++] = k;
                }
            }
            if (!b->parent) add_child(ln->container, b);
        }
    }
    if (ln->first_line) ln->first_baseline = ln->y + above;
    ln->y += above + below;
    ln->first_line = 0;
}

static frag_t *new_frag(line_t *ln, item_t *it) {
    frag_t *f = (frag_t *)arena_alloc(&ln->c->L->arena, sizeof(frag_t));
    const css_style_t *st = it->st;
    if (!f) return 0;
    f->font = font_pick(st->font_family, st->font_weight, st->font_italic, st->font_size);
    f->color = st->color;
    f->decoration = st->decoration;
    for (const css_style_t *p = it->bg; p && !f->decoration; p = 0) f->decoration = p->decoration;
    f->link = it->link;
    f->inline_bg = it->bg;
    f->node = it->node;
    return f;
}

static void place_word(line_t *ln, item_t *it, const char *w, int wl, float ww, float space_w, int can_break) {
    float avail = ln->line_right - ln->line_left;
    if (ln->np && can_break && ln->used + ln->pending_space + ww > avail + 0.01f) {
        finish_line(ln, 0);
        begin_line(ln);
    }
    if (ln->np >= 510) {
        finish_line(ln, 0);
        begin_line(ln);
    }
    /* extend the open fragment when the style and position allow */
    if (!(ln->open && ln->open->item->st == it->st && ln->open->item->link == it->link &&
          ln->open->item->bg == it->bg && ln->blen + wl + 1 < (int)sizeof(ln->buf))) {
        piece_t *p;
        close_frag(ln);
        p = &ln->p[ln->np++];
        memset(p, 0, sizeof(*p));
        p->item = it;
        p->x = ln->used + ln->pending_space;
        p->frag = new_frag(ln, it);
        if (!p->frag) {
            ln->np--;
            return;
        }
        {
            float a, b;
            strut(it->st, &a, &b);
            p->above = a;
            p->below = b;
            if (it->st->vertical_align == VA_SUPER) p->above += it->st->font_size * 0.4f;
            if (it->st->vertical_align == VA_SUB) p->below += it->st->font_size * 0.2f;
        }
        ln->open = p;
        ln->used = p->x;
        ln->pending_space = 0;
    } else if (ln->pending_space > 0) {
        if (ln->blen < (int)sizeof(ln->buf) - 1) ln->buf[ln->blen++] = ' ';
        ln->used += ln->pending_space;
        ln->open->w += ln->pending_space;
        ln->pending_space = 0;
    }
    memcpy(ln->buf + ln->blen, w, (size_t)wl);
    ln->blen += wl;
    ln->used += ww;
    ln->open->w += ww;
    ln->pending_space = space_w;
}

static void transform_text(char *s, int n, int tt) {
    int start = 1;
    for (int i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (tt == TT_UPPER || (tt == TT_CAPITALIZE && start)) {
            if (ch >= 'a' && ch <= 'z') s[i] = (char)(ch - 32);
        } else if (tt == TT_LOWER) {
            if (ch >= 'A' && ch <= 'Z') s[i] = (char)(ch + 32);
        }
        start = ch == ' ';
    }
}

static void layout_text_item(line_t *ln, item_t *it) {
    const css_style_t *st = it->st;
    font_t f = font_pick(st->font_family, st->font_weight, st->font_italic, st->font_size);
    float space_w = font_advance(&f, ' ') + st->word_spacing;
    const char *s = it->s, *end = it->s + it->n;
    int ws = st->white_space;
    int preserve = ws == WS_PRE || ws == WS_PRE_WRAP;
    int newlines = preserve || ws == WS_PRE_LINE;
    int wrap = ws == WS_NORMAL || ws == WS_PRE_WRAP || ws == WS_PRE_LINE;
    while (s < end) {
        const char *w = s;
        char word[256];
        int wl = 0;
        float ww = 0;
        if (*s == '\n' && newlines) {
            finish_line(ln, 1);
            begin_line(ln);
            s++;
            continue;
        }
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f') {
            if (preserve) {
                int count = *s == '\t' ? 8 : 1;
                if (ln->np == 0 && !ln->open) {
                    /* leading preserved space: a fragment holding spaces */
                    place_word(ln, it, "        ", count, space_w * count, 0, 0);
                } else {
                    ln->pending_space += space_w * count;
                }
            } else if (ln->np || ln->open) {
                ln->pending_space = space_w;
            }
            s++;
            continue;
        }
        while (s < end && !(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f') && wl < (int)sizeof(word) - 4) {
            const char *before = s;
            uint32_t cp = utf8_next(&s, end);
            float a = font_advance(&f, cp) + st->letter_spacing;
            memcpy(word + wl, before, (size_t)(s - before));
            wl += (int)(s - before);
            ww += a;
            /* overlong words (URLs) may break anywhere when they would overflow */
            if (wrap && ww > ln->right - ln->left && s < end && !(*s == ' ')) break;
        }
        (void)w;
        if (st->text_transform) transform_text(word, wl, st->text_transform);
        {
            int had_space = ln->pending_space > 0;
            float ps = had_space ? ln->pending_space : 0;
            ln->pending_space = ps;
            place_word(ln, it, word, wl, ww, 0, wrap);
        }
    }
}

static void layout_inline_run(lctx_t *c, box_t *container, bfc_t *bfc, items_t *items, float left, float right,
                              float *y, float *first_baseline) {
    line_t *ln;
    if (!items->n) return;
    ln = (line_t *)calloc(1, sizeof(line_t));
    if (!ln) return;
    ln->c = c;
    ln->container = container;
    ln->bfc = bfc;
    ln->cst = container->st;
    ln->left = left;
    ln->right = right;
    ln->y = *y;
    ln->first_line = 1;
    begin_line(ln);
    for (int i = 0; i < items->n; i++) {
        item_t *it = &items->it[i];
        switch (it->type) {
        case IT_TEXT:
            layout_text_item(ln, it);
            break;
        case IT_BR:
            if (ln->np == 0) {
                float a, b;
                strut(it->st, &a, &b);
                ln->y += a + b;
            }
            finish_line(ln, 1);
            begin_line(ln);
            break;
        case IT_SPACE_OPEN: case IT_SPACE_CLOSE:
            close_frag(ln);
            ln->used += ln->pending_space + it->space;
            ln->pending_space = 0;
            break;
        case IT_ATOMIC: {
            box_t *b = it->atomic;
            float bw = b->w + b->ml + b->mr;
            piece_t *p;
            close_frag(ln);
            if (ln->np && ln->used + ln->pending_space + bw > ln->line_right - ln->line_left + 0.01f &&
                it->st->white_space != WS_NOWRAP) {
                finish_line(ln, 0);
                begin_line(ln);
            }
            if (ln->np >= 510) break;
            p = &ln->p[ln->np++];
            memset(p, 0, sizeof(*p));
            p->item = it;
            p->x = ln->used + ln->pending_space;
            p->w = bw;
            p->above = b->h + b->mt + b->mb;
            if (b->kind != BX_REPLACED && b->frags) {
                /* inline-block baseline: its first line */
                p->above = b->frags->baseline - b->y + b->mt;
                p->below = b->h + b->mt + b->mb - p->above;
            }
            ln->used = p->x + bw;
            ln->pending_space = 0;
            break;
        }
        }
    }
    finish_line(ln, 1);
    if (first_baseline && !*first_baseline) *first_baseline = ln->first_baseline;
    *y = ln->y;
    free(ln);
}

/* ---- block formatting --------------------------------------------------- */

static float layout_block(lctx_t *c, box_t *parent, dom_node_t *n, const css_style_t *st, float x, float y,
                          float cb_w, float cb_h, bfc_t *bfc, int shrink);
static void layout_children(lctx_t *c, box_t *b, dom_node_t *n, bfc_t *bfc, float *content_h);
static void layout_flex(lctx_t *c, box_t *b, dom_node_t *n, bfc_t *bfc, float *content_h);
static void layout_table(lctx_t *c, box_t *b, dom_node_t *n, float *content_h);

static void marker_text(const box_t *b, char *out, size_t cap) {
    const css_style_t *st = b->st;
    int index = 1;
    for (dom_node_t *p = b->node->prev; p; p = p->prev) {
        if (p->type == N_ELEMENT && p->style && p->style->display == D_LIST_ITEM) index++;
    }
    if (b->node->parent && b->node->parent->tag == T_OL) {
        const char *start = dom_attr(b->node->parent, "start");
        if (start) index += atoi(start) - 1;
    }
    switch (st->list_style) {
    case LS_DISC: snprintf(out, cap, "\xE2\x80\xA2"); break;
    case LS_CIRCLE: snprintf(out, cap, "\xE2\x97\xA6"); break;
    case LS_SQUARE: snprintf(out, cap, "\xE2\x96\xAA"); break;
    case LS_DECIMAL: snprintf(out, cap, "%d.", index); break;
    case LS_LOWER_ALPHA: snprintf(out, cap, "%c.", 'a' + (index - 1) % 26); break;
    case LS_UPPER_ALPHA: snprintf(out, cap, "%c.", 'A' + (index - 1) % 26); break;
    case LS_LOWER_ROMAN: case LS_UPPER_ROMAN: {
        static const char *const r[] = { "", "i", "ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii" };
        snprintf(out, cap, "%s.", index < 13 ? r[index] : "x");
        if (st->list_style == LS_UPPER_ROMAN) {
            for (char *p = out; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            }
        }
        break;
    }
    default: out[0] = 0;
    }
}

static void add_marker(lctx_t *c, box_t *b, float first_baseline) {
    char text[16];
    frag_t *f;
    marker_text(b, text, sizeof(text));
    if (!text[0]) return;
    f = (frag_t *)arena_alloc(&c->L->arena, sizeof(frag_t));
    if (!f) return;
    f->font = font_pick(b->st->font_family, b->st->font_weight, 0, b->st->font_size);
    f->text = arena_strndup(&c->L->arena, text, strlen(text));
    f->len = (uint16_t)strlen(text);
    f->w = font_text_width(&f->font, f->text, f->len);
    f->x = b->x + b->bl + b->pl - f->w - b->st->font_size * 0.5f;
    if (!first_baseline) first_baseline = b->y + b->bt + b->pt + f->font.ascent + (b->st->line_height - (f->font.ascent - f->font.descent)) / 2;
    f->baseline = first_baseline;
    f->y = first_baseline - f->font.ascent;
    f->h = f->font.ascent - f->font.descent;
    f->color = b->st->color;
    f->kind = 1;
    f->next = b->frags;
    b->frags = f;
    if (!b->frags_tail) b->frags_tail = f;
}

static float first_baseline_of(box_t *b) {
    for (frag_t *f = b->frags; f; f = f->next) {
        if (f->kind == 0) return f->baseline;
    }
    for (box_t *k = b->first; k; k = k->next) {
        float v = first_baseline_of(k);
        if (v) return v;
    }
    return 0;
}

/* Lays out the in-flow and float children of n inside box b (content box
 * already positioned). */
static void layout_children(lctx_t *c, box_t *b, dom_node_t *n, bfc_t *bfc, float *content_h) {
    float left = b->x + b->bl + b->pl, right = left + (b->w - hframe(b));
    float top = b->y + b->bt + b->pt, y = top, prev_mb = 0;
    items_t items = { 0 };
    float fb = 0;
    int have_block = 0;
    for (dom_node_t *k = lfirst(n);; k = lnext(k)) {
        const css_style_t *ks = k && k->type == N_ELEMENT ? k->style : 0;
        int flush = !k;
        if (k && k->type == N_ELEMENT) {
            if (!ks || ks->display == D_NONE) continue;
            if (ks->display == D_CONTENTS) {
                /* children participate directly; approximate by inline collection */
                for (dom_node_t *g = lfirst(k); g; g = lnext(g)) collect_inline(c, &items, g, b->st, 0, link_of(k), right - left);
                continue;
            }
            if (ks->position == P_ABSOLUTE || ks->position == P_FIXED) {
                if (c->nabs < MAX_ABS) {
                    abs_t *a = &c->abs[c->nabs++];
                    a->node = k;
                    a->box = b;
                    a->static_x = left;
                    a->static_y = y;
                }
                continue;
            }
            if (ks->float_ || is_block_level(ks)) flush = 1;
        }
        if (flush && items.n) {
            float yy = y + (have_block ? prev_mb : 0);
            float before = yy;
            layout_inline_run(c, b, bfc, &items, left, right, &yy, &fb);
            if (yy > before) {
                y = yy;
                prev_mb = 0;
                have_block = 1;
            }
            items.n = 0;
        }
        if (!k) break;
        if (k->type == N_TEXT) {
            /* skip whitespace-only text between blocks */
            int blank = 1;
            for (size_t i = 0; i < k->text_len && blank; i++) {
                char ch = k->text[i];
                if (!(ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r')) blank = 0;
            }
            if (blank && items.n == 0 && b->st->white_space != WS_PRE) continue;
            collect_inline(c, &items, k, b->st, 0, link_of(n), right - left);
            continue;
        }
        if (k->type != N_ELEMENT) continue;
        if (ks->float_) {
            float fw, fh, fx, fy;
            box_t *fbx;
            float sy = clear_y(bfc, y, ks->clear);
            float h = layout_block(c, b, k, ks, 0, sy, right - left, -1, 0, 1);
            fbx = b->last;
            if (!fbx) continue;
            fw = fbx->w + fbx->ml + fbx->mr;
            fh = h;
            fy = sy;
            for (int guard = 0; guard < 100; guard++) {
                float l, r;
                line_span(bfc, fy, fh, left, right, &l, &r);
                if (r - l >= fw || (l == left && r == right)) {
                    fx = ks->float_ == F_LEFT ? l : r - fw;
                    break;
                }
                {
                    float next = 1e30f;
                    for (int i = 0; i < bfc->n; i++) {
                        if (bfc->f[i].bottom > fy && bfc->f[i].bottom < next) next = bfc->f[i].bottom;
                    }
                    if (next >= 1e29f) {
                        fx = ks->float_ == F_LEFT ? left : right - fw;
                        break;
                    }
                    fy = next;
                }
            }
            {
                /* move the float box to its final place */
                float dx = fx + fbx->ml - fbx->x, dy = fy + fbx->mt - fbx->y;
                box_t *stack[512];
                int sp = 0;
                stack[sp++] = fbx;
                while (sp) {
                    box_t *q = stack[--sp];
                    q->x += dx;
                    q->y += dy;
                    for (frag_t *f = q->frags; f; f = f->next) {
                        f->x += dx;
                        f->y += dy;
                        f->baseline += dy;
                    }
                    for (box_t *g = q->first; g && sp < 512; g = g->next) stack[sp++] = g;
                }
            }
            if (bfc->n < MAX_FLOATS) {
                flt_t *f = &bfc->f[bfc->n++];
                f->l = fx;
                f->r = fx + fw;
                f->top = fy;
                f->bottom = fy + fh;
                f->right = ks->float_ == F_RIGHT;
            }
            continue;
        }
        if (is_block_level(ks)) {
            float mt = res(ks->margin[0], right - left, 0);
            float h, by;
            if (ks->clear) {
                float cy = clear_y(bfc, y, ks->clear);
                if (cy > y) {
                    y = cy;
                    prev_mb = 0;
                }
            }
            /* sibling margins collapse to the larger one */
            by = y + (mt > prev_mb ? mt : prev_mb) - mt;
            if (!have_block) by = y;
            h = layout_block(c, b, k, ks, left, by, right - left, -1, bfc, 0);
            {
                box_t *kb = b->last;
                if (kb && kb->node == k) {
                    y = kb->y + kb->h;
                    prev_mb = kb->mb;
                } else {
                    y = by + h;
                    prev_mb = 0;
                }
            }
            if (!fb && b->last) fb = first_baseline_of(b->last);
            have_block = 1;
            continue;
        }
        collect_inline(c, &items, k, b->st, 0, link_of(n), right - left);
    }
    free(items.it);
    y += prev_mb;
    if (b->st->overflow != OV_VISIBLE || b->kind == BX_INLINE_BLOCK || b->st->float_ || b->kind == BX_ROOT ||
        b->st->display == D_TABLE_CELL || b->st->display == D_FLEX || b->st->display == D_GRID) {
        y = floats_bottom(bfc, y);
    }
    *content_h = y - top;
    if (b->st->display == D_LIST_ITEM && b->st->list_style != LS_NONE) add_marker(c, b, fb);
}

static box_t *layout_atomic(lctx_t *c, dom_node_t *n, const css_style_t *st, float cb_w) {
    box_t tmp_parent;
    box_t *b;
    memset(&tmp_parent, 0, sizeof(tmp_parent));
    if (is_replaced(n)) {
        float w, h;
        b = new_box(c, n, st, BX_REPLACED);
        if (!b) return 0;
        edges(b, cb_w);
        replaced_size(c, n, st, cb_w, &w, &h);
        if ((n->tag == T_IMG || n->tag == T_SVG) && c->img) {
            int iw, ih;
            c->img(n, c->img_ctx, &iw, &ih, &b->image);
        }
        b->w = w + hframe(b);
        b->h = h + vframe(b);
        b->x = 0;
        b->y = 0;
        return b;
    }
    layout_block(c, &tmp_parent, n, st, 0, 0, cb_w, -1, 0, 1);
    b = tmp_parent.first;
    if (b) {
        b->parent = 0;
        b->next = 0;
        b->kind = st->display == D_INLINE_FLEX ? BX_FLEX : BX_INLINE_BLOCK;
    }
    return b;
}

/* Lays out a block-level element; returns its margin-box height.  shrink:
 * width auto means shrink-to-fit (floats, inline-blocks, abs boxes). */
static float layout_block(lctx_t *c, box_t *parent, dom_node_t *n, const css_style_t *st, float x, float y,
                          float cb_w, float cb_h, bfc_t *bfc, int shrink) {
    box_t *b = new_box(c, n, st, BX_BLOCK);
    float cw, content_h = 0, h;
    bfc_t *own = 0;
    if (!b || c->depth > 120) return 0;
    c->depth++;
    if (st->display == D_TABLE || st->display == D_INLINE_TABLE) b->kind = BX_TABLE;
    else if (st->display == D_FLEX || st->display == D_INLINE_FLEX) b->kind = BX_FLEX;
    else if (st->display == D_GRID || st->display == D_INLINE_GRID) b->kind = BX_GRID;
    if (n->tag == T_HTML) b->kind = BX_ROOT;
    edges(b, cb_w);
    if (is_auto(st->margin[1]) && is_auto(st->margin[3])) b->ml = b->mr = 0;
    /* width */
    if (is_replaced(n) && n->tag != T_BUTTON) {
        float rw, rh;
        replaced_size(c, n, st, cb_w, &rw, &rh);
        cw = rw;
        b->kind = BX_REPLACED;
        if ((n->tag == T_IMG || n->tag == T_SVG) && c->img) {
            int iw, ih;
            c->img(n, c->img_ctx, &iw, &ih, &b->image);
        }
    } else if (!is_auto(st->width)) {
        cw = res(st->width, cb_w, 0);
        if (st->box_sizing) cw -= hframe(b);
    } else if (shrink || b->kind == BX_TABLE) {
        float mn, mx, avail = cb_w - b->ml - b->mr - hframe(b);
        intrinsic(c, n, st, &mn, &mx);
        mn -= hframe(b);
        mx -= hframe(b);
        cw = mx < avail ? mx : avail;
        if (cw < mn) cw = mn;
        if (b->kind == BX_TABLE && cw > avail) cw = avail > mn ? avail : mn;
    } else {
        cw = cb_w - b->ml - b->mr - hframe(b);
    }
    {
        float mx = res(st->max_w, cb_w, -1), mn = res(st->min_w, cb_w, 0);
        if (st->box_sizing) {
            if (mx >= 0) mx -= hframe(b);
            mn -= hframe(b);
        }
        cw = clampf(cw, mn > 0 ? mn : 0, mx);
    }
    if (cw < 0) cw = 0;
    b->w = cw + hframe(b);
    /* auto margins center a block with a definite width; <center> centers its blocks */
    if (!shrink && n->parent && n->parent->tag == T_CENTER && !is_auto(st->width)) {
        float slack = cb_w - b->w;
        if (slack > 0) b->ml = b->mr = slack / 2;
    } else if (!shrink && (is_auto(st->margin[1]) || is_auto(st->margin[3]))) {
        float slack = cb_w - b->w - (is_auto(st->margin[3]) ? 0 : b->ml) - (is_auto(st->margin[1]) ? 0 : b->mr);
        if (slack > 0) {
            if (is_auto(st->margin[1]) && is_auto(st->margin[3])) b->ml = b->mr = slack / 2;
            else if (is_auto(st->margin[3])) b->ml = slack;
            else b->mr = slack;
        }
    }
    b->x = x + b->ml;
    b->y = y + b->mt;
    add_child(parent, b);
    if (st->position == P_RELATIVE || st->position == P_STICKY) {
        float l = res(st->inset[3], cb_w, 0), t = res(st->inset[0], cb_h, 0);
        if (!is_auto(st->inset[3])) b->rel_x = l;
        else if (!is_auto(st->inset[1])) b->rel_x = -res(st->inset[1], cb_w, 0);
        if (st->position == P_RELATIVE) {
            if (!is_auto(st->inset[0])) b->rel_y = t;
            else if (!is_auto(st->inset[2])) b->rel_y = -res(st->inset[2], cb_h, 0);
        }
    }
    /* a new block formatting context isolates floats */
    if (!bfc || st->overflow != OV_VISIBLE || shrink || b->kind != BX_BLOCK || st->display == D_TABLE_CELL ||
        st->display == D_INLINE_BLOCK) {
        own = (bfc_t *)calloc(1, sizeof(bfc_t));
        bfc = own;
    }
    /* percentage heights inside resolve against this box when its height is
     * known before its content: given, or fixed by top and bottom */
    float saved_pct_h = c->pct_h;
    {
        float known = -1;
        if (!is_auto(st->height) && !(st->height.unit == U_PCT && cb_h < 0)) {
            known = res(st->height, cb_h, -1);
            if (known >= 0 && st->box_sizing) known -= vframe(b);
        } else if ((st->position == P_ABSOLUTE || st->position == P_FIXED) && cb_h >= 0 &&
                   !is_auto(st->inset[0]) && !is_auto(st->inset[2])) {
            known = cb_h - res(st->inset[0], cb_h, 0) - res(st->inset[2], cb_h, 0) - b->mt - b->mb - vframe(b);
        }
        c->pct_h = known;
    }
    {
        int abs_mark = c->nabs;
        if (b->kind == BX_REPLACED) content_h = 0;
        else if (b->kind == BX_FLEX || b->kind == BX_GRID) layout_flex(c, b, n, bfc, &content_h);
        else if (b->kind == BX_TABLE) layout_table(c, b, n, &content_h);
        else layout_children(c, b, n, bfc, &content_h);
        c->pct_h = saved_pct_h;
        if (b->kind == BX_REPLACED) {
            float rw, rh;
            replaced_size(c, n, st, cb_w, &rw, &rh);
            content_h = rh;
        }
        h = content_h;
        if (!is_auto(st->height) && !(st->height.unit == U_PCT && cb_h < 0)) {
            h = res(st->height, cb_h, content_h);
            if (st->box_sizing) h -= vframe(b);
        }
        {
            float mn = res(st->min_h, cb_h, 0), mx = res(st->max_h, cb_h, -1);
            if (st->box_sizing) {
                mn -= vframe(b);
                if (mx >= 0) mx -= vframe(b);
            }
            h = clampf(h, mn > 0 ? mn : 0, mx);
        }
        if (b->kind == BX_ROOT && h < c->L->viewport_h - vframe(b)) h = c->L->viewport_h - vframe(b);
        b->h = h + vframe(b);
        /* absolutely positioned descendants whose containing block is this box */
        if (st->position != P_STATIC || b->kind == BX_ROOT) {
            int end = c->nabs;
            for (int i = abs_mark; i < end; i++) {
                abs_t *a = &c->abs[i];
                const css_style_t *as = a->node->style;
                box_t *ab;
                float cbx, cby, cbw, cbh, w, ah;
                if (!a->node) continue;
                if (as->position == P_FIXED) {
                    cbx = 0;
                    cby = 0;
                    cbw = c->L->viewport_w;
                    cbh = c->L->viewport_h;
                } else {
                    cbx = b->x + b->bl;
                    cby = b->y + b->bt;
                    cbw = b->w - b->bl - b->br;
                    cbh = b->h - b->bt - b->bb;
                }
                {
                    int lset = !is_auto(as->inset[3]), rset = !is_auto(as->inset[1]);
                    int wide = is_auto(as->width) && lset && rset;
                    ah = layout_block(c, b, a->node, as, 0, 0, wide ? cbw - res(as->inset[3], cbw, 0) - res(as->inset[1], cbw, 0) : cbw,
                                      cbh, 0, !wide);
                }
                ab = b->last;
                if (!ab || ab->node != a->node) continue;
                ab->positioned = as->position == P_FIXED ? 2 : 1;
                w = ab->w + ab->ml + ab->mr;
                {
                    float nx, ny;
                    if (!is_auto(as->inset[3])) nx = cbx + res(as->inset[3], cbw, 0);
                    else if (!is_auto(as->inset[1])) nx = cbx + cbw - res(as->inset[1], cbw, 0) - w;
                    else nx = as->position == P_FIXED ? a->static_x : a->static_x;
                    if (!is_auto(as->inset[0])) ny = cby + res(as->inset[0], cbh, 0);
                    else if (!is_auto(as->inset[2])) ny = cby + cbh - res(as->inset[2], cbh, 0) - ah;
                    else ny = a->static_y;
                    {
                        float dx = nx + ab->ml - ab->x, dy = ny + ab->mt - ab->y;
                        box_t *stack[512];
                        int sp = 0;
                        stack[sp++] = ab;
                        while (sp) {
                            box_t *q = stack[--sp];
                            q->x += dx;
                            q->y += dy;
                            for (frag_t *f = q->frags; f; f = f->next) {
                                f->x += dx;
                                f->y += dy;
                                f->baseline += dy;
                            }
                            for (box_t *g = q->first; g && sp < 512; g = g->next) stack[sp++] = g;
                        }
                    }
                }
                a->node = 0;
            }
            c->nabs = abs_mark;
        }
    }
    free(own);
    c->depth--;
    return b->h + b->mt + b->mb;
}

/* ---- flexbox (and grid approximated as flex rows) ------------------------ */

typedef struct {
    dom_node_t *node;
    const css_style_t *st;
    float base, hyp, min, max, main, cross;
    box_t *box;
    int   anon;
} fitem_t;

static void layout_flex(lctx_t *c, box_t *b, dom_node_t *n, bfc_t *bfc, float *content_h) {
    const css_style_t *st = b->st;
    float left = b->x + b->bl + b->pl, top = b->y + b->bt + b->pt;
    float inner_w = b->w - hframe(b);
    int grid = b->kind == BX_GRID;
    int row = grid || st->flex_dir == FD_ROW || st->flex_dir == FD_ROW_REVERSE;
    int reverse = !grid && (st->flex_dir == FD_ROW_REVERSE || st->flex_dir == FD_COLUMN_REVERSE);
    float gap_main = row ? st->gap_col : st->gap_row, gap_cross = row ? st->gap_row : st->gap_col;
    fitem_t *items;
    int ni = 0, cap = 0;
    float y = top;
    (void)bfc;
    for (dom_node_t *k = lfirst(n); k; k = lnext(k)) cap++;
    items = (fitem_t *)calloc((size_t)(cap ? cap : 1), sizeof(fitem_t));
    if (!items) return;
    for (dom_node_t *k = lfirst(n); k; k = lnext(k)) {
        if (k->type == N_TEXT) {
            int blank = 1;
            for (size_t i = 0; i < k->text_len; i++) {
                char ch = k->text[i];
                if (!(ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r')) blank = 0;
            }
            if (!blank) {
                items[ni].node = k;
                items[ni].st = st;
                items[ni].anon = 1;
                ni++;
            }
            continue;
        }
        if (k->type != N_ELEMENT || !k->style || k->style->display == D_NONE) continue;
        if (k->style->position == P_ABSOLUTE || k->style->position == P_FIXED) {
            if (c->nabs < MAX_ABS) {
                abs_t *a = &c->abs[c->nabs++];
                a->node = k;
                a->box = b;
                a->static_x = left;
                a->static_y = top;
            }
            continue;
        }
        items[ni].node = k;
        items[ni].st = k->style;
        ni++;
    }
    /* order property (stable) */
    for (int i = 1; i < ni; i++) {
        fitem_t t = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].st->order > t.st->order && !items[j].anon && !t.anon) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = t;
    }
    if (grid) {
        int cols = st->grid_cols;
        float colw;
        if (cols <= 0) cols = inner_w > 0 ? (int)((inner_w + gap_main) / (240 + gap_main)) : 1;
        if (cols < 1) cols = 1;
        colw = (inner_w - gap_main * (cols - 1)) / cols;
        for (int i = 0; i < ni; i += cols) {
            float rowh = 0;
            for (int k = i; k < ni && k < i + cols; k++) {
                float x = left + (colw + gap_main) * (k - i);
                float h;
                if (items[k].anon) continue;
                h = layout_block(c, b, items[k].node, items[k].st, x, y, colw, -1, 0, 0);
                items[k].box = b->last;
                if (h > rowh) rowh = h;
            }
            y += rowh + (i + cols < ni ? gap_cross : 0);
        }
        *content_h = y - top;
        free(items);
        return;
    }
    if (row) {
        /* hypothetical main sizes */
        for (int i = 0; i < ni; i++) {
            fitem_t *it = &items[i];
            float mn, mx, margins;
            if (it->anon) {
                float a = 0, bb = 0;
                text_intrinsic(it->node->text, it->node->text_len, st, &a, &bb);
                it->base = bb;
                it->min = a;
                it->max = -1;
                continue;
            }
            margins = res(it->st->margin[1], inner_w, 0) + res(it->st->margin[3], inner_w, 0);
            intrinsic(c, it->node, it->st, &mn, &mx);
            if (!is_auto(it->st->flex_basis) && it->st->flex_basis.unit != U_NONE) {
                it->base = res(it->st->flex_basis, inner_w, mx);
                if (it->st->box_sizing == 0 && it->st->flex_basis.unit == U_PX) {
                    it->base += it->st->padding[1].v * (it->st->padding[1].unit == U_PX) + it->st->padding[3].v * (it->st->padding[3].unit == U_PX) +
                                it->st->border_w[1] + it->st->border_w[3];
                }
            } else if (!is_auto(it->st->width)) {
                it->base = res(it->st->width, inner_w, mx);
                if (!it->st->box_sizing) {
                    it->base += res(it->st->padding[1], inner_w, 0) + res(it->st->padding[3], inner_w, 0) +
                                it->st->border_w[1] + it->st->border_w[3];
                }
            } else {
                it->base = mx;
            }
            it->min = is_auto(it->st->min_w) ? (it->st->overflow == OV_VISIBLE ? (mn < it->base ? mn : it->base) : 0)
                                              : res(it->st->min_w, inner_w, 0);
            it->max = res(it->st->max_w, inner_w, -1);
            it->base += margins;
            it->min += margins;
            if (it->max >= 0) it->max += margins;
        }
        /* lines */
        {
            int start = 0;
            while (start < ni) {
                int end = start;
                float sum = 0, free_space, grow = 0, shrink = 0, linew = 0, maxh = 0, x;
                while (end < ni) {
                    float add = items[end].base + (end > start ? gap_main : 0);
                    if (st->flex_wrap && end > start && sum + add > inner_w + 0.5f) break;
                    sum += add;
                    end++;
                }
                free_space = inner_w - sum;
                for (int i = start; i < end; i++) {
                    grow += items[i].anon ? 0 : items[i].st->flex_grow;
                    shrink += items[i].anon ? items[i].base : items[i].st->flex_shrink * items[i].base;
                }
                for (int i = start; i < end; i++) {
                    fitem_t *it = &items[i];
                    it->main = it->base;
                    if (free_space > 0 && grow > 0 && !it->anon) it->main += free_space * it->st->flex_grow / grow;
                    else if (free_space < 0 && shrink > 0) {
                        float f = it->anon ? it->base : it->st->flex_shrink * it->base;
                        it->main += free_space * f / shrink;
                    }
                    it->main = clampf(it->main, it->min, it->max);
                    linew += it->main + (i > start ? gap_main : 0);
                }
                /* lay out at the resolved widths */
                for (int i = start; i < end; i++) {
                    fitem_t *it = &items[i];
                    float h;
                    if (it->anon) {
                        box_t *anon = new_box(c, it->node, st, BX_BLOCK);
                        items_t tmp = { 0 };
                        float yy = y;
                        if (!anon) continue;
                        anon->x = 0;
                        anon->y = y;
                        anon->w = it->main;
                        add_child(b, anon);
                        collect_inline(c, &tmp, it->node, st, 0, link_of(n), it->main);
                        layout_inline_run(c, anon, 0, &tmp, 0, it->main, &yy, 0);
                        free(tmp.it);
                        anon->h = yy - y;
                        it->box = anon;
                        h = anon->h;
                    } else {
                        float mw = it->main - res(it->st->margin[1], inner_w, 0) - res(it->st->margin[3], inner_w, 0);
                        css_style_t tmp = *it->st;
                        tmp.width.unit = U_PX;
                        tmp.width.v = mw;
                        tmp.box_sizing = 1;
                        tmp.min_w.unit = U_PX;
                        tmp.min_w.v = 0;
                        tmp.max_w.unit = U_NONE;
                        {
                            css_style_t *copy = (css_style_t *)arena_alloc(&c->L->arena, sizeof(css_style_t));
                            if (!copy) continue;
                            *copy = tmp;
                            h = layout_block(c, b, it->node, copy, 0, y, inner_w, -1, 0, 0);
                            it->box = b->last;
                            if (it->box) it->box->st = it->st;
                        }
                    }
                    it->cross = h;
                    if (h > maxh) maxh = h;
                }
                if (!st->flex_wrap && !is_auto(st->height) && st->height.unit == U_PX) {
                    float ch = st->height.v - (st->box_sizing ? vframe(b) : 0);
                    if (ch > maxh) maxh = ch;
                }
                /* main-axis distribution */
                {
                    float rem = inner_w - linew, gap_extra = 0, offset = 0;
                    int count = end - start;
                    if (rem > 0) {
                        switch (st->justify) {
                        case JC_END: offset = rem; break;
                        case JC_CENTER: offset = rem / 2; break;
                        case JC_SPACE_BETWEEN: gap_extra = count > 1 ? rem / (count - 1) : 0; break;
                        case JC_SPACE_AROUND: gap_extra = rem / count; offset = gap_extra / 2; break;
                        case JC_SPACE_EVENLY: gap_extra = rem / (count + 1); offset = gap_extra; break;
                        }
                    }
                    if (reverse) offset = rem > 0 ? rem - offset : 0;
                    x = left + offset;
                    for (int k = 0; k < count; k++) {
                        int i = reverse ? end - 1 - k : start + k;
                        fitem_t *it = &items[i];
                        box_t *ib = it->box;
                        float dx, dy = 0;
                        uint8_t al = it->anon ? st->align_items : (it->st->align_self != JC_AUTO ? it->st->align_self : st->align_items);
                        if (!ib) continue;
                        if (al == JC_CENTER) dy = (maxh - it->cross) / 2;
                        else if (al == JC_END) dy = maxh - it->cross;
                        else if (al == JC_STRETCH && !it->anon && is_auto(it->st->height)) {
                            ib->h = maxh - ib->mt - ib->mb;
                        }
                        dx = x + (it->anon ? 0 : ib->ml) - ib->x;
                        {
                            box_t *stack[512];
                            int sp = 0;
                            stack[sp++] = ib;
                            while (sp) {
                                box_t *q = stack[--sp];
                                q->x += dx;
                                q->y += dy;
                                for (frag_t *f = q->frags; f; f = f->next) {
                                    f->x += dx;
                                    f->y += dy;
                                    f->baseline += dy;
                                }
                                for (box_t *g = q->first; g && sp < 512; g = g->next) stack[sp++] = g;
                            }
                        }
                        x += it->main + gap_main + gap_extra;
                    }
                }
                y += maxh + (end < ni ? gap_cross : 0);
                start = end;
            }
        }
    } else {
        /* column: stack items, stretching their width */
        for (int i = 0; i < ni; i++) {
            fitem_t *it = &items[i];
            float h;
            if (it->anon) {
                box_t *anon = new_box(c, it->node, st, BX_BLOCK);
                items_t tmp = { 0 };
                float yy = y;
                if (!anon) continue;
                anon->x = left;
                anon->y = y;
                anon->w = inner_w;
                add_child(b, anon);
                collect_inline(c, &tmp, it->node, st, 0, link_of(n), inner_w);
                layout_inline_run(c, anon, 0, &tmp, left, left + inner_w, &yy, 0);
                free(tmp.it);
                anon->h = yy - y;
                h = anon->h;
            } else {
                int center = (it->st->align_self != JC_AUTO ? it->st->align_self : st->align_items) == JC_CENTER;
                h = layout_block(c, b, it->node, it->st, left, y, inner_w, -1, 0,
                                 (st->align_items != JC_STRETCH && is_auto(it->st->width)) || center);
                if (center && b->last) {
                    box_t *ib = b->last;
                    float dx = (inner_w - (ib->w + ib->ml + ib->mr)) / 2;
                    box_t *stack[512];
                    int sp = 0;
                    stack[sp++] = ib;
                    while (sp && dx > 0) {
                        box_t *q = stack[--sp];
                        q->x += dx;
                        for (frag_t *f = q->frags; f; f = f->next) f->x += dx;
                        for (box_t *g = q->first; g && sp < 512; g = g->next) stack[sp++] = g;
                    }
                }
            }
            y += h + (i + 1 < ni ? gap_main : 0);
        }
    }
    *content_h = y - top;
    free(items);
}

/* ---- tables ------------------------------------------------------------- */

#define MAX_COLS 64

typedef struct {
    dom_node_t *node;
    int col, span, row;
} tcell_t;

static void table_rows(dom_node_t *n, dom_node_t **rows, int *nr, int cap) {
    for (dom_node_t *k = n->first; k && *nr < cap; k = k->next) {
        if (k->type != N_ELEMENT || !k->style || k->style->display == D_NONE) continue;
        if (k->style->display == D_TABLE_ROW || k->tag == T_TR) rows[(*nr)++] = k;
        else if (k->style->display == D_TABLE_ROW_GROUP || k->style->display == D_TABLE_HEADER_GROUP ||
                 k->style->display == D_TABLE_FOOTER_GROUP || k->tag == T_TBODY || k->tag == T_THEAD || k->tag == T_TFOOT) {
            table_rows(k, rows, nr, cap);
        }
    }
}

static void layout_table(lctx_t *c, box_t *b, dom_node_t *n, float *content_h) {
    dom_node_t *rows[1024];
    int nr = 0, ncols = 0;
    float colmin[MAX_COLS] = { 0 }, colmax[MAX_COLS] = { 0 }, colw[MAX_COLS] = { 0 };
    float inner = b->w - hframe(b), spacing = b->st->border_collapse ? 0 : 2;
    float left = b->x + b->bl + b->pl, y = b->y + b->bt + b->pt + spacing;
    tcell_t *cells;
    int ncell = 0;
    const char *cp = dom_attr(n, "cellspacing");
    if (cp) spacing = (float)atoi(cp);
    table_rows(n, rows, &nr, 1024);
    /* captions first */
    for (dom_node_t *k = n->first; k; k = k->next) {
        if (k->type == N_ELEMENT && k->style && k->style->display == D_TABLE_CAPTION) {
            y += layout_block(c, b, k, k->style, left, y, inner, -1, 0, 0);
        }
    }
    cells = (tcell_t *)calloc(4096, sizeof(tcell_t));
    if (!cells) return;
    for (int r = 0; r < nr; r++) {
        int col = 0;
        for (dom_node_t *k = rows[r]->first; k && ncell < 4096; k = k->next) {
            int span;
            if (k->type != N_ELEMENT || !k->style || k->style->display == D_NONE) continue;
            span = dom_attr(k, "colspan") ? atoi(dom_attr(k, "colspan")) : 1;
            if (span < 1) span = 1;
            if (col + span > MAX_COLS) break;
            cells[ncell].node = k;
            cells[ncell].col = col;
            cells[ncell].span = span;
            cells[ncell].row = r;
            ncell++;
            col += span;
        }
        if (col > ncols) ncols = col;
    }
    if (!ncols) {
        *content_h = y - (b->y + b->bt + b->pt);
        free(cells);
        return;
    }
    for (int i = 0; i < ncell; i++) {
        float mn, mx;
        intrinsic(c, cells[i].node, cells[i].node->style, &mn, &mx);
        if (cells[i].span == 1) {
            if (mn > colmin[cells[i].col]) colmin[cells[i].col] = mn;
            if (mx > colmax[cells[i].col]) colmax[cells[i].col] = mx;
            if (cells[i].node->style->width.unit == U_PCT && inner > 0) {
                float w = inner * cells[i].node->style->width.v / 100;
                if (w > colmin[cells[i].col]) colmin[cells[i].col] = w;
                colmax[cells[i].col] = colmin[cells[i].col];
            }
        }
    }
    for (int i = 0; i < ncell; i++) {
        if (cells[i].span > 1) {
            float mn, mx, have = 0;
            intrinsic(c, cells[i].node, cells[i].node->style, &mn, &mx);
            for (int k = 0; k < cells[i].span; k++) have += colmin[cells[i].col + k];
            if (mn > have) {
                for (int k = 0; k < cells[i].span; k++) colmin[cells[i].col + k] += (mn - have) / cells[i].span;
            }
            have = 0;
            for (int k = 0; k < cells[i].span; k++) have += colmax[cells[i].col + k];
            if (mx > have) {
                for (int k = 0; k < cells[i].span; k++) colmax[cells[i].col + k] += (mx - have) / cells[i].span;
            }
        }
    }
    {
        float summin = 0, summax = 0, avail = inner - spacing * (ncols + 1);
        for (int i = 0; i < ncols; i++) {
            if (colmax[i] < colmin[i]) colmax[i] = colmin[i];
            summin += colmin[i];
            summax += colmax[i];
        }
        if (summax <= avail) {
            float extra = is_auto(b->st->width) ? 0 : avail - summax;
            for (int i = 0; i < ncols; i++) colw[i] = colmax[i] + (summax > 0 ? extra * colmax[i] / summax : extra / ncols);
            if (is_auto(b->st->width) && !b->parent) (void)0;
        } else if (summin >= avail) {
            for (int i = 0; i < ncols; i++) colw[i] = colmin[i];
        } else {
            float span = summax - summin;
            for (int i = 0; i < ncols; i++) colw[i] = colmin[i] + (avail - summin) * (colmax[i] - colmin[i]) / (span > 0 ? span : 1);
        }
        if (is_auto(b->st->width)) {
            float total = spacing * (ncols + 1);
            for (int i = 0; i < ncols; i++) total += colw[i];
            if (total < inner) b->w = total + hframe(b);
        }
    }
    {
        int ci = 0;
        for (int r = 0; r < nr; r++) {
            float rowh = 0;
            int first = ci;
            box_t *rowbox = new_box(c, rows[r], rows[r]->style, BX_BLOCK);
            if (rowbox) {
                rowbox->x = left;
                rowbox->y = y;
                rowbox->w = b->w - hframe(b);
                add_child(b, rowbox);
            }
            for (; ci < ncell && cells[ci].row == r; ci++) {
                float x = left + spacing, w = 0, h;
                for (int k = 0; k < cells[ci].col; k++) x += colw[k] + spacing;
                for (int k = 0; k < cells[ci].span; k++) w += colw[cells[ci].col + k] + (k ? spacing : 0);
                {
                    css_style_t *copy = (css_style_t *)arena_alloc(&c->L->arena, sizeof(css_style_t));
                    if (!copy) continue;
                    *copy = *cells[ci].node->style;
                    copy->width.unit = U_PX;
                    copy->width.v = w;
                    copy->box_sizing = 1;
                    copy->margin[0].v = copy->margin[1].v = copy->margin[2].v = copy->margin[3].v = 0;
                    h = layout_block(c, rowbox ? rowbox : b, cells[ci].node, copy, x, y, w, -1, 0, 0);
                    if ((rowbox ? rowbox : b)->last) (rowbox ? rowbox : b)->last->st = cells[ci].node->style;
                }
                if (h > rowh) rowh = h;
            }
            /* equalize cell heights and apply vertical-align */
            if (rowbox) {
                rowbox->h = rowh;
                for (box_t *cb = rowbox->first; cb; cb = cb->next) {
                    float dy = 0, slack = rowh - cb->h;
                    if (cb->st->vertical_align == VA_MIDDLE) dy = slack / 2;
                    else if (cb->st->vertical_align == VA_BOTTOM) dy = slack;
                    cb->h = rowh;
                    if (dy > 0) {
                        box_t *stack[512];
                        int sp = 0;
                        for (box_t *g = cb->first; g; g = g->next) stack[sp++] = g;
                        for (frag_t *f = cb->frags; f; f = f->next) {
                            f->y += dy;
                            f->baseline += dy;
                        }
                        while (sp) {
                            box_t *q = stack[--sp];
                            q->y += dy;
                            for (frag_t *f = q->frags; f; f = f->next) {
                                f->y += dy;
                                f->baseline += dy;
                            }
                            for (box_t *g = q->first; g && sp < 512; g = g->next) stack[sp++] = g;
                        }
                    }
                }
            }
            (void)first;
            y += rowh + spacing;
        }
    }
    free(cells);
    *content_h = y - (b->y + b->bt + b->pt);
}

/* ---- entry points ------------------------------------------------------- */

/* Scrollable height: the lowest box edge (content may overflow <html>). */
static float extent(box_t *b, float bottom) {
    for (box_t *k = b->first; k; k = k->next) {
        if (k->positioned != 2 && k->y + k->h + k->rel_y > bottom && k->h < 1e7f) bottom = k->y + k->h + k->rel_y;
        bottom = extent(k, bottom);
    }
    return bottom;
}

layout_t *layout_document(dom_doc_t *doc, float viewport_w, float viewport_h, image_size_fn img, void *img_ctx) {
    layout_t *L = (layout_t *)calloc(1, sizeof(layout_t));
    lctx_t *c = (lctx_t *)calloc(1, sizeof(lctx_t));
    box_t *root;
    bfc_t *bfc = (bfc_t *)calloc(1, sizeof(bfc_t));
    if (!L || !c || !bfc) {
        free(L);
        free(c);
        free(bfc);
        return 0;
    }
    c->pct_h = -1;
    L->viewport_w = viewport_w;
    L->viewport_h = viewport_h;
    c->L = L;
    c->img = img;
    c->img_ctx = img_ctx;
    root = new_box(c, doc->root, 0, BX_BLOCK);
    L->root = root;
    if (root && doc->html && doc->html->style) {
        float h = layout_block(c, root, doc->html, doc->html->style, 0, 0, viewport_w, viewport_h, bfc, 0);
        L->height = extent(root, h);
        L->width = viewport_w;
    }
    free(bfc);
    free(c);
    return L;
}

void layout_free(layout_t *l) {
    if (!l) return;
    arena_free(&l->arena);
    free(l);
}

static dom_node_t *hit(box_t *b, float x, float y, float ox, float oy) {
    dom_node_t *found = 0;
    ox += b->rel_x;
    oy += b->rel_y;
    for (box_t *k = b->first; k; k = k->next) {
        dom_node_t *h = hit(k, x, y, ox, oy);
        if (h) found = h;
    }
    if (found) return found;
    for (frag_t *f = b->frags; f; f = f->next) {
        if (f->link && x >= f->x + ox && x < f->x + ox + f->w && y >= f->y + oy && y < f->y + oy + f->h) return f->link;
    }
    if (b->link && x >= b->x + ox && x < b->x + ox + b->w && y >= b->y + oy && y < b->y + oy + b->h) return b->link;
    if (b->node && b->node->tag == T_A && dom_attr(b->node, "href") && x >= b->x + ox && x < b->x + ox + b->w &&
        y >= b->y + oy && y < b->y + oy + b->h) {
        return b->node;
    }
    return 0;
}

dom_node_t *layout_hit_link(layout_t *l, float x, float y) {
    return l && l->root ? hit(l->root, x, y, 0, 0) : 0;
}

static dom_node_t *hit_el(box_t *b, float x, float y, float ox, float oy) {
    dom_node_t *found = 0;
    ox += b->rel_x;
    oy += b->rel_y;
    /* later siblings paint on top, so the last match wins */
    for (box_t *k = b->first; k; k = k->next) {
        dom_node_t *h = hit_el(k, x, y, ox, oy);
        if (h) found = h;
    }
    if (found) return found;
    for (frag_t *f = b->frags; f; f = f->next) {
        if (f->node && x >= f->x + ox && x < f->x + ox + f->w && y >= f->y + oy && y < f->y + oy + f->h) {
            return f->node->type == N_TEXT ? f->node->parent : f->node;
        }
    }
    if (b->node && x >= b->x + ox && x < b->x + ox + b->w && y >= b->y + oy && y < b->y + oy + b->h) return b->node;
    return 0;
}

dom_node_t *layout_hit_element(layout_t *l, float x, float y) {
    return l && l->root ? hit_el(l->root, x, y, 0, 0) : 0;
}

static box_t *find_box(box_t *b, dom_node_t *n, float ox, float oy, float *rx, float *ry) {
    ox += b->rel_x;
    oy += b->rel_y;
    if (b->node == n) {
        *rx = b->x + ox;
        *ry = b->y + oy;
        return b;
    }
    for (box_t *k = b->first; k; k = k->next) {
        box_t *r = find_box(k, n, ox, oy, rx, ry);
        if (r) return r;
    }
    return 0;
}

int layout_box_rect(layout_t *l, dom_node_t *n, float *x, float *y, float *w, float *h) {
    box_t *b;
    if (!l || !l->root || !n) return 0;
    b = find_box(l->root, n, 0, 0, x, y);
    if (!b) return 0;
    *w = b->w;
    *h = b->h;
    return 1;
}
