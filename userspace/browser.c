/*
 * browser.c - ICDA web browser.
 *
 * A native GUI browser that fetches pages over HTTP/HTTPS using the
 * kernel's network stack and renders basic HTML (headings, paragraphs,
 * links, lists) into a scrollable view.  The address bar accepts
 * http:// and https:// URLs; Enter navigates, Back/Forward walk the
 * history, and links are clickable.
 */
#include "libicda.h"
#include "gui_proto.h"

/* Forward declarations */
static void draw_all(void);

#define BROWSER_URL_CAP   256
#define BROWSER_HTML_CAP  (1024 * 1024)
#define BROWSER_TEXT_CAP  (1024 * 1024)
#define BROWSER_TITLE_CAP 64
#define BROWSER_HISTORY   16
#define BROWSER_LINKS     256
#define BROWSER_LINE_CAP  512
#define FONT_W            8

#define ADDR_BAR_H  34
#define TOOLBAR_H   30
#define PAD         6

/* Links are byte ranges in the processed render text; the clickable
 * rectangle is computed at draw time from the monospace layout. */
typedef struct {
    uint64_t start;
    uint64_t end;
    char url[BROWSER_URL_CAP];
} browser_link_t;

static char current_url[BROWSER_URL_CAP];
static char address_buf[BROWSER_URL_CAP];
static char page_title[BROWSER_TITLE_CAP];
static char *html_buf = NULL;
static uint64_t html_len = 0;
static char *text_buf = NULL;
static uint64_t text_len = 0;
static uint64_t html_shm = 0;
static uint64_t text_shm = 0;

static char history[BROWSER_HISTORY][BROWSER_URL_CAP];
static int history_count = 0;
static int history_pos = -1;

static browser_link_t links[BROWSER_LINKS];
static int link_count = 0;

static int scroll_y = 0;
static int max_scroll = 0;
static int hover_link = -1;
static int loading = 0;
static char status[64];

static int addr_cursor = 0;
static int addr_active = 0;

/* ---- string helpers (freestanding) ---- */

static uint64_t b_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int b_streq(const char *a, const char *b) {
    uint64_t i = 0;
    if (!a || !b) return a == b;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static int b_strprefix(const char *s, const char *prefix) {
    uint64_t i = 0;
    if (!s || !prefix) return 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static void b_strcpy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void b_strcat(char *dst, const char *src, uint64_t cap) {
    uint64_t at = b_strlen(dst);
    uint64_t i = 0;
    if (!dst || cap == 0 || at >= cap) return;
    while (src && src[i] && at + 1 < cap) {
        dst[at++] = src[i++];
    }
    dst[at] = 0;
}

static char b_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static void b_uint_to_str(uint64_t v, char *out, uint64_t cap) {
    char tmp[32];
    uint64_t len = 0;
    uint64_t i = 0;
    if (!out || cap == 0) return;
    if (v == 0) {
        b_strcpy(out, "0", cap);
        return;
    }
    while (v && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (len && i + 1 < cap) {
        out[i++] = tmp[--len];
    }
    out[i] = 0;
}

/* ---- URL parsing ---- */

/*
 * parse_url_format: parse URL into scheme/host/port/path without DNS.
 * Returns 0 on success, -1 on format error.
 */
static int parse_url_format(const char *url, uint16_t *port_out,
                            int *https_out, char *host_out, uint64_t host_cap,
                            char *path_out, uint64_t path_cap) {
    const char *host;
    uint64_t host_len = 0;
    uint64_t path_len = 0;
    uint16_t port = 80;
    int use_https = 0;

    if (!url || !*url || !port_out || !https_out) return -1;
    if (b_strprefix(url, "http://")) {
        host = url + 7;
    } else if (b_strprefix(url, "https://")) {
        host = url + 8;
        port = 443;
        use_https = 1;
    } else {
        return -1;
    }

    while (host[host_len] && host[host_len] != ':' && host[host_len] != '/') {
        if (host_len + 1 >= host_cap) return -1;
        host_out[host_len] = host[host_len];
        host_len++;
    }
    if (host_len == 0) return -1;
    host_out[host_len] = 0;
    host += host_len;

    if (*host == ':') {
        uint32_t port_value = 0;
        host++;
        if (*host < '0' || *host > '9') return -1;
        while (*host >= '0' && *host <= '9') {
            port_value = port_value * 10U + (uint32_t)(*host - '0');
            if (port_value > 65535U) return -1;
            host++;
        }
        port = (uint16_t)port_value;
    }

    if (*host == 0) {
        b_strcpy(path_out, "/", path_cap);
    } else {
        if (*host != '/') return -1;
        while (host[path_len] && path_len + 1 < path_cap) {
            path_out[path_len] = host[path_len];
            path_len++;
        }
        path_out[path_len] = 0;
    }

    *port_out = port;
    *https_out = use_https;
    return 0;
}

/*
 * resolve_host: resolve hostname to IPv4. Returns 0 on success.
 * Tries IPv4 literal first, then DNS.
 */
static int resolve_host(const char *host, uint32_t *ip_out) {
    int is_ip = 1;
    const char *p = host;
    int dots = 0;
    while (*p) {
        if (*p == '.') dots++;
        else if (*p < '0' || *p > '9') { is_ip = 0; break; }
        p++;
    }
    if (is_ip && dots == 3) {
        uint32_t octets[4];
        uint64_t i = 0;
        for (int o = 0; o < 4; o++) {
            uint32_t v = 0;
            uint64_t start = i;
            while (host[i] >= '0' && host[i] <= '9') {
                v = v * 10U + (uint32_t)(host[i] - '0');
                if (v > 255U) { is_ip = 0; break; }
                i++;
            }
            if (i == start || (o < 3 && host[i] != '.')) { is_ip = 0; break; }
            if (o < 3) i++;
            octets[o] = v;
        }
        if (is_ip && host[i] == 0) {
            *ip_out = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
            return 0;
        }
    }
    /* DNS resolution */
    long rc = (long)icda_dns_resolve(host, ip_out);
    return (int)rc;
}

/* ---- HTML rendering ---- */

static void strip_tags(char *dst, uint64_t dst_cap, const char *src) {
    uint64_t di = 0;
    int in_tag = 0;
    int in_entity = 0;
    if (!dst || dst_cap == 0) return;
    while (*src && di + 1 < dst_cap) {
        if (*src == '<') {
            in_tag = 1;
            src++;
            continue;
        }
        if (*src == '>') {
            in_tag = 0;
            src++;
            continue;
        }
        if (in_tag) {
            src++;
            continue;
        }
        if (*src == '&') {
            in_entity = 1;
            src++;
            continue;
        }
        if (in_entity) {
            if (*src == ';') in_entity = 0;
            src++;
            continue;
        }
        dst[di++] = *src;
        src++;
    }
    dst[di] = 0;
}

static void decode_entities(char *dst, uint64_t dst_cap, const char *src) {
    uint64_t di = 0;
    /* Entity strings built char-by-char so the source survives any
     * HTML-escaping in the editor/toolchain. */
    static const char ent_amp[]  = { '&', 'a', 'm', 'p', ';', 0 };
    static const char ent_lt[]   = { '&', 'l', 't', ';', 0 };
    static const char ent_gt[]   = { '&', 'g', 't', ';', 0 };
    static const char ent_quot[] = { '&', 'q', 'u', 'o', 't', ';', 0 };
    static const char ent_nbsp[] = { '&', 'n', 'b', 's', 'p', ';', 0 };
    static const char ent_num[]  = { '&', '#', 0 };
    if (!dst || dst_cap == 0) return;
    while (*src && di + 1 < dst_cap) {
        if (*src == '&' && b_strprefix(src, ent_amp)) {
            dst[di++] = '&';
            src += 5;
        } else if (*src == '&' && b_strprefix(src, ent_lt)) {
            dst[di++] = '<';
            src += 4;
        } else if (*src == '&' && b_strprefix(src, ent_gt)) {
            dst[di++] = '>';
            src += 4;
        } else if (*src == '&' && b_strprefix(src, ent_quot)) {
            dst[di++] = '"';
            src += 6;
        } else if (*src == '&' && b_strprefix(src, ent_nbsp)) {
            dst[di++] = ' ';
            src += 6;
        } else if (*src == '&' && b_strprefix(src, ent_num)) {
            /* Skip numeric entities */
            src += 2;
            while (*src && *src != ';') src++;
            if (*src == ';') src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = 0;
}

static void extract_title(const char *html, char *title, uint64_t cap) {
    const char *p = html;
    const char *t_start = 0;
    const char *t_end = 0;
    if (!html || !title || cap == 0) return;
    b_strcpy(title, "ICDA Browser", cap);
    while (*p) {
        if (b_lower(p[0]) == '<' && b_lower(p[1]) == 't' &&
            b_lower(p[2]) == 'i' && b_lower(p[3]) == 't' &&
            b_lower(p[4]) == 'l' && b_lower(p[5]) == 'e') {
            p += 6;
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            t_start = p;
            while (*p && !(*p == '<' && b_lower(p[1]) == '/')) p++;
            t_end = p;
            break;
        }
        p++;
    }
    if (t_start && t_end && t_end > t_start) {
        char tmp[128];
        uint64_t len = (uint64_t)(t_end - t_start);
        if (len > sizeof(tmp) - 1) len = sizeof(tmp) - 1;
        for (uint64_t i = 0; i < len; i++) tmp[i] = t_start[i];
        tmp[len] = 0;
        strip_tags(title, cap, tmp);
        decode_entities(title, cap, title);
    }
}

/* ---- HTML -> plain text extraction ----
 *
 * Modern pages (React/Next/etc.) are dominated by <script>, <style>,
 * <svg> blobs and minified markup with no newlines.  This pipeline
 * produces a readable, word-wrapped text view: invisible subtrees are
 * dropped, entities and common UTF-8 punctuation are mapped to ASCII,
 * block-level tags force line breaks, and <a href> anchors are tracked
 * as byte ranges so the draw pass can highlight and hit-test them.
 */

static uint64_t rt_i;            /* write index into text_buf */
static uint64_t rt_line_start;   /* index where the current visual line began */
static uint64_t rt_last_space;   /* offset of the last emitted space (wrap point) */
static int rt_cols;              /* wrap width in characters */

static int match_word(const char *p, const char *w) {
    uint64_t i = 0;
    while (w[i]) {
        if (b_lower(p[i]) != w[i]) return 0;
        i++;
    }
    {
        char c = p[i];
        return c == 0 || c == '>' || c == '/' || c == ' ' ||
               c == '\t' || c == '\n' || c == '\r';
    }
}

/* Tags whose whole subtree carries no displayable text. */
static int tag_skips_content(const char *t) {
    return match_word(t, "script") || match_word(t, "style") ||
           match_word(t, "svg") || match_word(t, "head") ||
           match_word(t, "iframe") || match_word(t, "template") ||
           match_word(t, "canvas") || match_word(t, "video") ||
           match_word(t, "audio") || match_word(t, "object") ||
           match_word(t, "select") || match_word(t, "button") ||
           match_word(t, "input") || match_word(t, "textarea") ||
           match_word(t, "link") || match_word(t, "meta") ||
           match_word(t, "title") || match_word(t, "!doctype");
}

/* Tags that introduce a line break in the text view. */
static int tag_is_block(const char *t) {
    if (b_lower(t[0]) == 'h' && t[1] >= '1' && t[1] <= '6') {
        char c = t[2];
        return c == 0 || c == '>' || c == '/' || c == ' ';
    }
    return match_word(t, "p") || match_word(t, "div") ||
           match_word(t, "br") || match_word(t, "li") ||
           match_word(t, "ul") || match_word(t, "ol") ||
           match_word(t, "tr") || match_word(t, "td") ||
           match_word(t, "th") || match_word(t, "table") ||
           match_word(t, "hr") || match_word(t, "form") ||
           match_word(t, "section") || match_word(t, "article") ||
           match_word(t, "header") || match_word(t, "footer") ||
           match_word(t, "nav") || match_word(t, "main") ||
           match_word(t, "aside") || match_word(t, "blockquote") ||
           match_word(t, "pre") || match_word(t, "figure") ||
           match_word(t, "figcaption") || match_word(t, "dl") ||
           match_word(t, "dt") || match_word(t, "dd");
}

static void rt_newline(void) {
    if (rt_i == 0 || rt_i + 1 >= BROWSER_TEXT_CAP) return;
    if (text_buf[rt_i - 1] == '\n') return; /* collapse blank lines */
    text_buf[rt_i++] = '\n';
    rt_line_start = rt_i;
    rt_last_space = (uint64_t)-1;
}

static void rt_put(char c) {
    if (rt_i + 1 >= BROWSER_TEXT_CAP) return;
    text_buf[rt_i++] = c;
    /* Word wrap: rewind the current line at the last space. */
    if (rt_i - rt_line_start > (uint64_t)rt_cols &&
        rt_last_space != (uint64_t)-1 && rt_last_space >= rt_line_start) {
        text_buf[rt_last_space] = '\n';
        rt_line_start = rt_last_space + 1;
        rt_last_space = (uint64_t)-1;
    }
}

static void rt_space(void) {
    if (rt_i == 0 || rt_i + 1 >= BROWSER_TEXT_CAP) return;
    if (text_buf[rt_i - 1] == ' ' || text_buf[rt_i - 1] == '\n') return;
    rt_last_space = rt_i;
    rt_put(' ');
}

/* Map a Unicode codepoint to something the 8x16 font can show. */
static void rt_emit_cp(uint32_t cp) {
    char c;
    switch (cp) {
        case 0x2013: case 0x2014: c = '-'; break;   /* en/em dash */
        case 0x2018: case 0x2019: c = '\''; break;  /* quotes */
        case 0x201C: case 0x201D: c = '"'; break;
        case 0x2026: c = '.'; break;                /* ellipsis */
        case 0x00A0: c = ' '; break;                /* nbsp */
        default:
            if (cp >= 0x2000 && cp <= 0x206F) return; /* invisible format chars */
            if (cp >= 0x80) return;                  /* no glyph available */
            c = (char)cp;
    }
    if (c == ' ') rt_space(); else rt_put(c);
}

/* Decode one UTF-8 sequence; returns the codepoint and advance count. */
static uint32_t rt_utf8(const char *p, uint64_t i, uint64_t len, uint64_t *adv) {
    unsigned char c = (unsigned char)p[i];
    uint32_t cp;
    int extra;
    if (c < 0x80) { *adv = 1; return c; }
    if (c < 0xC0) { *adv = 1; return 0; }           /* stray continuation */
    if (c < 0xE0) { cp = c & 0x1F; extra = 1; }
    else if (c < 0xF0) { cp = c & 0x0F; extra = 2; }
    else { cp = c & 0x07; extra = 3; }
    for (int k = 0; k < extra; k++) {
        if (i + 1 + (uint64_t)k >= len) { *adv = 1 + (uint64_t)k; return 0; }
        unsigned char cc = (unsigned char)p[i + 1 + (uint64_t)k];
        if ((cc & 0xC0) != 0x80) { *adv = 1 + (uint64_t)k; return 0; }
        cp = (cp << 6) | (uint32_t)(cc & 0x3F);
    }
    *adv = 1 + (uint64_t)extra;
    return cp;
}

/* Copy a tag attribute value (e.g. href) out of a tag's source range. */
static void extract_attr(const char *p, uint64_t ts, uint64_t te,
                         const char *name, char *out, uint64_t cap) {
    uint64_t nlen = b_strlen(name);
    uint64_t i = ts;
    if (cap) out[0] = 0;
    while (i + nlen + 1 < te) {
        if (b_lower(p[i]) == b_lower(name[0])) {
            uint64_t k = 0;
            while (k < nlen && b_lower(p[i + k]) == b_lower(name[k])) k++;
            if (k == nlen) {
                uint64_t j = i + nlen;
                while (j < te && (p[j] == ' ' || p[j] == '=')) j++;
                if (j < te) {
                    uint64_t v0, v1;
                    if (p[j] == '"' || p[j] == '\'') {
                        char q = p[j++];
                        v0 = j;
                        while (j < te && p[j] != q) j++;
                        v1 = j;
                    } else {
                        v0 = j;
                        while (j < te && p[j] != ' ' && p[j] != '>') j++;
                        v1 = j;
                    }
                    uint64_t n = v1 > v0 ? v1 - v0 : 0;
                    if (n >= cap) n = cap - 1;
                    for (uint64_t b = 0; b < n; b++) out[b] = p[v0 + b];
                    out[n] = 0;
                    return;
                }
            }
        }
        i++;
    }
}

/* Turn a raw href into an absolute URL against the current page. */
static void resolve_href(const char *href, char *out, uint64_t cap) {
    uint16_t port;
    int https;
    char host[128];
    char path[256];

    if (cap) out[0] = 0;
    if (!href || !*href || cap == 0) return;
    if (b_strprefix(href, "http://") || b_strprefix(href, "https://")) {
        b_strcpy(out, href, cap);
        return;
    }
    if (parse_url_format(current_url, &port, &https, host, sizeof(host),
                         path, sizeof(path)) < 0) {
        b_strcpy(out, href, cap);
        return;
    }

    if (b_strprefix(href, "//")) {
        b_strcpy(out, https ? "https:" : "http:", cap);
        b_strcat(out, href, cap);
        return;
    }
    b_strcpy(out, https ? "https://" : "http://", cap);
    b_strcat(out, host, cap);
    if (href[0] == '/') {
        b_strcat(out, href, cap);
        return;
    }
    {
        /* Relative to the current path's directory. */
        uint64_t plen = b_strlen(path);
        while (plen > 0 && path[plen - 1] != '/') plen--;
        if (plen > 1) {
            path[plen] = 0;
            b_strcat(out, path, cap);
        } else {
            b_strcat(out, "/", cap);
        }
        b_strcat(out, href, cap);
    }
}

/* Build the word-wrapped plain-text view of the fetched page. */
static void build_render_text(void) {
    const char *p = html_buf ? html_buf : "";
    uint64_t len = html_len;
    uint64_t i = 0;
    int skip_depth = 0;
    int cur_link = -1;

    if (!text_buf) return;
    rt_i = 0;
    rt_line_start = 0;
    rt_last_space = (uint64_t)-1;
    rt_cols = (gui_window_width() - 2 * PAD) / FONT_W;
    if (rt_cols < 20) rt_cols = 20;
    link_count = 0;

    while (i < len && rt_i + 1 < BROWSER_TEXT_CAP) {
        char c = p[i];

        /* Tags (quoted '>' inside attributes is handled) */
        if (c == '<' && i + 1 < len &&
            ((p[i + 1] >= 'a' && p[i + 1] <= 'z') ||
             (p[i + 1] >= 'A' && p[i + 1] <= 'Z') ||
             p[i + 1] == '/' || p[i + 1] == '!')) {
            /* HTML comment */
            if (p[i + 1] == '!' && i + 3 < len && p[i + 2] == '-' && p[i + 3] == '-') {
                i += 4;
                while (i + 2 < len && !(p[i] == '-' && p[i + 1] == '-' && p[i + 2] == '>')) i++;
                i += 3;
                if (i > len) i = len;
                continue;
            }
            {
                uint64_t ts = i + 1;
                uint64_t te = i + 1;
                int closing = 0;
                char quote = 0;
                if (ts < len && p[ts] == '/') { closing = 1; ts++; }
                while (te < len) {
                    char t = p[te];
                    if (quote) { if (t == quote) quote = 0; }
                    else if (t == '"' || t == '\'') quote = t;
                    else if (t == '>') break;
                    te++;
                }
                {
                    const char *tn = (ts < len) ? p + ts : "";
                    if (tag_skips_content(tn)) {
                        if (closing) { if (skip_depth > 0) skip_depth--; }
                        else if (!match_word(tn, "meta") && !match_word(tn, "link") &&
                                 !match_word(tn, "input") && !match_word(tn, "!doctype")) {
                            skip_depth++;
                        }
                    } else if (skip_depth == 0) {
                        if (!closing && match_word(tn, "a")) {
                            char href[BROWSER_URL_CAP];
                            extract_attr(p, ts, te, "href", href, sizeof(href));
                            if (href[0] && href[0] != '#' &&
                                !b_strprefix(href, "javascript:") &&
                                link_count < BROWSER_LINKS) {
                                resolve_href(href, links[link_count].url, BROWSER_URL_CAP);
                                links[link_count].start = rt_i;
                                links[link_count].end = rt_i;
                                cur_link = (int)link_count++;
                            } else {
                                cur_link = -2; /* anchor without a usable href */
                            }
                        } else if (closing && match_word(tn, "a")) {
                            if (cur_link >= 0) links[cur_link].end = rt_i;
                            cur_link = -1;
                        } else if (tag_is_block(tn)) {
                            rt_newline();
                        }
                    }
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }
        }

        if (skip_depth > 0) { i++; continue; }

        /* Entities */
        if (c == '&') {
            uint64_t j = i + 1;
            uint32_t cp = 0;
            int ok = 0;
            if (j < len && p[j] == '#') {
                int hex = 0;
                uint32_t v = 0;
                uint64_t k = 0;
                j++;
                if (j < len && (p[j] == 'x' || p[j] == 'X')) { hex = 1; j++; }
                while (j + k < len && k < 8) {
                    char d = p[j + k];
                    int dig = -1;
                    if (d >= '0' && d <= '9') dig = d - '0';
                    else if (hex && d >= 'a' && d <= 'f') dig = d - 'a' + 10;
                    else if (hex && d >= 'A' && d <= 'F') dig = d - 'A' + 10;
                    else break;
                    v = hex ? v * 16U + (uint32_t)dig : v * 10U + (uint32_t)dig;
                    k++;
                }
                if (k > 0) {
                    cp = v;
                    ok = 1;
                    j += k;
                    if (j < len && p[j] == ';') j++;
                }
            } else if (b_strprefix(p + i, "&amp;")) { cp = '&'; ok = 1; j = i + 5; }
            else if (b_strprefix(p + i, "&lt;")) { cp = '<'; ok = 1; j = i + 4; }
            else if (b_strprefix(p + i, "&gt;")) { cp = '>'; ok = 1; j = i + 4; }
            else if (b_strprefix(p + i, "&quot;")) { cp = '"'; ok = 1; j = i + 6; }
            else if (b_strprefix(p + i, "&apos;")) { cp = '\''; ok = 1; j = i + 6; }
            else if (b_strprefix(p + i, "&nbsp;")) { cp = 0x00A0; ok = 1; j = i + 6; }
            if (ok) {
                rt_emit_cp(cp);
                i = j;
                continue;
            }
            rt_put('&');
            i++;
            continue;
        }

        /* Whitespace collapse */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            rt_space();
            i++;
            continue;
        }

        /* UTF-8 */
        if ((unsigned char)c >= 0x80) {
            uint64_t adv = 1;
            uint32_t cp = rt_utf8(p, i, len, &adv);
            if (cp) rt_emit_cp(cp);
            i += adv;
            continue;
        }

        rt_put(c);
        i++;
    }
    text_buf[rt_i] = 0;
    text_len = rt_i;
}

/* ---- page loading ---- */

static void navigate_to(const char *url) {
    uint32_t ip = 0;
    uint16_t port = 80;
    int https = 0;
    char host[128];
    char path[256];
    char out_path[64];
    uint64_t bytes = 0;
    long rc;

    if (!url || !*url) return;
    /* Auto-prepend http:// if no scheme given */
    if (!b_strprefix(url, "http://") && !b_strprefix(url, "https://")) {
        char full[BROWSER_URL_CAP];
        b_strcpy(full, "http://", sizeof(full));
        b_strcat(full, url, sizeof(full));
        b_strcpy(current_url, full, BROWSER_URL_CAP);
    } else {
        b_strcpy(current_url, url, BROWSER_URL_CAP);
    }
    b_strcpy(address_buf, current_url, BROWSER_URL_CAP);
    addr_cursor = (int)b_strlen(address_buf);

    /* Add to history */
    if (history_pos < 0 || !b_streq(history[history_pos], url)) {
        if (history_pos + 1 < BROWSER_HISTORY) {
            history_pos++;
            b_strcpy(history[history_pos], url, BROWSER_URL_CAP);
            history_count = history_pos + 1;
        }
    }

    loading = 1;
    b_strcpy(status, "Resolving...", sizeof(status));
    draw_all();

    /* Step 1: parse URL format (use current_url which has http:// prepended) */
    rc = parse_url_format(current_url, &port, &https, host, sizeof(host), path, sizeof(path));
    if (rc < 0) {
        b_strcpy(status, "Invalid URL format", sizeof(status));
        loading = 0;
        return;
    }

    /* Step 2: resolve hostname */
    b_strcpy(status, "Resolving hostname...", sizeof(status));
    draw_all();
    rc = (long)resolve_host(host, &ip);
    if (rc < 0) {
        long err = -rc;
        if (err == 2) b_strcpy(status, "DNS: ARP timeout", sizeof(status));
        else if (err == 3) b_strcpy(status, "DNS: network timeout", sizeof(status));
        else b_strcpy(status, "DNS resolution failed", sizeof(status));
        loading = 0;
        return;
    }

    /* Step 3: fetch page */
    b_strcpy(status, "Connecting...", sizeof(status));
    draw_all();

    b_strcpy(out_path, "/browser.page", sizeof(out_path));

    if (https) {
        rc = (long)icda_https_get_ipv4(ip, port, host, path, out_path, &bytes);
    } else {
        rc = (long)icda_http_get_ipv4(ip, port, host, path, out_path, &bytes);
    }

    if (rc < 0) {
        long err = -rc;
        if (err >= 2000 && err < 3000) {
            b_strcpy(status, "HTTP error", sizeof(status));
        } else if (err == 2) {
            b_strcpy(status, "ARP timeout", sizeof(status));
        } else if (err == 3) {
            b_strcpy(status, "TCP timeout", sizeof(status));
        } else if (err == 4) {
            b_strcpy(status, "Connection refused", sizeof(status));
        } else if (err == 5) {
            b_strcpy(status, "Bad HTTP response", sizeof(status));
        } else if (err == 6) {
            b_strcpy(status, "Response too large", sizeof(status));
        } else if (err == 11) {
            b_strcpy(status, "TLS handshake failed", sizeof(status));
        } else if (err == 12) {
            b_strcpy(status, "TLS recv failed", sizeof(status));
        } else {
            b_strcpy(status, "Network error", sizeof(status));
        }
        loading = 0;
        return;
    }

    if (!html_buf || !text_buf) {
        b_strcpy(status, "No memory for page", sizeof(status));
        loading = 0;
        return;
    }
    if (bytes > BROWSER_HTML_CAP - 1) bytes = BROWSER_HTML_CAP - 1;
    {
        uint64_t n = icda_read_file(out_path, html_buf, bytes);
        html_len = n > 0 ? n : 0;
        html_buf[html_len] = 0;
    }

    extract_title(html_buf, page_title, BROWSER_TITLE_CAP);
    build_render_text();
    scroll_y = 0;
    max_scroll = 0;
    loading = 0;
    b_strcpy(status, "Done ", sizeof(status));
    {
        char nb[16];
        b_uint_to_str(html_len, nb, sizeof(nb));
        b_strcat(status, nb, sizeof(status));
        b_strcat(status, " bytes", sizeof(status));
    }
}

static void go_back(void) {
    if (history_pos > 0) {
        history_pos--;
        b_strcpy(current_url, history[history_pos], BROWSER_URL_CAP);
        b_strcpy(address_buf, current_url, BROWSER_URL_CAP);
        addr_cursor = (int)b_strlen(address_buf);
        navigate_to(current_url);
    }
}

static void go_forward(void) {
    if (history_pos + 1 < history_count) {
        history_pos++;
        b_strcpy(current_url, history[history_pos], BROWSER_URL_CAP);
        b_strcpy(address_buf, current_url, BROWSER_URL_CAP);
        addr_cursor = (int)b_strlen(address_buf);
        navigate_to(current_url);
    }
}

/* ---- drawing ---- */

static void draw_toolbar(void) {
    int w = gui_window_width();
    uint32_t *px = gui_pixel_buffer();
    int y = ADDR_BAR_H;

    /* Toolbar background */
    gui_fill_rect(0, y, w, TOOLBAR_H, 0x00F1F3F4);
    gui_draw_hline(0, y + TOOLBAR_H - 1, w, 0x00DADCE0);

    /* Back button */
    gui_fill_rect(6, y + 4, 28, 22, hover_link == -2 ? 0x00E8EAED : 0x00F8F9FA);
    gui_draw_rect_outline(6, y + 4, 28, 22, 0x00DADCE0);
    gui_draw_text(12, y + 8, "<", 0x00202124, 0x00F8F9FA);

    /* Forward button */
    gui_fill_rect(38, y + 4, 28, 22, hover_link == -3 ? 0x00E8EAED : 0x00F8F9FA);
    gui_draw_rect_outline(38, y + 4, 28, 22, 0x00DADCE0);
    gui_draw_text(44, y + 8, ">", 0x00202124, 0x00F8F9FA);

    /* Refresh button */
    gui_fill_rect(70, y + 4, 28, 22, hover_link == -4 ? 0x00E8EAED : 0x00F8F9FA);
    gui_draw_rect_outline(70, y + 4, 28, 22, 0x00DADCE0);
    gui_draw_text(76, y + 8, "R", 0x00202124, 0x00F8F9FA);

    /* Address bar */
    gui_fill_rect(104, y + 4, w - 110, 22, 0x00FFFFFF);
    gui_draw_rect_outline(104, y + 4, w - 110, 22, 0x00DADCE0);
    if (addr_active) {
        gui_draw_rect_outline(104, y + 4, w - 110, 22, 0x001A73E8);
    }
    gui_draw_text(110, y + 8, address_buf, 0x00202124, 0x00FFFFFF);
    if (addr_active) {
        int cx = 110 + addr_cursor * 8;
        if (cx < w - 10) {
            gui_draw_vline(cx, y + 7, 16, 0x001A73E8);
        }
    }
}

static void draw_page(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int content_y = ADDR_BAR_H + TOOLBAR_H;
    int content_h = h - content_y;
    int line_h = 18;
    uint64_t i;
    uint64_t total_lines = 1;
    int row = 0;
    char line[BROWSER_LINE_CAP];

    /* Content background */
    gui_fill_rect(0, content_y, w, content_h, 0x00FFFFFF);

    if (loading) {
        gui_draw_text(PAD, content_y + PAD, "Loading...", 0x005F6368, 0x00FFFFFF);
        return;
    }

    if (!text_buf || text_len == 0) {
        gui_draw_text(PAD, content_y + PAD, "Enter a URL to browse the web.", 0x005F6368, 0x00FFFFFF);
        gui_draw_text(PAD, content_y + PAD + 20, "Example: example.com", 0x005F6368, 0x00FFFFFF);
        return;
    }

    /* Total document height for the scrollbar range. */
    for (i = 0; i < text_len; i++) {
        if (text_buf[i] == '\n') total_lines++;
    }
    max_scroll = (int)(total_lines * (uint64_t)line_h) - content_h + PAD;
    if (max_scroll < 0) max_scroll = 0;
    if (scroll_y > max_scroll) scroll_y = max_scroll;

    /* Render visible lines; link spans are overdrawn in blue. */
    i = 0;
    while (i < text_len) {
        uint64_t ls = i;
        uint64_t le;
        int y;

        while (i < text_len && text_buf[i] != '\n') i++;
        le = i;
        if (i < text_len) i++; /* consume the newline */

        y = content_y + PAD + row * line_h - scroll_y;
        row++;

        if (le == ls) continue;                 /* blank line */
        if (y + line_h <= content_y) continue;  /* above viewport */
        if (y >= content_y + content_h) break;  /* below viewport */
        if (le - ls >= BROWSER_LINE_CAP) le = ls + BROWSER_LINE_CAP - 1;

        {
            uint64_t n = le - ls;
            uint64_t b;
            for (b = 0; b < n; b++) line[b] = text_buf[ls + b];
            line[n] = 0;
            gui_draw_text(PAD, y, line, 0x003C4043, 0x00FFFFFF);
        }

        /* Link spans overlapping this line. */
        for (int li = 0; li < link_count; li++) {
            uint64_t s = links[li].start;
            uint64_t e = links[li].end;
            uint64_t c0, c1, b;
            if (e <= s || s >= le || e <= ls) continue;
            c0 = s > ls ? s - ls : 0;
            c1 = e < le ? e - ls : le - ls;
            if (c1 <= c0) continue;
            for (b = 0; b < c1 - c0; b++) line[b] = text_buf[ls + c0 + b];
            line[c1 - c0] = 0;
            gui_draw_text(PAD + (int)c0 * FONT_W, y, line, 0x001A73E8, 0x00FFFFFF);
            gui_draw_hline(PAD + (int)c0 * FONT_W, y + line_h - 3,
                           (int)(c1 - c0) * FONT_W, 0x001A73E8);
        }
    }
}

static void draw_status(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    gui_fill_rect(0, h - 20, w, 20, 0x00F1F3F4);
    gui_draw_hline(0, h - 21, w, 0x00DADCE0);
    gui_draw_text(6, h - 16, status, 0x005F6368, 0x00F1F3F4);
}

static void draw_all(void) {
    draw_toolbar();
    draw_page();
    draw_status();
    gui_flush();
}

/* ---- input handling ---- */

static int hit_rect(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static void handle_click(int mx, int my) {
    int w = gui_window_width();
    int h = gui_window_height();
    int content_y = ADDR_BAR_H + TOOLBAR_H;

    /* Toolbar buttons */
    if (hit_rect(mx, my, 6, ADDR_BAR_H + 4, 28, 22)) {
        go_back();
        return;
    }
    if (hit_rect(mx, my, 38, ADDR_BAR_H + 4, 28, 22)) {
        go_forward();
        return;
    }
    if (hit_rect(mx, my, 70, ADDR_BAR_H + 4, 28, 22)) {
        navigate_to(current_url);
        return;
    }

    /* Address bar */
    if (hit_rect(mx, my, 104, ADDR_BAR_H + 4, w - 110, 22)) {
        addr_active = 1;
        return;
    }
    addr_active = 0;

    /* Links: map the click to a byte offset in the render text, then test
     * it against each link's byte range (handles links that wrap across
     * visual lines). */
    if (text_buf && my >= content_y + PAD) {
        int line_h = 18;
        int row = (my - content_y - PAD + scroll_y) / line_h;
        int col = (mx - PAD) / FONT_W;
        if (row >= 0 && col >= 0) {
            uint64_t ls = 0;
            for (int r = 0; r < row && ls < text_len; r++) {
                while (ls < text_len && text_buf[ls] != '\n') ls++;
                if (ls < text_len) ls++;
            }
            {
                uint64_t le = ls;
                while (le < text_len && text_buf[le] != '\n') le++;
                if (le - ls >= BROWSER_LINE_CAP) le = ls + BROWSER_LINE_CAP - 1;
                uint64_t pos = ls + (uint64_t)col;
                if (pos < le) {
                    for (int li = 0; li < link_count; li++) {
                        if (links[li].url[0] && pos >= links[li].start && pos < links[li].end) {
                            navigate_to(links[li].url);
                            return;
                        }
                    }
                }
            }
        }
    }
}

static void handle_key(uint32_t key) {
    if (addr_active) {
        if (key == '\r' || key == '\n') {
            addr_active = 0;
            navigate_to(address_buf);
        } else if (key == '\b') {
            if (addr_cursor > 0) {
                addr_cursor--;
                address_buf[addr_cursor] = 0;
            }
        } else if (key >= 32 && key <= 126) {
            if (addr_cursor < BROWSER_URL_CAP - 1) {
                address_buf[addr_cursor++] = (char)key;
                address_buf[addr_cursor] = 0;
            }
        }
    } else {
        if (key == 'l' || key == 'L') {
            addr_active = 1;
            addr_cursor = (int)b_strlen(address_buf);
        } else if (key == '\r' || key == '\n') {
            navigate_to(address_buf);
        } else if (key == 27) { /* Escape */
            addr_active = 0;
        }
    }
}

static int on_event(void *ud, const gui_msg_t *msg) {
    (void)ud;
    if (msg->type == GUI_MSG_MOUSE_EVENT) {
        if (msg->mouse.buttons & GUI_BTN_LEFT) {
            handle_click(msg->mouse.x, msg->mouse.y);
        }
    } else if (msg->type == GUI_MSG_KEY_EVENT) {
        if (msg->key.pressed) {
            handle_key(msg->key.keycode);
        }
    } else if (msg->type == GUI_MSG_CLOSE_WINDOW) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Page buffers come from shared-memory regions - properly mapped,
     * process-owned memory.  (The previous hardcoded 0x60000000 pointed
     * at unmapped memory and page-faulted on the first fetch.) */
    html_shm = icda_shm_create(BROWSER_HTML_CAP);
    if (html_shm) html_buf = (char *)(uintptr_t)icda_shm_map(html_shm);
    text_shm = icda_shm_create(BROWSER_TEXT_CAP);
    if (text_shm) text_buf = (char *)(uintptr_t)icda_shm_map(text_shm);

    b_strcpy(current_url, "http://example.com", BROWSER_URL_CAP);
    b_strcpy(address_buf, current_url, BROWSER_URL_CAP);
    addr_cursor = (int)b_strlen(address_buf);
    b_strcpy(page_title, "ICDA Browser", BROWSER_TITLE_CAP);
    b_strcpy(status, "Ready", sizeof(status));

    if (gui_open_window("ICDA Browser", 900, 600) != 0) {
        return -1;
    }

    navigate_to(current_url);
    draw_all();

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
            if (msg.type == GUI_MSG_MOUSE_EVENT) {
                if (msg.mouse.buttons & GUI_BTN_LEFT) {
                    handle_click(msg.mouse.x, msg.mouse.y);
                }
            } else if (msg.type == GUI_MSG_KEY_EVENT) {
                if (msg.key.pressed) {
                    handle_key(msg.key.keycode);
                }
            }
        }
        if (changed) {
            draw_all();
        }
        icda_sleep(1);
    }
}