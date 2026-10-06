/* CSS for Surfer: stylesheet parsing (rules, selectors, @media, @import),
 * selector matching with a per-sheet index, and the cascade that produces a
 * computed style for every element. */
#include "css.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- properties --------------------------------------------------------- */

enum {
    PR_NONE = 0, PR_DISPLAY, PR_POSITION, PR_FLOAT, PR_CLEAR, PR_BOX_SIZING, PR_OVERFLOW, PR_OVERFLOW_X,
    PR_OVERFLOW_Y, PR_VISIBILITY, PR_WHITE_SPACE, PR_TEXT_ALIGN, PR_TEXT_TRANSFORM, PR_FONT_STYLE,
    PR_FONT_WEIGHT, PR_FONT_SIZE, PR_FONT_FAMILY, PR_FONT, PR_LINE_HEIGHT, PR_LETTER_SPACING,
    PR_WORD_SPACING, PR_TEXT_INDENT, PR_COLOR, PR_BACKGROUND, PR_BACKGROUND_COLOR, PR_BACKGROUND_IMAGE,
    PR_WIDTH, PR_HEIGHT, PR_MIN_WIDTH, PR_MIN_HEIGHT, PR_MAX_WIDTH, PR_MAX_HEIGHT,
    PR_MARGIN, PR_MARGIN_TOP, PR_MARGIN_RIGHT, PR_MARGIN_BOTTOM, PR_MARGIN_LEFT,
    PR_PADDING, PR_PADDING_TOP, PR_PADDING_RIGHT, PR_PADDING_BOTTOM, PR_PADDING_LEFT,
    PR_TOP, PR_RIGHT, PR_BOTTOM, PR_LEFT, PR_INSET,
    PR_BORDER, PR_BORDER_TOP, PR_BORDER_RIGHT, PR_BORDER_BOTTOM, PR_BORDER_LEFT,
    PR_BORDER_WIDTH, PR_BORDER_STYLE, PR_BORDER_COLOR, PR_BORDER_RADIUS,
    PR_BORDER_TOP_WIDTH, PR_BORDER_RIGHT_WIDTH, PR_BORDER_BOTTOM_WIDTH, PR_BORDER_LEFT_WIDTH,
    PR_BORDER_TOP_COLOR, PR_BORDER_RIGHT_COLOR, PR_BORDER_BOTTOM_COLOR, PR_BORDER_LEFT_COLOR,
    PR_BORDER_TOP_STYLE, PR_BORDER_RIGHT_STYLE, PR_BORDER_BOTTOM_STYLE, PR_BORDER_LEFT_STYLE,
    PR_LIST_STYLE, PR_LIST_STYLE_TYPE, PR_LIST_STYLE_POSITION, PR_VERTICAL_ALIGN, PR_TEXT_DECORATION,
    PR_TEXT_DECORATION_LINE, PR_OPACITY, PR_Z_INDEX, PR_FLEX, PR_FLEX_DIRECTION, PR_FLEX_WRAP, PR_FLEX_FLOW,
    PR_FLEX_GROW, PR_FLEX_SHRINK, PR_FLEX_BASIS, PR_JUSTIFY_CONTENT, PR_ALIGN_ITEMS, PR_ALIGN_SELF,
    PR_ALIGN_CONTENT, PR_ORDER, PR_GAP, PR_ROW_GAP, PR_COLUMN_GAP, PR_GRID_TEMPLATE_COLUMNS, PR_CONTENT,
    PR_TABLE_LAYOUT, PR_BORDER_COLLAPSE, PR_CURSOR, PR_BACKGROUND_SIZE, PR_BACKGROUND_REPEAT,
    PR_BACKGROUND_POSITION, PR_COUNT
};

static const char *const prop_names[PR_COUNT] = {
    "", "display", "position", "float", "clear", "box-sizing", "overflow", "overflow-x",
    "overflow-y", "visibility", "white-space", "text-align", "text-transform", "font-style",
    "font-weight", "font-size", "font-family", "font", "line-height", "letter-spacing",
    "word-spacing", "text-indent", "color", "background", "background-color", "background-image",
    "width", "height", "min-width", "min-height", "max-width", "max-height",
    "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
    "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
    "top", "right", "bottom", "left", "inset",
    "border", "border-top", "border-right", "border-bottom", "border-left",
    "border-width", "border-style", "border-color", "border-radius",
    "border-top-width", "border-right-width", "border-bottom-width", "border-left-width",
    "border-top-color", "border-right-color", "border-bottom-color", "border-left-color",
    "border-top-style", "border-right-style", "border-bottom-style", "border-left-style",
    "list-style", "list-style-type", "list-style-position", "vertical-align", "text-decoration",
    "text-decoration-line", "opacity", "z-index", "flex", "flex-direction", "flex-wrap", "flex-flow",
    "flex-grow", "flex-shrink", "flex-basis", "justify-content", "align-items", "align-self",
    "align-content", "order", "gap", "row-gap", "column-gap", "grid-template-columns", "content",
    "table-layout", "border-collapse", "cursor", "background-size", "background-repeat",
    "background-position"
};

#define PR_CUSTOM 0xFFFF

static int prop_lookup(const char *s, size_t n) {
    for (int i = 1; i < PR_COUNT; i++) {
        if (strlen(prop_names[i]) == n && memcmp(prop_names[i], s, n) == 0) return i;
    }
    return PR_NONE;
}

/* ---- structures --------------------------------------------------------- */

enum { S_TAG = 1, S_UNIVERSAL, S_ID, S_CLASS, S_ATTR, S_PSEUDO };
enum { A_EXISTS = 0, A_EQ, A_INCLUDES, A_DASH, A_PREFIX, A_SUFFIX, A_SUBSTR };
enum {
    PC_FIRST_CHILD = 1, PC_LAST_CHILD, PC_ONLY_CHILD, PC_NTH_CHILD, PC_NTH_LAST_CHILD, PC_FIRST_OF_TYPE,
    PC_LAST_OF_TYPE, PC_NTH_OF_TYPE, PC_ROOT, PC_EMPTY, PC_LINK, PC_NOT, PC_NEVER, PC_ALWAYS, PC_IS
};
enum { C_NONE = 0, C_DESC, C_CHILD, C_ADJ, C_SIB };

typedef struct compound compound_t;

typedef struct {
    uint8_t     type, op;
    uint16_t    tag;
    const char *name, *value;
    int         a, b;
    uint32_t    hash;         /* key hash for id, class and tag */
    compound_t *sub;          /* :not() / :is() argument (list) */
    int         nsub;
} simple_t;

struct compound {
    simple_t *s;
    int       n;
    uint8_t   comb;           /* combinator to the compound on the left */
};

typedef struct {
    compound_t *c;
    int         n;
    uint32_t    spec;
    uint8_t     pseudo_el;    /* 0, 1 ::before, 2 ::after */
    uint8_t     nanc;
    uint32_t    anc[4];       /* keys that must appear on some ancestor */
} selector_t;

typedef struct {
    uint16_t    prop;
    uint8_t     important;
    const char *name;         /* custom property name */
    const char *value;
} decl_t;

typedef struct {
    selector_t *sels;
    int         nsel;
    decl_t     *d;
    int         nd;
} rule_t;

typedef struct {
    int rule, sel;
} ref_t;

typedef struct bucket {
    const char    *key;
    ref_t         *refs;
    int            n, cap;
    struct bucket *next;
} bucket_t;

#define HASH 512

typedef struct {
    bucket_t   *by_id[HASH], *by_class[HASH], *by_tag[HASH], *by_attr[HASH];
    ref_t      *universal;
    int         nuniversal, capuniversal;
    int         used;
} index_t;

struct css_sheet {
    arena_t     arena;
    rule_t     *rules;
    int         nrules, cap;
    int         origin_author;
    index_t     idx[2];          /* [0] ordinary rules, [1] ::before / ::after */
    int         indexed;
    const char *imports[32];
    int         nimports;
};

/* ---- small helpers ------------------------------------------------------ */

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           (unsigned char)c >= 0x80 || c == '\\';
}

static uint32_t hash_str(const char *s, size_t n) {
    uint32_t h = 2166136261U;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619U;
    return h;
}

/* Keys for ids, classes and tags share one hash space, separated by kind. */
static uint32_t key_hash(int kind, const char *s) {
    return hash_str(s, strlen(s)) * 3U + (uint32_t)kind;
}

/* Counting Bloom filter of the keys on the current element's ancestors. */
static uint8_t bloom[8192];

static void bloom_add(uint32_t h, int d) {
    bloom[h & 8191] = (uint8_t)(bloom[h & 8191] + d);
    bloom[(h >> 13) & 8191] = (uint8_t)(bloom[(h >> 13) & 8191] + d);
}

static int bloom_has(uint32_t h) {
    return bloom[h & 8191] && bloom[(h >> 13) & 8191];
}

static int ieq(const char *a, size_t n, const char *lit) {
    size_t l = strlen(lit);
    if (n != l) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != lit[i]) return 0;
    }
    return 1;
}

static char *dup_lower(arena_t *a, const char *s, size_t n) {
    char *p = arena_strndup(a, s, n);
    for (size_t i = 0; p && i < n; i++) {
        if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32;
    }
    return p;
}

/* Copies an identifier, resolving simple backslash escapes. */
static const char *read_ident(arena_t *a, const char **sp, const char *end, int lower) {
    const char *s = *sp;
    char buf[256];
    size_t n = 0;
    while (s < end && is_ident(*s)) {
        if (*s == '\\' && s + 1 < end) {
            s++;
            if (n < sizeof(buf) - 1) buf[n++] = *s;
            s++;
            continue;
        }
        if (n < sizeof(buf) - 1) buf[n++] = (lower && *s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
        s++;
    }
    *sp = s;
    return arena_strndup(a, buf, n);
}

/* Skips a balanced block starting at '{' (s points at '{'). */
static const char *skip_block(const char *s, const char *end) {
    int depth = 0;
    while (s < end) {
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (s < end && *s != q) {
                if (*s == '\\') s++;
                s++;
            }
        } else if (*s == '{') {
            depth++;
        } else if (*s == '}') {
            if (--depth == 0) return s + 1;
        }
        s++;
    }
    return end;
}

/* ---- selectors ---------------------------------------------------------- */

static int parse_compound_list(css_sheet_t *sh, const char **sp, const char *end, compound_t **out, int *n,
                               char stop);

static int parse_nth(const char *s, const char *end, int *a, int *b) {
    char buf[32];
    size_t n = 0;
    while (s < end && n < sizeof(buf) - 1) {
        if (!is_ws(*s)) buf[n++] = (*s >= 'A' && *s <= 'Z') ? *s + 32 : *s;
        s++;
    }
    buf[n] = 0;
    if (strcmp(buf, "odd") == 0) { *a = 2; *b = 1; return 0; }
    if (strcmp(buf, "even") == 0) { *a = 2; *b = 0; return 0; }
    {
        char *npos = strchr(buf, 'n');
        if (!npos) {
            *a = 0;
            *b = atoi(buf);
            return 0;
        }
        *npos = 0;
        if (buf[0] == 0 || strcmp(buf, "+") == 0) *a = 1;
        else if (strcmp(buf, "-") == 0) *a = -1;
        else *a = atoi(buf);
        *b = npos[1] ? atoi(npos + 1) : 0;
    }
    return 0;
}

/* Parses one compound selector; returns -1 if it uses something unsupported
 * in a way that can never match (the rule is then dropped). */
static int parse_compound(css_sheet_t *sh, const char **sp, const char *end, compound_t *c, uint32_t *spec,
                          uint8_t *pseudo_el) {
    simple_t tmp[16];
    int n = 0;
    const char *s = *sp;
    int ok = 1;
    while (s < end && n < 16) {
        simple_t *x = &tmp[n];
        memset(x, 0, sizeof(*x));
        if (*s == '*') {
            x->type = S_UNIVERSAL;
            s++;
        } else if (*s == '#') {
            s++;
            x->type = S_ID;
            x->name = read_ident(&sh->arena, &s, end, 0);
            x->hash = key_hash(S_ID, x->name);
            *spec += 0x10000;
        } else if (*s == '.') {
            s++;
            x->type = S_CLASS;
            x->name = read_ident(&sh->arena, &s, end, 0);
            x->hash = key_hash(S_CLASS, x->name);
            *spec += 0x100;
        } else if (*s == '[') {
            const char *q;
            s++;
            while (s < end && is_ws(*s)) s++;
            x->type = S_ATTR;
            x->name = read_ident(&sh->arena, &s, end, 1);
            while (s < end && is_ws(*s)) s++;
            x->op = A_EXISTS;
            if (s < end && *s != ']') {
                if (*s == '=') x->op = A_EQ;
                else if (*s == '~') x->op = A_INCLUDES;
                else if (*s == '|') x->op = A_DASH;
                else if (*s == '^') x->op = A_PREFIX;
                else if (*s == '$') x->op = A_SUFFIX;
                else if (*s == '*') x->op = A_SUBSTR;
                while (s < end && *s != '=') s++;
                if (s < end) s++;
                while (s < end && is_ws(*s)) s++;
                if (s < end && (*s == '"' || *s == '\'')) {
                    char qc = *s++;
                    q = s;
                    while (s < end && *s != qc) s++;
                    x->value = arena_strndup(&sh->arena, q, (size_t)(s - q));
                    if (s < end) s++;
                } else {
                    x->value = read_ident(&sh->arena, &s, end, 0);
                }
            }
            while (s < end && *s != ']') s++;
            if (s < end) s++;
            *spec += 0x100;
        } else if (*s == ':') {
            const char *name;
            size_t nl;
            int element = 0;
            s++;
            if (s < end && *s == ':') {
                element = 1;
                s++;
            }
            name = s;
            while (s < end && is_ident(*s)) s++;
            nl = (size_t)(s - name);
            x->type = S_PSEUDO;
            if (element || ieq(name, nl, "before") || ieq(name, nl, "after") || ieq(name, nl, "first-line") ||
                ieq(name, nl, "first-letter")) {
                if (ieq(name, nl, "before")) *pseudo_el = 1;
                else if (ieq(name, nl, "after")) *pseudo_el = 2;
                else ok = 0;
                if (s < end && *s == '(') s = skip_block(s, end);   /* never happens for these */
                *spec += 1;
                continue;
            }
            *spec += 0x100;
            if (ieq(name, nl, "first-child")) x->op = PC_FIRST_CHILD;
            else if (ieq(name, nl, "last-child")) x->op = PC_LAST_CHILD;
            else if (ieq(name, nl, "only-child")) x->op = PC_ONLY_CHILD;
            else if (ieq(name, nl, "first-of-type")) x->op = PC_FIRST_OF_TYPE;
            else if (ieq(name, nl, "last-of-type")) x->op = PC_LAST_OF_TYPE;
            else if (ieq(name, nl, "root")) x->op = PC_ROOT;
            else if (ieq(name, nl, "empty")) x->op = PC_EMPTY;
            else if (ieq(name, nl, "link") || ieq(name, nl, "any-link")) x->op = PC_LINK;
            else if (ieq(name, nl, "visited") || ieq(name, nl, "hover") || ieq(name, nl, "active") ||
                     ieq(name, nl, "focus") || ieq(name, nl, "focus-within") || ieq(name, nl, "focus-visible") ||
                     ieq(name, nl, "checked") || ieq(name, nl, "target") || ieq(name, nl, "placeholder-shown") ||
                     ieq(name, nl, "invalid") || ieq(name, nl, "indeterminate")) {
                x->op = PC_NEVER;
            } else if (ieq(name, nl, "enabled") || ieq(name, nl, "defined")) {
                x->op = PC_ALWAYS;
            } else if (s < end && *s == '(' &&
                       (ieq(name, nl, "nth-child") || ieq(name, nl, "nth-last-child") || ieq(name, nl, "nth-of-type") ||
                        ieq(name, nl, "nth-last-of-type"))) {
                const char *arg = s + 1, *close = arg;
                while (close < end && *close != ')') close++;
                parse_nth(arg, close, &x->a, &x->b);
                x->op = ieq(name, nl, "nth-child") ? PC_NTH_CHILD : ieq(name, nl, "nth-last-child") ? PC_NTH_LAST_CHILD : PC_NTH_OF_TYPE;
                if (ieq(name, nl, "nth-last-of-type")) x->op = PC_NEVER;
                s = close < end ? close + 1 : end;
            } else if (s < end && *s == '(' && (ieq(name, nl, "not") || ieq(name, nl, "is") || ieq(name, nl, "where") ||
                                                ieq(name, nl, "matches"))) {
                s++;
                x->op = ieq(name, nl, "not") ? PC_NOT : PC_IS;
                if (parse_compound_list(sh, &s, end, &x->sub, &x->nsub, ')') != 0) {
                    x->op = x->op == PC_NOT ? PC_ALWAYS : PC_NEVER;
                }
                if (s < end && *s == ')') s++;
            } else {
                if (s < end && *s == '(') {
                    int depth = 0;
                    while (s < end) {
                        if (*s == '(') depth++;
                        else if (*s == ')' && --depth == 0) {
                            s++;
                            break;
                        }
                        s++;
                    }
                }
                x->op = PC_NEVER;
            }
        } else if (is_ident(*s)) {
            const char *name = read_ident(&sh->arena, &s, end, 1);
            x->type = S_TAG;
            x->name = name;
            x->hash = key_hash(S_TAG, name);
            x->tag = (uint16_t)tag_lookup(name, strlen(name));
            *spec += 1;
        } else {
            break;
        }
        n++;
    }
    *sp = s;
    if (!n) return -1;
    c->s = (simple_t *)arena_alloc(&sh->arena, sizeof(simple_t) * (size_t)n);
    if (!c->s) return -1;
    memcpy(c->s, tmp, sizeof(simple_t) * (size_t)n);
    c->n = n;
    return ok ? 0 : -1;
}

/* Parses a comma-separated list of compounds (used by :not and :is). */
static int parse_compound_list(css_sheet_t *sh, const char **sp, const char *end, compound_t **out, int *n,
                               char stop) {
    compound_t tmp[8];
    int k = 0;
    const char *s = *sp;
    while (s < end && *s != stop && k < 8) {
        uint32_t spec = 0;
        uint8_t pe = 0;
        while (s < end && is_ws(*s)) s++;
        if (parse_compound(sh, &s, end, &tmp[k], &spec, &pe) != 0) return -1;
        k++;
        while (s < end && is_ws(*s)) s++;
        if (s < end && *s == ',') s++;
        else if (s < end && *s != stop) return -1;        /* complex selector inside: unsupported */
    }
    *sp = s;
    *out = (compound_t *)arena_alloc(&sh->arena, sizeof(compound_t) * (size_t)(k ? k : 1));
    if (!*out) return -1;
    memcpy(*out, tmp, sizeof(compound_t) * (size_t)k);
    *n = k;
    return 0;
}

static int parse_selector(css_sheet_t *sh, const char *s, const char *end, selector_t *out) {
    compound_t tmp[32];
    int n = 0;
    uint8_t comb = C_NONE;
    memset(out, 0, sizeof(*out));
    while (s < end && is_ws(*s)) s++;
    while (s < end && n < 32) {
        tmp[n].comb = comb;
        if (parse_compound(sh, &s, end, &tmp[n], &out->spec, &out->pseudo_el) != 0) return -1;
        n++;
        {
            int ws = 0;
            while (s < end && is_ws(*s)) {
                s++;
                ws = 1;
            }
            if (s >= end) break;
            if (*s == '>') comb = C_CHILD, s++;
            else if (*s == '+') comb = C_ADJ, s++;
            else if (*s == '~') comb = C_SIB, s++;
            else if (ws) comb = C_DESC;
            else return -1;
            while (s < end && is_ws(*s)) s++;
        }
    }
    if (!n) return -1;
    out->c = (compound_t *)arena_alloc(&sh->arena, sizeof(compound_t) * (size_t)n);
    if (!out->c) return -1;
    memcpy(out->c, tmp, sizeof(compound_t) * (size_t)n);
    out->n = n;
    /* ancestor keys: compounds reached only through descendant/child combinators */
    for (int i = n - 1; i > 0 && out->nanc < 4; i--) {
        if (tmp[i].comb != C_DESC && tmp[i].comb != C_CHILD) break;
        for (int k = 0; k < tmp[i - 1].n && out->nanc < 4; k++) {
            const simple_t *x = &tmp[i - 1].s[k];
            if (x->type == S_ID || x->type == S_CLASS || x->type == S_TAG) out->anc[out->nanc++] = x->hash;
        }
    }
    return 0;
}

/* ---- declarations ------------------------------------------------------- */

static int parse_decls(css_sheet_t *sh, const char *s, const char *end, decl_t **out) {
    decl_t tmp[256];
    int n = 0;
    while (s < end && n < 256) {
        const char *name, *ne, *v, *ve;
        while (s < end && (is_ws(*s) || *s == ';')) s++;
        if (s >= end) break;
        name = s;
        while (s < end && *s != ':' && *s != ';') s++;
        if (s >= end || *s != ':') continue;
        ne = s;
        while (ne > name && is_ws(ne[-1])) ne--;
        s++;
        v = s;
        {
            int depth = 0;
            while (s < end) {
                if (*s == '"' || *s == '\'') {
                    char q = *s++;
                    while (s < end && *s != q) {
                        if (*s == '\\') s++;
                        s++;
                    }
                } else if (*s == '(') depth++;
                else if (*s == ')') depth--;
                else if (*s == ';' && depth <= 0) break;
                s++;
            }
        }
        ve = s;
        while (v < ve && is_ws(*v)) v++;
        while (ve > v && is_ws(ve[-1])) ve--;
        {
            decl_t *d = &tmp[n];
            const char *bang = 0;
            memset(d, 0, sizeof(*d));
            for (const char *p = v; p + 9 <= ve; p++) {
                if (*p == '!' && ieq(p + 1, 9 <= (size_t)(ve - p - 1) ? 9 : 0, "important")) {
                    bang = p;
                    break;
                }
            }
            if (bang) {
                d->important = 1;
                ve = bang;
                while (ve > v && is_ws(ve[-1])) ve--;
            }
            if (ne - name > 2 && name[0] == '-' && name[1] == '-') {
                d->prop = PR_CUSTOM;
                d->name = arena_strndup(&sh->arena, name, (size_t)(ne - name));
            } else {
                char lname[64];
                size_t ln = (size_t)(ne - name) < sizeof(lname) ? (size_t)(ne - name) : sizeof(lname) - 1;
                for (size_t i = 0; i < ln; i++) lname[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
                d->prop = (uint16_t)prop_lookup(lname, ln);
                if (d->prop == PR_NONE) continue;
            }
            d->value = arena_strndup(&sh->arena, v, (size_t)(ve - v));
            n++;
        }
    }
    *out = (decl_t *)arena_alloc(&sh->arena, sizeof(decl_t) * (size_t)(n ? n : 1));
    if (*out) memcpy(*out, tmp, sizeof(decl_t) * (size_t)n);
    return *out ? n : 0;
}

/* ---- media queries ------------------------------------------------------ */

static float media_px(const char *s, const char *end) {
    float v = (float)strtod(s, 0);
    while (s < end && ((*s >= '0' && *s <= '9') || *s == '.' || *s == '-')) s++;
    if (s + 2 <= end && (ieq(s, 2, "em") || ieq(s, 3, "rem"))) v *= 16;
    return v;
}

/* Evaluates a media query list against a screen of the given width. */
static int media_matches(const char *s, const char *end, int vw) {
    while (s < end) {
        const char *q = s, *qe;
        int result = 1, negate = 0;
        while (s < end && *s != ',') s++;
        qe = s;
        if (s < end) s++;
        while (q < qe) {
            while (q < qe && is_ws(*q)) q++;
            if (q >= qe) break;
            if (*q == '(') {
                const char *f = q + 1, *fe = f;
                while (fe < qe && *fe != ')') fe++;
                {
                    const char *colon = memchr(f, ':', (size_t)(fe - f));
                    if (colon) {
                        const char *fn = f;
                        size_t fl;
                        while (fn < colon && is_ws(*fn)) fn++;
                        fl = (size_t)(colon - fn);
                        while (fl && is_ws(fn[fl - 1])) fl--;
                        colon++;
                        while (colon < fe && is_ws(*colon)) colon++;
                        if (ieq(fn, fl, "min-width")) result &= vw >= media_px(colon, fe);
                        else if (ieq(fn, fl, "max-width")) result &= vw <= media_px(colon, fe);
                        else if (ieq(fn, fl, "prefers-color-scheme")) result &= (fe - colon >= 5 && ieq(colon, 5, "light"));
                        else if (ieq(fn, fl, "prefers-reduced-motion")) result &= (fe - colon >= 6 && ieq(colon, 6, "reduce"));
                        else if (ieq(fn, fl, "orientation")) result &= ieq(colon, 9, "landscape");
                        else if (ieq(fn, fl, "hover") || ieq(fn, fl, "any-hover")) result &= ieq(colon, 5, "hover");
                        else if (ieq(fn, fl, "pointer") || ieq(fn, fl, "any-pointer")) result &= ieq(colon, 4, "fine");
                        else if (ieq(fn, fl, "min-resolution") || ieq(fn, fl, "-webkit-min-device-pixel-ratio")) result &= 0;
                    } else {
                        /* range syntax "(width >= 600px)" */
                        const char *op = f;
                        while (op < fe && *op != '<' && *op != '>') op++;
                        if (op < fe) {
                            const char *num = op + 1;
                            int ge = *op == '>';
                            if (num < fe && *num == '=') num++;
                            while (num < fe && is_ws(*num)) num++;
                            result &= ge ? vw >= media_px(num, fe) : vw <= media_px(num, fe);
                        }
                    }
                }
                q = fe < qe ? fe + 1 : qe;
                continue;
            }
            {
                const char *w = q;
                while (q < qe && is_ident(*q)) q++;
                if (q == w) {
                    q++;
                    continue;
                }
                if (ieq(w, (size_t)(q - w), "not")) negate = 1;
                else if (ieq(w, (size_t)(q - w), "print") || ieq(w, (size_t)(q - w), "speech")) result = 0;
            }
        }
        if (result != negate) return 1;
    }
    return 0;
}

/* ---- sheet parsing ------------------------------------------------------ */

css_sheet_t *css_sheet_new(void) {
    return (css_sheet_t *)calloc(1, sizeof(css_sheet_t));
}

static void add_rule(css_sheet_t *sh, rule_t *r) {
    if (sh->nrules == sh->cap) {
        sh->cap = sh->cap ? sh->cap * 2 : 256;
        sh->rules = (rule_t *)realloc(sh->rules, sizeof(rule_t) * (size_t)sh->cap);
    }
    if (sh->rules) sh->rules[sh->nrules++] = *r;
    sh->indexed = 0;
}

static void parse_block(css_sheet_t *sh, const char *s, const char *end, int vw);

static void parse_rule(css_sheet_t *sh, const char *sel, const char *sel_end, const char *body, const char *body_end) {
    selector_t tmp[64];
    int n = 0;
    rule_t r;
    const char *s = sel;
    while (s < sel_end && n < 64) {
        const char *e = s;
        int depth = 0;
        while (e < sel_end) {
            if (*e == '(') depth++;
            else if (*e == ')') depth--;
            else if (*e == ',' && depth == 0) break;
            e++;
        }
        if (parse_selector(sh, s, e, &tmp[n]) == 0) n++;
        s = e < sel_end ? e + 1 : sel_end;
    }
    if (!n) return;
    memset(&r, 0, sizeof(r));
    r.sels = (selector_t *)arena_alloc(&sh->arena, sizeof(selector_t) * (size_t)n);
    if (!r.sels) return;
    memcpy(r.sels, tmp, sizeof(selector_t) * (size_t)n);
    r.nsel = n;
    r.nd = parse_decls(sh, body, body_end, &r.d);
    if (r.nd) add_rule(sh, &r);
}

static void parse_block(css_sheet_t *sh, const char *s, const char *end, int vw) {
    while (s < end) {
        const char *start;
        while (s < end && is_ws(*s)) s++;
        if (s + 1 < end && s[0] == '/' && s[1] == '*') {
            const char *c = s + 2;
            while (c + 1 < end && !(c[0] == '*' && c[1] == '/')) c++;
            s = c + 2 <= end ? c + 2 : end;
            continue;
        }
        if (s + 4 <= end && memcmp(s, "<!--", 4) == 0) { s += 4; continue; }
        if (s + 3 <= end && memcmp(s, "-->", 3) == 0) { s += 3; continue; }
        if (s >= end) break;
        start = s;
        if (*s == '@') {
            const char *kw = ++s, *prelude, *brace;
            size_t kl;
            while (s < end && is_ident(*s)) s++;
            kl = (size_t)(s - kw);
            prelude = s;
            while (s < end && *s != '{' && *s != ';') s++;
            if (s >= end) break;
            if (*s == ';') {
                if (ieq(kw, kl, "import") && sh->nimports < 32) {
                    const char *u = prelude;
                    while (u < s && *u != '"' && *u != '\'' && *u != '(') u++;
                    if (u < s) {
                        const char *ue;
                        char q = *u == '(' ? ')' : *u;
                        u++;
                        if (q == ')' && (*u == '"' || *u == '\'')) q = *u++;
                        ue = u;
                        while (ue < s && *ue != q) ue++;
                        {
                            const char *media = ue + 1;
                            if (q != ')' && media < s && *media == ')') media++;
                            while (media < s && is_ws(*media)) media++;
                            if (media >= s || media_matches(media, s, vw)) {
                                sh->imports[sh->nimports++] = arena_strndup(&sh->arena, u, (size_t)(ue - u));
                            }
                        }
                    }
                }
                s++;
                continue;
            }
            brace = s;
            s = skip_block(brace, end);
            if (ieq(kw, kl, "media")) {
                if (media_matches(prelude, brace, vw)) parse_block(sh, brace + 1, s - 1, vw);
            } else if (ieq(kw, kl, "supports") || ieq(kw, kl, "layer") || ieq(kw, kl, "container") ||
                       ieq(kw, kl, "document") || ieq(kw, kl, "-moz-document")) {
                const char *p = prelude;
                while (p < brace && is_ws(*p)) p++;
                if (!(brace - p >= 3 && ieq(p, 3, "not"))) parse_block(sh, brace + 1, s - 1, vw);
            }
            continue;
        }
        while (s < end && *s != '{') s++;
        if (s >= end) break;
        {
            const char *body = s + 1;
            const char *close = skip_block(s, end);
            parse_rule(sh, start, s, body, close - 1);
            s = close;
        }
    }
}

void css_parse(css_sheet_t *sheet, const char *text, size_t len, int origin_author, int viewport_w) {
    sheet->origin_author = origin_author;
    parse_block(sheet, text, text + len, viewport_w);
}

int css_imports(css_sheet_t *sheet, const char ***urls) {
    *urls = sheet->imports;
    return sheet->nimports;
}

void css_sheet_free(css_sheet_t *sheet) {
    if (!sheet) return;
    for (int x = 0; x < 2; x++) {
    index_t *ix = &sheet->idx[x];
    for (int i = 0; i < HASH; i++) {
        bucket_t *lists[4] = { ix->by_id[i], ix->by_class[i], ix->by_tag[i], ix->by_attr[i] };
        for (int k = 0; k < 4; k++) {
            bucket_t *b = lists[k];
            while (b) {
                bucket_t *next = b->next;
                free(b->refs);
                free(b);
                b = next;
            }
        }
    }
    free(ix->universal);
    }
    free(sheet->rules);
    arena_free(&sheet->arena);
    free(sheet);
}

/* ---- index -------------------------------------------------------------- */

static void bucket_add(bucket_t **table, const char *key, ref_t ref) {
    uint32_t h = hash_str(key, strlen(key)) % HASH;
    bucket_t *b = table[h];
    while (b && strcmp(b->key, key) != 0) b = b->next;
    if (!b) {
        b = (bucket_t *)calloc(1, sizeof(bucket_t));
        if (!b) return;
        b->key = key;
        b->next = table[h];
        table[h] = b;
    }
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4;
        b->refs = (ref_t *)realloc(b->refs, sizeof(ref_t) * (size_t)b->cap);
    }
    if (b->refs) b->refs[b->n++] = ref;
}

static bucket_t *bucket_get(bucket_t **table, const char *key) {
    uint32_t h = hash_str(key, strlen(key)) % HASH;
    bucket_t *b = table[h];
    while (b && strcmp(b->key, key) != 0) b = b->next;
    return b;
}

static void build_index(css_sheet_t *sh) {
    if (sh->indexed) return;
    for (int r = 0; r < sh->nrules; r++) {
        for (int k = 0; k < sh->rules[r].nsel; k++) {
            selector_t *sel = &sh->rules[r].sels[k];
            compound_t *c = &sel->c[sel->n - 1];
            ref_t ref = { r, k };
            index_t *ix = &sh->idx[sel->pseudo_el ? 1 : 0];
            const char *id = 0, *cls = 0, *tag = 0, *attr = 0;
            for (int i = 0; i < c->n; i++) {
                if (c->s[i].type == S_ID && !id) id = c->s[i].name;
                else if (c->s[i].type == S_CLASS && !cls) cls = c->s[i].name;
                else if (c->s[i].type == S_TAG && !tag) tag = c->s[i].name;
                else if (c->s[i].type == S_ATTR && !attr) attr = c->s[i].name;
            }
            ix->used = 1;
            if (id) bucket_add(ix->by_id, id, ref);
            else if (cls) bucket_add(ix->by_class, cls, ref);
            else if (tag) bucket_add(ix->by_tag, tag, ref);
            else if (attr) bucket_add(ix->by_attr, attr, ref);
            else {
                if (ix->nuniversal == ix->capuniversal) {
                    ix->capuniversal = ix->capuniversal ? ix->capuniversal * 2 : 64;
                    ix->universal = (ref_t *)realloc(ix->universal, sizeof(ref_t) * (size_t)ix->capuniversal);
                }
                if (ix->universal) ix->universal[ix->nuniversal++] = ref;
            }
        }
    }
    sh->indexed = 1;
}

/* ---- matching ----------------------------------------------------------- */

static dom_node_t *prev_el(dom_node_t *n) {
    for (n = n->prev; n; n = n->prev) {
        if (n->type == N_ELEMENT) return n;
    }
    return 0;
}

static dom_node_t *next_el(dom_node_t *n) {
    for (n = n->next; n; n = n->next) {
        if (n->type == N_ELEMENT) return n;
    }
    return 0;
}

static int nth_ok(int a, int b, int index) {
    if (a == 0) return index == b;
    return (index - b) % a == 0 && (index - b) / a >= 0;
}

static int match_compound(const compound_t *c, dom_node_t *el);

/* Class hashes on elements are only current while a cascade runs. */
static int in_cascade;

static int match_simple(const simple_t *x, dom_node_t *el) {
    switch (x->type) {
    case S_UNIVERSAL: return 1;
    case S_TAG: return x->tag ? el->tag == x->tag : (el->name && strcmp(el->name, x->name) == 0);
    case S_ID: return el->id && strcmp(el->id, x->name) == 0;
    case S_CLASS:
        if (in_cascade && el->chash) {
            for (int i = 0; i < el->nchash; i++) {
                if (el->chash[i] == x->hash) return dom_has_class(el, x->name);
            }
            return 0;
        }
        return el->klass && dom_has_class(el, x->name);
    case S_ATTR: {
        const char *v = dom_attr(el, x->name);
        size_t vl, xl;
        if (!v) return 0;
        if (x->op == A_EXISTS) return 1;
        if (!x->value) return 0;
        vl = strlen(v);
        xl = strlen(x->value);
        switch (x->op) {
        case A_EQ: return strcmp(v, x->value) == 0;
        case A_PREFIX: return xl && vl >= xl && memcmp(v, x->value, xl) == 0;
        case A_SUFFIX: return xl && vl >= xl && memcmp(v + vl - xl, x->value, xl) == 0;
        case A_SUBSTR: return xl && strstr(v, x->value) != 0;
        case A_DASH: return strcmp(v, x->value) == 0 || (vl > xl && memcmp(v, x->value, xl) == 0 && v[xl] == '-');
        case A_INCLUDES: {
            const char *p = v;
            while (*p) {
                const char *w;
                while (*p && is_ws(*p)) p++;
                w = p;
                while (*p && !is_ws(*p)) p++;
                if ((size_t)(p - w) == xl && memcmp(w, x->value, xl) == 0) return 1;
            }
            return 0;
        }
        }
        return 0;
    }
    case S_PSEUDO:
        switch (x->op) {
        case PC_FIRST_CHILD: return !prev_el(el);
        case PC_LAST_CHILD: return !next_el(el);
        case PC_ONLY_CHILD: return !prev_el(el) && !next_el(el);
        case PC_FIRST_OF_TYPE: {
            for (dom_node_t *p = prev_el(el); p; p = prev_el(p)) {
                if (p->tag == el->tag && (el->tag || strcmp(p->name, el->name) == 0)) return 0;
            }
            return 1;
        }
        case PC_LAST_OF_TYPE: {
            for (dom_node_t *p = next_el(el); p; p = next_el(p)) {
                if (p->tag == el->tag && (el->tag || strcmp(p->name, el->name) == 0)) return 0;
            }
            return 1;
        }
        case PC_NTH_CHILD: case PC_NTH_LAST_CHILD: case PC_NTH_OF_TYPE: {
            int idx = 1;
            if (x->op == PC_NTH_LAST_CHILD) {
                for (dom_node_t *p = next_el(el); p; p = next_el(p)) idx++;
            } else {
                for (dom_node_t *p = prev_el(el); p; p = prev_el(p)) {
                    if (x->op == PC_NTH_CHILD || p->tag == el->tag) idx++;
                }
            }
            return nth_ok(x->a, x->b, idx);
        }
        case PC_ROOT: return el->tag == T_HTML;
        case PC_EMPTY: return el->first == 0;
        case PC_LINK: return (el->tag == T_A || el->tag == T_AREA) && dom_attr(el, "href");
        case PC_NOT:
            for (int i = 0; i < x->nsub; i++) {
                if (match_compound(&x->sub[i], el)) return 0;
            }
            return 1;
        case PC_IS:
            for (int i = 0; i < x->nsub; i++) {
                if (match_compound(&x->sub[i], el)) return 1;
            }
            return 0;
        case PC_ALWAYS: return 1;
        default: return 0;
        }
    }
    return 0;
}

static int match_compound(const compound_t *c, dom_node_t *el) {
    for (int i = 0; i < c->n; i++) {
        if (!match_simple(&c->s[i], el)) return 0;
    }
    return 1;
}

/* Right-to-left matching with backtracking over descendant/sibling combinators. */
static int match_from(const selector_t *s, int ci, dom_node_t *el) {
    const compound_t *c = &s->c[ci];
    if (!match_compound(c, el)) return 0;
    if (ci == 0) return 1;
    switch (c->comb) {
    case C_CHILD: {
        dom_node_t *p = el->parent;
        return p && p->type == N_ELEMENT && match_from(s, ci - 1, p);
    }
    case C_DESC:
        for (dom_node_t *p = el->parent; p && p->type == N_ELEMENT; p = p->parent) {
            if (match_from(s, ci - 1, p)) return 1;
        }
        return 0;
    case C_ADJ: {
        dom_node_t *p = prev_el(el);
        return p && match_from(s, ci - 1, p);
    }
    case C_SIB:
        for (dom_node_t *p = prev_el(el); p; p = prev_el(p)) {
            if (match_from(s, ci - 1, p)) return 1;
        }
        return 0;
    }
    return 0;
}

/* ---- values ------------------------------------------------------------- */

typedef struct {
    const css_style_t *parent;
    float root_font, vw, vh;
} ctx_t;

static const struct {
    const char *name;
    uint32_t    rgb;
} named_colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 },
    { "silver", 0xC0C0C0 }, { "maroon", 0x800000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
    { "lime", 0x00FF00 }, { "olive", 0x808000 }, { "navy", 0x000080 }, { "teal", 0x008080 },
    { "aqua", 0x00FFFF }, { "orange", 0xFFA500 }, { "pink", 0xFFC0CB }, { "brown", 0xA52A2A },
    { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 }, { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "dimgray", 0x696969 }, { "darkblue", 0x00008B },
    { "darkred", 0x8B0000 }, { "darkgreen", 0x006400 }, { "lightblue", 0xADD8E6 }, { "skyblue", 0x87CEEB },
    { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 }, { "dodgerblue", 0x1E90FF }, { "crimson", 0xDC143C },
    { "gold", 0xFFD700 }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C },
    { "coral", 0xFF7F50 }, { "tomato", 0xFF6347 }, { "salmon", 0xFA8072 }, { "indigo", 0x4B0082 },
    { "violet", 0xEE82EE }, { "orchid", 0xDA70D6 }, { "tan", 0xD2B48C }, { "linen", 0xFAF0E6 },
    { "aliceblue", 0xF0F8FF }, { "ghostwhite", 0xF8F8FF }, { "snow", 0xFFFAFA }, { "mintcream", 0xF5FFFA },
    { "honeydew", 0xF0FFF0 }, { "lavender", 0xE6E6FA }, { "seashell", 0xFFF5EE }, { "cornsilk", 0xFFF8DC },
    { "lightyellow", 0xFFFFE0 }, { "lightgreen", 0x90EE90 }, { "darkorange", 0xFF8C00 },
    { "slategray", 0x708090 }, { "lightslategray", 0x778899 }, { "darkslategray", 0x2F4F4F },
    { "midnightblue", 0x191970 }, { "cyan", 0x00FFFF }, { "magenta", 0xFF00FF }, { "firebrick", 0xB22222 },
    { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 }, { "chocolate", 0xD2691E }, { "goldenrod", 0xDAA520 }
};

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static float hue2rgb(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

/* Returns ARGB; *ok = 0 if the text is not a color. */
uint32_t css_parse_color(const char *s, size_t n, int *ok) {
    *ok = 1;
    while (n && is_ws(*s)) s++, n--;
    while (n && is_ws(s[n - 1])) n--;
    if (n && s[0] == '#') {
        int d[8], k = 0;
        for (size_t i = 1; i < n && k < 8; i++) {
            int v = hexval(s[i]);
            if (v < 0) break;
            d[k++] = v;
        }
        if (k == 3 || k == 4) {
            uint32_t a = k == 4 ? (uint32_t)(d[3] * 17) : 255;
            return (a << 24) | ((uint32_t)(d[0] * 17) << 16) | ((uint32_t)(d[1] * 17) << 8) | (uint32_t)(d[2] * 17);
        }
        if (k == 6 || k == 8) {
            uint32_t a = k == 8 ? (uint32_t)(d[6] * 16 + d[7]) : 255;
            return (a << 24) | ((uint32_t)(d[0] * 16 + d[1]) << 16) | ((uint32_t)(d[2] * 16 + d[3]) << 8) |
                   (uint32_t)(d[4] * 16 + d[5]);
        }
        *ok = 0;
        return 0;
    }
    if (n > 4 && (ieq(s, 4, "rgb(") || ieq(s, 5, "rgba(") || ieq(s, 4, "hsl(") || ieq(s, 5, "hsla("))) {
        float v[4] = { 0, 0, 0, 1 };
        int pct[4] = { 0 }, k = 0, hsl = (s[0] | 32) == 'h';
        const char *p = memchr(s, '(', n) + 1, *end = s + n;
        while (p < end && k < 4) {
            char *e;
            while (p < end && (is_ws(*p) || *p == ',' || *p == '/')) p++;
            if (p >= end || *p == ')') break;
            v[k] = (float)strtod(p, &e);
            if (e == p) break;
            p = e;
            if (p < end && *p == '%') {
                pct[k] = 1;
                p++;
            }
            while (p < end && ((*p | 32) >= 'a' && (*p | 32) <= 'z')) p++;   /* deg */
            k++;
        }
        if (k < 3) {
            *ok = 0;
            return 0;
        }
        if (pct[3] || (k == 4 && v[3] > 1)) v[3] /= pct[3] ? 100 : 255;
        if (hsl) {
            float h = v[0] / 360, sat = v[1] / 100, l = v[2] / 100, q, pp;
            h -= (float)(int)h;
            if (h < 0) h += 1;
            q = l < 0.5f ? l * (1 + sat) : l + sat - l * sat;
            pp = 2 * l - q;
            v[0] = hue2rgb(pp, q, h + 1.0f / 3) * 255;
            v[1] = hue2rgb(pp, q, h) * 255;
            v[2] = hue2rgb(pp, q, h - 1.0f / 3) * 255;
        } else {
            for (int i = 0; i < 3; i++) {
                if (pct[i]) v[i] = v[i] * 255 / 100;
            }
        }
        for (int i = 0; i < 3; i++) v[i] = v[i] < 0 ? 0 : v[i] > 255 ? 255 : v[i];
        if (v[3] < 0) v[3] = 0;
        if (v[3] > 1) v[3] = 1;
        return ((uint32_t)(v[3] * 255 + 0.5f) << 24) | ((uint32_t)v[0] << 16) | ((uint32_t)v[1] << 8) | (uint32_t)v[2];
    }
    if (ieq(s, n, "transparent")) return 0;
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++) {
        if (ieq(s, n, named_colors[i].name)) return 0xFF000000U | named_colors[i].rgb;
    }
    *ok = 0;
    return 0;
}

/* Parses a length; returns 0 if s is not a length. font_size is the
 * element's font size (for em). */
static int parse_len(const char *s, const char *end, float font_size, const ctx_t *c, css_len_t *out) {
    char *e;
    float v;
    while (s < end && is_ws(*s)) s++;
    if (s >= end) return 0;
    if (ieq(s, (size_t)(end - s), "auto")) {
        out->unit = U_AUTO;
        out->v = 0;
        return 1;
    }
    if (ieq(s, (size_t)(end - s), "none")) {
        out->unit = U_NONE;
        out->v = 0;
        return 1;
    }
    if ((size_t)(end - s) > 5 && (ieq(s, 5, "calc(") || ieq(s, 4, "min(") || ieq(s, 4, "max(") || ieq(s, 6, "clamp("))) {
        /* evaluate the simple forms: sums/differences of lengths, min/max/clamp pick a term */
        const char *p = memchr(s, '(', (size_t)(end - s)) + 1;
        float px = 0, pct = 0, sign = 1;
        int terms = 0, minmax = (s[0] | 32) != 'c' || (s[1] | 32) == 'l';
        int is_max = (s[1] | 32) == 'a';
        float best = 0;
        int have_best = 0;
        while (p < end && *p != ')') {
            css_len_t t;
            const char *te;
            int depth = 0;
            while (p < end && is_ws(*p)) p++;
            if (*p == '+' && minmax == 0) { sign = 1; p++; continue; }
            if (*p == '-' && p + 1 < end && is_ws(p[1])) { sign = -1; p++; continue; }
            if (*p == ',') { p++; continue; }
            te = p;
            while (te < end && (depth > 0 || (!is_ws(*te) && *te != ',' && *te != ')'))) {
                if (*te == '(') depth++;
                if (*te == ')') depth--;
                te++;
            }
            if (*p == '*' || *p == '/') {
                float f = (float)strtod(p + 1, 0);
                if (*p == '*') { px *= f; pct *= f; }
                else if (f != 0) { px /= f; pct /= f; }
                p = te;
                continue;
            }
            if (parse_len(p, te, font_size, c, &t)) {
                if (minmax) {
                    float val = t.unit == U_PX ? t.v : t.v * c->vw / 100;
                    if (!have_best || (is_max ? val > best : val < best)) best = val;
                    if ((s[0] | 32) == 'c' && (s[1] | 32) == 'l' && terms == 1) {
                        best = val;      /* clamp: the preferred value */
                    }
                    have_best = 1;
                } else if (t.unit == U_PX) px += sign * t.v;
                else if (t.unit == U_PCT) pct += sign * t.v;
                terms++;
            } else {
                float f = (float)strtod(p, &e);
                if (e != p) px += sign * f;
            }
            sign = 1;
            p = te;
        }
        if (minmax) {
            out->unit = U_PX;
            out->v = best;
            return have_best;
        }
        if (pct != 0 && px == 0) {
            out->unit = U_PCT;
            out->v = pct;
        } else {
            out->unit = U_PX;
            out->v = px + pct * c->vw / 100 * 0;   /* percentage part unknown here: dropped */
        }
        return 1;
    }
    if (ieq(s, 4, "var(")) return 0;
    v = (float)strtod(s, &e);
    if (e == s) return 0;
    {
        const char *u = e;
        size_t ul = 0;
        while (u + ul < end && ((u[ul] | 32) >= 'a' && (u[ul] | 32) <= 'z')) ul++;
        if (u + ul < end && u[ul] == '%') {
            out->unit = U_PCT;
            out->v = v;
            return 1;
        }
        out->unit = U_PX;
        if (ul == 0 || ieq(u, ul, "px")) out->v = v;
        else if (ieq(u, ul, "em")) out->v = v * font_size;
        else if (ieq(u, ul, "rem")) out->v = v * c->root_font;
        else if (ieq(u, ul, "ex") || ieq(u, ul, "ch")) out->v = v * font_size * 0.5f;
        else if (ieq(u, ul, "pt")) out->v = v * 4 / 3;
        else if (ieq(u, ul, "pc")) out->v = v * 16;
        else if (ieq(u, ul, "in")) out->v = v * 96;
        else if (ieq(u, ul, "cm")) out->v = v * 37.795f;
        else if (ieq(u, ul, "mm")) out->v = v * 3.7795f;
        else if (ieq(u, ul, "vw") || ieq(u, ul, "dvw") || ieq(u, ul, "svw") || ieq(u, ul, "lvw")) out->v = v * c->vw / 100;
        else if (ieq(u, ul, "vh") || ieq(u, ul, "dvh") || ieq(u, ul, "svh") || ieq(u, ul, "lvh")) out->v = v * c->vh / 100;
        else if (ieq(u, ul, "vmin")) out->v = v * (c->vw < c->vh ? c->vw : c->vh) / 100;
        else if (ieq(u, ul, "vmax")) out->v = v * (c->vw > c->vh ? c->vw : c->vh) / 100;
        else if (ieq(u, ul, "fr")) { out->unit = U_PCT; out->v = 100; }
        else return 0;
    }
    return 1;
}

/* Splits a value into whitespace-separated tokens, keeping (...) groups. */
static int tokens(const char *s, const char **tok, size_t *len, int max) {
    int n = 0;
    while (*s && n < max) {
        const char *t;
        int depth = 0;
        while (*s && (is_ws(*s) || *s == ',')) s++;
        if (!*s) break;
        t = s;
        if (*s == '"' || *s == '\'') {
            char q = *s++;
            while (*s && *s != q) s++;
            if (*s) s++;
        } else {
            while (*s && (depth > 0 || (!is_ws(*s) && *s != ','))) {
                if (*s == '(') depth++;
                if (*s == ')') depth--;
                s++;
            }
        }
        tok[n] = t;
        len[n] = (size_t)(s - t);
        n++;
    }
    return n;
}

static int keyword(const char *v, const char *const *words, int n) {
    size_t l = strlen(v);
    while (l && is_ws(v[l - 1])) l--;
    for (int i = 0; i < n; i++) {
        if (words[i][0] && ieq(v, l, words[i])) return i;
    }
    return -1;
}

static void set_box4(css_len_t *dst, const char *v, float fs, const ctx_t *c) {
    const char *tok[4];
    size_t len[4];
    css_len_t l[4];
    int n = tokens(v, tok, len, 4);
    for (int i = 0; i < n; i++) {
        if (!parse_len(tok[i], tok[i] + len[i], fs, c, &l[i])) return;
    }
    if (n == 1) l[1] = l[2] = l[3] = l[0];
    else if (n == 2) { l[2] = l[0]; l[3] = l[1]; }
    else if (n == 3) l[3] = l[1];
    else if (n != 4) return;
    for (int i = 0; i < 4; i++) dst[i] = l[i];
}

static float border_width_kw(const char *t, size_t n, float fs, const ctx_t *c, int *ok) {
    css_len_t l;
    *ok = 1;
    if (ieq(t, n, "thin")) return 1;
    if (ieq(t, n, "medium")) return 3;
    if (ieq(t, n, "thick")) return 5;
    if (parse_len(t, t + n, fs, c, &l) && l.unit == U_PX) return l.v;
    *ok = 0;
    return 0;
}

static int border_style_kw(const char *t, size_t n) {
    if (ieq(t, n, "none") || ieq(t, n, "hidden")) return BS_NONE;
    if (ieq(t, n, "solid")) return BS_SOLID;
    if (ieq(t, n, "dashed")) return BS_DASHED;
    if (ieq(t, n, "dotted")) return BS_DOTTED;
    if (ieq(t, n, "double")) return BS_DOUBLE;
    if (ieq(t, n, "groove") || ieq(t, n, "ridge") || ieq(t, n, "inset") || ieq(t, n, "outset")) return BS_OTHER;
    return -1;
}

static void set_border_side(css_style_t *st, int side, const char *v, const ctx_t *c) {
    const char *tok[4];
    size_t len[4];
    int n = tokens(v, tok, len, 4);
    float w = 3;
    int style = BS_NONE;
    uint32_t color = st->color;
    for (int i = 0; i < n; i++) {
        int ok, bs = border_style_kw(tok[i], len[i]);
        float bw;
        uint32_t col;
        if (bs >= 0) {
            style = bs;
            continue;
        }
        bw = border_width_kw(tok[i], len[i], st->font_size, c, &ok);
        if (ok) {
            w = bw;
            continue;
        }
        col = css_parse_color(tok[i], len[i], &ok);
        if (ok) color = col;
        else if (ieq(tok[i], len[i], "currentcolor")) color = st->color;
    }
    for (int s = 0; s < 4; s++) {
        if (side >= 0 && s != side) continue;
        st->border_w[s] = w;
        st->border_style[s] = (uint8_t)style;
        st->border_color[s] = color;
    }
}

static const char *const display_kw[] = {
    "inline", "block", "inline-block", "list-item", "none", "table", "inline-table", "table-row",
    "table-cell", "table-row-group", "table-header-group", "table-footer-group", "table-caption",
    "table-column", "table-column-group", "flex", "inline-flex", "grid", "inline-grid", "contents"
};

static uint8_t align_kw(const char *v) {
    static const char *const w[] = { "flex-start", "flex-end", "center", "space-between", "space-around",
                                     "space-evenly", "stretch", "baseline", "auto" };
    int k = keyword(v, w, 9);
    if (k >= 0) return (uint8_t)k;
    if (keyword(v, (const char *const[]){ "start", "left", "self-start", "normal" }, 4) >= 0) return JC_START;
    if (keyword(v, (const char *const[]){ "end", "right", "self-end" }, 3) >= 0) return JC_END;
    return JC_START;
}

static const char *font_family_value(arena_t *a, const char *v) {
    const char *s = v;
    const char *e;
    while (*s && is_ws(*s)) s++;
    if (*s == '"' || *s == '\'') {
        char q = *s++;
        e = s;
        while (*e && *e != q) e++;
    } else {
        e = s;
        while (*e && *e != ',') e++;
        while (e > s && is_ws(e[-1])) e--;
    }
    return dup_lower(a, s, (size_t)(e - s));
}

/* Applies one declaration (after var() substitution) to st. */
/* ---- backgrounds ---------------------------------------------------------- */

static int bg_repeat(css_style_t *st, const char *v, size_t vl) {
    if (ieq(v, vl, "no-repeat")) st->bg_repeat = 1;
    else if (ieq(v, vl, "repeat-x")) st->bg_repeat = 2;
    else if (ieq(v, vl, "repeat-y")) st->bg_repeat = 3;
    else if (ieq(v, vl, "repeat") || ieq(v, vl, "space") || ieq(v, vl, "round")) st->bg_repeat = 0;
    else return 0;
    return 1;
}

/* a position keyword as a percentage; axis: 0 x, 1 y, -1 either */
static int bg_keyword(const char *v, size_t vl, int *axis, float *pct) {
    if (ieq(v, vl, "left")) { *axis = 0; *pct = 0; }
    else if (ieq(v, vl, "right")) { *axis = 0; *pct = 100; }
    else if (ieq(v, vl, "top")) { *axis = 1; *pct = 0; }
    else if (ieq(v, vl, "bottom")) { *axis = 1; *pct = 100; }
    else if (ieq(v, vl, "center")) { *axis = -1; *pct = 50; }
    else return 0;
    return 1;
}

static void bg_position_tokens(css_style_t *st, const char **tok, size_t *len, int n, const ctx_t *c) {
    css_len_t pos[2];
    int set[2] = { 0, 0 }, k = 0;
    pos[0].unit = pos[1].unit = U_PCT;
    pos[0].v = pos[1].v = 50;
    for (int i = 0; i < n && k < 2; i++) {
        int axis;
        float pct;
        css_len_t l;
        if (bg_keyword(tok[i], len[i], &axis, &pct)) {
            int slot = axis >= 0 ? axis : (set[0] ? 1 : 0);
            pos[slot].unit = U_PCT;
            pos[slot].v = pct;
            set[slot] = 1;
            k++;
        } else if (parse_len(tok[i], tok[i] + len[i], 16, c, &l)) {
            int slot = set[0] ? 1 : 0;
            pos[slot] = l;
            set[slot] = 1;
            k++;
        }
    }
    if (k) {
        st->bg_pos[0] = pos[0];
        st->bg_pos[1] = pos[1];
    }
}

static void bg_position(css_style_t *st, const char *v, size_t vl, const ctx_t *c) {
    const char *tok[6];
    size_t len[6];
    int n;
    (void)vl;
    n = tokens(v, tok, len, 6);
    bg_position_tokens(st, tok, len, n, c);
}

static void bg_size(css_style_t *st, const char *v, size_t vl, const ctx_t *c) {
    const char *tok[4];
    size_t len[4];
    int n;
    if (ieq(v, vl, "cover")) { st->bg_size_mode = 1; return; }
    if (ieq(v, vl, "contain")) { st->bg_size_mode = 2; return; }
    n = tokens(v, tok, len, 4);
    st->bg_size[0].unit = st->bg_size[1].unit = U_AUTO;
    for (int i = 0; i < n && i < 2; i++) {
        css_len_t l;
        if (!ieq(tok[i], len[i], "auto") && parse_len(tok[i], tok[i] + len[i], 16, c, &l)) st->bg_size[i] = l;
    }
    st->bg_size_mode = st->bg_size[0].unit == U_AUTO && st->bg_size[1].unit == U_AUTO ? 0 : 3;
}

static void apply(css_style_t *st, int prop, const char *v, const ctx_t *c, arena_t *a) {
    css_len_t l;
    int ok;
    size_t vl = strlen(v);
    const css_style_t *ps = c->parent;
    if (ieq(v, vl, "inherit")) {
        if (!ps) return;
        switch (prop) {
        case PR_COLOR: st->color = ps->color; break;
        case PR_BACKGROUND_COLOR: st->bg_color = ps->bg_color; break;
        case PR_FONT_SIZE: st->font_size = ps->font_size; break;
        case PR_DISPLAY: st->display = ps->display; break;
        case PR_WIDTH: st->width = ps->width; break;
        case PR_HEIGHT: st->height = ps->height; break;
        default: break;
        }
        return;
    }
    if (ieq(v, vl, "initial") || ieq(v, vl, "unset") || ieq(v, vl, "revert")) return;
    switch (prop) {
    case PR_DISPLAY: {
        int k = keyword(v, display_kw, 20);
        if (k >= 0) st->display = (uint8_t)k;
        else if (strstr(v, "flex")) st->display = D_FLEX;
        else if (strstr(v, "grid")) st->display = D_GRID;
        else if (strstr(v, "block")) st->display = D_BLOCK;
        break;
    }
    case PR_POSITION: {
        int k = keyword(v, (const char *const[]){ "static", "relative", "absolute", "fixed", "sticky", "-webkit-sticky" }, 6);
        if (k >= 0) st->position = (uint8_t)(k == 5 ? P_STICKY : k);
        break;
    }
    case PR_FLOAT: {
        int k = keyword(v, (const char *const[]){ "none", "left", "right", "inline-start", "inline-end" }, 5);
        if (k >= 0) st->float_ = (uint8_t)(k == 3 ? F_LEFT : k == 4 ? F_RIGHT : k);
        break;
    }
    case PR_CLEAR: {
        int k = keyword(v, (const char *const[]){ "none", "left", "right", "both" }, 4);
        if (k >= 0) st->clear = (uint8_t)k;
        break;
    }
    case PR_BOX_SIZING: st->box_sizing = ieq(v, vl, "border-box"); break;
    case PR_OVERFLOW: case PR_OVERFLOW_X: case PR_OVERFLOW_Y: {
        int k = keyword(v, (const char *const[]){ "visible", "hidden", "scroll", "auto", "clip" }, 5);
        if (k >= 0) st->overflow = (uint8_t)(k == 4 ? OV_HIDDEN : k);
        break;
    }
    case PR_VISIBILITY: st->visibility = !ieq(v, vl, "hidden") && !ieq(v, vl, "collapse"); break;
    case PR_WHITE_SPACE: {
        int k = keyword(v, (const char *const[]){ "normal", "pre", "nowrap", "pre-wrap", "pre-line", "break-spaces" }, 6);
        if (k >= 0) st->white_space = (uint8_t)(k == 5 ? WS_PRE_WRAP : k);
        break;
    }
    case PR_TEXT_ALIGN: {
        int k = keyword(v, (const char *const[]){ "left", "right", "center", "justify", "start", "end", "-webkit-center" }, 7);
        if (k >= 0) st->text_align = (uint8_t)(k == 4 ? TA_LEFT : k == 5 ? TA_RIGHT : k == 6 ? TA_CENTER : k);
        break;
    }
    case PR_TEXT_TRANSFORM: {
        int k = keyword(v, (const char *const[]){ "none", "uppercase", "lowercase", "capitalize" }, 4);
        if (k >= 0) st->text_transform = (uint8_t)k;
        break;
    }
    case PR_FONT_STYLE: st->font_italic = ieq(v, vl, "italic") || ieq(v, vl, "oblique"); break;
    case PR_FONT_WEIGHT:
        if (ieq(v, vl, "bold") || ieq(v, vl, "bolder")) st->font_weight = 700;
        else if (ieq(v, vl, "normal") || ieq(v, vl, "lighter")) st->font_weight = 400;
        else if (v[0] >= '1' && v[0] <= '9') st->font_weight = (uint16_t)atoi(v);
        break;
    case PR_FONT_SIZE: {
        float base = ps ? ps->font_size : 16;
        static const char *const kw[] = { "xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large", "smaller", "larger" };
        static const float px[] = { 9, 10, 13, 16, 18, 24, 32, 48 };
        int k = keyword(v, kw, 10);
        if (k >= 0 && k < 8) st->font_size = px[k];
        else if (k == 8) st->font_size = base / 1.2f;
        else if (k == 9) st->font_size = base * 1.2f;
        else if (parse_len(v, v + vl, base, c, &l)) {
            if (l.unit == U_PX) st->font_size = l.v;
            else if (l.unit == U_PCT) st->font_size = base * l.v / 100;
        }
        if (st->font_size < 1) st->font_size = 1;
        if (st->font_size > 400) st->font_size = 400;
        break;
    }
    case PR_FONT_FAMILY: st->font_family = font_family_value(a, v); break;
    case PR_FONT: {
        const char *tok[12];
        size_t len[12];
        int n = tokens(v, tok, len, 12);
        for (int i = 0; i < n; i++) {
            if (ieq(tok[i], len[i], "italic") || ieq(tok[i], len[i], "oblique")) st->font_italic = 1;
            else if (ieq(tok[i], len[i], "bold")) st->font_weight = 700;
            else if (len[i] == 3 && tok[i][0] >= '1' && tok[i][0] <= '9' && tok[i][1] == '0') st->font_weight = (uint16_t)atoi(tok[i]);
            else if ((tok[i][0] >= '0' && tok[i][0] <= '9') || tok[i][0] == '.') {
                const char *slash = memchr(tok[i], '/', len[i]);
                float base = ps ? ps->font_size : 16;
                if (parse_len(tok[i], slash ? slash : tok[i] + len[i], base, c, &l) && l.unit != U_AUTO) {
                    st->font_size = l.unit == U_PCT ? base * l.v / 100 : l.v;
                }
                if (slash) {
                    char tmp[32];
                    size_t tl = (size_t)(tok[i] + len[i] - slash - 1);
                    if (tl < sizeof(tmp)) {
                        memcpy(tmp, slash + 1, tl);
                        tmp[tl] = 0;
                        apply(st, PR_LINE_HEIGHT, tmp, c, a);
                    }
                }
                if (i + 1 < n) st->font_family = font_family_value(a, tok[i + 1]);
                break;
            }
        }
        break;
    }
    case PR_LINE_HEIGHT:
        if (ieq(v, vl, "normal")) {
            st->lh_type = LH_NORMAL;
        } else {
            char *e;
            float f = (float)strtod(v, &e);
            if (e != v && (*e == 0 || is_ws(*e))) {
                st->lh_type = LH_MULT;
                st->lh_value = f;
            } else if (parse_len(v, v + vl, st->font_size, c, &l)) {
                st->lh_type = LH_PX;
                st->lh_value = l.unit == U_PCT ? st->font_size * l.v / 100 : l.v;
            }
        }
        break;
    case PR_LETTER_SPACING: if (parse_len(v, v + vl, st->font_size, c, &l) && l.unit == U_PX) st->letter_spacing = l.v; break;
    case PR_WORD_SPACING: if (parse_len(v, v + vl, st->font_size, c, &l) && l.unit == U_PX) st->word_spacing = l.v; break;
    case PR_TEXT_INDENT: if (parse_len(v, v + vl, st->font_size, c, &l)) st->text_indent = l; break;
    case PR_COLOR: {
        uint32_t col = css_parse_color(v, vl, &ok);
        if (ok) st->color = col;
        break;
    }
    case PR_BACKGROUND_COLOR: {
        uint32_t col = css_parse_color(v, vl, &ok);
        if (ok) st->bg_color = col;
        else if (ieq(v, vl, "currentcolor")) st->bg_color = st->color;
        break;
    }
    case PR_BACKGROUND: case PR_BACKGROUND_IMAGE: {
        const char *tok[12];
        size_t len[12];
        int n = tokens(v, tok, len, 12);
        const char *ptok[4];
        size_t plen[4];
        int np = 0;
        if (prop == PR_BACKGROUND) {
            st->bg_color = 0;
            st->bg_image = 0;
            st->bg_repeat = 0;
            st->bg_size_mode = 0;
            st->bg_pos[0].unit = st->bg_pos[1].unit = U_PCT;
            st->bg_pos[0].v = st->bg_pos[1].v = 0;
        }
        for (int i = 0; i < n; i++) {
            int axis;
            float pct;
            if (len[i] > 4 && ieq(tok[i], 4, "url(")) {
                const char *u = tok[i] + 4, *ue = tok[i] + len[i] - 1;
                if (*u == '"' || *u == '\'') u++;
                if (ue > u && (ue[-1] == '"' || ue[-1] == '\'')) ue--;
                st->bg_image = arena_strndup(a, u, (size_t)(ue - u));
            } else if (prop == PR_BACKGROUND) {
                /* shorthand: colour, repeat, position [/ size] */
                uint32_t col = css_parse_color(tok[i], len[i], &ok);
                if (ok) st->bg_color = col;
                else if (bg_repeat(st, tok[i], len[i])) {}
                else if (ieq(tok[i], len[i], "cover")) st->bg_size_mode = 1;
                else if (ieq(tok[i], len[i], "contain")) st->bg_size_mode = 2;
                else if ((bg_keyword(tok[i], len[i], &axis, &pct) || (tok[i][0] >= '0' && tok[i][0] <= '9')) && np < 4) {
                    ptok[np] = tok[i];
                    plen[np++] = len[i];
                }
            }
        }
        if (np) bg_position_tokens(st, ptok, plen, np, c);
        break;
    }
    case PR_WIDTH: if (parse_len(v, v + vl, st->font_size, c, &l)) st->width = l; break;
    case PR_HEIGHT: if (parse_len(v, v + vl, st->font_size, c, &l)) st->height = l; break;
    case PR_MIN_WIDTH: if (parse_len(v, v + vl, st->font_size, c, &l)) st->min_w = l; break;
    case PR_MIN_HEIGHT: if (parse_len(v, v + vl, st->font_size, c, &l)) st->min_h = l; break;
    case PR_MAX_WIDTH: if (parse_len(v, v + vl, st->font_size, c, &l)) st->max_w = l; break;
    case PR_MAX_HEIGHT: if (parse_len(v, v + vl, st->font_size, c, &l)) st->max_h = l; break;
    case PR_MARGIN: set_box4(st->margin, v, st->font_size, c); break;
    case PR_PADDING: set_box4(st->padding, v, st->font_size, c); break;
    case PR_INSET: set_box4(st->inset, v, st->font_size, c); break;
    case PR_MARGIN_TOP: case PR_MARGIN_RIGHT: case PR_MARGIN_BOTTOM: case PR_MARGIN_LEFT:
        if (parse_len(v, v + vl, st->font_size, c, &l)) st->margin[prop - PR_MARGIN_TOP] = l;
        break;
    case PR_PADDING_TOP: case PR_PADDING_RIGHT: case PR_PADDING_BOTTOM: case PR_PADDING_LEFT:
        if (parse_len(v, v + vl, st->font_size, c, &l)) st->padding[prop - PR_PADDING_TOP] = l;
        break;
    case PR_TOP: case PR_RIGHT: case PR_BOTTOM: case PR_LEFT:
        if (parse_len(v, v + vl, st->font_size, c, &l)) st->inset[prop - PR_TOP] = l;
        break;
    case PR_BORDER: set_border_side(st, -1, v, c); break;
    case PR_BORDER_TOP: case PR_BORDER_RIGHT: case PR_BORDER_BOTTOM: case PR_BORDER_LEFT:
        set_border_side(st, prop - PR_BORDER_TOP, v, c);
        break;
    case PR_BORDER_WIDTH: {
        const char *tok[4];
        size_t len[4];
        float w[4];
        int n = tokens(v, tok, len, 4);
        for (int i = 0; i < n; i++) {
            w[i] = border_width_kw(tok[i], len[i], st->font_size, c, &ok);
            if (!ok) return;
        }
        if (n == 1) w[1] = w[2] = w[3] = w[0];
        else if (n == 2) { w[2] = w[0]; w[3] = w[1]; }
        else if (n == 3) w[3] = w[1];
        for (int i = 0; i < 4 && n; i++) st->border_w[i] = w[i];
        break;
    }
    case PR_BORDER_STYLE: {
        const char *tok[4];
        size_t len[4];
        int s[4], n = tokens(v, tok, len, 4);
        for (int i = 0; i < n; i++) {
            s[i] = border_style_kw(tok[i], len[i]);
            if (s[i] < 0) return;
        }
        if (n == 1) s[1] = s[2] = s[3] = s[0];
        else if (n == 2) { s[2] = s[0]; s[3] = s[1]; }
        else if (n == 3) s[3] = s[1];
        for (int i = 0; i < 4 && n; i++) st->border_style[i] = (uint8_t)s[i];
        break;
    }
    case PR_BORDER_COLOR: {
        const char *tok[4];
        size_t len[4];
        uint32_t col[4];
        int n = tokens(v, tok, len, 4);
        for (int i = 0; i < n; i++) {
            col[i] = css_parse_color(tok[i], len[i], &ok);
            if (!ok) col[i] = st->color;
        }
        if (n == 1) col[1] = col[2] = col[3] = col[0];
        else if (n == 2) { col[2] = col[0]; col[3] = col[1]; }
        else if (n == 3) col[3] = col[1];
        for (int i = 0; i < 4 && n; i++) st->border_color[i] = col[i];
        break;
    }
    case PR_BORDER_TOP_WIDTH: case PR_BORDER_RIGHT_WIDTH: case PR_BORDER_BOTTOM_WIDTH: case PR_BORDER_LEFT_WIDTH: {
        float w = border_width_kw(v, vl, st->font_size, c, &ok);
        if (ok) st->border_w[prop - PR_BORDER_TOP_WIDTH] = w;
        break;
    }
    case PR_BORDER_TOP_COLOR: case PR_BORDER_RIGHT_COLOR: case PR_BORDER_BOTTOM_COLOR: case PR_BORDER_LEFT_COLOR: {
        uint32_t col = css_parse_color(v, vl, &ok);
        if (ok) st->border_color[prop - PR_BORDER_TOP_COLOR] = col;
        break;
    }
    case PR_BORDER_TOP_STYLE: case PR_BORDER_RIGHT_STYLE: case PR_BORDER_BOTTOM_STYLE: case PR_BORDER_LEFT_STYLE: {
        int s = border_style_kw(v, vl);
        if (s >= 0) st->border_style[prop - PR_BORDER_TOP_STYLE] = (uint8_t)s;
        break;
    }
    case PR_BORDER_RADIUS:
        if (parse_len(v, v + (strcspn(v, " /") ? strcspn(v, " /") : vl), st->font_size, c, &l)) {
            st->radius = l.unit == U_PX ? l.v : l.unit == U_PCT ? -l.v : 0;   /* negative = percent */
        }
        break;
    case PR_LIST_STYLE: case PR_LIST_STYLE_TYPE: case PR_LIST_STYLE_POSITION: {
        const char *tok[4];
        size_t len[4];
        static const char *const w[] = { "none", "disc", "circle", "square", "decimal", "lower-alpha", "upper-alpha", "lower-roman", "upper-roman" };
        int n = tokens(v, tok, len, 4);
        for (int i = 0; i < n; i++) {
            if (ieq(tok[i], len[i], "inside")) st->list_inside = 1;
            else if (ieq(tok[i], len[i], "outside")) st->list_inside = 0;
            else if (ieq(tok[i], len[i], "lower-latin")) st->list_style = LS_LOWER_ALPHA;
            else if (ieq(tok[i], len[i], "upper-latin")) st->list_style = LS_UPPER_ALPHA;
            else if (ieq(tok[i], len[i], "decimal-leading-zero")) st->list_style = LS_DECIMAL;
            else {
                for (int k = 0; k < 9; k++) {
                    if (ieq(tok[i], len[i], w[k])) st->list_style = (uint8_t)k;
                }
            }
        }
        break;
    }
    case PR_VERTICAL_ALIGN: {
        int k = keyword(v, (const char *const[]){ "baseline", "top", "middle", "bottom", "sub", "super", "text-top", "text-bottom" }, 8);
        if (k >= 0) st->vertical_align = (uint8_t)k;
        break;
    }
    case PR_TEXT_DECORATION: case PR_TEXT_DECORATION_LINE: {
        const char *tok[6];
        size_t len[6];
        int n = tokens(v, tok, len, 6);
        st->decoration = 0;
        for (int i = 0; i < n; i++) {
            if (ieq(tok[i], len[i], "underline")) st->decoration |= TD_UNDERLINE;
            else if (ieq(tok[i], len[i], "line-through")) st->decoration |= TD_LINE_THROUGH;
            else if (ieq(tok[i], len[i], "overline")) st->decoration |= TD_OVERLINE;
        }
        break;
    }
    case PR_OPACITY: {
        float o = (float)strtod(v, 0);
        if (strchr(v, '%')) o /= 100;
        st->opacity = o < 0 ? 0 : o > 1 ? 1 : o;
        break;
    }
    case PR_Z_INDEX:
        if (ieq(v, vl, "auto")) st->z_auto = 1;
        else {
            st->z_auto = 0;
            st->z_index = atoi(v);
        }
        break;
    case PR_FLEX: {
        const char *tok[3];
        size_t len[3];
        int n = tokens(v, tok, len, 3);
        if (ieq(v, vl, "none")) { st->flex_grow = 0; st->flex_shrink = 0; st->flex_basis.unit = U_AUTO; break; }
        if (ieq(v, vl, "auto")) { st->flex_grow = 1; st->flex_shrink = 1; st->flex_basis.unit = U_AUTO; break; }
        if (n >= 1) {
            char *e;
            float g = (float)strtod(tok[0], &e);
            if (e != tok[0] && (size_t)(e - tok[0]) == len[0]) {
                st->flex_grow = g;
                st->flex_shrink = 1;
                st->flex_basis.unit = U_PX;
                st->flex_basis.v = 0;
                if (n >= 2) {
                    float sh = (float)strtod(tok[1], &e);
                    if (e != tok[1] && (size_t)(e - tok[1]) == len[1]) {
                        st->flex_shrink = sh;
                        if (n >= 3) parse_len(tok[2], tok[2] + len[2], st->font_size, c, &st->flex_basis);
                    } else {
                        parse_len(tok[1], tok[1] + len[1], st->font_size, c, &st->flex_basis);
                    }
                }
            } else if (parse_len(tok[0], tok[0] + len[0], st->font_size, c, &l)) {
                st->flex_grow = 1;
                st->flex_shrink = 1;
                st->flex_basis = l;
            }
        }
        break;
    }
    case PR_FLEX_DIRECTION: {
        int k = keyword(v, (const char *const[]){ "row", "row-reverse", "column", "column-reverse" }, 4);
        if (k >= 0) st->flex_dir = (uint8_t)k;
        break;
    }
    case PR_FLEX_WRAP: st->flex_wrap = ieq(v, vl, "wrap") || ieq(v, vl, "wrap-reverse"); break;
    case PR_FLEX_FLOW: {
        const char *tok[2];
        size_t len[2];
        int n = tokens(v, tok, len, 2);
        for (int i = 0; i < n; i++) {
            char tmp[32];
            size_t tl = len[i] < sizeof(tmp) - 1 ? len[i] : sizeof(tmp) - 1;
            memcpy(tmp, tok[i], tl);
            tmp[tl] = 0;
            if (strstr(tmp, "wrap")) apply(st, PR_FLEX_WRAP, tmp, c, a);
            else apply(st, PR_FLEX_DIRECTION, tmp, c, a);
        }
        break;
    }
    case PR_FLEX_GROW: st->flex_grow = (float)strtod(v, 0); break;
    case PR_FLEX_SHRINK: st->flex_shrink = (float)strtod(v, 0); break;
    case PR_FLEX_BASIS:
        if (ieq(v, vl, "content")) st->flex_basis.unit = U_AUTO;
        else parse_len(v, v + vl, st->font_size, c, &st->flex_basis);
        break;
    case PR_JUSTIFY_CONTENT: st->justify = align_kw(v); break;
    case PR_ALIGN_ITEMS: st->align_items = align_kw(v); if (ieq(v, vl, "normal")) st->align_items = JC_STRETCH; break;
    case PR_ALIGN_SELF: st->align_self = align_kw(v); break;
    case PR_ALIGN_CONTENT: st->align_content = align_kw(v); break;
    case PR_ORDER: st->order = atoi(v); break;
    case PR_GAP: case PR_ROW_GAP: case PR_COLUMN_GAP: {
        const char *tok[2];
        size_t len[2];
        int n = tokens(v, tok, len, 2);
        css_len_t g[2] = { { 0, U_PX }, { 0, U_PX } };
        for (int i = 0; i < n; i++) parse_len(tok[i], tok[i] + len[i], st->font_size, c, &g[i]);
        if (n == 1) g[1] = g[0];
        if (prop != PR_COLUMN_GAP) st->gap_row = g[0].unit == U_PX ? g[0].v : 0;
        if (prop != PR_ROW_GAP) st->gap_col = (prop == PR_COLUMN_GAP ? g[0] : g[1]).unit == U_PX ? (prop == PR_COLUMN_GAP ? g[0] : g[1]).v : 0;
        break;
    }
    case PR_GRID_TEMPLATE_COLUMNS: {
        const char *r = strstr(v, "repeat(");
        if (r) {
            int k = atoi(r + 7);
            st->grid_cols = k > 0 ? k : 0;
            if (!k && (strstr(r, "auto-fill") || strstr(r, "auto-fit"))) st->grid_cols = -1;
        } else {
            const char *tok[16];
            size_t len[16];
            st->grid_cols = tokens(v, tok, len, 16);
        }
        break;
    }
    case PR_CONTENT:
        if (v[0] == '"' || v[0] == '\'') {
            const char *e = strchr(v + 1, v[0]);
            st->content = arena_strndup(a, v + 1, e ? (size_t)(e - v - 1) : strlen(v + 1));
        } else if (ieq(v, vl, "none") || ieq(v, vl, "normal")) {
            st->content = 0;
        } else {
            st->content = "";
        }
        break;
    case PR_TABLE_LAYOUT: st->table_layout_fixed = ieq(v, vl, "fixed"); break;
    case PR_BORDER_COLLAPSE: st->border_collapse = ieq(v, vl, "collapse"); break;
    case PR_CURSOR: st->cursor_pointer = ieq(v, vl, "pointer"); break;
    case PR_BACKGROUND_SIZE: bg_size(st, v, vl, c); break;
    case PR_BACKGROUND_REPEAT: (void)bg_repeat(st, v, vl); break;
    case PR_BACKGROUND_POSITION: bg_position(st, v, vl, c); break;
    }
}

/* Replaces var(--x, fallback) with the variable's value (recursively). */
static const char *subst_vars(const char *v, const css_style_t *st, char *buf, size_t cap, int depth) {
    const char *p = strstr(v, "var(");
    size_t o = 0;
    if (!p || depth > 8) return v;
    while (*v && o + 1 < cap) {
        if (v == p) {
            const char *name = p + 4, *ne, *fb = 0, *close;
            int d = 1;
            while (*name && is_ws(*name)) name++;
            ne = name;
            while (*ne && *ne != ',' && *ne != ')' && !is_ws(*ne)) ne++;
            close = ne;
            while (*close && d) {
                if (*close == '(') d++;
                else if (*close == ')') d--;
                else if (*close == ',' && d == 1 && !fb) fb = close + 1;
                if (d) close++;
            }
            {
                const char *val = 0;
                char tmp[1024];
                for (css_var_t *x = st->vars; x; x = x->next) {
                    if (strlen(x->name) == (size_t)(ne - name) && memcmp(x->name, name, (size_t)(ne - name)) == 0) {
                        val = x->value;
                        break;
                    }
                }
                if (!val && fb) {
                    size_t fl = (size_t)(close - fb);
                    if (fl >= sizeof(tmp)) fl = sizeof(tmp) - 1;
                    memcpy(tmp, fb, fl);
                    tmp[fl] = 0;
                    val = tmp;
                    while (*val && is_ws(*val)) val++;
                }
                if (val) {
                    char inner[1024];
                    const char *r = subst_vars(val, st, inner, sizeof(inner), depth + 1);
                    size_t rl = strlen(r);
                    if (o + rl >= cap) rl = cap - o - 1;
                    memcpy(buf + o, r, rl);
                    o += rl;
                }
            }
            v = *close ? close + 1 : close;
            p = strstr(v, "var(");
            continue;
        }
        buf[o++] = *v++;
    }
    buf[o] = 0;
    return buf;
}

/* ---- the UA stylesheet -------------------------------------------------- */

static const char ua_css[] =
    "html,address,blockquote,body,dd,div,dl,dt,fieldset,form,frame,frameset,h1,h2,h3,h4,h5,h6,noframes,ol,p,ul,"
    "center,dir,hr,menu,pre,article,aside,footer,header,hgroup,main,nav,section,figure,figcaption,details,summary,"
    "legend,dialog,search{display:block}"
    "li{display:list-item}head,script,style,link,meta,title,noscript,template,base,param,datalist,area,map,"
    "[hidden],input[type=hidden],dialog:not([open]){display:none}"
    "table{display:table;border-spacing:2px;text-align:left}tr{display:table-row}thead{display:table-header-group}"
    "tbody{display:table-row-group}tfoot{display:table-footer-group}col{display:table-column}"
    "colgroup{display:table-column-group}td,th{display:table-cell;padding:1px;vertical-align:middle}"
    "caption{display:table-caption;text-align:center}"
    "body{margin:8px}p,dl,blockquote,figure,pre,ul,ol,menu{margin:1em 0}blockquote,figure{margin-left:40px;margin-right:40px}"
    "dd{margin-left:40px}ul,ol,menu{padding-left:40px}ol{list-style-type:decimal}ul{list-style-type:disc}"
    "ul ul,ol ul{list-style-type:circle;margin:0}ol ol,ul ol{margin:0}"
    "h1{font-size:2em;margin:.67em 0;font-weight:bold}h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
    "h3{font-size:1.17em;margin:1em 0;font-weight:bold}h4{margin:1.33em 0;font-weight:bold}"
    "h5{font-size:.83em;margin:1.67em 0;font-weight:bold}h6{font-size:.67em;margin:2.33em 0;font-weight:bold}"
    "b,strong,th,dt{font-weight:bold}i,em,cite,var,dfn,address{font-style:italic}"
    "pre,code,kbd,samp,tt{font-family:monospace}pre{white-space:pre}code,kbd,samp,tt{font-size:.9em}"
    "small{font-size:smaller}big{font-size:larger}sub{vertical-align:sub;font-size:smaller}"
    "sup{vertical-align:super;font-size:smaller}u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "a:link{color:#0b57d0;text-decoration:underline;cursor:pointer}mark{background-color:#ff0;color:#000}"
    "hr{border:1px inset #aaa;margin:.5em auto;height:0}center{text-align:center}th{text-align:center}"
    "img,video,canvas,svg,iframe,embed,object{display:inline-block}"
    "input,button,select,textarea{display:inline-block;font-size:13px;border:1px solid #888;padding:2px 4px;"
    "background-color:#fff;color:#000}button{background-color:#eee}textarea{white-space:pre-wrap}"
    "nobr{white-space:nowrap}fieldset{margin:0 2px;padding:.35em .75em .625em;border:2px groove #ccc}"
    "summary{font-weight:bold}abbr[title]{text-decoration:underline}";

static css_sheet_t *ua_sheet;

/* ---- cascade ------------------------------------------------------------ */

typedef struct {
    const decl_t *d;
    uint32_t      spec;
    uint32_t      order;
} hit_t;

typedef struct {
    hit_t *h;
    int    n, cap;
} hits_t;

static void hit_add(hits_t *hs, const rule_t *r, uint32_t spec, uint32_t order) {
    for (int i = 0; i < r->nd; i++) {
        if (hs->n == hs->cap) {
            hs->cap = hs->cap ? hs->cap * 2 : 256;
            hs->h = (hit_t *)realloc(hs->h, sizeof(hit_t) * (size_t)hs->cap);
            if (!hs->h) return;
        }
        hs->h[hs->n].d = &r->d[i];
        hs->h[hs->n].spec = spec;
        hs->h[hs->n].order = order;
        hs->n++;
    }
}

static int hit_cmp(const void *a, const void *b) {
    const hit_t *x = (const hit_t *)a, *y = (const hit_t *)b;
    if (x->spec != y->spec) return x->spec < y->spec ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static void collect_ref(hits_t *hs, css_sheet_t *sh, int sheet_index, ref_t ref, dom_node_t *el, int pseudo) {
    const rule_t *r = &sh->rules[ref.rule];
    const selector_t *sel = &r->sels[ref.sel];
    if (sel->pseudo_el != pseudo) return;
    for (int i = 0; i < sel->nanc; i++) {
        if (!bloom_has(sel->anc[i])) return;
    }
    if (!match_from(sel, sel->n - 1, el)) return;
    /* origin: UA rules always lose to author rules */
    hit_add(hs, r, sel->spec + (sh->origin_author ? 0x10000000U : 0), ((uint32_t)sheet_index << 20) | (uint32_t)ref.rule);
}

static void collect(hits_t *hs, css_sheet_t *sh, int sheet_index, dom_node_t *el, int pseudo) {
    bucket_t *b;
    index_t *ix;
    build_index(sh);
    ix = &sh->idx[pseudo ? 1 : 0];
    if (!ix->used) return;
    if (el->id && (b = bucket_get(ix->by_id, el->id)) != 0) {
        for (int i = 0; i < b->n; i++) collect_ref(hs, sh, sheet_index, b->refs[i], el, pseudo);
    }
    if (el->klass) {
        const char *p = el->klass;
        char cls[128];
        while (*p) {
            const char *w;
            size_t l;
            while (*p && is_ws(*p)) p++;
            w = p;
            while (*p && !is_ws(*p)) p++;
            l = (size_t)(p - w);
            if (!l || l >= sizeof(cls)) continue;
            memcpy(cls, w, l);
            cls[l] = 0;
            /* skip duplicate class names on the same element */
            {
                int dup = 0;
                for (const char *q = el->klass; q < w; q++) {
                    if ((q == el->klass || is_ws(q[-1])) && strncmp(q, cls, l) == 0 && (is_ws(q[l]) || q[l] == 0)) dup = 1;
                }
                if (dup) continue;
            }
            if ((b = bucket_get(ix->by_class, cls)) != 0) {
                for (int i = 0; i < b->n; i++) collect_ref(hs, sh, sheet_index, b->refs[i], el, pseudo);
            }
        }
    }
    if (el->name && (b = bucket_get(ix->by_tag, el->name)) != 0) {
        for (int i = 0; i < b->n; i++) collect_ref(hs, sh, sheet_index, b->refs[i], el, pseudo);
    }
    for (const dom_attr_t *a = el->attrs; a; a = a->next) {
        if ((b = bucket_get(ix->by_attr, a->name)) != 0) {
            for (int i = 0; i < b->n; i++) collect_ref(hs, sh, sheet_index, b->refs[i], el, pseudo);
        }
    }
    for (int i = 0; i < ix->nuniversal; i++) collect_ref(hs, sh, sheet_index, ix->universal[i], el, pseudo);
}

static void inherit(css_style_t *st, const css_style_t *p) {
    memset(st, 0, sizeof(*st));
    st->opacity = 1;
    st->visibility = 1;
    st->z_auto = 1;
    st->flex_shrink = 1;
    st->align_items = JC_STRETCH;
    st->align_self = JC_AUTO;
    st->width.unit = st->height.unit = U_AUTO;
    st->min_w.unit = st->min_h.unit = U_PX;
    st->max_w.unit = st->max_h.unit = U_NONE;
    st->flex_basis.unit = U_AUTO;
    for (int i = 0; i < 4; i++) {
        st->margin[i].unit = U_PX;
        st->padding[i].unit = U_PX;
        st->inset[i].unit = U_AUTO;
    }
    if (p) {
        st->color = p->color;
        st->font_size = p->font_size;
        st->font_weight = p->font_weight;
        st->font_italic = p->font_italic;
        st->font_family = p->font_family;
        st->lh_type = p->lh_type;
        st->lh_value = p->lh_value;
        st->letter_spacing = p->letter_spacing;
        st->word_spacing = p->word_spacing;
        st->text_align = p->text_align;
        st->text_indent = p->text_indent;
        st->text_transform = p->text_transform;
        st->white_space = p->white_space;
        st->visibility = p->visibility;
        st->list_style = p->list_style;
        st->list_inside = p->list_inside;
        st->cursor_pointer = p->cursor_pointer;
        st->border_collapse = p->border_collapse;
        st->vars = p->vars;
        st->decoration = 0;
    } else {
        st->color = 0xFF000000U;
        st->font_size = 16;
        st->font_weight = 400;
        st->list_style = LS_DISC;
    }
    for (int i = 0; i < 4; i++) st->border_color[i] = st->color;
}

static void apply_hits(css_style_t *st, hits_t *hs, const ctx_t *c, arena_t *a, int important_pass, int font_pass) {
    for (int i = 0; i < hs->n; i++) {
        const decl_t *d = hs->h[i].d;
        char buf[2048];
        if (d->important != important_pass) continue;
        if (d->prop == PR_CUSTOM) {
            if (font_pass) {
                css_var_t *v = (css_var_t *)arena_alloc(a, sizeof(css_var_t));
                if (!v) continue;
                v->name = d->name;
                v->value = subst_vars(d->value, st, buf, sizeof(buf), 0);
                if (v->value == buf) v->value = arena_strndup(a, buf, strlen(buf));
                v->next = st->vars;
                st->vars = v;
            }
            continue;
        }
        if (font_pass != (d->prop == PR_FONT_SIZE || d->prop == PR_FONT)) continue;
        apply(st, d->prop, subst_vars(d->value, st, buf, sizeof(buf), 0), c, a);
    }
}

/* Presentational attributes from legacy HTML (lowest author priority). */
static void apply_hints(css_style_t *st, dom_node_t *el, const ctx_t *c, arena_t *a) {
    const char *v;
    char tmp[64];
    if ((v = dom_attr(el, "bgcolor")) != 0) apply(st, PR_BACKGROUND_COLOR, v, c, a);
    if (el->tag == T_FONT && (v = dom_attr(el, "color")) != 0) apply(st, PR_COLOR, v, c, a);
    if ((el->tag == T_BODY) && (v = dom_attr(el, "text")) != 0) apply(st, PR_COLOR, v, c, a);
    if ((v = dom_attr(el, "align")) != 0) {
        if (el->tag == T_IMG || el->tag == T_TABLE) {
            if (strcmp(v, "left") == 0 || strcmp(v, "right") == 0) apply(st, PR_FLOAT, v, c, a);
            else if (strcmp(v, "center") == 0 && el->tag == T_TABLE) {
                st->margin[1].unit = st->margin[3].unit = U_AUTO;
            }
        } else {
            apply(st, PR_TEXT_ALIGN, v, c, a);
        }
    }
    if (el->tag == T_TD || el->tag == T_TH || el->tag == T_TR) {
        if ((v = dom_attr(el, "valign")) != 0) apply(st, PR_VERTICAL_ALIGN, v, c, a);
        if (dom_attr(el, "nowrap")) st->white_space = WS_NOWRAP;
    }
    if (el->tag == T_IMG || el->tag == T_TABLE || el->tag == T_TD || el->tag == T_TH || el->tag == T_IFRAME ||
        el->tag == T_VIDEO || el->tag == T_CANVAS || el->tag == T_HR || el->tag == T_COL || el->tag == T_SVG) {
        if ((v = dom_attr(el, "width")) != 0) {
            snprintf(tmp, sizeof(tmp), "%s%s", v, strchr(v, '%') ? "" : "px");
            apply(st, PR_WIDTH, tmp, c, a);
        }
        if ((v = dom_attr(el, "height")) != 0) {
            snprintf(tmp, sizeof(tmp), "%s%s", v, strchr(v, '%') ? "" : "px");
            apply(st, PR_HEIGHT, tmp, c, a);
        }
    }
    if (el->tag == T_TABLE && (v = dom_attr(el, "border")) != 0 && atoi(v) > 0) {
        snprintf(tmp, sizeof(tmp), "%dpx outset #888", atoi(v));
        apply(st, PR_BORDER, tmp, c, a);
    }
    if (el->tag == T_TABLE && (v = dom_attr(el, "cellpadding")) != 0) (void)v;
    if (el->tag == T_FONT && (v = dom_attr(el, "size")) != 0) {
        static const char *const sizes[] = { "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large" };
        int n = atoi(v);
        if (v[0] == '+') n = 3 + atoi(v + 1);
        else if (v[0] == '-') n = 3 - atoi(v + 1);
        if (n < 1) n = 1;
        if (n > 7) n = 7;
        apply(st, PR_FONT_SIZE, sizes[n - 1], c, a);
    }
    if (el->tag == T_FONT && (v = dom_attr(el, "face")) != 0) apply(st, PR_FONT_FAMILY, v, c, a);
}

static void fixup(css_style_t *st, dom_node_t *el) {
    /* floats and absolutely positioned boxes are blockified */
    if (st->float_ || st->position == P_ABSOLUTE || st->position == P_FIXED || el->tag == T_HTML) {
        if (st->display == D_INLINE || st->display == D_INLINE_BLOCK || st->display == D_TABLE_CELL ||
            st->display == D_TABLE_ROW || st->display == D_TABLE_ROW_GROUP) {
            st->display = D_BLOCK;
        } else if (st->display == D_INLINE_FLEX) st->display = D_FLEX;
        else if (st->display == D_INLINE_TABLE) st->display = D_TABLE;
    }
    if (st->position == P_ABSOLUTE || st->position == P_FIXED) st->float_ = F_NONE;
    if (st->lh_type == LH_PX) st->line_height = st->lh_value;
    else if (st->lh_type == LH_MULT) st->line_height = st->font_size * st->lh_value;
    else st->line_height = st->font_size * 1.2f;
    for (int i = 0; i < 4; i++) {
        if (st->border_style[i] == BS_NONE) st->border_w[i] = 0;
    }
}

typedef struct {
    css_sheet_t **sheets;
    int           n;
    float         vw, vh, root_font;
    hits_t        hits;
    arena_t      *arena;
} cascade_t;

static css_style_t *compute(cascade_t *cs, dom_node_t *el, const css_style_t *parent, int pseudo) {
    css_style_t *st = (css_style_t *)arena_alloc(cs->arena, sizeof(css_style_t));
    ctx_t c;
    const char *inline_style = pseudo ? 0 : dom_attr(el, "style");
    decl_t *inl = 0;
    int ninl = 0;
    if (!st) return 0;
    inherit(st, parent);
    c.parent = parent;
    c.root_font = cs->root_font;
    c.vw = cs->vw;
    c.vh = cs->vh;
    cs->hits.n = 0;
    if (ua_sheet) collect(&cs->hits, ua_sheet, 0, el, pseudo);
    for (int i = 0; i < cs->n; i++) collect(&cs->hits, cs->sheets[i], i + 1, el, pseudo);
    if (pseudo && cs->hits.n == 0) return 0;
    if (cs->hits.n > 1) qsort(cs->hits.h, (size_t)cs->hits.n, sizeof(hit_t), hit_cmp);
    if (inline_style) {
        /* parse the style attribute into the cascade arena (as an extra hit set) */
        css_sheet_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.arena = *cs->arena;
        ninl = parse_decls(&tmp, inline_style, inline_style + strlen(inline_style), &inl);
        *cs->arena = tmp.arena;
        for (int i = 0; i < ninl; i++) {
            if (cs->hits.n == cs->hits.cap) {
                cs->hits.cap = cs->hits.cap ? cs->hits.cap * 2 : 256;
                cs->hits.h = (hit_t *)realloc(cs->hits.h, sizeof(hit_t) * (size_t)cs->hits.cap);
                if (!cs->hits.h) break;
            }
            cs->hits.h[cs->hits.n].d = &inl[i];
            cs->hits.h[cs->hits.n].spec = 0xFFFFFFFFU;
            cs->hits.h[cs->hits.n].order = 0;
            cs->hits.n++;
        }
    }
    if (!pseudo) apply_hints(st, el, &c, cs->arena);
    /* font size first (other lengths depend on it), then everything else */
    apply_hits(st, &cs->hits, &c, cs->arena, 0, 1);
    apply_hits(st, &cs->hits, &c, cs->arena, 1, 1);
    apply_hits(st, &cs->hits, &c, cs->arena, 0, 0);
    apply_hits(st, &cs->hits, &c, cs->arena, 1, 0);
    if (!pseudo) fixup(st, el);
    else {
        if (!st->content) return 0;
        if (st->lh_type == LH_PX) st->line_height = st->lh_value;
        else if (st->lh_type == LH_MULT) st->line_height = st->font_size * st->lh_value;
        else st->line_height = st->font_size * 1.2f;
    }
    return st;
}

/* Hashes an element's class list once, so class selectors compare numbers. */
static void hash_classes(cascade_t *cs, dom_node_t *el) {
    uint32_t tmp[64];
    int n = 0;
    const char *p = el->klass;
    char cls[128];
    while (p && *p && n < 64) {
        const char *w;
        size_t l;
        while (*p && is_ws(*p)) p++;
        w = p;
        while (*p && !is_ws(*p)) p++;
        l = (size_t)(p - w);
        if (!l || l >= sizeof(cls)) continue;
        memcpy(cls, w, l);
        cls[l] = 0;
        tmp[n++] = key_hash(S_CLASS, cls);
    }
    el->nchash = (uint16_t)n;
    el->chash = (uint32_t *)arena_alloc(cs->arena, sizeof(uint32_t) * (size_t)(n ? n : 1));
    if (el->chash) memcpy(el->chash, tmp, sizeof(uint32_t) * (size_t)n);
}

static void bloom_element(dom_node_t *el, int d) {
    if (el->name) bloom_add(key_hash(S_TAG, el->name), d);
    if (el->id) bloom_add(key_hash(S_ID, el->id), d);
    for (int i = 0; i < el->nchash; i++) bloom_add(el->chash[i], d);
}

static void clear_styles(dom_node_t *n) {
    for (dom_node_t *c = n->first; c; c = c->next) {
        c->style = 0;
        clear_styles(c);
    }
}

static void walk(cascade_t *cs, dom_node_t *n, const css_style_t *parent) {
    for (dom_node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEMENT) continue;
        hash_classes(cs, c);
        c->style = compute(cs, c, parent, 0);
        if (!c->style) {
            clear_styles(c);
            continue;
        }
        if (c->tag == T_HTML) cs->root_font = c->style->font_size;
        if (c->style->display == D_NONE) {
            clear_styles(c);   /* hidden subtree: no styles, none left dangling */
            continue;
        }
        c->style->before = compute(cs, c, c->style, 1);
        c->style->after = compute(cs, c, c->style, 2);
        bloom_element(c, 1);
        walk(cs, c, c->style);
        bloom_element(c, -1);
    }
}

void css_cascade(dom_doc_t *doc, css_sheet_t **sheets, int n, int viewport_w, int viewport_h) {
    cascade_t cs;
    if (!ua_sheet) {
        ua_sheet = css_sheet_new();
        if (ua_sheet) css_parse(ua_sheet, ua_css, sizeof(ua_css) - 1, 0, viewport_w);
    }
    memset(&cs, 0, sizeof(cs));
    cs.sheets = sheets;
    cs.n = n;
    cs.vw = (float)viewport_w;
    cs.vh = (float)viewport_h;
    cs.root_font = 16;
    /* styles from the previous cascade (if any) are dropped wholesale */
    arena_free(&doc->style_arena);
    cs.arena = &doc->style_arena;
    in_cascade = 1;
    walk(&cs, doc->root, 0);
    in_cascade = 0;
    free(cs.hits.h);
}

void css_sheet_stats(css_sheet_t *sh, int *rules, int *universal) {
    build_index(sh);
    *rules = sh->nrules;
    *universal = sh->idx[0].nuniversal + sh->idx[1].nuniversal;
}

/* ---- selector API (querySelector / matches) ----------------------------- */

struct css_selector_list {
    css_sheet_t *owner;          /* arena for the compiled selectors */
    selector_t  *sels;
    int          n;
};

css_selector_list_t *css_selector_parse(const char *text) {
    css_selector_list_t *l;
    const char *s = text, *end;
    int cap = 8;
    if (!text) return 0;
    end = text + strlen(text);
    l = (css_selector_list_t *)calloc(1, sizeof(*l));
    if (!l) return 0;
    l->owner = css_sheet_new();
    l->sels = (selector_t *)malloc(sizeof(selector_t) * (size_t)cap);
    if (!l->owner || !l->sels) { css_selector_free(l); return 0; }
    while (s < end) {
        /* split at top-level commas (not inside :is(...), :not(...), [..]) */
        const char *e = s;
        int depth = 0;
        char quote = 0;
        while (e < end) {
            if (quote) { if (*e == quote) quote = 0; }
            else if (*e == '"' || *e == '\'') quote = *e;
            else if (*e == '(' || *e == '[') depth++;
            else if (*e == ')' || *e == ']') depth--;
            else if (*e == ',' && depth == 0) break;
            e++;
        }
        if (l->n == cap) {
            selector_t *ns = (selector_t *)realloc(l->sels, sizeof(selector_t) * (size_t)(cap *= 2));
            if (!ns) { css_selector_free(l); return 0; }
            l->sels = ns;
        }
        if (parse_selector(l->owner, s, e, &l->sels[l->n]) != 0 || l->sels[l->n].pseudo_el) {
            css_selector_free(l);   /* invalid selector: the DOM API throws */
            return 0;
        }
        l->n++;
        s = e < end ? e + 1 : end;
    }
    if (!l->n) { css_selector_free(l); return 0; }
    return l;
}

int css_selector_matches(const css_selector_list_t *l, dom_node_t *el) {
    if (!l || !el || el->type != N_ELEMENT) return 0;
    for (int i = 0; i < l->n; i++) {
        if (match_from(&l->sels[i], l->sels[i].n - 1, el)) return 1;
    }
    return 0;
}

void css_selector_free(css_selector_list_t *l) {
    if (!l) return;
    if (l->owner) css_sheet_free(l->owner);
    free(l->sels);
    free(l);
}
