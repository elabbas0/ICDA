#ifndef SURFER_DOM_H
#define SURFER_DOM_H

#include <stdint.h>
#include <stddef.h>

/* DOM for Surfer.  Every node, attribute and string of a document lives in
 * one arena, so building a page is a few pointer bumps and closing it is a
 * single free. */

typedef struct arena_block arena_block_t;
typedef struct {
    arena_block_t *head;
    size_t         used_total;
} arena_t;

void *arena_alloc(arena_t *a, size_t size);
char *arena_strndup(arena_t *a, const char *s, size_t n);
void  arena_free(arena_t *a);

enum {
    T_UNKNOWN = 0,
    T_HTML, T_HEAD, T_BODY, T_TITLE, T_META, T_LINK, T_STYLE, T_SCRIPT, T_NOSCRIPT, T_BASE,
    T_DIV, T_SPAN, T_P, T_A, T_IMG, T_BR, T_HR, T_UL, T_OL, T_LI, T_DL, T_DT, T_DD,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_PRE, T_CODE, T_BLOCKQUOTE, T_EM, T_STRONG, T_B, T_I,
    T_U, T_S, T_SMALL, T_SUB, T_SUP, T_TABLE, T_THEAD, T_TBODY, T_TFOOT, T_TR, T_TD, T_TH,
    T_CAPTION, T_COL, T_COLGROUP, T_FORM, T_INPUT, T_BUTTON, T_SELECT, T_OPTION, T_TEXTAREA,
    T_LABEL, T_NAV, T_HEADER, T_FOOTER, T_MAIN, T_SECTION, T_ARTICLE, T_ASIDE, T_FIGURE,
    T_FIGCAPTION, T_IFRAME, T_SVG, T_CANVAS, T_VIDEO, T_AUDIO, T_SOURCE, T_PICTURE, T_TEMPLATE,
    T_CENTER, T_FONT, T_ABBR, T_CITE, T_Q, T_KBD, T_SAMP, T_VAR, T_MARK, T_TIME, T_WBR,
    T_DETAILS, T_SUMMARY, T_FIELDSET, T_LEGEND, T_OPTGROUP, T_ADDRESS, T_DEL, T_INS, T_BIG,
    T_TT, T_NOBR, T_AREA, T_MAP, T_EMBED, T_OBJECT, T_PARAM, T_TRACK, T_DIALOG, T_MENU,
    T_COUNT
};

enum { N_DOCUMENT = 0, N_ELEMENT, N_TEXT, N_COMMENT };

typedef struct dom_attr {
    const char      *name;    /* lower case */
    const char      *value;
    struct dom_attr *next;
} dom_attr_t;

struct css_style;

typedef struct dom_node {
    uint8_t          type;
    uint16_t         tag;      /* T_* for elements */
    const char      *name;     /* tag name, lower case */
    const char      *text;     /* text/comment data */
    size_t           text_len;
    dom_attr_t      *attrs;
    const char      *id;       /* cached attribute values */
    const char      *klass;
    struct dom_node *parent, *first, *last, *prev, *next;
    struct css_style *style;   /* computed style, set by the cascade */
    uint32_t        *chash;    /* class name hashes, filled by the cascade */
    uint16_t         nchash;
    void            *layout;   /* layout box, owned by the layout pass */
    struct form_ctl *ctl;      /* live form-control state (value, checked), see form.h */
    void            *js;       /* script wrapper object, owned by the script engine */
} dom_node_t;

typedef struct {
    arena_t     arena;
    dom_node_t *root;          /* the document node */
    dom_node_t *html, *head, *body;
    char        title[256];
    char        base_url[2048];
} dom_doc_t;

dom_doc_t  *html_parse(const char *src, size_t len, const char *url);
void        dom_free(dom_doc_t *doc);

int         tag_lookup(const char *name, size_t len);
const char *tag_name(int tag);
const char *dom_attr(const dom_node_t *n, const char *name);
int         dom_has_class(const dom_node_t *n, const char *cls);
dom_node_t *dom_find(dom_node_t *from, int tag);
/* Concatenated text content, whitespace collapsed (title, alt text). */
size_t      dom_text(const dom_node_t *n, char *out, size_t cap);

#endif
