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
    images_clear();
    if (sf.L) layout_free(sf.L);
    sf.L = 0;
    for (int i = 0; i < sf.nsheets; i++) css_sheet_free(sf.sheets[i]);
    sf.nsheets = 0;
    if (sf.doc) dom_free(sf.doc);
    sf.doc = 0;
    if (sf.page_req) http_free(sf.page_req);
    sf.page_req = 0;
    free(sf.page_src);
    sf.page_src = 0;
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
        http_req_t *r;
        if (url_resolve(base, imp[i], url, sizeof url) != 0) continue;
        r = http_get(url, fin, sizeof fin);
        if (r && r->state == HTTP_DONE && r->status == 200 && r->body) add_sheet((const char *)r->body, r->body_len, fin, vw, depth + 1);
        http_free(r);
    }
    if (sf.nsheets < SHEETS_CAP) sf.sheets[sf.nsheets++] = s;
    else css_sheet_free(s);
}

static void gather_sheets(dom_node_t *n, int vw) {
    for (dom_node_t *c = n->first; c; c = c->next) {
        if (c->type != N_ELEMENT) continue;
        if (c->tag == T_STYLE && c->first) {
            add_sheet(c->first->text, c->first->text_len, sf.doc->base_url, vw, 0);
        } else if (c->tag == T_LINK) {
            const char *rel = dom_attr(c, "rel"), *href = dom_attr(c, "href"), *media = dom_attr(c, "media");
            if (rel && strstr(rel, "stylesheet") && !strstr(rel, "alternate") && href && !(media && strstr(media, "print"))) {
                char url[URL_CAP], fin[URL_CAP];
                http_req_t *r;
                set_status("Loading styles...");
                if (url_resolve(sf.doc->base_url, href, url, sizeof url) != 0) continue;
                r = http_get(url, fin, sizeof fin);
                if (r && r->state == HTTP_DONE && r->status == 200 && r->body) add_sheet((const char *)r->body, r->body_len, fin, vw, 0);
                http_free(r);
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
        http_req_t *r = http_get(sf.nav_url, final_url, sizeof final_url);
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
        paint_layout(sf.L, sf.doc, &t);
    }
    ic_canvas_pop_clip(c, &saved);
    if (sf.L->height > (float)p.h) ic_ui_scrollbar(c, p, (int)sf.scroll, (int)sf.L->height, 1.0f);
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
    if (sf.L && (int)sf.L->viewport_w != p.w) relayout(app);
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
}

/* ---- input -------------------------------------------------------------- */

static void scroll_by(ic_app_t *app, float dy) {
    ic_rect_t p = page_rect(app);
    float max = sf.L ? sf.L->height - (float)p.h : 0;
    sf.scroll += dy;
    if (sf.scroll > max) sf.scroll = max;
    if (sf.scroll < 0) sf.scroll = 0;
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
        ic_app_set_cursor(app, ic_ui_hit(a, ev->x, ev->y) ? IC_CURSOR_TEXT : IC_CURSOR_ARROW);
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
        if (ic_ui_hit(p, ev->x, ev->y)) {
            dom_node_t *ln = link_at(app, ev->x, ev->y);
            char line[URL_CAP + 64];
            snprintf(line, sizeof line, "[surfer] click %d,%d -> %s\n", ev->x - p.x, ev->y - p.y + (int)sf.scroll,
                     ln && dom_attr(ln, "href") ? dom_attr(ln, "href") : "(none)");
            icda_write_file("/dev/serial", line, strlen(line));
            if (ln) open_link(dom_attr(ln, "href"));
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
