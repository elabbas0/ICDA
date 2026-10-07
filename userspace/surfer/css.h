#ifndef SURFER_CSS_H
#define SURFER_CSS_H

#include <stdint.h>
#include <stddef.h>
#include "dom.h"

/* ---- computed style ----------------------------------------------------- */

enum { U_AUTO = 0, U_PX, U_PCT, U_NONE };

/* A length.  U_PCT is v percent of the reference plus px (calc(100% - 20px)),
 * kept within lo / hi when bounds says so (min(100%, 640px), clamp()). */
typedef struct {
    float   v;
    uint8_t unit;
    uint8_t bounds;           /* 1: lo applies, 2: hi applies */
    float   px, lo, hi;
} css_len_t;

static inline float css_pct(css_len_t l, float base) {
    float v = base * l.v / 100 + l.px;
    if ((l.bounds & 1) && v < l.lo) v = l.lo;
    if ((l.bounds & 2) && v > l.hi) v = l.hi;
    return v;
}

enum {
    D_INLINE = 0, D_BLOCK, D_INLINE_BLOCK, D_LIST_ITEM, D_NONE, D_TABLE, D_INLINE_TABLE, D_TABLE_ROW,
    D_TABLE_CELL, D_TABLE_ROW_GROUP, D_TABLE_HEADER_GROUP, D_TABLE_FOOTER_GROUP, D_TABLE_CAPTION,
    D_TABLE_COLUMN, D_TABLE_COLUMN_GROUP, D_FLEX, D_INLINE_FLEX, D_GRID, D_INLINE_GRID, D_CONTENTS
};
enum { P_STATIC = 0, P_RELATIVE, P_ABSOLUTE, P_FIXED, P_STICKY };
enum { F_NONE = 0, F_LEFT, F_RIGHT };
enum { WS_NORMAL = 0, WS_PRE, WS_NOWRAP, WS_PRE_WRAP, WS_PRE_LINE };
enum { TA_LEFT = 0, TA_RIGHT, TA_CENTER, TA_JUSTIFY };
enum { TT_NONE = 0, TT_UPPER, TT_LOWER, TT_CAPITALIZE };
enum { LS_NONE = 0, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_LOWER_ALPHA, LS_UPPER_ALPHA, LS_LOWER_ROMAN, LS_UPPER_ROMAN };
enum { VA_BASELINE = 0, VA_TOP, VA_MIDDLE, VA_BOTTOM, VA_SUB, VA_SUPER, VA_TEXT_TOP, VA_TEXT_BOTTOM };
enum { OV_VISIBLE = 0, OV_HIDDEN, OV_SCROLL, OV_AUTO };
enum { BS_NONE = 0, BS_SOLID, BS_DASHED, BS_DOTTED, BS_DOUBLE, BS_OTHER };
enum { TD_UNDERLINE = 1, TD_LINE_THROUGH = 2, TD_OVERLINE = 4 };
enum { FD_ROW = 0, FD_ROW_REVERSE, FD_COLUMN, FD_COLUMN_REVERSE };
enum { JC_START = 0, JC_END, JC_CENTER, JC_SPACE_BETWEEN, JC_SPACE_AROUND, JC_SPACE_EVENLY, JC_STRETCH, JC_BASELINE, JC_AUTO };
enum { LH_NORMAL = 0, LH_PX, LH_MULT };

typedef struct css_var {
    const char     *name;
    const char     *value;
    struct css_var *next;
} css_var_t;

typedef struct css_style {
    uint8_t  display, position, float_, clear, box_sizing, overflow, visibility, white_space;
    uint8_t  text_align, text_transform, font_italic, list_style, list_inside, vertical_align;
    uint8_t  decoration, flex_dir, flex_wrap, justify, align_items, align_self, align_content;
    uint8_t  lh_type, z_auto, border_style[4], table_layout_fixed, border_collapse, cursor_pointer;
    uint8_t  line_clamp;                /* -webkit-line-clamp: lines shown, 0 none */
    uint8_t  text_ellipsis;             /* text-overflow: ellipsis */
    uint16_t font_weight;
    float    font_size, line_height, letter_spacing, word_spacing, opacity;
    float    lh_value;                  /* specified line-height: multiplier or px */
    css_len_t text_indent;
    uint32_t color, bg_color, border_color[4];
    css_len_t width, height, min_w, min_h, max_w, max_h;
    css_len_t margin[4], padding[4], inset[4];   /* top right bottom left */
    float    border_w[4], radius;
    css_len_t flex_basis;
    float    flex_grow, flex_shrink, gap_row, gap_col;
    int      order, z_index;
    int      grid_cols;                 /* repeat count for simple grid-template-columns */
    const char *font_family;
    const char *bg_image;               /* url, unresolved */
    uint8_t  bg_repeat;                 /* 0 repeat, 1 no-repeat, 2 repeat-x, 3 repeat-y */
    uint8_t  bg_size_mode;              /* 0 auto, 1 cover, 2 contain, 3 bg_size */
    css_len_t bg_size[2], bg_pos[2];    /* x, y; positions as % or px */
    const char *content;                /* ::before / ::after text */
    css_var_t  *vars;
    struct css_style *before, *after;   /* pseudo-element styles */
} css_style_t;

/* ---- stylesheets -------------------------------------------------------- */

typedef struct css_sheet css_sheet_t;

css_sheet_t *css_sheet_new(void);
/* Parses CSS text; base_url resolves @import, viewport_w drives @media. */
void         css_parse(css_sheet_t *sheet, const char *text, size_t len, int origin_author, int viewport_w);
void         css_sheet_free(css_sheet_t *sheet);
/* @import URLs found while parsing (caller fetches and parses them). */
int          css_imports(css_sheet_t *sheet, const char ***urls);

/* Computes styles for the whole document (author sheets after the UA sheet). */
void         css_cascade(dom_doc_t *doc, css_sheet_t **sheets, int n, int viewport_w, int viewport_h);

uint32_t     css_parse_color(const char *s, size_t n, int *ok);

/* Compiled selector lists for querySelector() and matches().  Parsing returns
 * 0 for an invalid selector. */
typedef struct css_selector_list css_selector_list_t;
css_selector_list_t *css_selector_parse(const char *text);
int                  css_selector_matches(const css_selector_list_t *l, dom_node_t *el);
void                 css_selector_free(css_selector_list_t *l);

#endif
