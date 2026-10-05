/* HTML parser for Surfer: an HTML5-style tokenizer (tags, attributes,
 * comments, doctype, raw text and RCDATA, character references) feeding a
 * forgiving tree builder that handles the implied structure real pages rely
 * on.  It never fails; malformed input just produces a sensible tree. */
#include "dom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- arena -------------------------------------------------------------- */

struct arena_block {
    arena_block_t *next;
    size_t         size, used;
    unsigned char  data[];
};

void *arena_alloc(arena_t *a, size_t size) {
    arena_block_t *b = a->head;
    size = (size + 15) & ~(size_t)15;
    if (!b || b->used + size > b->size) {
        size_t cap = size > 60000 ? size : 60000;
        b = (arena_block_t *)malloc(sizeof(arena_block_t) + cap);
        if (!b) return 0;
        b->size = cap;
        b->used = 0;
        b->next = a->head;
        a->head = b;
    }
    {
        void *p = b->data + b->used;
        b->used += size;
        a->used_total += size;
        memset(p, 0, size);
        return p;
    }
}

char *arena_strndup(arena_t *a, const char *s, size_t n) {
    char *p = (char *)arena_alloc(a, n + 1);
    if (!p) return 0;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void arena_free(arena_t *a) {
    arena_block_t *b = a->head;
    while (b) {
        arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    a->head = 0;
    a->used_total = 0;
}

/* ---- tags --------------------------------------------------------------- */

static const char *const tag_names[T_COUNT] = {
    "", "html", "head", "body", "title", "meta", "link", "style", "script", "noscript", "base",
    "div", "span", "p", "a", "img", "br", "hr", "ul", "ol", "li", "dl", "dt", "dd",
    "h1", "h2", "h3", "h4", "h5", "h6", "pre", "code", "blockquote", "em", "strong", "b", "i",
    "u", "s", "small", "sub", "sup", "table", "thead", "tbody", "tfoot", "tr", "td", "th",
    "caption", "col", "colgroup", "form", "input", "button", "select", "option", "textarea",
    "label", "nav", "header", "footer", "main", "section", "article", "aside", "figure",
    "figcaption", "iframe", "svg", "canvas", "video", "audio", "source", "picture", "template",
    "center", "font", "abbr", "cite", "q", "kbd", "samp", "var", "mark", "time", "wbr",
    "details", "summary", "fieldset", "legend", "optgroup", "address", "del", "ins", "big",
    "tt", "nobr", "area", "map", "embed", "object", "param", "track", "dialog", "menu"
};

int tag_lookup(const char *name, size_t len) {
    for (int i = 1; i < T_COUNT; i++) {
        if (strlen(tag_names[i]) == len && memcmp(tag_names[i], name, len) == 0) return i;
    }
    return T_UNKNOWN;
}

const char *tag_name(int tag) {
    return tag > 0 && tag < T_COUNT ? tag_names[tag] : "";
}

static int is_void(int t) {
    return t == T_AREA || t == T_BASE || t == T_BR || t == T_COL || t == T_EMBED || t == T_HR ||
           t == T_IMG || t == T_INPUT || t == T_LINK || t == T_META || t == T_PARAM || t == T_SOURCE ||
           t == T_TRACK || t == T_WBR;
}

/* elements whose start tag closes an open <p> */
static int closes_p(int t) {
    switch (t) {
    case T_ADDRESS: case T_ARTICLE: case T_ASIDE: case T_BLOCKQUOTE: case T_DETAILS: case T_DIV:
    case T_DL: case T_FIELDSET: case T_FIGCAPTION: case T_FIGURE: case T_FOOTER: case T_FORM:
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: case T_HEADER: case T_HR:
    case T_MAIN: case T_MENU: case T_NAV: case T_OL: case T_P: case T_PRE: case T_SECTION:
    case T_TABLE: case T_UL: case T_CENTER: case T_DIALOG:
        return 1;
    default:
        return 0;
    }
}

static int head_only(int t) {
    return t == T_TITLE || t == T_META || t == T_LINK || t == T_STYLE || t == T_BASE;
}

/* ---- character references ----------------------------------------------- */

typedef struct {
    const char *name;
    uint32_t    cp;
} entity_t;

static const entity_t entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "hellip", 0x2026 }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "sbquo", 0x201A }, { "ldquo", 0x201C },
    { "rdquo", 0x201D }, { "bdquo", 0x201E }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "laquo", 0xAB },
    { "raquo", 0xBB }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203A }, { "times", 0xD7 }, { "divide", 0xF7 },
    { "deg", 0xB0 }, { "plusmn", 0xB1 }, { "para", 0xB6 }, { "sect", 0xA7 }, { "cent", 0xA2 },
    { "pound", 0xA3 }, { "euro", 0x20AC }, { "yen", 0xA5 }, { "curren", 0xA4 }, { "frac12", 0xBD },
    { "frac14", 0xBC }, { "frac34", 0xBE }, { "sup1", 0xB9 }, { "sup2", 0xB2 }, { "sup3", 0xB3 },
    { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "acute", 0xB4 }, { "uml", 0xA8 }, { "cedil", 0xB8 },
    { "ordf", 0xAA }, { "ordm", 0xBA }, { "not", 0xAC }, { "shy", 0xAD }, { "macr", 0xAF }, { "micro", 0xB5 },
    { "brvbar", 0xA6 }, { "dagger", 0x2020 }, { "Dagger", 0x2021 }, { "permil", 0x2030 }, { "prime", 0x2032 },
    { "Prime", 0x2033 }, { "larr", 0x2190 }, { "uarr", 0x2191 }, { "rarr", 0x2192 }, { "darr", 0x2193 },
    { "harr", 0x2194 }, { "ensp", 0x2002 }, { "emsp", 0x2003 }, { "thinsp", 0x2009 }, { "zwnj", 0x200C },
    { "zwj", 0x200D }, { "lrm", 0x200E }, { "rlm", 0x200F }, { "hearts", 0x2665 }, { "check", 0x2713 },
    { "Agrave", 0xC0 }, { "Aacute", 0xC1 }, { "Acirc", 0xC2 }, { "Atilde", 0xC3 }, { "Auml", 0xC4 },
    { "Aring", 0xC5 }, { "AElig", 0xC6 }, { "Ccedil", 0xC7 }, { "Egrave", 0xC8 }, { "Eacute", 0xC9 },
    { "Ecirc", 0xCA }, { "Euml", 0xCB }, { "Igrave", 0xCC }, { "Iacute", 0xCD }, { "Icirc", 0xCE },
    { "Iuml", 0xCF }, { "ETH", 0xD0 }, { "Ntilde", 0xD1 }, { "Ograve", 0xD2 }, { "Oacute", 0xD3 },
    { "Ocirc", 0xD4 }, { "Otilde", 0xD5 }, { "Ouml", 0xD6 }, { "Oslash", 0xD8 }, { "Ugrave", 0xD9 },
    { "Uacute", 0xDA }, { "Ucirc", 0xDB }, { "Uuml", 0xDC }, { "Yacute", 0xDD }, { "THORN", 0xDE },
    { "szlig", 0xDF }, { "agrave", 0xE0 }, { "aacute", 0xE1 }, { "acirc", 0xE2 }, { "atilde", 0xE3 },
    { "auml", 0xE4 }, { "aring", 0xE5 }, { "aelig", 0xE6 }, { "ccedil", 0xE7 }, { "egrave", 0xE8 },
    { "eacute", 0xE9 }, { "ecirc", 0xEA }, { "euml", 0xEB }, { "igrave", 0xEC }, { "iacute", 0xED },
    { "icirc", 0xEE }, { "iuml", 0xEF }, { "eth", 0xF0 }, { "ntilde", 0xF1 }, { "ograve", 0xF2 },
    { "oacute", 0xF3 }, { "ocirc", 0xF4 }, { "otilde", 0xF5 }, { "ouml", 0xF6 }, { "oslash", 0xF8 },
    { "ugrave", 0xF9 }, { "uacute", 0xFA }, { "ucirc", 0xFB }, { "uuml", 0xFC }, { "yacute", 0xFD },
    { "thorn", 0xFE }, { "yuml", 0xFF }, { "OElig", 0x152 }, { "oelig", 0x153 }, { "Scaron", 0x160 },
    { "scaron", 0x161 }, { "alpha", 0x3B1 }, { "beta", 0x3B2 }, { "gamma", 0x3B3 }, { "delta", 0x3B4 },
    { "pi", 0x3C0 }, { "mu", 0x3BC }, { "sigma", 0x3C3 }, { "omega", 0x3C9 }, { "infin", 0x221E },
    { "ne", 0x2260 }, { "le", 0x2264 }, { "ge", 0x2265 }, { "minus", 0x2212 }, { "sum", 0x2211 },
    { "radic", 0x221A }, { "asymp", 0x2248 }, { "equiv", 0x2261 }, { "star", 0x2606 }, { "starf", 0x2605 }
};

static size_t put_utf8(char *out, uint32_t cp) {
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Windows-1252 values for 0x80..0x9F numeric references */
static const uint16_t cp1252[32] = {
    0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030, 0x160, 0x2039, 0x152, 0x8D,
    0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161,
    0x203A, 0x153, 0x9D, 0x17E, 0x178
};

/* Decodes one reference at s (pointing just after '&'); returns the number
 * of input bytes consumed (0 if it is not a reference). */
static size_t decode_ref(const char *s, const char *end, uint32_t *cp) {
    const char *p = s;
    if (p < end && *p == '#') {
        uint32_t v = 0;
        int hex = 0, digits = 0;
        p++;
        if (p < end && (*p == 'x' || *p == 'X')) {
            hex = 1;
            p++;
        }
        while (p < end) {
            int d;
            if (*p >= '0' && *p <= '9') d = *p - '0';
            else if (hex && ((*p | 32) >= 'a' && (*p | 32) <= 'f')) d = (*p | 32) - 'a' + 10;
            else break;
            if (v < 0x110000) v = v * (hex ? 16 : 10) + (uint32_t)d;
            digits++;
            p++;
        }
        if (!digits) return 0;
        if (p < end && *p == ';') p++;
        if (v >= 0x80 && v <= 0x9F) v = cp1252[v - 0x80];
        *cp = v;
        return (size_t)(p - s);
    }
    {
        size_t n = 0;
        while (s + n < end && n < 10 && (((s[n] | 32) >= 'a' && (s[n] | 32) <= 'z') || (s[n] >= '0' && s[n] <= '9'))) n++;
        for (size_t len = n; len >= 2; len--) {
            for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
                if (strlen(entities[i].name) == len && memcmp(entities[i].name, s, len) == 0) {
                    if (s + len < end && s[len] == ';') len++;
                    else if (len != n) continue;
                    *cp = entities[i].cp;
                    return len;
                }
            }
        }
    }
    return 0;
}

/* Copies text into the arena, decoding character references. */
static char *decode_text(arena_t *a, const char *s, size_t n, size_t *out_len) {
    char *out = (char *)arena_alloc(a, n + 1), *o = out;
    const char *end = s + n;
    if (!out) return 0;
    while (s < end) {
        if (*s == '&') {
            uint32_t cp;
            size_t used = decode_ref(s + 1, end, &cp);
            if (used) {
                char tmp[4];
                size_t k = put_utf8(tmp, cp);
                if ((size_t)(o - out) + k <= n) {
                    memcpy(o, tmp, k);
                    o += k;
                }
                s += 1 + used;
                continue;
            }
        }
        *o++ = *s++;
    }
    *o = 0;
    if (out_len) *out_len = (size_t)(o - out);
    return out;
}

/* ---- tree building ------------------------------------------------------ */

#define STACK_MAX 512

typedef struct {
    dom_doc_t  *doc;
    dom_node_t *stack[STACK_MAX];
    int         depth;
    int         in_body;
} builder_t;

static dom_node_t *new_node(builder_t *b, int type) {
    dom_node_t *n = (dom_node_t *)arena_alloc(&b->doc->arena, sizeof(dom_node_t));
    if (n) n->type = (uint8_t)type;
    return n;
}

static void append(dom_node_t *parent, dom_node_t *child) {
    child->parent = parent;
    child->prev = parent->last;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static dom_node_t *current(builder_t *b) {
    return b->stack[b->depth - 1];
}

static void push(builder_t *b, dom_node_t *n) {
    if (b->depth < STACK_MAX) b->stack[b->depth++] = n;
}

static int in_stack(builder_t *b, int tag, int stop_at_table) {
    for (int i = b->depth - 1; i >= 0; i--) {
        int t = b->stack[i]->tag;
        if (t == tag) return i;
        if (t == T_HTML || t == T_BODY) return -1;
        if (stop_at_table && (t == T_TABLE || t == T_TD || t == T_TH)) return -1;
    }
    return -1;
}

static void pop_to(builder_t *b, int index) {
    if (index >= 2) b->depth = index;   /* never pop html/body */
}

static void ensure_body(builder_t *b) {
    if (b->in_body) return;
    b->in_body = 1;
    b->depth = 1;
    push(b, b->doc->body);
}

static void insert_text(builder_t *b, const char *s, size_t n, int raw) {
    dom_node_t *t;
    dom_node_t *parent;
    if (!n) return;
    if (!b->in_body) {
        size_t i = 0;
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f')) i++;
        if (i == n && current(b)->tag != T_TITLE) return;     /* whitespace in head */
        if (current(b) == b->doc->head || current(b) == b->doc->html) ensure_body(b);
    }
    parent = current(b);
    /* merge with a preceding text node */
    if (parent->last && parent->last->type == N_TEXT && !raw) {
        dom_node_t *prev = parent->last;
        size_t dl;
        char *dec = decode_text(&b->doc->arena, s, n, &dl);
        char *joined = (char *)arena_alloc(&b->doc->arena, prev->text_len + dl + 1);
        if (!dec || !joined) return;
        memcpy(joined, prev->text, prev->text_len);
        memcpy(joined + prev->text_len, dec, dl);
        prev->text = joined;
        prev->text_len += dl;
        return;
    }
    t = new_node(b, N_TEXT);
    if (!t) return;
    if (raw) {
        t->text = arena_strndup(&b->doc->arena, s, n);
        t->text_len = n;
    } else {
        t->text = decode_text(&b->doc->arena, s, n, &t->text_len);
    }
    append(parent, t);
}

static void start_tag(builder_t *b, dom_node_t *el, int self_closing) {
    int t = el->tag;
    if (t == T_HTML) {
        /* merge attributes onto the existing root */
        if (!b->doc->html->attrs) b->doc->html->attrs = el->attrs;
        return;
    }
    if (t == T_BODY) {
        if (!b->doc->body->attrs) b->doc->body->attrs = el->attrs;
        ensure_body(b);
        return;
    }
    if (t == T_HEAD) return;
    if (!b->in_body && !head_only(t) && t != T_SCRIPT && t != T_NOSCRIPT && t != T_TEMPLATE) ensure_body(b);
    if (b->in_body) {
        int i;
        if (closes_p(t) && (i = in_stack(b, T_P, 1)) >= 0) pop_to(b, i);
        if (t == T_LI && (i = in_stack(b, T_LI, 0)) >= 0) {
            int ok = 1;
            for (int k = i + 1; k < b->depth; k++) {
                if (b->stack[k]->tag == T_UL || b->stack[k]->tag == T_OL) ok = 0;
            }
            if (ok) pop_to(b, i);
        }
        if ((t == T_DT || t == T_DD) && ((i = in_stack(b, T_DD, 0)) >= 0 || (i = in_stack(b, T_DT, 0)) >= 0)) pop_to(b, i);
        if (t == T_OPTION && current(b)->tag == T_OPTION) b->depth--;
        if (t == T_TR && (i = in_stack(b, T_TR, 1)) >= 0) pop_to(b, i);
        if ((t == T_TD || t == T_TH) && ((i = in_stack(b, T_TD, 1)) >= 0 || (i = in_stack(b, T_TH, 1)) >= 0)) pop_to(b, i);
        if ((t == T_THEAD || t == T_TBODY || t == T_TFOOT) && current(b)->tag != T_TABLE) {
            int k = in_stack(b, T_TABLE, 0);
            if (k >= 0) pop_to(b, k + 1);
        }
        if (t == T_A && (i = in_stack(b, T_A, 0)) >= 0) pop_to(b, i);
        if ((t >= T_H1 && t <= T_H6) && current(b)->tag >= T_H1 && current(b)->tag <= T_H6) b->depth--;
    }
    append(current(b), el);
    if (!is_void(t) && !self_closing) push(b, el);
}

static void end_tag(builder_t *b, int tag, const char *name, size_t nlen) {
    if (tag == T_BODY || tag == T_HTML || tag == T_HEAD) return;
    if (tag == T_P && in_stack(b, T_P, 1) < 0) return;
    for (int i = b->depth - 1; i >= 2; i--) {
        dom_node_t *n = b->stack[i];
        if (tag != T_UNKNOWN ? n->tag == tag : (n->tag == T_UNKNOWN && strlen(n->name) == nlen && memcmp(n->name, name, nlen) == 0)) {
            pop_to(b, i);
            return;
        }
        /* do not close across these boundaries */
        if ((n->tag == T_TABLE || n->tag == T_TD || n->tag == T_TH) && tag != T_TABLE && tag != T_TD &&
            tag != T_TH && tag != T_TR && tag != T_TBODY && tag != T_THEAD && tag != T_TFOOT) {
            return;
        }
    }
    if (!b->in_body) {
        for (int i = b->depth - 1; i >= 1; i--) {
            if (b->stack[i]->tag == tag) {
                b->depth = i;
                return;
            }
        }
    }
}

/* ---- tokenizer ---------------------------------------------------------- */

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static char *lower_dup(arena_t *a, const char *s, size_t n) {
    char *p = arena_strndup(a, s, n);
    for (size_t i = 0; p && i < n; i++) {
        if (p[i] >= 'A' && p[i] <= 'Z') p[i] += 32;
    }
    return p;
}

/* Parses a start tag at s ('<' already consumed). */
static const char *parse_start_tag(builder_t *b, const char *s, const char *end, dom_node_t **out, int *self_closing) {
    const char *name = s;
    dom_node_t *el;
    dom_attr_t **tail;
    while (s < end && !is_space(*s) && *s != '>' && *s != '/') s++;
    el = new_node(b, N_ELEMENT);
    if (!el) return end;
    el->name = lower_dup(&b->doc->arena, name, (size_t)(s - name));
    el->tag = (uint16_t)tag_lookup(el->name, (size_t)(s - name));
    tail = &el->attrs;
    *self_closing = 0;
    for (;;) {
        const char *an, *av = 0;
        size_t anl, avl = 0;
        while (s < end && (is_space(*s) || *s == '/')) {
            if (*s == '/' && s + 1 < end && s[1] == '>') *self_closing = 1;
            s++;
        }
        if (s >= end) break;
        if (*s == '>') {
            s++;
            break;
        }
        an = s;
        while (s < end && !is_space(*s) && *s != '=' && *s != '>' && !(*s == '/' && s + 1 < end && s[1] == '>')) s++;
        anl = (size_t)(s - an);
        while (s < end && is_space(*s)) s++;
        if (s < end && *s == '=') {
            s++;
            while (s < end && is_space(*s)) s++;
            if (s < end && (*s == '"' || *s == '\'')) {
                char q = *s++;
                av = s;
                while (s < end && *s != q) s++;
                avl = (size_t)(s - av);
                if (s < end) s++;
            } else {
                av = s;
                while (s < end && !is_space(*s) && *s != '>') s++;
                avl = (size_t)(s - av);
            }
        }
        if (anl) {
            dom_attr_t *a = (dom_attr_t *)arena_alloc(&b->doc->arena, sizeof(dom_attr_t));
            if (!a) break;
            a->name = lower_dup(&b->doc->arena, an, anl);
            a->value = av ? decode_text(&b->doc->arena, av, avl, 0) : "";
            *tail = a;
            tail = &a->next;
            if (strcmp(a->name, "id") == 0) el->id = a->value;
            else if (strcmp(a->name, "class") == 0) el->klass = a->value;
        }
    }
    *out = el;
    return s;
}

dom_doc_t *html_parse(const char *src, size_t len, const char *url) {
    dom_doc_t *doc = (dom_doc_t *)calloc(1, sizeof(dom_doc_t));
    builder_t *b = (builder_t *)calloc(1, sizeof(builder_t));
    const char *s = src, *end = src + len, *text = src;
    if (!doc || !b) {
        free(doc);
        free(b);
        return 0;
    }
    snprintf(doc->base_url, sizeof(doc->base_url), "%s", url ? url : "");
    b->doc = doc;
    doc->root = new_node(b, N_DOCUMENT);
    doc->html = new_node(b, N_ELEMENT);
    doc->head = new_node(b, N_ELEMENT);
    doc->body = new_node(b, N_ELEMENT);
    doc->html->tag = T_HTML;
    doc->html->name = "html";
    doc->head->tag = T_HEAD;
    doc->head->name = "head";
    doc->body->tag = T_BODY;
    doc->body->name = "body";
    append(doc->root, doc->html);
    append(doc->html, doc->head);
    append(doc->html, doc->body);
    push(b, doc->html);
    push(b, doc->head);
    /* skip a UTF-8 byte order mark */
    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s += 3;
        text = s;
    }
    while (s < end) {
        if (*s != '<') {
            s++;
            continue;
        }
        if (s + 1 >= end) break;
        if (s[1] == '!' || s[1] == '?') {
            insert_text(b, text, (size_t)(s - text), 0);
            if (s + 4 <= end && s[2] == '-' && s[3] == '-') {
                const char *c = s + 4;
                while (c + 3 <= end && !(c[0] == '-' && c[1] == '-' && c[2] == '>')) c++;
                s = c + 3 <= end ? c + 3 : end;
            } else {
                while (s < end && *s != '>') s++;
                if (s < end) s++;
            }
            text = s;
            continue;
        }
        if (s[1] == '/') {
            const char *n = s + 2, *ne;
            if (n >= end || !((*n | 32) >= 'a' && (*n | 32) <= 'z')) {
                s++;
                continue;
            }
            insert_text(b, text, (size_t)(s - text), 0);
            ne = n;
            while (ne < end && !is_space(*ne) && *ne != '>') ne++;
            {
                char lower[32];
                size_t nl = (size_t)(ne - n) < sizeof(lower) ? (size_t)(ne - n) : sizeof(lower) - 1;
                for (size_t i = 0; i < nl; i++) lower[i] = (n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i];
                end_tag(b, tag_lookup(lower, nl), lower, nl);
            }
            while (ne < end && *ne != '>') ne++;
            s = ne < end ? ne + 1 : end;
            text = s;
            continue;
        }
        if (!((s[1] | 32) >= 'a' && (s[1] | 32) <= 'z')) {
            s++;
            continue;
        }
        insert_text(b, text, (size_t)(s - text), 0);
        {
            dom_node_t *el = 0;
            int self_closing = 0;
            s = parse_start_tag(b, s + 1, end, &el, &self_closing);
            text = s;
            if (!el) continue;
            start_tag(b, el, self_closing);
            /* raw text and RCDATA elements consume everything up to their end tag */
            if (el->tag == T_SCRIPT || el->tag == T_STYLE || el->tag == T_TEXTAREA || el->tag == T_TITLE ||
                el->tag == T_NOSCRIPT || el->tag == T_IFRAME || el->tag == T_TEMPLATE) {
                const char *name = el->name;
                size_t nl = strlen(name);
                const char *c = s;
                while (c + 2 + nl <= end && !(c[0] == '<' && c[1] == '/' && ieq_n(c + 2, name, nl))) c++;
                if (c + 2 + nl > end) c = end;
                if (el->tag != T_NOSCRIPT && el->tag != T_TEMPLATE) {
                    insert_text(b, s, (size_t)(c - s), el->tag == T_SCRIPT || el->tag == T_STYLE);
                }
                if (current(b) == el) b->depth--;
                while (c < end && *c != '>') c++;
                s = c < end ? c + 1 : end;
                text = s;
            }
        }
    }
    insert_text(b, text, (size_t)(end - text), 0);
    {
        dom_node_t *t = dom_find(doc->head, T_TITLE);
        if (t) dom_text(t, doc->title, sizeof(doc->title));
    }
    {
        dom_node_t *base = dom_find(doc->head, T_BASE);
        const char *href = base ? dom_attr(base, "href") : 0;
        if (href && *href) snprintf(doc->base_url, sizeof(doc->base_url), "%s", href);
    }
    free(b);
    return doc;
}

void dom_free(dom_doc_t *doc) {
    if (!doc) return;
    arena_free(&doc->arena);
    free(doc);
}

const char *dom_attr(const dom_node_t *n, const char *name) {
    for (const dom_attr_t *a = n ? n->attrs : 0; a; a = a->next) {
        if (strcmp(a->name, name) == 0) return a->value;
    }
    return 0;
}

int dom_has_class(const dom_node_t *n, const char *cls) {
    const char *p = n->klass;
    size_t cl = strlen(cls);
    while (p && *p) {
        while (*p && is_space(*p)) p++;
        {
            const char *w = p;
            while (*p && !is_space(*p)) p++;
            if ((size_t)(p - w) == cl && memcmp(w, cls, cl) == 0) return 1;
        }
    }
    return 0;
}

dom_node_t *dom_find(dom_node_t *from, int tag) {
    for (dom_node_t *c = from ? from->first : 0; c; c = c->next) {
        dom_node_t *hit;
        if (c->type == N_ELEMENT && c->tag == tag) return c;
        if ((hit = dom_find(c, tag)) != 0) return hit;
    }
    return 0;
}

static void text_walk(const dom_node_t *n, char *out, size_t cap, size_t *len, int *space) {
    for (const dom_node_t *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            for (size_t i = 0; i < c->text_len && *len + 1 < cap; i++) {
                char ch = c->text[i];
                if (is_space(ch)) {
                    if (!*space && *len) out[(*len)++] = ' ';
                    *space = 1;
                } else {
                    out[(*len)++] = ch;
                    *space = 0;
                }
            }
        } else if (c->type == N_ELEMENT && c->tag != T_SCRIPT && c->tag != T_STYLE) {
            text_walk(c, out, cap, len, space);
        }
    }
}

size_t dom_text(const dom_node_t *n, char *out, size_t cap) {
    size_t len = 0;
    int space = 1;
    if (!cap) return 0;
    text_walk(n, out, cap, &len, &space);
    while (len && out[len - 1] == ' ') len--;
    out[len] = 0;
    return len;
}
