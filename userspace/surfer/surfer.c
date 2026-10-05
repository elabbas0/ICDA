/* Surfer: ICDA's native web browser.
 *
 * Pipeline: http_get -> html_parse -> stylesheets (+@import) -> css_cascade
 * -> layout_document -> paint_layout into the page area of an ic_app window.
 * Images stream in afterwards, a few requests at a time, and the page is
 * laid out again as they arrive. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libicda.h"
#include "http.h"
#include "dom.h"
#include "css.h"
#include "font.h"
#include "layout.h"
#include "paint.h"
#include "image.h"
#include "form.h"
#include "js.h"

#define WIN_W        1024
#define WIN_H        720
#define STATUS_H     24
#define HISTORY_CAP  64
#define SHEETS_CAP   96
#define IMAGES_CAP   384
#define IMG_PARALLEL 3
#define IMG_BYTES_CAP (8u * 1024 * 1024)
#define WHEEL_STEP   64

#define HOME_URL     "about:home"
#define SEARCH_URL   "https://lite.duckduckgo.com/lite/?q="

typedef struct {
    char       *url;
    int         state;          /* 0 queued, 1 loading, 2 ready, -1 failed */
    int         redirects;
    http_req_t *req;
    image_t     img;
} img_ent_t;

static struct {
    char        url[URL_CAP];           /* page being shown */
    char        addr_buf[URL_CAP];
    ic_textfield_t addr;
    int         addr_focused;
    int         addr_select_all;

    char        history[HISTORY_CAP][URL_CAP];
    int         hist_count, hist_pos;

    /* pending navigation, carried out by tick() after a "Loading" frame */
    char        nav_url[URL_CAP];
    int         nav_pending, nav_push, nav_drawn;
    char       *nav_body;               /* form POST body, 0 for GET */
    size_t      nav_body_len;

    dom_node_t *focus;                  /* focused form control on the page */
    char       *focus_value;            /* value when it gained focus, for "change" */
    js_page_t  *js;
    ic_app_t   *app;
    int         js_dirty;               /* JS_DIRTY_* waiting for a restyle */
    uint32_t    last_restyle_ms;
    struct { char *url; char *text; size_t len; } css_cache[SHEETS_CAP];
    int         ncss_cache;
    dom_node_t *popup;                  /* <select> whose option list is open */
    int         popup_hover;

    http_req_t *page_req;
    char       *page_src;               /* generated pages (home, errors) */
    dom_doc_t  *doc;
    css_sheet_t *sheets[SHEETS_CAP];
    int         nsheets;
    layout_t   *L;
    float       scroll;
    int         need_layout;
    uint32_t    last_layout_ms;

    img_ent_t   imgs[IMAGES_CAP];
    int         nimgs;

    char        status[URL_CAP + 32];
    char        hover_url[URL_CAP];
    int         hover_back, hover_fwd, hover_reload;
    int         fonts_ok;
} sf;

/* ---- helpers ------------------------------------------------------------ */

static void set_status(const char *s) {
    snprintf(sf.status, sizeof sf.status, "%s", s ? s : "");
}

static void set_address(const char *s) {
    snprintf(sf.addr_buf, sizeof sf.addr_buf, "%s", s ? s : "");
    sf.addr.text = sf.addr_buf;
    sf.addr.cursor = (int)strlen(sf.addr_buf);
    sf.addr.sel_start = sf.addr.sel_end = sf.addr.cursor;
    sf.addr.scroll_px = 0;
}

static uint32_t now_ms(void) { return ic_time_ms(); }

static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

/* Turns address-bar input into a URL: full URLs pass through, things that
 * look like host names get https://, anything else becomes a search. */
static void normalize_input(const char *in, char *out, size_t cap) {
    const char *s = in;
    size_t n;
    int dotted = 0, spaced = 0;
    while (*s == ' ') s++;
    n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '.' || s[i] == ':' || s[i] == '/') dotted = 1;
        if (s[i] == ' ') spaced = 1;
    }
    if (n == 0) { snprintf(out, cap, "%s", HOME_URL); return; }
    if (strstr(s, "://") || starts_with(s, "about:")) { snprintf(out, cap, "%.*s", (int)n, s); return; }
    if (dotted && !spaced) { snprintf(out, cap, "https://%.*s", (int)n, s); return; }
    {
        static const char hex[] = "0123456789ABCDEF";
        size_t o = (size_t)snprintf(out, cap, "%s", SEARCH_URL);
        for (size_t i = 0; i < n && o + 4 < cap; i++) {
            unsigned char ch = (unsigned char)s[i];
            if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                ch == '-' || ch == '_' || ch == '.' || ch == '~') out[o++] = (char)ch;
            else if (ch == ' ') out[o++] = '+';
            else { out[o++] = '%'; out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 15]; }
        }
        out[o] = 0;
    }
}

/* ---- layout rectangles -------------------------------------------------- */

static ic_rect_t toolbar_rect(ic_app_t *app) { return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR); }

static ic_rect_t back_rect(ic_app_t *app) {
    (void)app;
    return ic_rect_make(IC_SP_3, (IC_H_TOOLBAR - IC_H_CONTROL) / 2, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t fwd_rect(ic_app_t *app) {
    ic_rect_t r = back_rect(app);
    return ic_rect_make(r.x + r.w + IC_SP_1, r.y, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t reload_rect(ic_app_t *app) {
    ic_rect_t r = fwd_rect(app);
    return ic_rect_make(r.x + r.w + IC_SP_1, r.y, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t addr_rect(ic_app_t *app) {
    ic_rect_t r = reload_rect(app);
    int x = r.x + r.w + IC_SP_2;
    int w = app->width - x - IC_SP_3;
    if (w < 80) w = 80;
    return ic_rect_make(x, r.y, w, IC_H_CONTROL);
}

static ic_rect_t page_rect(ic_app_t *app) {
    return ic_rect_make(0, IC_H_TOOLBAR, app->width, app->height - IC_H_TOOLBAR - STATUS_H);
}

static ic_rect_t status_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - STATUS_H, app->width, STATUS_H);
}

/* ---- images ------------------------------------------------------------- */

static img_ent_t *image_lookup(const char *url, int add) {
    for (int i = 0; i < sf.nimgs; i++) {
        if (strcmp(sf.imgs[i].url, url) == 0) return &sf.imgs[i];
    }
    if (!add || sf.nimgs >= IMAGES_CAP) return 0;
    {
        img_ent_t *e = &sf.imgs[sf.nimgs];
        memset(e, 0, sizeof(*e));
        e->url = strdup(url);
        if (!e->url) return 0;
        sf.nimgs++;
        return e;
    }
}

/* Layout callback: intrinsic size of an <img> if it has been decoded. */
static int image_size(dom_node_t *n, void *ctx, int *w, int *h, void **image) {
    const char *src = dom_attr(n, "src");
    const char *lazy = dom_attr(n, "data-src");
    char url[URL_CAP];
    img_ent_t *e;
    (void)ctx;
    *w = *h = 0;
    *image = 0;
    if ((!src || starts_with(src, "data:")) && lazy) src = lazy;
    if (!src || !src[0] || starts_with(src, "data:")) return 0;
    if (url_resolve(sf.doc->base_url, src, url, sizeof url) != 0) return 0;
    if (!starts_with(url, "http://") && !starts_with(url, "https://")) return 0;
    e = image_lookup(url, 1);
    if (!e || e->state != 2) return 0;
    *w = e->img.w;
    *h = e->img.h;
    *image = &e->img;
    return 1;
}

static void images_clear(void) {
    for (int i = 0; i < sf.nimgs; i++) {
        if (sf.imgs[i].req) http_free(sf.imgs[i].req);
        image_release(&sf.imgs[i].img);
        free(sf.imgs[i].url);
    }
    sf.nimgs = 0;
}

static int image_start(img_ent_t *e, const char *url) {
    e->req = http_open(url, "GET", "Accept: image/png,image/jpeg,image/gif,image/*;q=0.8\r\n");
    if (!e->req) { e->state = -1; return -1; }
    e->state = 1;
    return 0;
}

/* Advances image downloads; returns 1 if an image finished. */
static int images_pump(void) {
    int active = 0, finished = 0;
    for (int i = 0; i < sf.nimgs; i++) {
        img_ent_t *e = &sf.imgs[i];
        http_req_t *r = e->req;
        if (e->state != 1 || !r) continue;
        active++;
        if (http_poll(r, 0) == HTTP_PENDING && r->body_len <= IMG_BYTES_CAP) continue;
        if (r->state == HTTP_DONE && r->status >= 300 && r->status < 400 && r->location[0] && e->redirects < 4) {
            char next[URL_CAP];
            e->redirects++;
            if (url_resolve(e->url, r->location, next, sizeof next) == 0) {
                http_free(r);
                e->req = 0;
                if (image_start(e, next) == 0) continue;
            }
        }
        e->state = (r->state == HTTP_DONE && r->status == 200 && image_decode(r->body, r->body_len, &e->img) == 0) ? 2 : -1;
        if (e->req) http_free(e->req);
        e->req = 0;
        active--;
        if (e->state == 2) finished = 1;
    }
    for (int i = 0; i < sf.nimgs && active < IMG_PARALLEL; i++) {
        if (sf.imgs[i].state == 0 && image_start(&sf.imgs[i], sf.imgs[i].url) == 0) active++;
    }
    return finished;
}

static int images_busy(void) {
    for (int i = 0; i < sf.nimgs; i++) {
        if (sf.imgs[i].state == 0 || sf.imgs[i].state == 1) return 1;
    }
    return 0;
}

/* ---- page loading ------------------------------------------------------- */

static void page_clear(void) {
    if (sf.js) {
        js_page_free(sf.js);
        sf.js = 0;
    }
    sf.js_dirty = 0;
    free(sf.focus_value);
    sf.focus_value = 0;
    for (int i = 0; i < sf.ncss_cache; i++) {
        free(sf.css_cache[i].url);
        free(sf.css_cache[i].text);
    }
    sf.ncss_cache = 0;
    images_clear();
    if (sf.L) layout_free(sf.L);
    sf.L = 0;
    sf.focus = sf.popup = 0;
    for (int i = 0; i < sf.nsheets; i++) css_sheet_free(sf.sheets[i]);
    sf.nsheets = 0;
    if (sf.doc) {
        form_release(sf.doc->root);
        dom_free(sf.doc);
    }
    sf.doc = 0;
    if (sf.page_req) http_free(sf.page_req);
    sf.page_req = 0;
    free(sf.page_src);
    sf.page_src = 0;
}

/* Stylesheet text by URL, kept for the page's lifetime so restyles after a
 * script changes the DOM do not refetch. */
static int css_fetch(const char *url, const char **text, size_t *len, char *fin, size_t fin_cap) {
    http_req_t *r;
    for (int i = 0; i < sf.ncss_cache; i++) {
        if (strcmp(sf.css_cache[i].url, url) == 0) {
            if (!sf.css_cache[i].text) return -1;
            *text = sf.css_cache[i].text;
            *len = sf.css_cache[i].len;
            snprintf(fin, fin_cap, "%s", url);
            return 0;
        }
    }
    set_status("Loading styles...");
    r = http_get(url, fin, fin_cap);
    if (sf.ncss_cache < SHEETS_CAP) {
        int i = sf.ncss_cache++;
        sf.css_cache[i].url = strdup(url);
        sf.css_cache[i].text = 0;
        sf.css_cache[i].len = 0;
        if (r && r->state == HTTP_DONE && r->status == 200 && r->body) {
            sf.css_cache[i].text = (char *)malloc(r->body_len + 1);
            if (sf.css_cache[i].text) {
                memcpy(sf.css_cache[i].text, r->body, r->body_len);
                sf.css_cache[i].text[r->body_len] = 0;
                sf.css_cache[i].len = r->body_len;
            }
        }
        http_free(r);
        if (!sf.css_cache[i].text) return -1;
        *text = sf.css_cache[i].text;
        *len = sf.css_cache[i].len;
        return 0;
    }
    http_free(r);
    return -1;
}

static void add_sheet(const char *text, size_t len, const char *base, int vw, int depth) {
    css_sheet_t *s;
    const char **imp;
    int n;
    if (sf.nsheets >= SHEETS_CAP) return;
    s = css_sheet_new();
    if (!s) return;
    css_parse(s, text, len, 1, vw);
    n = css_imports(s, &imp);
    for (int i = 0; i < n && depth < 3; i++) {
        char url[URL_CAP], fin[URL_CAP];
        const char *t;
        size_t tl;
        if (url_resolve(base, imp[i], url, sizeof url) != 0) continue;
        if (css_fetch(url, &t, &tl, fin, sizeof fin) == 0) add_sheet(t, tl, fin, vw, depth + 1);
    }
    if (sf.nsheets < SHEETS_CAP) sf.sheets[sf.nsheets++] = s;
    else css_sheet_free(s);
}

static void gather_sheets(dom_node_t *n, int vw) {
    for (dom_node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEMENT) continue;
        if (c->tag == T_STYLE) {
            /* scripts may build a <style> from several text nodes */
            size_t len = 0;
            for (dom_node_t *t = c->first; t; t = t->next) if (t->type == N_TEXT) len += t->text_len;
            if (len) {
                char *css = (char *)malloc(len + 1);
                if (css) {
                    size_t o = 0;
                    for (dom_node_t *t = c->first; t; t = t->next) {
                        if (t->type != N_TEXT) continue;
                        memcpy(css + o, t->text, t->text_len);
                        o += t->text_len;
                    }
                    css[o] = 0;
                    add_sheet(css, o, sf.doc->base_url, vw, 0);
                    free(css);
                }
            }
            continue;
        } else if (c->tag == T_LINK) {
            const char *rel = dom_attr(c, "rel"), *href = dom_attr(c, "href"), *media = dom_attr(c, "media");
            if (rel && strstr(rel, "stylesheet") && !strstr(rel, "alternate") && href && !(media && strstr(media, "print"))) {
                char url[URL_CAP], fin[URL_CAP];
                const char *t;
                size_t tl;
                if (url_resolve(sf.doc->base_url, href, url, sizeof url) != 0) continue;
                if (css_fetch(url, &t, &tl, fin, sizeof fin) == 0) add_sheet(t, tl, fin, vw, 0);
            }
        }
        gather_sheets(c, vw);
    }
}

static void html_escape_into(char *out, size_t cap, size_t *o, const char *s) {
    for (; *s && *o + 8 < cap; s++) {
        const char *rep = *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;" : *s == '"' ? "&quot;" : 0;
        if (rep) { size_t k = strlen(rep); memcpy(out + *o, rep, k); *o += k; }
        else out[(*o)++] = *s;
    }
    out[*o] = 0;
}

static const char home_html[] =
    "<!doctype html><html><head><title>Surfer</title><style>"
    "body{font-family:sans-serif;background:#f6f7f9;color:#1d1f23;margin:0}"
    ".wrap{max-width:640px;margin:72px auto;padding:0 24px}"
    "h1{font-size:40px;margin:0 0 4px;font-weight:600}"
    "p.sub{color:#5b6070;margin:0 0 32px}"
    ".grid{display:flex;flex-wrap:wrap;gap:12px}"
    ".grid a{display:block;width:180px;padding:14px 16px;background:#fff;border:1px solid #dde0e6;border-radius:10px;color:#1d1f23;text-decoration:none}"
    ".grid a b{display:block;margin-bottom:2px}.grid a span{color:#6b7080;font-size:13px}"
    "</style></head><body><div class=wrap><h1>Surfer</h1>"
    "<p class=sub>A fast, lightweight web browser for ICDA. Type an address or a search above.</p>"
    "<div class=grid>"
    "<a href='https://lite.duckduckgo.com/lite/'><b>DuckDuckGo</b><span>lite.duckduckgo.com</span></a>"
    "<a href='https://en.wikipedia.org/wiki/Main_Page'><b>Wikipedia</b><span>en.wikipedia.org</span></a>"
    "<a href='https://news.ycombinator.com/'><b>Hacker News</b><span>news.ycombinator.com</span></a>"
    "<a href='https://example.com/'><b>Example</b><span>example.com</span></a>"
    "<a href='https://text.npr.org/'><b>NPR Text</b><span>text.npr.org</span></a>"
    "<a href='https://github.com/elabbas0/ICDA'><b>ICDA</b><span>github.com</span></a>"
    "</div></div></body></html>";

static void load_generated(const char *url, const char *html, size_t len) {
    sf.page_src = malloc(len + 1);
    if (!sf.page_src) return;
    memcpy(sf.page_src, html, len);
    sf.page_src[len] = 0;
    sf.doc = html_parse(sf.page_src, len, url);
}

static void load_error(const char *url, const char *why) {
    size_t cap = 4096, o;
    char *buf = malloc(cap);
    if (!buf) return;
    o = (size_t)snprintf(buf, cap,
        "<!doctype html><html><head><title>Can't open page</title><style>"
        "body{font-family:sans-serif;color:#1d1f23;margin:0}.w{max-width:560px;margin:96px auto;padding:0 24px}"
        "h1{font-size:26px;font-weight:600}p{color:#5b6070}code{color:#1d1f23}</style></head>"
        "<body><div class=w><h1>Surfer can't open this page</h1><p><code>");
    html_escape_into(buf, cap, &o, url);
    o += (size_t)snprintf(buf + o, cap - o, "</code></p><p>");
    html_escape_into(buf, cap, &o, why);
    o += (size_t)snprintf(buf + o, cap - o, "</p><p>Check the address and the network, then press Reload.</p></div></body></html>");
    load_generated(url, buf, o);
    free(buf);
}

static void relayout(ic_app_t *app) {
    ic_rect_t p = page_rect(app);
    float max;
    if (!sf.doc) return;
    if (sf.L) layout_free(sf.L);
    sf.L = layout_document(sf.doc, (float)p.w, (float)p.h, image_size, 0);
    sf.need_layout = 0;
    sf.last_layout_ms = now_ms();
    max = sf.L ? sf.L->height - (float)p.h : 0;
    if (sf.scroll > max) sf.scroll = max;
    if (sf.scroll < 0) sf.scroll = 0;
}

static void scroll_to_fragment(const char *url) {
    const char *hash = strchr(url, '#');
    dom_node_t *stack[256];
    int sp = 0;
    if (!hash || !hash[1] || !sf.L) return;
    hash++;
    /* depth-first over boxes looking for id= or <a name=> matching the fragment */
    if (sf.L->root) stack[sp++] = (dom_node_t *)(void *)sf.L->root;
    while (sp > 0) {
        box_t *b = (box_t *)(void *)stack[--sp];
        const char *id = b->node ? dom_attr(b->node, "id") : 0;
        const char *nm = b->node ? dom_attr(b->node, "name") : 0;
        if ((id && strcmp(id, hash) == 0) || (nm && strcmp(nm, hash) == 0)) {
            sf.scroll = b->y;
            return;
        }
        for (box_t *k = b->first; k && sp < 256; k = k->next) stack[sp++] = (dom_node_t *)(void *)k;
    }
}

static void js_start(ic_app_t *app);

static void do_navigate(ic_app_t *app) {
    char final_url[URL_CAP];
    ic_rect_t p = page_rect(app);
    uint32_t t0 = now_ms();

    page_clear();
    sf.scroll = 0;
    snprintf(final_url, sizeof final_url, "%s", sf.nav_url);

    if (strcmp(sf.nav_url, HOME_URL) == 0 || strcmp(sf.nav_url, "about:blank") == 0) {
        load_generated(sf.nav_url, home_html, sizeof home_html - 1);
    } else {
        http_req_t *r = sf.nav_body
            ? http_request("POST", sf.nav_url, "application/x-www-form-urlencoded", sf.nav_body, sf.nav_body_len,
                           final_url, sizeof final_url)
            : http_get(sf.nav_url, final_url, sizeof final_url);
        free(sf.nav_body);
        sf.nav_body = 0;
        if (!r || r->state != HTTP_DONE) {
            load_error(sf.nav_url, r && r->error[0] ? r->error : "The server did not respond.");
            http_free(r);
        } else if (starts_with(r->content_type, "image/")) {
            char html[URL_CAP + 128];
            int n = snprintf(html, sizeof html, "<html><body style='margin:0;background:#202124;text-align:center'><img src=\"%s\"></body></html>", final_url);
            http_free(r);
            load_generated(final_url, html, (size_t)n);
        } else if (r->content_type[0] && !strstr(r->content_type, "html") && !strstr(r->content_type, "xml")) {
            /* plain text and anything else readable: show it preformatted */
            size_t cap = r->body_len * 6 + 128, o = 0;
            char *buf = malloc(cap);
            if (buf) {
                o = (size_t)snprintf(buf, cap, "<html><body><pre style='white-space:pre-wrap'>");
                for (size_t i = 0; i < r->body_len && o + 8 < cap; i++) {
                    char ch = (char)r->body[i];
                    if (ch == '<') { memcpy(buf + o, "&lt;", 4); o += 4; }
                    else if (ch == '&') { memcpy(buf + o, "&amp;", 5); o += 5; }
                    else buf[o++] = ch;
                }
                o += (size_t)snprintf(buf + o, cap - o, "</pre></body></html>");
                load_generated(final_url, buf, o);
                free(buf);
            }
            http_free(r);
        } else {
            sf.page_req = r;
            sf.doc = html_parse((const char *)r->body, r->body_len, final_url);
        }
    }
    if (!sf.doc) {
        set_status("Out of memory");
        return;
    }

    snprintf(sf.url, sizeof sf.url, "%s", final_url);
    if (!sf.addr_focused) set_address(sf.url);
    if (sf.nav_push) {
        if (sf.hist_pos + 1 >= HISTORY_CAP) {
            memmove(sf.history[0], sf.history[1], sizeof(sf.history[0]) * (HISTORY_CAP - 1));
            sf.hist_pos--;
        }
        sf.hist_pos++;
        snprintf(sf.history[sf.hist_pos], URL_CAP, "%s", sf.url);
        sf.hist_count = sf.hist_pos + 1;
    } else if (sf.hist_pos >= 0) {
        snprintf(sf.history[sf.hist_pos], URL_CAP, "%s", sf.url);
    }

    gather_sheets(sf.doc->root, p.w);
    set_status("Laying out...");
    css_cascade(sf.doc, sf.sheets, sf.nsheets, p.w, p.h);
    relayout(app);
    scroll_to_fragment(sf.url);
    js_start(app);
    {
        char msg[96];
        char line[URL_CAP + 128];
        unsigned ms = (unsigned)(now_ms() - t0);
        snprintf(msg, sizeof msg, "Done in %u ms", ms);
        set_status(msg);
        snprintf(line, sizeof line, "[surfer] %s %u ms sheets=%d height=%d\n", sf.url, ms, sf.nsheets,
                 sf.L ? (int)sf.L->height : -1);
        icda_write_file("/dev/serial", line, strlen(line));
    }
}

static void navigate(const char *url, int push) {
    if (!url || !url[0]) return;
    free(sf.nav_body);
    sf.nav_body = 0;
    snprintf(sf.nav_url, sizeof sf.nav_url, "%s", url);
    sf.nav_pending = 1;
    sf.nav_push = push;
    sf.nav_drawn = 0;
    set_status("Loading...");
}

static void go_back(void) {
    if (sf.hist_pos > 0) { sf.hist_pos--; navigate(sf.history[sf.hist_pos], 0); }
}

static void go_forward(void) {
    if (sf.hist_pos + 1 < sf.hist_count) { sf.hist_pos++; navigate(sf.history[sf.hist_pos], 0); }
}

/* Follows a link from the page: same-document fragments only scroll. */
static void open_link(const char *href) {
    char url[URL_CAP];
    if (!href || starts_with(href, "javascript:") || starts_with(href, "mailto:")) return;
    if (url_resolve(sf.doc ? sf.doc->base_url : sf.url, href, url, sizeof url) != 0) return;
    if (href[0] == '#') {
        scroll_to_fragment(url);
        return;
    }
    navigate(url, 1);
}

/* ---- forms -------------------------------------------------------------- */

static int fire(dom_node_t *target, const char *type, int x, int y, int button, int mods, uint32_t key);

static void submit_form(dom_node_t *form, dom_node_t *submitter) {
    char url[URL_CAP];
    char *body = 0;
    size_t body_len = 0;
    int post = 0;
    if (!sf.doc || !form) return;
    if (form_submission(sf.doc, form, submitter, url, sizeof url, &post, &body, &body_len) != 0) {
        set_status("Could not submit the form");
        return;
    }
    navigate(url, 1);
    if (post) {
        sf.nav_body = body;
        sf.nav_body_len = body_len;
    }
}

/* Enter in a field: submit through the form's first submit button, or
 * directly when the form has a single text field (HTML implicit submission). */
static dom_node_t *first_submit(dom_node_t *root, dom_node_t *n, dom_node_t *form) {
    for (dom_node_t *k = n->first; k; k = k->next) {
        dom_node_t *r;
        int kind;
        if (k->type != N_ELEMENT) continue;
        kind = form_kind(k);
        if ((kind == FK_SUBMIT || kind == FK_IMAGE) && !form_disabled(k) && form_owner(root, k) == form) return k;
        r = first_submit(root, k, form);
        if (r) return r;
    }
    return 0;
}

/* A user-initiated submit: scripts see a cancelable "submit" event first. */
static void request_submit(dom_node_t *form, dom_node_t *submitter) {
    if (!form) return;
    if (fire(form, "submit", 0, 0, 0, 0, 0)) return;
    submit_form(form, submitter);
}

static void implicit_submit(dom_node_t *field) {
    dom_node_t *form = form_owner(sf.doc->root, field);
    if (form) request_submit(form, first_submit(sf.doc->root, sf.doc->root, form));
}

static int control_box(dom_node_t *n, ic_rect_t *out, ic_app_t *app) {
    ic_rect_t p = page_rect(app);
    float x, y, w, h;
    if (!layout_box_rect(sf.L, n, &x, &y, &w, &h)) return 0;
    *out = ic_rect_make(p.x + (int)x, p.y + (int)(y - sf.scroll), (int)w, (int)h);
    return 1;
}

/* Caret position for a click at window x inside a single-line field. */
static void place_cursor(ic_app_t *app, dom_node_t *n, int click_x) {
    form_ctl_t *c = form_ctl(n);
    const css_style_t *st = n->style;
    ic_rect_t r;
    font_t f;
    float x;
    size_t i = 0;
    if (!c || !st || form_kind(n) == FK_TEXTAREA || !control_box(n, &r, app)) {
        if (c) c->cursor = c->anchor = c->len;
        return;
    }
    f = font_pick(st->font_family, st->font_weight, 0, st->font_size);
    x = (float)r.x + st->border_w[3] + (st->padding[3].unit == U_PX ? st->padding[3].v : 0) - c->scroll_x;
    while (i < c->len) {
        const char *p = c->value + i;
        uint32_t cp = utf8_next(&p, c->value + c->len);
        float w = form_kind(n) == FK_PASSWORD ? font_advance(&f, 0x2022) : font_advance(&f, cp);
        if ((float)click_x < x + w / 2) break;
        x += w;
        i = (size_t)(p - c->value);
    }
    c->cursor = c->anchor = i;
}

static void focus_changed(dom_node_t *old, dom_node_t *now);

static void set_focus(dom_node_t *n) {
    dom_node_t *old = sf.focus;
    sf.focus = n;
    sf.popup = 0;
    if (n) form_ctl(n);
    focus_changed(old, n);
}

/* Keeps the focused control on screen (Tab can move far away). */
static void reveal(ic_app_t *app, dom_node_t *n) {
    ic_rect_t p = page_rect(app);
    float x, y, w, h;
    if (!sf.L || !layout_box_rect(sf.L, n, &x, &y, &w, &h)) return;
    if (y < sf.scroll + 8) sf.scroll = y - 8;
    else if (y + h > sf.scroll + (float)p.h - 8) sf.scroll = y + h - (float)p.h + 8;
    if (sf.scroll < 0) sf.scroll = 0;
}

static void collect_focusable(dom_node_t *n, dom_node_t **out, int *count, int cap) {
    for (dom_node_t *k = n->first; k && *count < cap; k = k->next) {
        if (k->type != N_ELEMENT || !k->style || k->style->display == D_NONE) continue;
        if (form_is_focusable(k) || (k->tag == T_A && dom_attr(k, "href"))) out[(*count)++] = k;
        if (k->tag != T_SELECT) collect_focusable(k, out, count, cap);
    }
}

static void focus_step(ic_app_t *app, int dir) {
    static dom_node_t *list[2048];
    int count = 0, at = -1;
    if (!sf.doc) return;
    collect_focusable(sf.doc->root, list, &count, 2048);
    if (!count) return;
    for (int i = 0; i < count; i++) if (list[i] == sf.focus) at = i;
    at = at < 0 ? (dir > 0 ? 0 : count - 1) : (at + dir + count) % count;
    set_focus(list[at]);
    if (form_is_text(list[at])) form_select_all(list[at]);
    reveal(app, list[at]);
}

/* Activates a control or link the way a click or Enter/Space would. */
static void activate(ic_app_t *app, dom_node_t *n, int click_x) {
    int kind = form_kind(n);
    if (n->tag == T_A) {
        open_link(dom_attr(n, "href"));
        return;
    }
    if (form_disabled(n)) return;
    switch (kind) {
    case FK_TEXT: case FK_PASSWORD: case FK_TEXTAREA:
        set_focus(n);
        place_cursor(app, n, click_x);
        ic_app_caret_reset(app);
        break;
    case FK_CHECKBOX: case FK_RADIO:
        set_focus(n);
        form_toggle(sf.doc->root, n);
        fire(n, "input", 0, 0, 0, 0, 0);
        fire(n, "change", 0, 0, 0, 0, 0);
        break;
    case FK_SELECT:
        set_focus(n);
        sf.popup = n;
        sf.popup_hover = form_ctl(n)->selected;
        break;
    case FK_SUBMIT: case FK_IMAGE:
        set_focus(n);
        request_submit(form_owner(sf.doc->root, n), n);
        break;
    case FK_RESET:
        form_reset(sf.doc->root, form_owner(sf.doc->root, n));
        break;
    case FK_FILE:
        set_status("File uploads are not supported yet");
        break;
    default:
        set_focus(n);
        break;
    }
}

/* What a click at page element el means: the control itself, a control a
 * <label> points at, or the enclosing link. */
static dom_node_t *click_target(dom_node_t *el) {
    for (dom_node_t *p = el; p && p->type == N_ELEMENT; p = p->parent) {
        if (form_kind(p) != FK_NONE) return p;
        if (p->tag == T_LABEL) {
            const char *for_id = dom_attr(p, "for");
            if (for_id) {
                static dom_node_t *list[2048];
                int count = 0;
                collect_focusable(sf.doc->root, list, &count, 2048);
                for (int i = 0; i < count; i++) if (list[i]->id && strcmp(list[i]->id, for_id) == 0) return list[i];
            } else {
                static dom_node_t *list[64];
                int count = 0;
                collect_focusable(p, list, &count, 64);
                if (count) return list[0];
            }
        }
        if (p->tag == T_A && dom_attr(p, "href")) return p;
    }
    return 0;
}

/* <select> option list geometry, in window coordinates. */
#define POPUP_ROW 24
static int popup_rect(ic_app_t *app, ic_rect_t *out) {
    ic_rect_t r, p = page_rect(app);
    int n, h;
    if (!sf.popup || !control_box(sf.popup, &r, app)) return 0;
    n = form_option_count(sf.popup);
    h = n * POPUP_ROW + 8;
    if (h > p.h - 16) h = p.h - 16;
    out->w = r.w > 180 ? r.w : 180;
    out->h = h;
    out->x = r.x;
    out->y = r.y + r.h + h <= p.y + p.h ? r.y + r.h : r.y - h;
    if (out->y < p.y) out->y = p.y;
    if (out->x + out->w > p.x + p.w) out->x = p.x + p.w - out->w;
    return 1;
}

static int popup_hit(ic_app_t *app, int x, int y) {
    ic_rect_t r;
    if (!popup_rect(app, &r) || !ic_ui_hit(r, x, y)) return -2;
    {
        int first = form_ctl(sf.popup)->selected - (r.h - 8) / POPUP_ROW + 1;
        int i;
        if (first < 0) first = 0;
        i = first + (y - r.y - 4) / POPUP_ROW;
        return i < form_option_count(sf.popup) ? i : -1;
    }
}

static void draw_popup(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *pal = ic_palette();
    const ic_face_t *face = ic_font(IC_FONT_BODY);
    ic_rect_t r;
    int count, rows, first;
    if (!popup_rect(app, &r)) return;
    count = form_option_count(sf.popup);
    rows = (r.h - 8) / POPUP_ROW;
    first = form_ctl(sf.popup)->selected - rows + 1;
    if (first < 0) first = 0;
    ic_gfx_shadow(c, r.x, r.y, r.w, r.h, 8, 16, 4, 60);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 8, pal->content);
    for (int i = first; i < count && i < first + rows; i++) {
        char label[256];
        ic_rect_t row = ic_rect_make(r.x + 4, r.y + 4 + (i - first) * POPUP_ROW, r.w - 8, POPUP_ROW);
        dom_node_t *o = form_option(sf.popup, i);
        int disabled = dom_attr(o, "disabled") != 0;
        if (i == sf.popup_hover && !disabled) ic_gfx_rrect(c, row.x, row.y, row.w, row.h, 5, pal->accent);
        form_option_label(o, label, sizeof label);
        ic_text_draw_in(c, face, ic_rect_make(row.x + 8, row.y, row.w - 16, row.h), label,
                        disabled ? pal->label_tertiary : i == sf.popup_hover ? 0xFFFFFFFFu : pal->label, IC_ALIGN_LEFT);
    }
}

static void copy_selection(dom_node_t *n, int cut) {
    char buf[8192];
    size_t len = form_selection(n, buf, sizeof buf);
    if (form_kind(n) == FK_PASSWORD) return;
    if (len) ic_clipboard_set(buf, len);
    if (cut) {
        form_ctl_t *c = form_ctl(n);
        if (c->anchor != c->cursor) form_backspace(n);
    }
}

/* Keys for the focused page control.  Returns 1 if the key was used. */
static int page_key(ic_app_t *app, const ic_event_t *ev) {
    dom_node_t *n = sf.focus;
    int kind = form_kind(n), shift = (ev->mods & IC_MOD_SHIFT) != 0, ctrl = (ev->mods & IC_MOD_CTRL) != 0;
    if (!n) return 0;
    if (sf.popup) {
        int count = form_option_count(sf.popup);
        switch (ev->key) {
        case IC_KEY_UP: if (sf.popup_hover > 0) sf.popup_hover--; return 1;
        case IC_KEY_DOWN: if (sf.popup_hover + 1 < count) sf.popup_hover++; return 1;
        case IC_KEY_ENTER: case ' ':
            {
                dom_node_t *sel = sf.popup;
                form_choose(sel, sf.popup_hover);
                sf.popup = 0;
                fire(sel, "input", 0, 0, 0, 0, 0);
                fire(sel, "change", 0, 0, 0, 0, 0);
            }
            return 1;
        case IC_KEY_ESCAPE: sf.popup = 0; return 1;
        default: return 1;
        }
    }
    if (ev->key == IC_KEY_TAB) { focus_step(app, shift ? -1 : 1); return 1; }
    if (ev->key == IC_KEY_ESCAPE) { set_focus(0); return 1; }
    if (kind == FK_TEXT || kind == FK_PASSWORD || kind == FK_TEXTAREA) {
        ic_app_caret_reset(app);
        if (ctrl) {
            switch (ev->key) {
            case 'a': case 'A': form_select_all(n); return 1;
            case 'c': case 'C': copy_selection(n, 0); return 1;
            case 'x': case 'X': copy_selection(n, 1); return 1;
            case 'v': case 'V': {
                char clip[8192];
                long len = ic_clipboard_get(clip, sizeof clip);
                if (len > 0) form_insert(n, clip, (size_t)len);
                return 1;
            }
            default: return 0;
            }
        }
        switch (ev->key) {
        case IC_KEY_ENTER:
            if (kind == FK_TEXTAREA) form_insert(n, "\n", 1);
            else implicit_submit(n);
            return 1;
        case IC_KEY_BACKSPACE: form_backspace(n); return 1;
        case IC_KEY_DELETE: form_delete(n); return 1;
        case IC_KEY_LEFT: form_move(n, -1, shift); return 1;
        case IC_KEY_RIGHT: form_move(n, 1, shift); return 1;
        case IC_KEY_HOME: form_home_end(n, 0, shift); return 1;
        case IC_KEY_END: form_home_end(n, 1, shift); return 1;
        case IC_KEY_UP: case IC_KEY_DOWN:
            if (kind == FK_TEXTAREA) return 1;
            return 0;
        default:
            if (ev->key >= 32 && ev->key < 127 && !(ev->mods & IC_MOD_ALT)) {
                char ch = (char)ev->key;
                form_insert(n, &ch, 1);
                return 1;
            }
            return 0;
        }
    }
    switch (kind) {
    case FK_CHECKBOX: case FK_RADIO:
        if (ev->key == ' ') {
            form_toggle(sf.doc->root, n);
            fire(n, "input", 0, 0, 0, 0, 0);
            fire(n, "change", 0, 0, 0, 0, 0);
            return 1;
        }
        if (ev->key == IC_KEY_ENTER) { implicit_submit(n); return 1; }
        return 0;
    case FK_SELECT:
        if (ev->key == IC_KEY_UP || ev->key == IC_KEY_DOWN) {
            form_ctl_t *c = form_ctl(n);
            int next = c->selected + (ev->key == IC_KEY_UP ? -1 : 1);
            form_choose(n, next);
            fire(n, "input", 0, 0, 0, 0, 0);
            fire(n, "change", 0, 0, 0, 0, 0);
            return 1;
        }
        if (ev->key == ' ' || ev->key == IC_KEY_ENTER) { activate(app, n, 0); return 1; }
        return 0;
    default:
        if (ev->key == IC_KEY_ENTER || (ev->key == ' ' && n->tag != T_A)) { activate(app, n, 0); return 1; }
        return 0;
    }
}

/* ---- scripting ---------------------------------------------------------- */

/* Brings styles and layout up to date after scripts changed the page. */
static void restyle(ic_app_t *app) {
    ic_rect_t p = page_rect(app);
    int d = sf.js_dirty | js_take_dirty(sf.js);
    sf.js_dirty = 0;
    if (!sf.doc || !d) return;
    if (d & JS_DIRTY_STYLE) {
        for (int i = 0; i < sf.nsheets; i++) css_sheet_free(sf.sheets[i]);
        sf.nsheets = 0;
        gather_sheets(sf.doc->root, p.w);
    }
    css_cascade(sf.doc, sf.sheets, sf.nsheets, p.w, p.h);
    relayout(app);
    sf.last_restyle_ms = now_ms();
    if (sf.focus && !sf.focus->style) sf.focus = 0;   /* hidden or removed */
    ic_app_invalidate(app);
}

static int h_fetch(void *ctx, const char *url, char **body, size_t *len, char *final_url, size_t cap) {
    http_req_t *r;
    char saved[sizeof sf.status];
    (void)ctx;
    snprintf(saved, sizeof saved, "%s", sf.status);
    set_status("Loading scripts...");
    r = http_get(url, final_url, cap);
    set_status(saved);
    if (!r || r->state != HTTP_DONE || r->status != 200) {
        http_free(r);
        return -1;
    }
    *body = (char *)malloc(r->body_len + 1);
    if (!*body) { http_free(r); return -1; }
    memcpy(*body, r->body ? (const char *)r->body : "", r->body_len);
    (*body)[r->body_len] = 0;
    *len = r->body_len;
    http_free(r);
    return 0;
}

static void h_navigate(void *ctx, const char *url, int replace) {
    (void)ctx;
    navigate(url, !replace);
}

static void h_set_url(void *ctx, const char *url) {
    (void)ctx;
    {
        char line[URL_CAP + 32];
        snprintf(line, sizeof line, "[surfer] url %s\n", url);
        icda_write_file("/dev/serial", line, strlen(line));
    }
    snprintf(sf.url, sizeof sf.url, "%s", url);
    if (!sf.addr_focused) set_address(sf.url);
    if (sf.hist_pos >= 0) snprintf(sf.history[sf.hist_pos], URL_CAP, "%s", sf.url);
}

static void h_history_go(void *ctx, int delta) {
    (void)ctx;
    if (delta < 0) go_back();
    else if (delta > 0) go_forward();
    else navigate(sf.url, 0);
}

static void focus_changed(dom_node_t *old, dom_node_t *now);

static void h_focus(void *ctx, dom_node_t *n) {
    dom_node_t *old = sf.focus;
    (void)ctx;
    if (n && !form_is_focusable(n) && n->tag != T_A && !dom_attr(n, "tabindex")) n = 0;
    sf.focus = n;
    if (n) form_ctl(n);
    focus_changed(old, n);
}

static dom_node_t *h_active(void *ctx) {
    (void)ctx;
    return sf.focus;
}

static void scroll_by(ic_app_t *app, float dy);

static void h_scroll_to(void *ctx, float x, float y) {
    (void)ctx; (void)x;
    sf.scroll = y;
    if (sf.app) scroll_by(sf.app, 0);
}

static float h_scroll_y(void *ctx) {
    (void)ctx;
    return sf.scroll;
}

static void h_viewport(void *ctx, int *w, int *h) {
    ic_rect_t p;
    (void)ctx;
    if (!sf.app) return;
    p = page_rect(sf.app);
    *w = p.w;
    *h = p.h;
}

static void h_status(void *ctx, const char *msg) {
    (void)ctx;
    set_status(msg);
}

static int h_box(void *ctx, dom_node_t *n, float *x, float *y, float *w, float *h) {
    (void)ctx;
    /* scripts measuring the page see the effect of their own changes */
    if (sf.app && sf.js) {
        sf.js_dirty |= js_take_dirty(sf.js);
        if (sf.js_dirty) restyle(sf.app);
    }
    *x = *y = *w = *h = 0;
    return sf.L ? layout_box_rect(sf.L, n, x, y, w, h) : 0;
}

static void h_log(void *ctx, const char *line) {
    char buf[1100];
    size_t n;
    (void)ctx;
    n = (size_t)snprintf(buf, sizeof buf, "%s\n", line);
    if (n > sizeof buf - 1) n = sizeof buf - 1;
    icda_write_file("/dev/serial", buf, n);
}

static void h_submit(void *ctx, dom_node_t *form, dom_node_t *submitter) {
    (void)ctx;
    submit_form(form, submitter);
}

static void js_start(ic_app_t *app) {
    js_host_t host;
    uint32_t t0 = now_ms();
    if (!sf.doc) return;
    memset(&host, 0, sizeof host);
    host.fetch = h_fetch;
    host.navigate = h_navigate;
    host.set_url = h_set_url;
    host.history_go = h_history_go;
    host.focus = h_focus;
    host.active = h_active;
    host.scroll_to = h_scroll_to;
    host.scroll_y = h_scroll_y;
    host.viewport = h_viewport;
    host.status = h_status;
    host.box = h_box;
    host.log = h_log;
    host.submit = h_submit;
    sf.app = app;
    set_status("Running scripts...");
    sf.js = js_page_new(sf.doc, sf.url, &host);
    if (!sf.js) return;
    js_run_scripts(sf.js);
    restyle(app);
    {
        char line[160];
        snprintf(line, sizeof line, "[surfer] scripts %u ms height=%d\n", (unsigned)(now_ms() - t0), sf.L ? (int)sf.L->height : -1);
        icda_write_file("/dev/serial", line, strlen(line));
    }
}

/* Fires a DOM event; returns 1 if a script cancelled the default action. */
static int fire(dom_node_t *target, const char *type, int x, int y, int button, int mods, uint32_t key) {
    js_event_t ev;
    int prevented;
    if (!sf.js || !target) return 0;
    ev.x = x;
    ev.y = y;
    ev.client_x = x;
    ev.client_y = (int)((float)y - sf.scroll);
    ev.button = button;
    ev.mods = mods;
    ev.key = key;
    prevented = js_dispatch(sf.js, target, type, &ev);
    sf.js_dirty |= js_take_dirty(sf.js);
    return prevented;
}

static void focus_changed(dom_node_t *old, dom_node_t *now) {
    if (old == now) return;
    if (old) {
        /* a text field whose value changed while focused fires "change" */
        if (form_is_text(old) && sf.focus_value && strcmp(sf.focus_value, form_value(old)) != 0) fire(old, "change", 0, 0, 0, 0, 0);
        fire(old, "blur", 0, 0, 0, 0, 0);
        fire(old, "focusout", 0, 0, 0, 0, 0);
    }
    free(sf.focus_value);
    sf.focus_value = now && form_is_text(now) ? strdup(form_value(now)) : 0;
    if (now) {
        fire(now, "focus", 0, 0, 0, 0, 0);
        fire(now, "focusin", 0, 0, 0, 0, 0);
    }
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    int can_back = sf.hist_pos > 0, can_fwd = sf.hist_pos + 1 < sf.hist_count;
    ic_textfield_t tf = sf.addr;
    ic_ui_toolbar(c, toolbar_rect(app));
    ic_ui_icon_button(c, back_rect(app), IC_SYM_CHEVRON_LEFT,
                      !can_back ? IC_STATE_DISABLED : sf.hover_back ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, fwd_rect(app), IC_SYM_CHEVRON_RIGHT,
                      !can_fwd ? IC_STATE_DISABLED : sf.hover_fwd ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, reload_rect(app), IC_SYM_RELOAD,
                      sf.nav_pending ? IC_STATE_PRESSED : sf.hover_reload ? IC_STATE_HOVER : IC_STATE_NORMAL);
    tf.text = sf.addr_buf;
    tf.focused = sf.addr_focused;
    tf.caret_on = ic_app_caret_visible(app);
    tf.placeholder = "Search or enter an address";
    tf.leading = starts_with(sf.url, "https://") ? IC_SYM_GLOBE : IC_SYM_SEARCH;
    ic_ui_textfield(c, addr_rect(app), &tf);
}

static void draw_page(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t p = page_rect(app);
    ic_rect_t saved;
    ic_gfx_fill(c, p.x, p.y, p.w, p.h, 0xFFFFFFFFu);
    if (!sf.fonts_ok) {
        ic_ui_empty_state(c, p, IC_SYM_WARNING, "Fonts missing", "Surfer needs /usr/share/fonts/Inter-Regular.ttf");
        return;
    }
    if (sf.nav_pending && !sf.L) {
        ic_ui_empty_state(c, p, IC_SYM_RELOAD, "Loading", sf.nav_url);
        return;
    }
    if (!sf.L || !sf.doc) return;
    ic_canvas_push_clip(c, p.x, p.y, p.w, p.h, &saved);
    {
        int s = c->scale > 0 ? c->scale : 1;
        paint_target_t t;
        t.px = c->px;
        t.stride = c->w;
        t.x = p.x * s;
        t.y = p.y * s;
        t.w = p.w * s;
        t.h = p.h * s;
        t.scale = (float)s;
        t.scroll_y = sf.scroll;
        t.focus = sf.focus;
        t.caret_on = sf.focus && form_is_text(sf.focus) && !sf.addr_focused ? ic_app_caret_visible(app) : 0;
        paint_layout(sf.L, sf.doc, &t);
    }
    ic_canvas_pop_clip(c, &saved);
    if (sf.L->height > (float)p.h) ic_ui_scrollbar(c, p, (int)sf.scroll, (int)sf.L->height, 1.0f);
    if (sf.popup) draw_popup(app, c);
}

static void draw_status(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = status_rect(app);
    const ic_palette_t *pal = ic_palette();
    ic_ui_statusbar(c, s, sf.hover_url[0] ? sf.hover_url : sf.status);
    if (sf.doc && sf.doc->title[0] && !sf.hover_url[0]) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(s.x + s.w / 2, s.y, s.w / 2 - IC_SP_3, s.h),
                        sf.doc->title, pal->label_tertiary, IC_ALIGN_RIGHT);
    }
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t p = page_rect(app);
    if (sf.L && (int)sf.L->viewport_w != p.w) {
        relayout(app);
        if (sf.js && sf.doc) fire(sf.doc->root, "resize", 0, 0, 0, 0, 0);
    }
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_page(app, c);
    draw_status(app, c);
    if (sf.nav_pending) {
        sf.nav_drawn = 1;
        ic_app_animate(app);
    }
    if (sf.addr_focused || images_busy()) ic_app_animate(app);
}

static void tick(ic_app_t *app) {
    if (sf.nav_pending && sf.nav_drawn) {
        sf.nav_pending = 0;
        do_navigate(app);
        ic_app_invalidate(app);
        return;
    }
    if (sf.nimgs && images_pump()) sf.need_layout = 1;
    if (sf.need_layout && (!images_busy() || now_ms() - sf.last_layout_ms > 400)) {
        relayout(app);
        ic_app_invalidate(app);
    }
    if (images_busy()) ic_app_animate(app);
    if (sf.js) {
        int r = js_tick(sf.js);
        sf.js_dirty |= js_take_dirty(sf.js);
        (void)r;
        /* batch script changes: at most one restyle per 30 ms */
        if (sf.js_dirty && now_ms() - sf.last_restyle_ms >= 30) restyle(app);
    }
}

/* ---- input -------------------------------------------------------------- */

static void scroll_by(ic_app_t *app, float dy) {
    ic_rect_t p = page_rect(app);
    float max = sf.L ? sf.L->height - (float)p.h : 0, old = sf.scroll;
    sf.scroll += dy;
    if (sf.scroll > max) sf.scroll = max;
    if (sf.scroll < 0) sf.scroll = 0;
    if (sf.scroll != old && sf.js && sf.doc) fire(sf.doc->root, "scroll", 0, 0, 0, 0, 0);
}

static void addr_delete_selection(void) {
    int a = sf.addr.sel_start < sf.addr.sel_end ? sf.addr.sel_start : sf.addr.sel_end;
    int b = sf.addr.sel_start < sf.addr.sel_end ? sf.addr.sel_end : sf.addr.sel_start;
    int len = (int)strlen(sf.addr_buf);
    if (a == b || a < 0 || b > len) return;
    memmove(sf.addr_buf + a, sf.addr_buf + b, (size_t)(len - b + 1));
    sf.addr.cursor = a;
    sf.addr.sel_start = sf.addr.sel_end = a;
}

static void addr_submit(void) {
    char url[URL_CAP];
    sf.addr_focused = 0;
    normalize_input(sf.addr_buf, url, sizeof url);
    set_address(url);
    navigate(url, 1);
}

static void addr_key(ic_app_t *app, const ic_event_t *ev) {
    ic_rect_t a = addr_rect(app);
    int len = (int)strlen(sf.addr_buf);
    int has_sel = sf.addr.sel_start != sf.addr.sel_end;
    switch (ev->key) {
    case IC_KEY_ENTER: addr_submit(); return;
    case IC_KEY_ESCAPE: sf.addr_focused = 0; set_address(sf.url); return;
    case IC_KEY_LEFT: if (sf.addr.cursor > 0) sf.addr.cursor--; break;
    case IC_KEY_RIGHT: if (sf.addr.cursor < len) sf.addr.cursor++; break;
    case IC_KEY_HOME: sf.addr.cursor = 0; break;
    case IC_KEY_END: sf.addr.cursor = len; break;
    case IC_KEY_BACKSPACE:
        if (has_sel) { addr_delete_selection(); break; }
        if (sf.addr.cursor > 0) {
            memmove(sf.addr_buf + sf.addr.cursor - 1, sf.addr_buf + sf.addr.cursor, (size_t)(len - sf.addr.cursor + 1));
            sf.addr.cursor--;
        }
        break;
    case IC_KEY_DELETE:
        if (has_sel) { addr_delete_selection(); break; }
        if (sf.addr.cursor < len) memmove(sf.addr_buf + sf.addr.cursor, sf.addr_buf + sf.addr.cursor + 1, (size_t)(len - sf.addr.cursor));
        break;
    default:
        if ((ev->mods & IC_MOD_CTRL) && (ev->key == 'a' || ev->key == 'A')) {
            sf.addr.sel_start = 0;
            sf.addr.sel_end = sf.addr.cursor = len;
            ic_ui_textfield_scroll(a, &sf.addr);
            return;
        }
        if ((ev->mods & IC_MOD_CTRL) && (ev->key == 'v' || ev->key == 'V')) {
            char clip[URL_CAP];
            long n = ic_clipboard_get(clip, sizeof clip - 1);
            if (n <= 0) return;
            clip[n] = 0;
            addr_delete_selection();
            len = (int)strlen(sf.addr_buf);
            for (long i = 0; i < n && len + 1 < URL_CAP; i++) {
                if ((unsigned char)clip[i] < 32) continue;
                memmove(sf.addr_buf + sf.addr.cursor + 1, sf.addr_buf + sf.addr.cursor, (size_t)(len - sf.addr.cursor + 1));
                sf.addr_buf[sf.addr.cursor++] = clip[i];
                len++;
            }
            break;
        }
        if (ev->key >= 32 && ev->key < 127 && !(ev->mods & (IC_MOD_CTRL | IC_MOD_ALT))) {
            addr_delete_selection();
            len = (int)strlen(sf.addr_buf);
            if (len + 1 < URL_CAP) {
                memmove(sf.addr_buf + sf.addr.cursor + 1, sf.addr_buf + sf.addr.cursor, (size_t)(len - sf.addr.cursor + 1));
                sf.addr_buf[sf.addr.cursor++] = (char)ev->key;
            }
        }
        break;
    }
    sf.addr.sel_start = sf.addr.sel_end = sf.addr.cursor;
    ic_ui_textfield_scroll(a, &sf.addr);
    ic_app_caret_reset(app);
}

static dom_node_t *link_at(ic_app_t *app, int x, int y) {
    ic_rect_t p = page_rect(app);
    if (!sf.L || !ic_ui_hit(p, x, y)) return 0;
    return layout_hit_link(sf.L, (float)(x - p.x), (float)(y - p.y) + sf.scroll);
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    ic_rect_t a = addr_rect(app);
    ic_rect_t p = page_rect(app);
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE: {
        dom_node_t *ln = link_at(app, ev->x, ev->y);
        const char *href = ln ? dom_attr(ln, "href") : 0;
        char url[URL_CAP];
        sf.hover_back = ic_ui_hit(back_rect(app), ev->x, ev->y);
        sf.hover_fwd = ic_ui_hit(fwd_rect(app), ev->x, ev->y);
        sf.hover_reload = ic_ui_hit(reload_rect(app), ev->x, ev->y);
        if (sf.popup) {
            int h = popup_hit(app, ev->x, ev->y);
            if (h >= 0 && h != sf.popup_hover) { sf.popup_hover = h; ic_app_invalidate(app); }
        }
        {
            int text = ic_ui_hit(a, ev->x, ev->y);
            if (!text && sf.L && ic_ui_hit(p, ev->x, ev->y)) {
                dom_node_t *el = layout_hit_element(sf.L, (float)(ev->x - p.x), (float)(ev->y - p.y) + sf.scroll);
                text = el && form_is_text(el);
            }
            ic_app_set_cursor(app, text ? IC_CURSOR_TEXT : IC_CURSOR_ARROW);
        }
        if (href && sf.doc && url_resolve(sf.doc->base_url, href, url, sizeof url) == 0) {
            if (strcmp(url, sf.hover_url) == 0) return;
            snprintf(sf.hover_url, sizeof sf.hover_url, "%s", url);
        } else {
            if (!sf.hover_url[0] && !sf.hover_back && !sf.hover_fwd && !sf.hover_reload) return;
            sf.hover_url[0] = 0;
        }
        break;
    }
    case IC_EV_MOUSE_LEAVE:
        sf.hover_back = sf.hover_fwd = sf.hover_reload = 0;
        sf.hover_url[0] = 0;
        break;
    case IC_EV_MOUSE_DOWN:
        if (ev->button != GUI_BTN_LEFT) return;
        if (sf.hover_back) { go_back(); break; }
        if (sf.hover_fwd) { go_forward(); break; }
        if (sf.hover_reload) { if (sf.url[0]) navigate(sf.url, 0); break; }
        if (ic_ui_hit(a, ev->x, ev->y)) {
            if (!sf.addr_focused) {
                /* first click selects the whole address, like other browsers */
                sf.addr_focused = 1;
                sf.addr.sel_start = 0;
                sf.addr.sel_end = sf.addr.cursor = (int)strlen(sf.addr_buf);
            } else {
                sf.addr.cursor = ic_ui_textfield_index_at(a, &sf.addr, ev->x);
                sf.addr.sel_start = sf.addr.sel_end = sf.addr.cursor;
            }
            ic_ui_textfield_scroll(a, &sf.addr);
            ic_app_caret_reset(app);
            break;
        }
        sf.addr_focused = 0;
        if (sf.popup) {
            int i = popup_hit(app, ev->x, ev->y);
            if (i >= 0 && !dom_attr(form_option(sf.popup, i), "disabled")) {
                dom_node_t *sel = sf.popup;
                form_choose(sel, i);
                fire(sel, "input", 0, 0, 0, 0, 0);
                fire(sel, "change", 0, 0, 0, 0, 0);
            }
            if (i != -2 || !ic_ui_hit(p, ev->x, ev->y)) { sf.popup = 0; break; }
            sf.popup = 0;
        }
        if (ic_ui_hit(p, ev->x, ev->y) && sf.L) {
            int px = ev->x - p.x, py = ev->y - p.y + (int)sf.scroll;
            dom_node_t *el = layout_hit_element(sf.L, (float)px, (float)py);
            dom_node_t *target = el ? click_target(el) : 0;
            if (el) {
                /* scripts see the click first and may cancel the default action */
                int prevented = fire(el, "mousedown", px, py, 0, (int)ev->mods, 0);
                fire(el, "mouseup", px, py, 0, (int)ev->mods, 0);
                prevented |= fire(el, "click", px, py, 0, (int)ev->mods, 0);
                if (prevented) {
                    if (target && form_is_text(target)) activate(app, target, ev->x);
                    break;
                }
            }
            if (target) activate(app, target, ev->x);
            else set_focus(0);
        }
        break;
    case IC_EV_SCROLL:
        scroll_by(app, (float)(ev->wheel * WHEEL_STEP));
        break;
    case IC_EV_KEY:
        if ((ev->mods & IC_MOD_CTRL) && (ev->key == 'l' || ev->key == 'L')) {
            sf.addr_focused = 1;
            sf.addr.sel_start = 0;
            sf.addr.sel_end = sf.addr.cursor = (int)strlen(sf.addr_buf);
            break;
        }
        if ((ev->mods & IC_MOD_CTRL) && (ev->key == 'r' || ev->key == 'R')) { navigate(sf.url, 0); break; }
        if (ev->mods & IC_MOD_ALT) {
            if (ev->key == IC_KEY_LEFT) go_back();
            else if (ev->key == IC_KEY_RIGHT) go_forward();
            else if (ev->key == IC_KEY_HOME) navigate(HOME_URL, 1);
            break;
        }
        if (sf.addr_focused) { addr_key(app, ev); break; }
        if (sf.js && !sf.popup) {
            /* keydown can be cancelled; typing reports "input" when the value changed */
            dom_node_t *t = sf.focus ? sf.focus : sf.doc ? sf.doc->body : 0;
            char *before = sf.focus && form_is_text(sf.focus) ? strdup(form_value(sf.focus)) : 0;
            int prevented = fire(t, "keydown", 0, 0, 0, (int)ev->mods, ev->key);
            int used = 0;
            if (!prevented && ev->key >= 32 && ev->key < 127) prevented = fire(t, "keypress", 0, 0, 0, (int)ev->mods, ev->key);
            if (!prevented && sf.focus) used = page_key(app, ev);
            if (before && sf.focus && strcmp(before, form_value(sf.focus)) != 0) fire(sf.focus, "input", 0, 0, 0, 0, 0);
            free(before);
            fire(sf.focus ? sf.focus : t, "keyup", 0, 0, 0, (int)ev->mods, ev->key);
            if (prevented || used) break;
        } else if (sf.focus && page_key(app, ev)) {
            break;
        }
        if (ev->key == IC_KEY_TAB) { focus_step(app, (ev->mods & IC_MOD_SHIFT) ? -1 : 1); break; }
        switch (ev->key) {
        case IC_KEY_DOWN: scroll_by(app, 40); break;
        case IC_KEY_UP: scroll_by(app, -40); break;
        case IC_KEY_PAGE_DOWN: case ' ': scroll_by(app, (float)p.h - 48); break;
        case IC_KEY_PAGE_UP: scroll_by(app, -((float)p.h - 48)); break;
        case IC_KEY_HOME: sf.scroll = 0; break;
        case IC_KEY_END: scroll_by(app, 1e9f); break;
        case IC_KEY_BACKSPACE: go_back(); break;
        default: return;
        }
        break;
    case IC_EV_BLUR:
        sf.addr_focused = 0;
        break;
    case IC_EV_RESIZE:
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    const char *arg = (const char *)app->user;
    char url[URL_CAP];
    sf.hist_pos = -1;
    sf.addr.text = sf.addr_buf;
    sf.fonts_ok = font_init() == 0;
    normalize_input(arg && arg[0] ? arg : HOME_URL, url, sizeof url);
    set_address(url);
    navigate(url, 1);
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Surfer", WIN_W, WIN_H, init, draw, event, tick };
    const char *arg = argc > 1 && argv ? argv[1] : 0;
    if (ic_app_run(&desc, (void *)arg) != 0) {
        icda_write("surfer requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
