/* Surfer JavaScript: QuickJS with a DOM over Surfer's tree.
 *
 * The native layer here is deliberately small: node wrappers and tree
 * mutation, attributes, (de)serialization, selectors, form state, layout
 * queries, timers, network requests and script/module loading.  Everything
 * else in the DOM API (events, classList, style, URL, XHR, storage, ...) is
 * written in JavaScript in prelude.js, on top of these primitives. */
#include "js.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "icda_sys.h"
#include "third_party/quickjs/quickjs.h"
#include "http.h"
#include "css.h"
#include "form.h"

extern const char     surfer_prelude_js[];
extern const unsigned surfer_prelude_js_len;

#define MAX_TIMERS_PER_TICK 256
#define MAX_JOBS_PER_TICK   100000

typedef struct {
    int     id;
    double  due, interval;     /* ms; interval < 0: one shot */
    int     raf;
    JSValue fn;
    int     argc;
    JSValue argv[4];
} js_timer_t;

typedef struct {
    int         id;
    http_req_t *r;
    JSValue     cb;
    char        method[16];
    char        url[URL_CAP];
    char       *headers;       /* extra request headers, CRLF separated */
    char       *body;
    size_t      body_len;
    int         redirects;
} js_req_t;

struct js_page {
    JSRuntime  *rt;
    JSContext  *ctx;
    dom_doc_t  *doc;
    js_host_t   host;
    char        url[URL_CAP];
    JSValue     proto_target, proto_node, proto_element, proto_chardata, proto_text, proto_comment,
                proto_document, proto_fragment;
    JSValue     tag_proto[T_COUNT];
    JSValue    *wrappers;
    int         nwrap, capwrap;
    js_timer_t *timers;
    int         ntimers, captimers, next_timer;
    js_req_t   *reqs;
    int         nreqs, capreqs, next_req;
    dom_node_t **ran;          /* script elements already started */
    int         nran, capran;
    dom_node_t **queued;       /* dynamically inserted scripts waiting to run */
    int         nqueued, capqueued;
    dom_node_t **loads;        /* inserted <link>/<img> elements owed a "load" event */
    int         nloads, caploads;
    int         dirty;
    int         loading;       /* still running parser-inserted scripts */
    /* js_run_step state */
    dom_node_t **step_list;
    int         step_count, step_cap, step_i, step_phase;
    int         abort;         /* Stop pressed */
    double      deadline_ms;   /* watchdog for the script running now */
};

static JSClassID node_class;

/* a script (or timer, or event handler) that runs longer than this is stopped */
#define SCRIPT_BUDGET_MS 20000
static int interrupt_cb(JSRuntime *rt, void *opaque);

/* ---- helpers ------------------------------------------------------------ */

static js_page_t *page_of(JSContext *ctx) {
    return (js_page_t *)JS_GetContextOpaque(ctx);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void logf_(js_page_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(js_page_t *p, const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (p->host.log) p->host.log(p->host.ctx, line);
}

static void report_exception(js_page_t *p, const char *where) {
    JSContext *ctx = p->ctx;
    JSValue ex = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, ex);
    JSValue stack = JS_IsObject(ex) ? JS_GetPropertyStr(ctx, ex, "stack") : JS_UNDEFINED;
    const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : 0;
    logf_(p, "[js] error in %s: %s %s", where, msg ? msg : "?", st ? st : "");
    if (msg) JS_FreeCString(ctx, msg);
    if (st) JS_FreeCString(ctx, st);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, ex);
}

static int ptr_in(dom_node_t **list, int n, dom_node_t *x) {
    for (int i = 0; i < n; i++) if (list[i] == x) return 1;
    return 0;
}

static void ptr_push(dom_node_t ***list, int *n, int *cap, dom_node_t *x) {
    if (*n == *cap) {
        int nc = *cap ? *cap * 2 : 32;
        dom_node_t **nl = (dom_node_t **)realloc(*list, sizeof(dom_node_t *) * (size_t)nc);
        if (!nl) return;
        *list = nl;
        *cap = nc;
    }
    (*list)[(*n)++] = x;
}

/* ---- wrappers ----------------------------------------------------------- */

static JSValue proto_for(js_page_t *p, dom_node_t *n) {
    switch (n->type) {
    case N_ELEMENT:
        if (n->tag < T_COUNT && !JS_IsUndefined(p->tag_proto[n->tag])) return p->tag_proto[n->tag];
        return p->proto_element;
    case N_TEXT: return p->proto_text;
    case N_COMMENT: return p->proto_comment;
    case N_DOCUMENT: return p->proto_document;
    default: return p->proto_fragment;
    }
}

static JSValue wrap(js_page_t *p, dom_node_t *n) {
    JSValue obj;
    if (!n) return JS_NULL;
    if (n->js) return JS_DupValue(p->ctx, p->wrappers[(intptr_t)n->js - 1]);
    obj = JS_NewObjectProtoClass(p->ctx, proto_for(p, n), node_class);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, n);
    if (p->nwrap == p->capwrap) {
        int nc = p->capwrap ? p->capwrap * 2 : 256;
        JSValue *nw = (JSValue *)realloc(p->wrappers, sizeof(JSValue) * (size_t)nc);
        if (!nw) return obj;   /* unremembered wrapper: identity is lost, nothing worse */
        p->wrappers = nw;
        p->capwrap = nc;
    }
    /* wrappers live as long as the page so expando properties (listeners)
       stay attached to their node */
    p->wrappers[p->nwrap++] = JS_DupValue(p->ctx, obj);
    n->js = (void *)(intptr_t)p->nwrap;
    return obj;
}

static dom_node_t *unwrap(JSContext *ctx, JSValueConst v) {
    (void)ctx;
    return (dom_node_t *)JS_GetOpaque(v, node_class);
}

#define THIS_NODE(n)                                                     \
    dom_node_t *n = unwrap(ctx, this_val);                               \
    js_page_t *p = page_of(ctx);                                         \
    (void)p;                                                             \
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation")

static dom_node_t *arg_node(JSContext *ctx, JSValueConst v) {
    return unwrap(ctx, v);
}

/* ---- tree --------------------------------------------------------------- */

static int connected(js_page_t *p, dom_node_t *n) {
    while (n && n->parent) n = n->parent;
    return n == p->doc->root;
}

static int contains(dom_node_t *a, dom_node_t *b) {
    for (; b; b = b->parent) if (a == b) return 1;
    return 0;
}

static void unlink_node(dom_node_t *n) {
    dom_node_t *par = n->parent;
    if (!par) return;
    if (n->prev) n->prev->next = n->next;
    else par->first = n->next;
    if (n->next) n->next->prev = n->prev;
    else par->last = n->prev;
    n->parent = n->prev = n->next = 0;
}

static void link_before(dom_node_t *par, dom_node_t *n, dom_node_t *before) {
    n->parent = par;
    if (!before) {
        n->prev = par->last;
        n->next = 0;
        if (par->last) par->last->next = n;
        else par->first = n;
        par->last = n;
    } else {
        n->next = before;
        n->prev = before->prev;
        if (before->prev) before->prev->next = n;
        else par->first = n;
        before->prev = n;
    }
}

static void note_inserted(js_page_t *p, dom_node_t *n) {
    /* scripts run when they become connected; styles force a sheet reload */
    if (n->type != N_ELEMENT) return;
    if (n->tag == T_STYLE || (n->tag == T_LINK && dom_attr(n, "rel"))) p->dirty |= JS_DIRTY_STYLE;
    /* Surfer fetches stylesheets and images itself; scripts waiting on their
       load events (bundlers' preload helpers) get one on the next tick */
    if ((n->tag == T_LINK && dom_attr(n, "href")) || (n->tag == T_IMG && dom_attr(n, "src")))
        ptr_push(&p->loads, &p->nloads, &p->caploads, n);
    if (n->tag == T_SCRIPT && !ptr_in(p->ran, p->nran, n) && !ptr_in(p->queued, p->nqueued, n)) {
        ptr_push(&p->queued, &p->nqueued, &p->capqueued, n);
    }
    for (dom_node_t *k = n->first; k; k = k->next) note_inserted(p, k);
}

static void mark_scripts_ran(js_page_t *p, dom_node_t *n) {
    if (n->type == N_ELEMENT && n->tag == T_SCRIPT && !ptr_in(p->ran, p->nran, n)) ptr_push(&p->ran, &p->nran, &p->capran, n);
    for (dom_node_t *k = n->first; k; k = k->next) mark_scripts_ran(p, k);
}

static int insert_node(JSContext *ctx, js_page_t *p, dom_node_t *par, dom_node_t *n, dom_node_t *before) {
    if (!par || !n) { JS_ThrowTypeError(ctx, "parameter is not of type 'Node'"); return -1; }
    if (before && before->parent != par) { JS_ThrowReferenceError(ctx, "NotFoundError: reference node is not a child"); return -1; }
    if (contains(n, par)) { JS_ThrowTypeError(ctx, "HierarchyRequestError"); return -1; }
    if (par->type == N_TEXT || par->type == N_COMMENT) { JS_ThrowTypeError(ctx, "HierarchyRequestError"); return -1; }
    if (n == before) return 0;
    if (n->type == N_FRAGMENT) {
        dom_node_t *k = n->first;
        while (k) {
            dom_node_t *next = k->next;
            unlink_node(k);
            link_before(par, k, before);
            if (connected(p, par)) note_inserted(p, k);
            k = next;
        }
    } else {
        int was_style = n->type == N_ELEMENT && (n->tag == T_STYLE || n->tag == T_LINK);
        unlink_node(n);
        link_before(par, n, before);
        if (connected(p, par)) note_inserted(p, n);
        if (was_style) p->dirty |= JS_DIRTY_STYLE;
    }
    p->dirty |= JS_DIRTY_DOM;
    return 0;
}

static void remove_node(js_page_t *p, dom_node_t *n) {
    if (n->type == N_ELEMENT && (n->tag == T_STYLE || n->tag == T_LINK)) p->dirty |= JS_DIRTY_STYLE;
    unlink_node(n);
    p->dirty |= JS_DIRTY_DOM;
}

/* ---- attributes --------------------------------------------------------- */

static void cache_attr(dom_node_t *n, const char *name, const char *value) {
    if (strcmp(name, "id") == 0) n->id = value;
    else if (strcmp(name, "class") == 0) n->klass = value;
}

static void set_attr(js_page_t *p, dom_node_t *n, const char *name, const char *value, size_t vlen) {
    dom_attr_t *a, *last = 0;
    char lname[128];
    size_t nl = strlen(name);
    const char *v;
    if (nl >= sizeof lname) nl = sizeof lname - 1;
    for (size_t i = 0; i < nl; i++) lname[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];
    lname[nl] = 0;
    v = arena_strndup(&p->doc->arena, value, vlen);
    if (!v) return;
    for (a = n->attrs; a; last = a, a = a->next) {
        if (strcmp(a->name, lname) == 0) {
            a->value = v;
            cache_attr(n, lname, v);
            p->dirty |= JS_DIRTY_DOM;
            if (n->tag == T_LINK || n->tag == T_STYLE) p->dirty |= JS_DIRTY_STYLE;
            return;
        }
    }
    a = (dom_attr_t *)arena_alloc(&p->doc->arena, sizeof(dom_attr_t));
    if (!a) return;
    a->name = arena_strndup(&p->doc->arena, lname, nl);
    a->value = v;
    if (last) last->next = a;
    else n->attrs = a;
    cache_attr(n, lname, v);
    p->dirty |= JS_DIRTY_DOM;
    if (n->tag == T_LINK || n->tag == T_STYLE) p->dirty |= JS_DIRTY_STYLE;
}

static void remove_attr(js_page_t *p, dom_node_t *n, const char *name) {
    dom_attr_t *a, *prev = 0;
    for (a = n->attrs; a; prev = a, a = a->next) {
        if (strcmp(a->name, name) == 0) {
            if (prev) prev->next = a->next;
            else n->attrs = a->next;
            cache_attr(n, name, 0);
            p->dirty |= JS_DIRTY_DOM;
            return;
        }
    }
}

/* ---- text and serialization --------------------------------------------- */

typedef struct {
    char  *s;
    size_t len, cap;
} buf_t;

static void buf_add(buf_t *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        char *ns;
        while (cap < b->len + n + 1) cap *= 2;
        ns = (char *)realloc(b->s, cap);
        if (!ns) return;
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = 0;
}

static void buf_str(buf_t *b, const char *s) { buf_add(b, s, strlen(s)); }

static void text_of(const dom_node_t *n, buf_t *b) {
    if (n->type == N_TEXT) { buf_add(b, n->text, n->text_len); return; }
    if (n->type == N_COMMENT) return;
    for (const dom_node_t *k = n->first; k; k = k->next) text_of(k, b);
}

static void escape(buf_t *b, const char *s, size_t n, int attr) {
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        const char *rep = 0;
        if (s[i] == '&') rep = "&amp;";
        else if (s[i] == '<' && !attr) rep = "&lt;";
        else if (s[i] == '>' && !attr) rep = "&gt;";
        else if (s[i] == '"' && attr) rep = "&quot;";
        else if ((unsigned char)s[i] == 0xC2 && i + 1 < n && (unsigned char)s[i + 1] == 0xA0) {
            buf_add(b, s + start, i - start);
            buf_str(b, "&nbsp;");
            start = i + 2;
            i++;
            continue;
        }
        if (rep) {
            buf_add(b, s + start, i - start);
            buf_str(b, rep);
            start = i + 1;
        }
    }
    buf_add(b, s + start, n - start);
}

static int is_void_tag(int t) {
    return t == T_BR || t == T_IMG || t == T_INPUT || t == T_HR || t == T_META || t == T_LINK || t == T_COL ||
           t == T_AREA || t == T_BASE || t == T_EMBED || t == T_PARAM || t == T_SOURCE || t == T_TRACK || t == T_WBR;
}

static void serialize(const dom_node_t *n, buf_t *b, int self) {
    if (n->type == N_TEXT) {
        const dom_node_t *par = n->parent;
        if (par && par->type == N_ELEMENT && (par->tag == T_SCRIPT || par->tag == T_STYLE)) buf_add(b, n->text, n->text_len);
        else escape(b, n->text, n->text_len, 0);
        return;
    }
    if (n->type == N_COMMENT) {
        buf_str(b, "<!--");
        buf_add(b, n->text, n->text_len);
        buf_str(b, "-->");
        return;
    }
    if (self && n->type == N_ELEMENT) {
        buf_str(b, "<");
        buf_str(b, n->name);
        for (const dom_attr_t *a = n->attrs; a; a = a->next) {
            buf_str(b, " ");
            buf_str(b, a->name);
            buf_str(b, "=\"");
            escape(b, a->value ? a->value : "", a->value ? strlen(a->value) : 0, 1);
            buf_str(b, "\"");
        }
        buf_str(b, ">");
        if (is_void_tag(n->tag)) return;
    }
    for (const dom_node_t *k = n->first; k; k = k->next) serialize(k, b, 1);
    if (self && n->type == N_ELEMENT) {
        buf_str(b, "</");
        buf_str(b, n->name);
        buf_str(b, ">");
    }
}

static dom_node_t *clone_node(js_page_t *p, const dom_node_t *n, int deep) {
    dom_node_t *c;
    if (n->type == N_ELEMENT) {
        dom_attr_t *last = 0;
        c = dom_new_element(p->doc, n->name);
        if (!c) return 0;
        for (const dom_attr_t *a = n->attrs; a; a = a->next) {
            dom_attr_t *na = (dom_attr_t *)arena_alloc(&p->doc->arena, sizeof(dom_attr_t));
            if (!na) break;
            na->name = a->name;
            na->value = a->value;
            if (last) last->next = na;
            else c->attrs = na;
            last = na;
            cache_attr(c, na->name, na->value);
        }
    } else if (n->type == N_TEXT || n->type == N_COMMENT) {
        return dom_new_text(p->doc, n->type, n->text, n->text_len);
    } else {
        c = html_parse_fragment(p->doc, "", 0);
        if (!c) return 0;
    }
    if (deep) {
        for (const dom_node_t *k = n->first; k; k = k->next) {
            dom_node_t *ck = clone_node(p, k, 1);
            if (ck) link_before(c, ck, 0);
        }
    }
    return c;
}

/* ---- Node methods and properties ---------------------------------------- */

static JSValue node_get_type(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    switch (n->type) {
    case N_ELEMENT: return JS_NewInt32(ctx, 1);
    case N_TEXT: return JS_NewInt32(ctx, 3);
    case N_COMMENT: return JS_NewInt32(ctx, 8);
    case N_DOCUMENT: return JS_NewInt32(ctx, 9);
    default: return JS_NewInt32(ctx, 11);
    }
}

static JSValue upper_name(JSContext *ctx, const char *name) {
    char buf[128];
    size_t i = 0;
    for (; name && name[i] && i < sizeof buf - 1; i++) buf[i] = (name[i] >= 'a' && name[i] <= 'z') ? (char)(name[i] - 32) : name[i];
    buf[i] = 0;
    return JS_NewString(ctx, buf);
}

static JSValue node_get_name(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    switch (n->type) {
    case N_ELEMENT: return upper_name(ctx, n->name);
    case N_TEXT: return JS_NewString(ctx, "#text");
    case N_COMMENT: return JS_NewString(ctx, "#comment");
    case N_DOCUMENT: return JS_NewString(ctx, "#document");
    default: return JS_NewString(ctx, "#document-fragment");
    }
}

static JSValue node_get_local_name(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    return n->type == N_ELEMENT ? JS_NewString(ctx, n->name) : JS_NULL;
}

#define REL_GETTER(fname, expr)                                          \
    static JSValue fname(JSContext *ctx, JSValueConst this_val) {        \
        THIS_NODE(n);                                                    \
        return wrap(p, (expr));                                          \
    }
REL_GETTER(node_get_parent, n->parent)
REL_GETTER(node_get_first, n->first)
REL_GETTER(node_get_last, n->last)
REL_GETTER(node_get_next, n->next)
REL_GETTER(node_get_prev, n->prev)

static JSValue node_get_parent_el(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    return wrap(p, n->parent && n->parent->type == N_ELEMENT ? n->parent : 0);
}

static JSValue node_get_connected(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    return JS_NewBool(ctx, connected(p, n));
}

static JSValue node_get_text(JSContext *ctx, JSValueConst this_val) {
    buf_t b = { 0, 0, 0 };
    JSValue v;
    THIS_NODE(n);
    if (n->type == N_DOCUMENT) return JS_NULL;
    text_of(n, &b);
    v = JS_NewStringLen(ctx, b.s ? b.s : "", b.len);
    free(b.s);
    return v;
}

static JSValue node_set_text(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    size_t len;
    const char *s;
    THIS_NODE(n);
    s = JS_IsNull(val) || JS_IsUndefined(val) ? "" : JS_ToCStringLen(ctx, &len, val);
    if (!s) return JS_EXCEPTION;
    if (JS_IsNull(val) || JS_IsUndefined(val)) len = 0;
    if (n->type == N_TEXT || n->type == N_COMMENT) {
        n->text = arena_strndup(&p->doc->arena, s, len);
        n->text_len = len;
    } else if (n->type == N_ELEMENT || n->type == N_FRAGMENT) {
        while (n->first) unlink_node(n->first);
        if (len) {
            dom_node_t *t = dom_new_text(p->doc, N_TEXT, s, len);
            if (t) link_before(n, t, 0);
        }
        if (n->tag == T_STYLE) p->dirty |= JS_DIRTY_STYLE;
        if (n->tag == T_TITLE && n->parent == p->doc->head) snprintf(p->doc->title, sizeof p->doc->title, "%.*s", (int)len, s);
    }
    if (!JS_IsNull(val) && !JS_IsUndefined(val)) JS_FreeCString(ctx, s);
    p->dirty |= JS_DIRTY_DOM;
    return JS_UNDEFINED;
}

static JSValue node_get_data(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    if (n->type != N_TEXT && n->type != N_COMMENT) return JS_NULL;
    return JS_NewStringLen(ctx, n->text ? n->text : "", n->text_len);
}

static JSValue node_append_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    dom_node_t *c;
    THIS_NODE(n);
    (void)argc;
    c = arg_node(ctx, argv[0]);
    if (insert_node(ctx, p, n, c, 0) != 0) return JS_EXCEPTION;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue node_insert_before(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    dom_node_t *c, *ref;
    THIS_NODE(n);
    c = arg_node(ctx, argv[0]);
    ref = argc > 1 ? arg_node(ctx, argv[1]) : 0;
    if (insert_node(ctx, p, n, c, ref) != 0) return JS_EXCEPTION;
    return JS_DupValue(ctx, argv[0]);
}

static JSValue node_remove_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    dom_node_t *c;
    THIS_NODE(n);
    (void)argc;
    c = arg_node(ctx, argv[0]);
    if (!c || c->parent != n) return JS_ThrowReferenceError(ctx, "NotFoundError: node is not a child");
    remove_node(p, c);
    return JS_DupValue(ctx, argv[0]);
}

static JSValue node_replace_child(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    dom_node_t *nw, *old, *next;
    THIS_NODE(n);
    (void)argc;
    nw = arg_node(ctx, argv[0]);
    old = arg_node(ctx, argv[1]);
    if (!old || old->parent != n) return JS_ThrowReferenceError(ctx, "NotFoundError: node is not a child");
    next = old->next == nw ? nw->next : old->next;
    if (nw == old) return JS_DupValue(ctx, argv[1]);
    remove_node(p, old);
    if (insert_node(ctx, p, n, nw, next) != 0) return JS_EXCEPTION;
    return JS_DupValue(ctx, argv[1]);
}

static JSValue node_remove(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    THIS_NODE(n);
    (void)argc; (void)argv;
    if (n->parent) remove_node(p, n);
    return JS_UNDEFINED;
}

static JSValue node_clone(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    dom_node_t *c;
    THIS_NODE(n);
    c = clone_node(p, n, argc > 0 && JS_ToBool(ctx, argv[0]));
    return c ? wrap(p, c) : JS_NULL;
}

static JSValue node_contains(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    dom_node_t *o;
    THIS_NODE(n);
    (void)argc;
    o = arg_node(ctx, argv[0]);
    return JS_NewBool(ctx, o && contains(n, o));
}

static JSValue node_has_children(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    THIS_NODE(n);
    (void)argc; (void)argv;
    return JS_NewBool(ctx, n->first != 0);
}

/* ---- Element ------------------------------------------------------------ */

static JSValue el_get_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *name, *v;
    char lname[128];
    size_t i;
    THIS_NODE(n);
    (void)argc;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    for (i = 0; name[i] && i < sizeof lname - 1; i++) lname[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];
    lname[i] = 0;
    JS_FreeCString(ctx, name);
    v = dom_attr(n, lname);
    return v ? JS_NewString(ctx, v) : JS_NULL;
}

static JSValue el_set_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *name, *value;
    size_t vlen;
    THIS_NODE(n);
    (void)argc;
    if (n->type != N_ELEMENT) return JS_UNDEFINED;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    value = JS_ToCStringLen(ctx, &vlen, argv[1]);
    if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    set_attr(p, n, name, value, vlen);
    JS_FreeCString(ctx, name);
    JS_FreeCString(ctx, value);
    return JS_UNDEFINED;
}

static JSValue el_remove_attr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *name;
    char lname[128];
    size_t i;
    THIS_NODE(n);
    (void)argc;
    name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    for (i = 0; name[i] && i < sizeof lname - 1; i++) lname[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];
    lname[i] = 0;
    JS_FreeCString(ctx, name);
    remove_attr(p, n, lname);
    return JS_UNDEFINED;
}

static JSValue el_attr_names(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue arr;
    uint32_t i = 0;
    THIS_NODE(n);
    (void)argc; (void)argv;
    arr = JS_NewArray(ctx);
    for (const dom_attr_t *a = n->attrs; a; a = a->next) JS_SetPropertyUint32(ctx, arr, i++, JS_NewString(ctx, a->name));
    return arr;
}

static JSValue el_get_inner_html(JSContext *ctx, JSValueConst this_val) {
    buf_t b = { 0, 0, 0 };
    JSValue v;
    THIS_NODE(n);
    serialize(n, &b, 0);
    v = JS_NewStringLen(ctx, b.s ? b.s : "", b.len);
    free(b.s);
    return v;
}

static JSValue el_get_outer_html(JSContext *ctx, JSValueConst this_val) {
    buf_t b = { 0, 0, 0 };
    JSValue v;
    THIS_NODE(n);
    serialize(n, &b, 1);
    v = JS_NewStringLen(ctx, b.s ? b.s : "", b.len);
    free(b.s);
    return v;
}

static JSValue el_set_inner_html(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    size_t len;
    const char *s;
    dom_node_t *frag;
    THIS_NODE(n);
    s = JS_ToCStringLen(ctx, &len, val);
    if (!s) return JS_EXCEPTION;
    while (n->first) unlink_node(n->first);
    frag = html_parse_fragment(p->doc, s, len);
    JS_FreeCString(ctx, s);
    if (frag) {
        mark_scripts_ran(p, frag);   /* innerHTML never runs scripts */
        if (insert_node(ctx, p, n, frag, 0) != 0) return JS_EXCEPTION;
    }
    p->dirty |= JS_DIRTY_DOM;
    return JS_UNDEFINED;
}

static JSValue el_insert_adjacent_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *where, *s;
    size_t len;
    dom_node_t *frag;
    int rc = 0;
    THIS_NODE(n);
    (void)argc;
    where = JS_ToCString(ctx, argv[0]);
    if (!where) return JS_EXCEPTION;
    s = JS_ToCStringLen(ctx, &len, argv[1]);
    if (!s) { JS_FreeCString(ctx, where); return JS_EXCEPTION; }
    frag = html_parse_fragment(p->doc, s, len);
    if (frag) {
        mark_scripts_ran(p, frag);
        if (!strcmp(where, "beforebegin") && n->parent) rc = insert_node(ctx, p, n->parent, frag, n);
        else if (!strcmp(where, "afterbegin")) rc = insert_node(ctx, p, n, frag, n->first);
        else if (!strcmp(where, "beforeend")) rc = insert_node(ctx, p, n, frag, 0);
        else if (!strcmp(where, "afterend") && n->parent) rc = insert_node(ctx, p, n->parent, frag, n->next);
    }
    JS_FreeCString(ctx, where);
    JS_FreeCString(ctx, s);
    return rc ? JS_EXCEPTION : JS_UNDEFINED;
}

static void select_walk(js_page_t *p, dom_node_t *root, const css_selector_list_t *sel, JSValue arr, uint32_t *count, int first) {
    for (dom_node_t *k = root->first; k; k = k->next) {
        if (first && *count) return;
        if (k->type != N_ELEMENT) continue;
        if (css_selector_matches(sel, k)) JS_SetPropertyUint32(p->ctx, arr, (*count)++, wrap(p, k));
        select_walk(p, k, sel, arr, count, first);
    }
}

static JSValue query(JSContext *ctx, JSValueConst this_val, JSValueConst selv, int first) {
    const char *text;
    css_selector_list_t *sel;
    JSValue arr;
    uint32_t count = 0;
    THIS_NODE(n);
    text = JS_ToCString(ctx, selv);
    if (!text) return JS_EXCEPTION;
    sel = css_selector_parse(text);
    if (!sel) {
        JSValue e = JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", text);
        JS_FreeCString(ctx, text);
        return e;
    }
    JS_FreeCString(ctx, text);
    arr = JS_NewArray(ctx);
    select_walk(p, n, sel, arr, &count, first);
    css_selector_free(sel);
    if (first) {
        JSValue v = count ? JS_GetPropertyUint32(ctx, arr, 0) : JS_NULL;
        JS_FreeValue(ctx, arr);
        return v;
    }
    return arr;
}

static JSValue el_query(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc;
    return query(ctx, this_val, argv[0], 1);
}

static JSValue el_query_all(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)argc;
    return query(ctx, this_val, argv[0], 0);
}

static JSValue el_matches(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *text;
    css_selector_list_t *sel;
    int r;
    THIS_NODE(n);
    (void)argc;
    text = JS_ToCString(ctx, argv[0]);
    if (!text) return JS_EXCEPTION;
    sel = css_selector_parse(text);
    if (!sel) {
        JSValue e = JS_ThrowSyntaxError(ctx, "'%s' is not a valid selector", text);
        JS_FreeCString(ctx, text);
        return e;
    }
    JS_FreeCString(ctx, text);
    r = css_selector_matches(sel, n);
    css_selector_free(sel);
    return JS_NewBool(ctx, r);
}

/* Form controls: live value/checked/selection state from form.c. */
static JSValue el_get_value(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    if (n->tag == T_OPTION) {
        const char *v = dom_attr(n, "value");
        char label[512];
        if (v) return JS_NewString(ctx, v);
        form_option_label(n, label, sizeof label);
        return JS_NewString(ctx, label);
    }
    if (form_kind(n) == FK_NONE) {
        const char *v = dom_attr(n, "value");
        return v ? JS_NewString(ctx, v) : JS_NewString(ctx, "");
    }
    return JS_NewString(ctx, form_value(n));
}

static JSValue el_set_value(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    const char *s;
    THIS_NODE(n);
    s = JS_IsNull(val) ? "" : JS_ToCString(ctx, val);
    if (!s) return JS_EXCEPTION;
    if (form_kind(n) != FK_NONE) form_set_value(n, s);
    else set_attr(p, n, "value", s, strlen(s));
    if (!JS_IsNull(val)) JS_FreeCString(ctx, s);
    p->dirty |= JS_DIRTY_DOM;
    return JS_UNDEFINED;
}

static JSValue el_get_checked(JSContext *ctx, JSValueConst this_val) {
    form_ctl_t *c;
    THIS_NODE(n);
    c = form_ctl(n);
    return JS_NewBool(ctx, c && c->checked);
}

static JSValue el_set_checked(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    form_ctl_t *c;
    int on;
    THIS_NODE(n);
    on = JS_ToBool(ctx, val);
    c = form_ctl(n);
    if (!c) return JS_UNDEFINED;
    if (on && form_kind(n) == FK_RADIO && !c->checked) form_toggle(p->doc->root, n);
    else c->checked = (uint8_t)on;
    p->dirty |= JS_DIRTY_DOM;
    return JS_UNDEFINED;
}

static JSValue el_get_selected_index(JSContext *ctx, JSValueConst this_val) {
    THIS_NODE(n);
    if (form_kind(n) != FK_SELECT) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, form_ctl(n)->selected);
}

static JSValue el_set_selected_index(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    int32_t i = -1;
    THIS_NODE(n);
    JS_ToInt32(ctx, &i, val);
    if (form_kind(n) == FK_SELECT) {
        if (i < 0) form_ctl(n)->selected = -1;
        else form_choose(n, i);
        p->dirty |= JS_DIRTY_DOM;
    }
    return JS_UNDEFINED;
}

static JSValue el_options(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue arr;
    int count;
    THIS_NODE(n);
    (void)argc; (void)argv;
    arr = JS_NewArray(ctx);
    count = form_option_count(n);
    for (int i = 0; i < count; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrap(p, form_option(n, i)));
    return arr;
}

static JSValue el_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    float x = 0, y = 0, w = 0, h = 0, sy = 0;
    JSValue o;
    THIS_NODE(n);
    (void)argc; (void)argv;
    if (p->host.box) p->host.box(p->host.ctx, n, &x, &y, &w, &h);
    if (p->host.scroll_y) sy = p->host.scroll_y(p->host.ctx);
    y -= sy;
    o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, o, "left", JS_NewFloat64(ctx, x));
    JS_SetPropertyStr(ctx, o, "top", JS_NewFloat64(ctx, y));
    JS_SetPropertyStr(ctx, o, "width", JS_NewFloat64(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewFloat64(ctx, h));
    JS_SetPropertyStr(ctx, o, "right", JS_NewFloat64(ctx, x + w));
    JS_SetPropertyStr(ctx, o, "bottom", JS_NewFloat64(ctx, y + h));
    return o;
}

static JSValue el_focus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    THIS_NODE(n);
    (void)argc; (void)argv;
    if (p->host.focus) p->host.focus(p->host.ctx, n);
    return JS_UNDEFINED;
}

static JSValue el_blur(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    THIS_NODE(n);
    (void)argc; (void)argv;
    if (p->host.focus && p->host.active && p->host.active(p->host.ctx) == n) p->host.focus(p->host.ctx, 0);
    return JS_UNDEFINED;
}

/* A few computed-style fields scripts commonly read. */
static void color_str(char *out, size_t cap, uint32_t c) {
    if ((c >> 24) == 0xFF) snprintf(out, cap, "rgb(%u, %u, %u)", (c >> 16) & 255, (c >> 8) & 255, c & 255);
    else snprintf(out, cap, "rgba(%u, %u, %u, %.3g)", (c >> 16) & 255, (c >> 8) & 255, c & 255, (double)(c >> 24) / 255.0);
}

static JSValue el_computed(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    static const char *const displays[] = { "inline", "block", "inline-block", "list-item", "none", "table",
        "inline-table", "table-row", "table-cell", "table-row-group", "table-header-group", "table-footer-group",
        "table-caption", "table-column", "table-column-group", "flex", "inline-flex", "grid", "inline-grid", "contents" };
    static const char *const positions[] = { "static", "relative", "absolute", "fixed", "sticky" };
    const css_style_t *s;
    JSValue o;
    char buf[64];
    THIS_NODE(n);
    (void)argc; (void)argv;
    o = JS_NewObject(ctx);
    s = n->style;
    if (!s) {
        JS_SetPropertyStr(ctx, o, "display", JS_NewString(ctx, "none"));
        return o;
    }
    JS_SetPropertyStr(ctx, o, "display", JS_NewString(ctx, s->display < 20 ? displays[s->display] : "block"));
    JS_SetPropertyStr(ctx, o, "position", JS_NewString(ctx, s->position < 5 ? positions[s->position] : "static"));
    JS_SetPropertyStr(ctx, o, "visibility", JS_NewString(ctx, s->visibility ? "hidden" : "visible"));
    color_str(buf, sizeof buf, s->color);
    JS_SetPropertyStr(ctx, o, "color", JS_NewString(ctx, buf));
    color_str(buf, sizeof buf, s->bg_color);
    JS_SetPropertyStr(ctx, o, "backgroundColor", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%gpx", (double)s->font_size);
    JS_SetPropertyStr(ctx, o, "fontSize", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%u", s->font_weight);
    JS_SetPropertyStr(ctx, o, "fontWeight", JS_NewString(ctx, buf));
    snprintf(buf, sizeof buf, "%g", (double)s->opacity);
    JS_SetPropertyStr(ctx, o, "opacity", JS_NewString(ctx, buf));
    JS_SetPropertyStr(ctx, o, "fontFamily", JS_NewString(ctx, s->font_family ? s->font_family : "sans-serif"));
    return o;
}

/* ---- Document ----------------------------------------------------------- */

static JSValue doc_create_element(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *name;
    dom_node_t *n;
    js_page_t *p = page_of(ctx);
    (void)this_val;
    name = JS_ToCString(ctx, argv[argc > 1 ? 1 : 0]);   /* createElementNS(ns, name) passes 2 */
    if (!name) return JS_EXCEPTION;
    {
        /* createElementNS("…svg", "svg:rect") style qualified names */
        const char *colon = strchr(name, ':');
        n = dom_new_element(p->doc, colon ? colon + 1 : name);
    }
    JS_FreeCString(ctx, name);
    return n ? wrap(p, n) : JS_ThrowInternalError(ctx, "out of memory");
}

static JSValue doc_create_text(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic) {
    size_t len;
    const char *s;
    dom_node_t *n;
    js_page_t *p = page_of(ctx);
    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    n = dom_new_text(p->doc, magic ? N_COMMENT : N_TEXT, s, len);
    JS_FreeCString(ctx, s);
    return n ? wrap(p, n) : JS_ThrowInternalError(ctx, "out of memory");
}

static JSValue doc_create_fragment(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    (void)this_val; (void)argc; (void)argv;
    return wrap(p, html_parse_fragment(p->doc, "", 0));
}

static dom_node_t *find_id(dom_node_t *n, const char *id) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        dom_node_t *r;
        if (k->type != N_ELEMENT) continue;
        if (k->id && strcmp(k->id, id) == 0) return k;
        r = find_id(k, id);
        if (r) return r;
    }
    return 0;
}

static JSValue doc_get_by_id(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *id;
    dom_node_t *n;
    THIS_NODE(root);
    (void)argc;
    id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    n = find_id(root, id);
    JS_FreeCString(ctx, id);
    return wrap(p, n);
}

static JSValue doc_get_html(JSContext *ctx, JSValueConst this_val) {
    js_page_t *p = page_of(ctx);
    (void)this_val;
    return wrap(p, p->doc->html);
}

static JSValue doc_get_head(JSContext *ctx, JSValueConst this_val) {
    js_page_t *p = page_of(ctx);
    (void)this_val;
    return wrap(p, p->doc->head);
}

static JSValue doc_get_body(JSContext *ctx, JSValueConst this_val) {
    js_page_t *p = page_of(ctx);
    (void)this_val;
    return wrap(p, p->doc->body);
}

static JSValue doc_get_title(JSContext *ctx, JSValueConst this_val) {
    js_page_t *p = page_of(ctx);
    (void)this_val;
    return JS_NewString(ctx, p->doc->title);
}

static JSValue doc_set_title(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    js_page_t *p = page_of(ctx);
    const char *s = JS_ToCString(ctx, val);
    (void)this_val;
    if (!s) return JS_EXCEPTION;
    snprintf(p->doc->title, sizeof p->doc->title, "%s", s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue doc_get_cookie(JSContext *ctx, JSValueConst this_val) {
    js_page_t *p = page_of(ctx);
    char buf[8192];
    (void)this_val;
    http_cookie_get(p->url, buf, sizeof buf);
    return JS_NewString(ctx, buf);
}

static JSValue doc_set_cookie(JSContext *ctx, JSValueConst this_val, JSValueConst val) {
    js_page_t *p = page_of(ctx);
    const char *s = JS_ToCString(ctx, val);
    (void)this_val;
    if (!s) return JS_EXCEPTION;
    http_cookie_set(p->url, s);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue doc_active(JSContext *ctx, JSValueConst this_val) {
    js_page_t *p = page_of(ctx);
    dom_node_t *n = p->host.active ? p->host.active(p->host.ctx) : 0;
    (void)this_val;
    return wrap(p, n ? n : p->doc->body);
}

/* ---- timers ------------------------------------------------------------- */

static JSValue g_set_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    js_timer_t *t;
    double delay = 0;
    (void)this_val;
    if (!JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    if (argc > 1) JS_ToFloat64(ctx, &delay, argv[1]);
    if (!(delay >= 0)) delay = 0;
    if (p->ntimers == p->captimers) {
        int nc = p->captimers ? p->captimers * 2 : 32;
        js_timer_t *nt = (js_timer_t *)realloc(p->timers, sizeof(js_timer_t) * (size_t)nc);
        if (!nt) return JS_ThrowInternalError(ctx, "out of memory");
        p->timers = nt;
        p->captimers = nc;
    }
    t = &p->timers[p->ntimers++];
    memset(t, 0, sizeof(*t));
    t->id = ++p->next_timer;
    t->fn = JS_DupValue(ctx, argv[0]);
    t->interval = argc > 2 && JS_ToBool(ctx, argv[2]) ? (delay < 4 ? 4 : delay) : -1;
    t->raf = argc > 3 && JS_ToBool(ctx, argv[3]);
    t->due = now_ms() + (t->raf ? 16 : delay);
    for (int i = 4; i < argc && t->argc < 4; i++) t->argv[t->argc++] = JS_DupValue(ctx, argv[i]);
#ifdef TLS_HOST
    if (getenv("JSTRACE")) logf_(p, "[trace] timer %d set delay=%g repeat=%d raf=%d", t->id, delay, t->interval >= 0, t->raf);
#endif
    return JS_NewInt32(ctx, t->id);
}

static void timer_free(js_page_t *p, js_timer_t *t) {
    JS_FreeValue(p->ctx, t->fn);
    for (int i = 0; i < t->argc; i++) JS_FreeValue(p->ctx, t->argv[i]);
    t->id = 0;
}

static JSValue g_clear_timer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    int32_t id = 0;
    (void)this_val; (void)argc;
    JS_ToInt32(ctx, &id, argv[0]);
#ifdef TLS_HOST
    if (getenv("JSTRACE")) logf_(p, "[trace] clear %d (ntimers=%d)", id, p->ntimers);
#endif
    for (int i = 0; i < p->ntimers; i++) {
        if (p->timers[i].id == id && id) {
            timer_free(p, &p->timers[i]);
            p->timers[i] = p->timers[--p->ntimers];
            break;
        }
    }
    return JS_UNDEFINED;
}

/* ---- network ------------------------------------------------------------ */

static int req_start(js_req_t *q) {
    int body = strcmp(q->method, "GET") != 0 && strcmp(q->method, "HEAD") != 0;
    q->r = body ? http_open_body(q->url, q->method, q->headers, q->body ? q->body : "", q->body_len)
                : http_open(q->url, q->method, q->headers);
    return q->r ? 0 : -1;
}

/* __fetch(method, url, headers, body, callback(status, headers, ArrayBuffer, finalUrl, error)) */
static JSValue g_fetch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    js_req_t *q;
    const char *method, *url, *hdr;
    char abs[URL_CAP];
    (void)this_val;
    if (argc < 5) return JS_ThrowTypeError(ctx, "__fetch: bad arguments");
    method = JS_ToCString(ctx, argv[0]);
    url = JS_ToCString(ctx, argv[1]);
    hdr = JS_IsString(argv[2]) ? JS_ToCString(ctx, argv[2]) : 0;
    if (!method || !url) {
        if (method) JS_FreeCString(ctx, method);
        if (url) JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }
    if (url_resolve(p->doc->base_url, url, abs, sizeof abs) != 0) snprintf(abs, sizeof abs, "%s", url);
    if (p->nreqs == p->capreqs) {
        int nc = p->capreqs ? p->capreqs * 2 : 8;
        js_req_t *nr = (js_req_t *)realloc(p->reqs, sizeof(js_req_t) * (size_t)nc);
        if (!nr) return JS_ThrowInternalError(ctx, "out of memory");
        p->reqs = nr;
        p->capreqs = nc;
    }
    q = &p->reqs[p->nreqs++];
    memset(q, 0, sizeof(*q));
    q->id = ++p->next_req;
    snprintf(q->method, sizeof q->method, "%s", method);
    snprintf(q->url, sizeof q->url, "%s", abs);
    q->headers = hdr ? strdup(hdr) : 0;
    q->cb = JS_DupValue(ctx, argv[4]);
    if (JS_IsString(argv[3])) {
        size_t bl;
        const char *b = JS_ToCStringLen(ctx, &bl, argv[3]);
        if (b) {
            q->body = (char *)malloc(bl + 1);
            if (q->body) { memcpy(q->body, b, bl); q->body_len = bl; }
            JS_FreeCString(ctx, b);
        }
    } else if (JS_IsObject(argv[3])) {
        size_t bl;
        uint8_t *b = JS_GetArrayBuffer(ctx, &bl, argv[3]);
        if (b) {
            q->body = (char *)malloc(bl + 1);
            if (q->body) { memcpy(q->body, b, bl); q->body_len = bl; }
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    if (hdr) JS_FreeCString(ctx, hdr);
    logf_(p, "[js] fetch %s %s", q->method, q->url);
    req_start(q);
    return JS_NewInt32(ctx, q->id);
}

static void req_finish(js_page_t *p, int i) {
    js_req_t *q = &p->reqs[i];
    JSContext *ctx = p->ctx;
    JSValue args[5], ret;
    http_req_t *r = q->r;
    int ok = r && r->state == HTTP_DONE;
    logf_(p, "[js] fetch done %s -> %d %s", q->url, ok ? r->status : 0, ok ? "" : (r && r->error[0] ? r->error : "error"));
    args[0] = JS_NewInt32(ctx, ok ? r->status : 0);
    args[1] = JS_NewString(ctx, ok && r->raw_headers ? r->raw_headers : "");
    args[2] = ok ? JS_NewArrayBufferCopy(ctx, r->body ? r->body : (const uint8_t *)"", r->body_len) : JS_NULL;
    args[3] = JS_NewString(ctx, q->url);
    args[4] = ok ? JS_NULL : JS_NewString(ctx, r && r->error[0] ? r->error : "network error");
    ret = JS_Call(ctx, q->cb, JS_UNDEFINED, 5, args);
    if (JS_IsException(ret)) report_exception(p, "fetch callback");
    JS_FreeValue(ctx, ret);
    for (int k = 0; k < 5; k++) JS_FreeValue(ctx, args[k]);
    JS_FreeValue(ctx, q->cb);
    http_free(q->r);
    free(q->headers);
    free(q->body);
    p->reqs[i] = p->reqs[--p->nreqs];
}

static int pump_requests(js_page_t *p) {
    int progressed = 0;
    for (int i = 0; i < p->nreqs; i++) {
        js_req_t *q = &p->reqs[i];
        if (q->r && q->r->state == HTTP_PENDING) {
            http_poll(q->r, 0);
            if (q->r->state == HTTP_PENDING) continue;
        }
        if (q->r && q->r->state == HTTP_DONE && q->r->status >= 300 && q->r->status < 400 && q->r->location[0] &&
            q->redirects < 8) {
            char next[URL_CAP];
            if (url_resolve(q->url, q->r->location, next, sizeof next) == 0) {
                int keep = q->r->status == 307 || q->r->status == 308;
                http_free(q->r);
                q->r = 0;
                snprintf(q->url, sizeof q->url, "%s", next);
                if (!keep) snprintf(q->method, sizeof q->method, "GET");
                q->redirects++;
                req_start(q);
                progressed = 1;
                continue;
            }
        }
        req_finish(p, i);
        i--;
        progressed = 1;
    }
    return progressed;
}

/* ---- misc globals ------------------------------------------------------- */

static JSValue g_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    const char *s;
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    s = JS_ToCString(ctx, argv[0]);
    if (s) {
        logf_(p, "[console] %s", s);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue g_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, now_ms());
}

static JSValue g_navigate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    const char *s = JS_ToCString(ctx, argv[0]);
    char abs[URL_CAP];
    (void)this_val;
    if (!s) return JS_EXCEPTION;
    if (url_resolve(p->doc->base_url, s, abs, sizeof abs) != 0) snprintf(abs, sizeof abs, "%s", s);
    JS_FreeCString(ctx, s);
    if (p->host.navigate) p->host.navigate(p->host.ctx, abs, argc > 1 && JS_ToBool(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue g_set_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    const char *s = JS_ToCString(ctx, argv[0]);
    char abs[URL_CAP];
    (void)this_val; (void)argc;
    if (!s) return JS_EXCEPTION;
    if (url_resolve(p->url, s, abs, sizeof abs) != 0) snprintf(abs, sizeof abs, "%s", s);
    JS_FreeCString(ctx, s);
    snprintf(p->url, sizeof p->url, "%s", abs);
    if (p->host.set_url) p->host.set_url(p->host.ctx, abs);
    return JS_NewString(ctx, abs);
}

static JSValue g_get_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    (void)this_val; (void)argc; (void)argv;
    return JS_NewString(ctx, p->url);
}

static JSValue g_history_go(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    int32_t d = 0;
    (void)this_val; (void)argc;
    JS_ToInt32(ctx, &d, argv[0]);
    if (p->host.history_go) p->host.history_go(p->host.ctx, d);
    return JS_UNDEFINED;
}

static JSValue g_viewport(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    int w = 1024, h = 768;
    JSValue arr = JS_NewArray(ctx);
    (void)this_val; (void)argc; (void)argv;
    if (p->host.viewport) p->host.viewport(p->host.ctx, &w, &h);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, w));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, h));
    JS_SetPropertyUint32(ctx, arr, 2, JS_NewFloat64(ctx, p->host.scroll_y ? p->host.scroll_y(p->host.ctx) : 0));
    return arr;
}

static JSValue g_scroll_to(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    double x = 0, y = 0;
    (void)this_val;
    if (argc > 0) JS_ToFloat64(ctx, &x, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &y, argv[1]);
    if (p->host.scroll_to) p->host.scroll_to(p->host.ctx, (float)x, (float)y);
    return JS_UNDEFINED;
}

static JSValue g_status(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    const char *s = argc ? JS_ToCString(ctx, argv[0]) : 0;
    (void)this_val;
    if (s) {
        if (p->host.status) p->host.status(p->host.ctx, s);
        logf_(p, "[alert] %s", s);
        JS_FreeCString(ctx, s);
    }
    return JS_UNDEFINED;
}

static JSValue g_random(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t n = 0;
    uint8_t buf[1024];
    static uint64_t s = 0x9E3779B97F4A7C15ULL;
    (void)this_val; (void)argc;
    JS_ToInt32(ctx, &n, argv[0]);
    if (n < 0) n = 0;
    if (n > (int32_t)sizeof buf) n = (int32_t)sizeof buf;
    for (int32_t i = 0; i < n; i++) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        s ^= ((uint64_t)hi << 32 | lo) + (uint64_t)i;
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        buf[i] = (uint8_t)(s >> 24);
    }
    return JS_NewArrayBufferCopy(ctx, buf, (size_t)n);
}

static JSValue g_utf8_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len;
    uint8_t *b;
    (void)this_val; (void)argc;
    b = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!b) return JS_EXCEPTION;
    return JS_NewStringLen(ctx, (const char *)b, len);
}

static JSValue g_utf8_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len;
    const char *s;
    JSValue r;
    (void)this_val; (void)argc;
    s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    r = JS_NewArrayBufferCopy(ctx, (const uint8_t *)s, len);
    JS_FreeCString(ctx, s);
    return r;
}

/* localStorage: one JSON file per origin. */
static void storage_path(const char *origin, char *out, size_t cap) {
    size_t o = (size_t)snprintf(out, cap, "/home/.surfer-storage-");
    for (const char *s = origin; *s && o + 1 < cap; s++) {
        char c = *s;
        out[o++] = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ? c : '_';
    }
    out[o] = 0;
}

static JSValue g_storage_load(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *origin = JS_ToCString(ctx, argv[0]);
    char path[256];
    static char buf[256 * 1024];
    long n;
    (void)this_val; (void)argc;
    if (!origin) return JS_EXCEPTION;
    storage_path(origin, path, sizeof path);
    JS_FreeCString(ctx, origin);
#ifdef TLS_HOST
    n = -1;
    (void)buf;
#else
    n = (long)icda_read_file(path, buf, sizeof buf - 1);
#endif
    if (n <= 0) return JS_NULL;
    return JS_NewStringLen(ctx, buf, (size_t)n);
}

static JSValue g_storage_save(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *origin = JS_ToCString(ctx, argv[0]);
    size_t len;
    const char *data;
    char path[256];
    (void)this_val; (void)argc;
    if (!origin) return JS_EXCEPTION;
    data = JS_ToCStringLen(ctx, &len, argv[1]);
    storage_path(origin, path, sizeof path);
    JS_FreeCString(ctx, origin);
    if (!data) return JS_EXCEPTION;
#ifndef TLS_HOST
    icda_write_file(path, data, len);
#endif
    JS_FreeCString(ctx, data);
    return JS_UNDEFINED;
}

static JSValue g_resolve(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *base, *rel;
    char out[URL_CAP];
    int ok;
    (void)this_val; (void)argc;
    base = JS_ToCString(ctx, argv[0]);
    rel = JS_ToCString(ctx, argv[1]);
    ok = base && rel && url_resolve(base, rel, out, sizeof out) == 0;
    if (base) JS_FreeCString(ctx, base);
    if (rel) JS_FreeCString(ctx, rel);
    return ok ? JS_NewString(ctx, out) : JS_NULL;
}

static JSValue g_submit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_page_t *p = page_of(ctx);
    dom_node_t *form = unwrap(ctx, argv[0]);
    dom_node_t *submitter = argc > 1 ? unwrap(ctx, argv[1]) : 0;
    (void)this_val;
    if (form && p->host.submit) p->host.submit(p->host.ctx, form, submitter);
    return JS_UNDEFINED;
}

/* ---- scripts and modules ------------------------------------------------ */

static char *script_source(js_page_t *p, dom_node_t *el, size_t *len, char *url, size_t url_cap) {
    const char *src = dom_attr(el, "src");
    if (src && *src) {
        char abs[URL_CAP];
        char *body = 0;
        if (url_resolve(p->doc->base_url, src, abs, sizeof abs) != 0) return 0;
        if (!p->host.fetch || p->host.fetch(p->host.ctx, abs, &body, len, url, url_cap) != 0) {
            logf_(p, "[js] could not load script %s", abs);
            return 0;
        }
        return body;
    } else {
        buf_t b = { 0, 0, 0 };
        text_of(el, &b);
        snprintf(url, url_cap, "%s", p->url);
        *len = b.len;
        if (!b.s) b.s = (char *)calloc(1, 1);
        return b.s;
    }
}

static int script_kind(dom_node_t *el) {
    /* 0 skip, 1 classic, 2 module */
    const char *type = dom_attr(el, "type");
    if (dom_attr(el, "nomodule")) return 0;
    if (!type || !*type || !strcmp(type, "text/javascript") || !strcmp(type, "application/javascript") ||
        !strcmp(type, "text/ecmascript") || !strcmp(type, "application/x-javascript")) return 1;
    if (!strcmp(type, "module")) return 2;
    return 0;
}

static void run_jobs(js_page_t *p) {
    JSContext *c1;
    for (int i = 0; i < MAX_JOBS_PER_TICK; i++) {
        int r = JS_ExecutePendingJob(p->rt, &c1);
        if (r == 0) break;
        if (r < 0) report_exception(p, "promise job");
    }
}

static void fire_simple(js_page_t *p, dom_node_t *n, const char *type) {
    js_event_t ev;
    memset(&ev, 0, sizeof ev);
    js_dispatch(p, n, type, &ev);
}

static void set_import_meta(js_page_t *p, JSValue func, const char *url) {
    JSModuleDef *m = (JSModuleDef *)JS_VALUE_GET_PTR(func);
    JSValue meta = JS_GetImportMeta(p->ctx, m);
    if (!JS_IsException(meta)) {
        JS_SetPropertyStr(p->ctx, meta, "url", JS_NewString(p->ctx, url));
        JS_FreeValue(p->ctx, meta);
    }
}

static void run_script(js_page_t *p, dom_node_t *el) {
    JSContext *ctx = p->ctx;
    char url[URL_CAP];
    size_t len = 0;
    if (p->abort) return;
    p->deadline_ms = now_ms() + SCRIPT_BUDGET_MS;
    int kind = script_kind(el);
    char *src;
    JSValue r, g;
    if (!kind || ptr_in(p->ran, p->nran, el)) return;
    ptr_push(&p->ran, &p->nran, &p->capran, el);
    src = script_source(p, el, &len, url, sizeof url);
    /* big bundles (YouTube's app is 10 MB) need time just to compile: the
     * budget grows by 4 s per MB.  Stop and Escape still end it at once. */
    p->deadline_ms = now_ms() + SCRIPT_BUDGET_MS + (double)(len >> 20) * 4000.0;
    if (!src) {
        if (dom_attr(el, "src")) fire_simple(p, el, "error");
        return;
    }
    {
        double ts = now_ms();
    g = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, g, "__currentScript", kind == 1 ? wrap(p, el) : JS_NULL);
    if (kind == 2) {
        r = JS_Eval(ctx, src, len, url, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(r)) {
            set_import_meta(p, r, url);
            r = JS_EvalFunction(ctx, r);
        }
    } else {
        r = JS_Eval(ctx, src, len, url, JS_EVAL_TYPE_GLOBAL);
    }
    if (JS_IsException(r)) {
        report_exception(p, url);
    } else if (kind == 2) {
        run_jobs(p);
        if (JS_PromiseState(ctx, r) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, r);
            JS_Throw(ctx, reason);
            report_exception(p, url);
        }
    }
    JS_FreeValue(ctx, r);
    JS_SetPropertyStr(ctx, g, "__currentScript", JS_NULL);
    JS_FreeValue(ctx, g);
    free(src);
    run_jobs(p);
        logf_(p, "[js] %s %s (%u bytes) %.0f ms", kind == 2 ? "module" : "script", url, (unsigned)len, now_ms() - ts);
    }
    if (dom_attr(el, "src")) fire_simple(p, el, "load");
}

static char *module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    js_page_t *p = (js_page_t *)opaque;
    char abs[URL_CAP];
    char *out;
    int absolute_base = base && (!strncmp(base, "http://", 7) || !strncmp(base, "https://", 8));
    if (url_resolve(absolute_base ? base : p->doc->base_url, name, abs, sizeof abs) != 0) snprintf(abs, sizeof abs, "%s", name);
    out = (char *)js_malloc(ctx, strlen(abs) + 1);
    if (out) strcpy(out, abs);
    return out;
}

static JSModuleDef *module_load(JSContext *ctx, const char *name, void *opaque) {
    js_page_t *p = (js_page_t *)opaque;
    char *body = 0, final_url[URL_CAP];
    size_t len = 0;
    JSValue func;
    if (!p->host.fetch || p->host.fetch(p->host.ctx, name, &body, &len, final_url, sizeof final_url) != 0) {
        JS_ThrowReferenceError(ctx, "could not load module '%s'", name);
        return 0;
    }
    func = JS_Eval(ctx, body, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(body);
    if (JS_IsException(func)) return 0;
    set_import_meta(p, func, name);
    JS_FreeValue(ctx, func);   /* the module stays registered with the runtime */
    return (JSModuleDef *)JS_VALUE_GET_PTR(func);
}

static void rejection_tracker(JSContext *ctx, JSValueConst promise, JSValueConst reason, JS_BOOL handled, void *opaque) {
    js_page_t *p = (js_page_t *)opaque;
    (void)promise;
    if (!handled) {
        const char *s = JS_ToCString(ctx, reason);
        JSValue stack = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED;
        const char *st = JS_IsString(stack) ? JS_ToCString(ctx, stack) : 0;
        logf_(p, "[js] unhandled rejection: %s %s", s ? s : "?", st ? st : "");
        if (s) JS_FreeCString(ctx, s);
        if (st) JS_FreeCString(ctx, st);
        JS_FreeValue(ctx, stack);
    }
}

/* ---- setup -------------------------------------------------------------- */

static const JSCFunctionListEntry node_funcs[] = {
    JS_CGETSET_DEF("nodeType", node_get_type, 0),
    JS_CGETSET_DEF("nodeName", node_get_name, 0),
    JS_CGETSET_DEF("parentNode", node_get_parent, 0),
    JS_CGETSET_DEF("parentElement", node_get_parent_el, 0),
    JS_CGETSET_DEF("firstChild", node_get_first, 0),
    JS_CGETSET_DEF("lastChild", node_get_last, 0),
    JS_CGETSET_DEF("nextSibling", node_get_next, 0),
    JS_CGETSET_DEF("previousSibling", node_get_prev, 0),
    JS_CGETSET_DEF("isConnected", node_get_connected, 0),
    JS_CGETSET_DEF("textContent", node_get_text, node_set_text),
    JS_CFUNC_DEF("appendChild", 1, node_append_child),
    JS_CFUNC_DEF("insertBefore", 2, node_insert_before),
    JS_CFUNC_DEF("removeChild", 1, node_remove_child),
    JS_CFUNC_DEF("replaceChild", 2, node_replace_child),
    JS_CFUNC_DEF("cloneNode", 1, node_clone),
    JS_CFUNC_DEF("contains", 1, node_contains),
    JS_CFUNC_DEF("hasChildNodes", 0, node_has_children),
    JS_CFUNC_DEF("__remove", 0, node_remove),
    JS_CFUNC_DEF("querySelector", 1, el_query),
    JS_CFUNC_DEF("querySelectorAll", 1, el_query_all),
};

static const JSCFunctionListEntry chardata_funcs[] = {
    JS_CGETSET_DEF("data", node_get_data, node_set_text),
    JS_CGETSET_DEF("nodeValue", node_get_data, node_set_text),
};

static const JSCFunctionListEntry element_funcs[] = {
    JS_CGETSET_DEF("localName", node_get_local_name, 0),
    JS_CGETSET_DEF("tagName", node_get_name, 0),
    JS_CGETSET_DEF("innerHTML", el_get_inner_html, el_set_inner_html),
    JS_CGETSET_DEF("outerHTML", el_get_outer_html, 0),
    JS_CGETSET_DEF("value", el_get_value, el_set_value),
    JS_CGETSET_DEF("checked", el_get_checked, el_set_checked),
    JS_CGETSET_DEF("selectedIndex", el_get_selected_index, el_set_selected_index),
    JS_CFUNC_DEF("getAttribute", 1, el_get_attr),
    JS_CFUNC_DEF("setAttribute", 2, el_set_attr),
    JS_CFUNC_DEF("removeAttribute", 1, el_remove_attr),
    JS_CFUNC_DEF("getAttributeNames", 0, el_attr_names),
    JS_CFUNC_DEF("insertAdjacentHTML", 2, el_insert_adjacent_html),
    JS_CFUNC_DEF("matches", 1, el_matches),
    JS_CFUNC_DEF("getBoundingClientRect", 0, el_rect),
    JS_CFUNC_DEF("focus", 0, el_focus),
    JS_CFUNC_DEF("blur", 0, el_blur),
    JS_CFUNC_DEF("__options", 0, el_options),
    JS_CFUNC_DEF("__computed", 0, el_computed),
};

static const JSCFunctionListEntry document_funcs[] = {
    JS_CFUNC_DEF("createElement", 1, doc_create_element),
    JS_CFUNC_DEF("createElementNS", 2, doc_create_element),
    JS_CFUNC_MAGIC_DEF("createTextNode", 1, doc_create_text, 0),
    JS_CFUNC_MAGIC_DEF("createComment", 1, doc_create_text, 1),
    JS_CFUNC_DEF("createDocumentFragment", 0, doc_create_fragment),
    JS_CFUNC_DEF("getElementById", 1, doc_get_by_id),
    JS_CGETSET_DEF("documentElement", doc_get_html, 0),
    JS_CGETSET_DEF("head", doc_get_head, 0),
    JS_CGETSET_DEF("body", doc_get_body, 0),
    JS_CGETSET_DEF("title", doc_get_title, doc_set_title),
    JS_CGETSET_DEF("cookie", doc_get_cookie, doc_set_cookie),
    JS_CGETSET_DEF("activeElement", doc_active, 0),
};

static const JSCFunctionListEntry fragment_funcs[] = {
    JS_CFUNC_DEF("getElementById", 1, doc_get_by_id),
};

static const JSCFunctionListEntry global_funcs[] = {
    JS_CFUNC_DEF("__setTimer", 4, g_set_timer),
    JS_CFUNC_DEF("__clearTimer", 1, g_clear_timer),
    JS_CFUNC_DEF("__fetch", 5, g_fetch),
    JS_CFUNC_DEF("__log", 1, g_log),
    JS_CFUNC_DEF("__now", 0, g_now),
    JS_CFUNC_DEF("__navigate", 2, g_navigate),
    JS_CFUNC_DEF("__setUrl", 1, g_set_url),
    JS_CFUNC_DEF("__getUrl", 0, g_get_url),
    JS_CFUNC_DEF("__historyGo", 1, g_history_go),
    JS_CFUNC_DEF("__viewport", 0, g_viewport),
    JS_CFUNC_DEF("__scrollTo", 2, g_scroll_to),
    JS_CFUNC_DEF("__status", 1, g_status),
    JS_CFUNC_DEF("__randomBytes", 1, g_random),
    JS_CFUNC_DEF("__utf8Decode", 1, g_utf8_decode),
    JS_CFUNC_DEF("__utf8Encode", 1, g_utf8_encode),
    JS_CFUNC_DEF("__storageLoad", 1, g_storage_load),
    JS_CFUNC_DEF("__storageSave", 2, g_storage_save),
    JS_CFUNC_DEF("__resolve", 2, g_resolve),
    JS_CFUNC_DEF("__submit", 2, g_submit),
};

static JSValue illegal_ctor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* Interface objects so `x instanceof HTMLInputElement` and friends work. */
static JSValue make_interface(JSContext *ctx, JSValue global, const char *name, JSValueConst parent_proto) {
    JSValue proto = JS_NewObjectProto(ctx, parent_proto);
    JSValue ctor = JS_NewCFunction2(ctx, illegal_ctor, name, 0, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_SetPropertyStr(ctx, global, name, ctor);
    return proto;
}

static const struct { int tag; const char *name; } tag_interfaces[] = {
    { T_INPUT, "HTMLInputElement" }, { T_TEXTAREA, "HTMLTextAreaElement" }, { T_SELECT, "HTMLSelectElement" },
    { T_OPTION, "HTMLOptionElement" }, { T_BUTTON, "HTMLButtonElement" }, { T_FORM, "HTMLFormElement" },
    { T_A, "HTMLAnchorElement" }, { T_IMG, "HTMLImageElement" }, { T_IFRAME, "HTMLIFrameElement" },
    { T_SCRIPT, "HTMLScriptElement" }, { T_STYLE, "HTMLStyleElement" }, { T_LINK, "HTMLLinkElement" },
    { T_TEMPLATE, "HTMLTemplateElement" }, { T_CANVAS, "HTMLCanvasElement" }, { T_VIDEO, "HTMLVideoElement" },
    { T_AUDIO, "HTMLAudioElement" }, { T_DIV, "HTMLDivElement" }, { T_SPAN, "HTMLSpanElement" },
    { T_P, "HTMLParagraphElement" }, { T_UL, "HTMLUListElement" }, { T_OL, "HTMLOListElement" },
    { T_LI, "HTMLLIElement" }, { T_TABLE, "HTMLTableElement" }, { T_TR, "HTMLTableRowElement" },
    { T_TD, "HTMLTableCellElement" }, { T_BODY, "HTMLBodyElement" }, { T_HTML, "HTMLHtmlElement" },
    { T_HEAD, "HTMLHeadElement" }, { T_LABEL, "HTMLLabelElement" }, { T_META, "HTMLMetaElement" },
    { T_SVG, "SVGSVGElement" }, { T_DIALOG, "HTMLDialogElement" }, { T_DETAILS, "HTMLDetailsElement" },
};

js_page_t *js_page_new(dom_doc_t *doc, const char *url, const js_host_t *host) {
    js_page_t *p = (js_page_t *)calloc(1, sizeof(js_page_t));
    JSContext *ctx;
    JSValue global, elproto, r;
    if (!p) return 0;
    p->doc = doc;
    p->host = *host;
    snprintf(p->url, sizeof p->url, "%s", url);
    p->rt = JS_NewRuntime();
    if (!p->rt) { free(p); return 0; }
    JS_SetMemoryLimit(p->rt, 512u * 1024 * 1024);
    JS_SetMaxStackSize(p->rt, 768 * 1024);
    JS_SetInterruptHandler(p->rt, interrupt_cb, p);
    p->ctx = ctx = JS_NewContext(p->rt);
    if (!ctx) { JS_FreeRuntime(p->rt); free(p); return 0; }
    JS_SetContextOpaque(ctx, p);
    JS_SetModuleLoaderFunc(p->rt, module_normalize, module_load, p);
    JS_SetHostPromiseRejectionTracker(p->rt, rejection_tracker, p);
    if (!node_class) JS_NewClassID(&node_class);
    {
        JSClassDef def;
        memset(&def, 0, sizeof def);
        def.class_name = "Node";
        JS_NewClass(p->rt, node_class, &def);
    }
    for (int i = 0; i < T_COUNT; i++) p->tag_proto[i] = JS_UNDEFINED;

    global = JS_GetGlobalObject(ctx);
    p->proto_target = make_interface(ctx, global, "EventTarget", JS_NULL);
    {
        /* EventTarget's prototype should chain to Object.prototype */
        JSValue objproto = JS_GetPropertyStr(ctx, global, "Object");
        JSValue op = JS_GetPropertyStr(ctx, objproto, "prototype");
        JS_SetPrototype(ctx, p->proto_target, op);
        JS_FreeValue(ctx, op);
        JS_FreeValue(ctx, objproto);
    }
    p->proto_node = make_interface(ctx, global, "Node", p->proto_target);
    JS_SetPropertyFunctionList(ctx, p->proto_node, node_funcs, sizeof node_funcs / sizeof node_funcs[0]);
    p->proto_chardata = make_interface(ctx, global, "CharacterData", p->proto_node);
    JS_SetPropertyFunctionList(ctx, p->proto_chardata, chardata_funcs, sizeof chardata_funcs / sizeof chardata_funcs[0]);
    p->proto_text = make_interface(ctx, global, "Text", p->proto_chardata);
    p->proto_comment = make_interface(ctx, global, "Comment", p->proto_chardata);
    {
        /* never created by the parser, but polyfills (ShadyDOM) patch their prototypes */
        JSValue cdata = make_interface(ctx, global, "CDATASection", p->proto_text);
        JSValue pi = make_interface(ctx, global, "ProcessingInstruction", p->proto_chardata);
        JS_FreeValue(ctx, cdata);
        JS_FreeValue(ctx, pi);
    }
    elproto = make_interface(ctx, global, "Element", p->proto_node);
    JS_SetPropertyFunctionList(ctx, elproto, element_funcs, sizeof element_funcs / sizeof element_funcs[0]);
    p->proto_element = make_interface(ctx, global, "HTMLElement", elproto);
    JS_FreeValue(ctx, elproto);
    {
        JSValue svg = make_interface(ctx, global, "SVGElement", p->proto_element);
        JS_FreeValue(ctx, svg);
    }
    for (size_t i = 0; i < sizeof tag_interfaces / sizeof tag_interfaces[0]; i++) {
        p->tag_proto[tag_interfaces[i].tag] = make_interface(ctx, global, tag_interfaces[i].name, p->proto_element);
    }
    p->proto_document = make_interface(ctx, global, "Document", p->proto_node);
    JS_SetPropertyFunctionList(ctx, p->proto_document, document_funcs, sizeof document_funcs / sizeof document_funcs[0]);
    {
        JSValue h = make_interface(ctx, global, "HTMLDocument", p->proto_document);
        JS_FreeValue(ctx, h);
    }
    p->proto_fragment = make_interface(ctx, global, "DocumentFragment", p->proto_node);
    JS_SetPropertyFunctionList(ctx, p->proto_fragment, fragment_funcs, sizeof fragment_funcs / sizeof fragment_funcs[0]);
    JS_SetPropertyFunctionList(ctx, global, global_funcs, sizeof global_funcs / sizeof global_funcs[0]);
    JS_SetPropertyStr(ctx, global, "document", wrap(p, doc->root));
    JS_SetPropertyStr(ctx, global, "__currentScript", JS_NULL);
    JS_FreeValue(ctx, global);

    r = JS_Eval(ctx, surfer_prelude_js, surfer_prelude_js_len, "surfer:prelude", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) report_exception(p, "prelude");
    JS_FreeValue(ctx, r);
    return p;
}

void js_page_free(js_page_t *p) {
    if (!p) return;
    for (int i = 0; i < p->ntimers; i++) timer_free(p, &p->timers[i]);
    for (int i = 0; i < p->nreqs; i++) {
        JS_FreeValue(p->ctx, p->reqs[i].cb);
        http_free(p->reqs[i].r);
        free(p->reqs[i].headers);
        free(p->reqs[i].body);
    }
    for (int i = 0; i < p->nwrap; i++) {
        dom_node_t *n = (dom_node_t *)JS_GetOpaque(p->wrappers[i], node_class);
        if (n) n->js = 0;
        JS_FreeValue(p->ctx, p->wrappers[i]);
    }
    JS_FreeValue(p->ctx, p->proto_target);
    JS_FreeValue(p->ctx, p->proto_node);
    JS_FreeValue(p->ctx, p->proto_element);
    JS_FreeValue(p->ctx, p->proto_chardata);
    JS_FreeValue(p->ctx, p->proto_text);
    JS_FreeValue(p->ctx, p->proto_comment);
    JS_FreeValue(p->ctx, p->proto_document);
    JS_FreeValue(p->ctx, p->proto_fragment);
    for (int i = 0; i < T_COUNT; i++) JS_FreeValue(p->ctx, p->tag_proto[i]);
    JS_FreeContext(p->ctx);
    JS_FreeRuntime(p->rt);
    free(p->wrappers);
    free(p->timers);
    free(p->reqs);
    free(p->ran);
    free(p->queued);
    free(p->loads);
    free(p->step_list);
    free(p);
}

static void collect_scripts(dom_node_t *n, dom_node_t ***list, int *count, int *cap) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        if (k->type != N_ELEMENT) continue;
        if (k->tag == T_SCRIPT) ptr_push(list, count, cap, k);
        else if (k->tag != T_TEMPLATE) collect_scripts(k, list, count, cap);
    }
}

static void set_ready_state(js_page_t *p, const char *state) {
    JSValue g = JS_GetGlobalObject(p->ctx);
    JS_SetPropertyStr(p->ctx, g, "__readyState", JS_NewString(p->ctx, state));
    JS_FreeValue(p->ctx, g);
}

/* Scripts run one per js_run_step() call so the browser can draw, scroll and
 * take a Stop click between them: parser-blocking classic scripts first,
 * then deferred ones and modules, then DOMContentLoaded and load. */
/* runs one script and logs it when it is slow */
static void run_script_timed(js_page_t *p, dom_node_t *el) {
    double t0 = now_ms();
    const char *src = dom_attr(el, "src");
    run_script(p, el);
    if (now_ms() - t0 > 100) logf_(p, "[js] %s %d ms", src ? src : "(inline script)", (int)(now_ms() - t0));
}

static void finish_loading(js_page_t *p) {
    p->nqueued = 0;      /* scripts found by the scan already ran */
    p->loading = 0;
    set_ready_state(p, "interactive");
    fire_simple(p, p->doc->root, "DOMContentLoaded");
    run_jobs(p);
    set_ready_state(p, "complete");
    {
        JSValue g = JS_GetGlobalObject(p->ctx);
        JSValue fn = JS_GetPropertyStr(p->ctx, g, "__fireWindowLoad");
        if (JS_IsFunction(p->ctx, fn)) {
            JSValue r = JS_Call(p->ctx, fn, g, 0, 0);
            if (JS_IsException(r)) report_exception(p, "load event");
            JS_FreeValue(p->ctx, r);
        }
        JS_FreeValue(p->ctx, fn);
        JS_FreeValue(p->ctx, g);
    }
    run_jobs(p);
}

int js_run_step(js_page_t *p) {
    if (!p || p->step_phase == 3) return 0;
    if (p->step_phase == 0) {
        p->loading = 1;
        collect_scripts(p->doc->root, &p->step_list, &p->step_count, &p->step_cap);
        p->step_phase = 1;
        p->step_i = 0;
    }
    while (p->step_phase == 1 && p->step_i < p->step_count) {
        dom_node_t *el = p->step_list[p->step_i++];
        if (script_kind(el) == 1 && !(dom_attr(el, "defer") && dom_attr(el, "src")) && !dom_attr(el, "async")) {
            run_script_timed(p, el);
            return 1;
        }
    }
    if (p->step_phase == 1) {
        p->step_phase = 2;
        p->step_i = 0;
    }
    while (p->step_phase == 2 && p->step_i < p->step_count) {
        dom_node_t *el = p->step_list[p->step_i++];
        if (script_kind(el) && !ptr_in(p->ran, p->nran, el)) {
            run_script_timed(p, el);
            return 1;
        }
        if (!script_kind(el)) ptr_push(&p->ran, &p->nran, &p->capran, el);
    }
    free(p->step_list);
    p->step_list = 0;
    p->step_count = p->step_cap = 0;
    p->step_phase = 3;
    finish_loading(p);
    return 0;
}

/* The script js_run_step() will run next, so the browser can wait for just
 * that file instead of every script on the page. */
dom_node_t *js_next_script(js_page_t *p) {
    if (!p || p->step_phase == 3) return 0;
    if (p->step_phase == 0) {
        p->loading = 1;
        collect_scripts(p->doc->root, &p->step_list, &p->step_count, &p->step_cap);
        p->step_phase = 1;
        p->step_i = 0;
    }
    if (p->step_phase == 1) {
        for (int i = p->step_i; i < p->step_count; i++) {
            dom_node_t *el = p->step_list[i];
            if (script_kind(el) == 1 && !(dom_attr(el, "defer") && dom_attr(el, "src")) && !dom_attr(el, "async"))
                return el;
        }
        for (int i = 0; i < p->step_count; i++) {
            dom_node_t *el = p->step_list[i];
            if (script_kind(el) && !ptr_in(p->ran, p->nran, el)) return el;
        }
        return 0;
    }
    for (int i = p->step_i; i < p->step_count; i++) {
        dom_node_t *el = p->step_list[i];
        if (script_kind(el) && !ptr_in(p->ran, p->nran, el)) return el;
    }
    return 0;
}

void js_run_scripts(js_page_t *p) {
    while (js_run_step(p)) {}
}

/* Stop: the next interrupt check ends whatever script is running. */
void js_abort(js_page_t *p) {
    if (p) p->abort = 1;
}

/* QuickJS polls this every few thousand operations. */
static int interrupt_cb(JSRuntime *rt, void *opaque) {
    js_page_t *p = (js_page_t *)opaque;
    (void)rt;
    if (p->abort) return 1;
    if (p->deadline_ms > 0 && now_ms() > p->deadline_ms) {
        logf_(p, "script stopped after %d s without finishing", SCRIPT_BUDGET_MS / 1000);
        return 1;
    }
    return 0;
}

int js_dispatch(js_page_t *p, dom_node_t *target, const char *type, const js_event_t *ev) {
    JSContext *ctx;
    JSValue g, fn, args[9], r;
    int prevented = 0;
    if (!p || !target || p->abort) return 0;
    p->deadline_ms = now_ms() + SCRIPT_BUDGET_MS;
    ctx = p->ctx;
    g = JS_GetGlobalObject(ctx);
    fn = JS_GetPropertyStr(ctx, g, "__dispatchFromHost");
    if (JS_IsFunction(ctx, fn)) {
        args[0] = wrap(p, target);
        args[1] = JS_NewString(ctx, type);
        args[2] = JS_NewInt32(ctx, ev ? ev->client_x : 0);
        args[3] = JS_NewInt32(ctx, ev ? ev->client_y : 0);
        args[4] = JS_NewInt32(ctx, ev ? ev->button : 0);
        args[5] = JS_NewInt32(ctx, ev ? ev->mods : 0);
        args[6] = JS_NewInt32(ctx, ev ? (int32_t)ev->key : 0);
        args[7] = JS_NewInt32(ctx, ev ? ev->x : 0);
        args[8] = JS_NewInt32(ctx, ev ? ev->y : 0);
        r = JS_Call(ctx, fn, g, 9, args);
        if (JS_IsException(r)) report_exception(p, type);
        else prevented = JS_ToBool(ctx, r);
        JS_FreeValue(ctx, r);
        for (int i = 0; i < 9; i++) JS_FreeValue(ctx, args[i]);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, g);
    run_jobs(p);
    return prevented;
}

int js_tick(js_page_t *p) {
    double t;
    int ran = 0, busy;
    if (!p || p->abort) return 0;
    p->deadline_ms = now_ms() + SCRIPT_BUDGET_MS;
    t = now_ms();
#ifdef TLS_HOST
    { static int last = -1; if (getenv("JSTRACE") && p->ntimers != last) { logf_(p, "[trace] tick start ntimers=%d t=%.0f", p->ntimers, t); last = p->ntimers; } }
#endif
    /* timers due now; callbacks may add or clear timers, so look them up by id */
    {
        int ids[MAX_TIMERS_PER_TICK], nid = 0;
        for (int i = 0; i < p->ntimers && nid < MAX_TIMERS_PER_TICK; i++) {
            if (p->timers[i].due <= t) ids[nid++] = p->timers[i].id;
        }
        for (int k = 0; k < nid; k++) {
            int idx = -1;
            js_timer_t tm;
            JSValue r, arg;
            for (int i = 0; i < p->ntimers; i++) if (p->timers[i].id == ids[k]) { idx = i; break; }
            if (idx < 0) continue;
            tm = p->timers[idx];
            if (tm.interval >= 0) {
                p->timers[idx].due = t + tm.interval;
                tm.fn = JS_DupValue(p->ctx, tm.fn);
            } else {
                p->timers[idx] = p->timers[--p->ntimers];
            }
#ifdef TLS_HOST
            if (getenv("JSTRACE")) logf_(p, "[trace] timer %d fire", tm.id);
#endif
#ifdef TLS_HOST
            double tf0 = now_ms();
#endif
            arg = tm.raf ? JS_NewFloat64(p->ctx, t) : JS_UNDEFINED;
            r = tm.raf ? JS_Call(p->ctx, tm.fn, JS_UNDEFINED, 1, &arg)
                       : JS_Call(p->ctx, tm.fn, JS_UNDEFINED, tm.argc, tm.argv);
            if (JS_IsException(r)) report_exception(p, tm.raf ? "animation frame" : "timer");
#ifdef TLS_HOST
            if (getenv("JSTRACE")) logf_(p, "[trace] timer %d took %.0f ms", tm.id, now_ms() - tf0);
#endif
            JS_FreeValue(p->ctx, r);
            JS_FreeValue(p->ctx, arg);
            JS_FreeValue(p->ctx, tm.fn);
            if (tm.interval < 0) for (int i = 0; i < tm.argc; i++) JS_FreeValue(p->ctx, tm.argv[i]);
            run_jobs(p);
            ran = 1;
        }
    }
    if (pump_requests(p)) ran = 1;
    run_jobs(p);
    while (p->nloads > 0) {
        dom_node_t *el = p->loads[--p->nloads];
        fire_simple(p, el, "load");
        ran = 1;
    }
    /* scripts added to the document since the last tick */
    while (p->nqueued > 0) {
        dom_node_t *el = p->queued[0];
        memmove(p->queued, p->queued + 1, sizeof(dom_node_t *) * (size_t)(p->nqueued - 1));
        p->nqueued--;
        if (connected(p, el)) run_script(p, el);
        ran = 1;
    }
    (void)ran;
    busy = p->ntimers > 0 || p->nreqs > 0 || p->nqueued > 0 || p->nloads > 0;
#ifdef TLS_HOST
    if (getenv("JSTRACE") && ran) logf_(p, "[trace] tick end ntimers=%d nreqs=%d", p->ntimers, p->nreqs);
#endif
    return p->dirty | (busy ? JS_BUSY : 0);
}

int js_take_dirty(js_page_t *p) {
    int d;
    if (!p) return 0;
    d = p->dirty;
    p->dirty = 0;
    return d;
}

long js_next_due(js_page_t *p) {
    double t, best = 1e18;
    if (!p) return -1;
    t = now_ms();
    for (int i = 0; i < p->ntimers; i++) {
        double d = p->timers[i].due - t;
        if (d < best) best = d;
    }
    if (p->nreqs && best > 5) best = 5;
    if (p->ntimers == 0 && !p->nreqs) return -1;
    return best < 0 ? 0 : (long)best;
}

/* Evaluates code in the page and returns its result as a string (debugging aid). */
size_t js_eval_string(js_page_t *p, const char *code, char *out, size_t cap) {
    JSValue r;
    const char *s;
    size_t n = 0;
    if (!p || !out || cap == 0) return 0;
    out[0] = 0;
    r = JS_Eval(p->ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        report_exception(p, "eval");
        snprintf(out, cap, "(exception)");
        return strlen(out);
    }
    run_jobs(p);
    s = JS_ToCString(p->ctx, r);
    if (s) {
        n = (size_t)snprintf(out, cap, "%s", s);
        JS_FreeCString(p->ctx, s);
    }
    JS_FreeValue(p->ctx, r);
    return n < cap ? n : cap - 1;
}
