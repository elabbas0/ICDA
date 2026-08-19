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

#define BROWSER_URL_CAP   256
#define BROWSER_HTML_CAP  (256 * 1024)
#define BROWSER_TITLE_CAP 64
#define BROWSER_HISTORY   16
#define BROWSER_LINKS     64
#define BROWSER_LINE_CAP  128

#define ADDR_BAR_H  34
#define TOOLBAR_H   30
#define PAD         6

typedef struct {
    int x, y, w, h;
    char url[BROWSER_URL_CAP];
} browser_link_t;

static char current_url[BROWSER_URL_CAP];
static char address_buf[BROWSER_URL_CAP];
static char page_title[BROWSER_TITLE_CAP];
static char *html_buf = NULL;
static uint64_t html_len = 0;

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

static int parse_url(const char *url, uint32_t *ip_out, uint16_t *port_out,
                     int *https_out, char *host_out, uint64_t host_cap,
                     char *path_out, uint64_t path_cap) {
    const char *host;
    uint64_t host_len = 0;
    uint64_t path_len = 0;
    uint16_t port = 80;
    int use_https = 0;

    if (!url || !*url || !ip_out || !port_out || !https_out) return -1;
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

    {
        uint32_t resolved = 0;
        /* Try IPv4 literal first */
        int is_ip = 1;
        const char *p = host_out;
        int dots = 0;
        while (*p) {
            if (*p == '.') dots++;
            else if (*p < '0' || *p > '9') { is_ip = 0; break; }
            p++;
        }
        if (is_ip && dots == 3) {
            /* Parse dotted quad */
            uint32_t octets[4];
            uint64_t i = 0;
            uint64_t used = 0;
            for (int o = 0; o < 4; o++) {
                uint32_t v = 0;
                uint64_t start = i;
                while (host_out[i] >= '0' && host_out[i] <= '9') {
                    v = v * 10U + (uint32_t)(host_out[i] - '0');
                    if (v > 255U) { is_ip = 0; break; }
                    i++;
                }
                if (i == start || (o < 3 && host_out[i] != '.')) { is_ip = 0; break; }
                if (o < 3) i++;
                octets[o] = v;
            }
            if (is_ip && host_out[i] == 0) {
                resolved = octets[0] | (octets[1] << 8) | (octets[2] << 16) | (octets[3] << 24);
            } else {
                is_ip = 0;
            }
        } else {
            is_ip = 0;
        }
        if (!is_ip) {
            long rc = (long)icda_dns_resolve(host_out, &resolved);
            if (rc < 0) return (int)rc;
        }
        *ip_out = resolved;
    }
    *port_out = port;
    *https_out = use_https;
    return 0;
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

/* Parse HTML into a list of clickable links.  Returns the number found. */
static int extract_links(const char *html, browser_link_t *out, int max_links,
                         int view_w, int view_h) {
    const char *p = html;
    int count = 0;
    int y = ADDR_BAR_H + TOOLBAR_H + PAD;
    int x = PAD;
    int line_h = 18;

    if (!html || !out || max_links <= 0) return 0;

    while (*p && count < max_links) {
        if (*p == '<') {
            const char *tag = p + 1;
            if (b_lower(tag[0]) == 'a') {
                const char *text_start = 0;
                /* Find href */
                const char *q = tag;
                const char *href = 0;
                const char *href_end = 0;
                while (*q && *q != '>') {
                    if (b_lower(q[0]) == 'h' && b_lower(q[1]) == 'r' &&
                        b_lower(q[2]) == 'e' && b_lower(q[3]) == 'f') {
                        q += 4;
                        while (*q == ' ' || *q == '=') q++;
                        if (*q == '"' || *q == '\'') {
                            char quote = *q++;
                            href = q;
                            while (*q && *q != quote) q++;
                            href_end = q;
                        } else {
                            href = q;
                            while (*q && *q != ' ' && *q != '>') q++;
                            href_end = q;
                        }
                        break;
                    }
                    q++;
                }
                /* Find link text */
                while (*p && *p != '>') p++;
                if (*p == '>') p++;
                {
                    text_start = p;
                    while (*p && !(*p == '<' && b_lower(p[1]) == '/')) p++;
                    if (href && href_end && href_end > href) {
                        char url[BROWSER_URL_CAP];
                        char text[64];
                        uint64_t url_len = (uint64_t)(href_end - href);
                        uint64_t text_len = (uint64_t)(p - text_start);
                        if (url_len > BROWSER_URL_CAP - 1) url_len = BROWSER_URL_CAP - 1;
                        if (text_len > sizeof(text) - 1) text_len = sizeof(text) - 1;
                        for (uint64_t i = 0; i < url_len; i++) url[i] = href[i];
                        url[url_len] = 0;
                        for (uint64_t i = 0; i < text_len; i++) text[i] = text_start[i];
                        text[text_len] = 0;
                        strip_tags(text, sizeof(text), text);
                        decode_entities(text, sizeof(text), text);
                        if (text[0]) {
                            out[count].x = x;
                            out[count].y = y;
                            out[count].w = ic_text_width(text);
                            out[count].h = line_h;
                            b_strcpy(out[count].url, url, BROWSER_URL_CAP);
                            count++;
                            x += out[count - 1].w + 8;
                            if (x > view_w - PAD) {
                                x = PAD;
                                y += line_h;
                            }
                        }
                    }
                }
                /* Skip closing tag */
                while (*p && !(*p == '>' && p > text_start)) p++;
                if (*p == '>') p++;
                continue;
            }
            /* Handle block elements: newline */
            if (b_lower(tag[0]) == 'p' || b_lower(tag[0]) == 'd' ||
                b_lower(tag[0]) == 'h' || b_lower(tag[0]) == 'l' ||
                b_lower(tag[0]) == 'b' || b_lower(tag[0]) == 'u' ||
                b_lower(tag[0]) == 'o' || b_lower(tag[0]) == 't' ||
                b_lower(tag[0]) == 'c' || b_lower(tag[0]) == 'f' ||
                b_lower(tag[0]) == 's' || b_lower(tag[0]) == 'n') {
                x = PAD;
                y += line_h;
            }
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            continue;
        }
        p++;
    }
    return count;
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
    b_strcpy(current_url, url, BROWSER_URL_CAP);
    b_strcpy(address_buf, url, BROWSER_URL_CAP);
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
    b_strcpy(status, "Loading...", sizeof(status));

    rc = parse_url(url, &ip, &port, &https, host, sizeof(host), path, sizeof(path));
    if (rc < 0) {
        b_strcpy(status, "Invalid URL", sizeof(status));
        loading = 0;
        return;
    }

    b_strcpy(out_path, "/tmp/.browser.page", sizeof(out_path));

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

    if (bytes > BROWSER_HTML_CAP - 1) bytes = BROWSER_HTML_CAP - 1;
    if (html_buf) {
        uint64_t n = icda_read_file(out_path, html_buf, bytes);
        html_len = n > 0 ? n : 0;
        if (html_len > 0) html_buf[html_len] = 0;
    }

    extract_title(html_buf ? html_buf : "", page_title, BROWSER_TITLE_CAP);
    scroll_y = 0;
    max_scroll = 0;
    loading = 0;
    b_strcpy(status, "Done", sizeof(status));
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
    int y = content_y + PAD - scroll_y;
    int x = PAD;
    int line_h = 18;
    char line[BROWSER_LINE_CAP];
    uint64_t li = 0;
    const char *p = html_buf ? html_buf : "";
    int in_tag = 0;
    int in_entity = 0;
    int is_link = 0;
    int link_idx = -1;
    int bold = 0;
    int heading = 0;

    /* Content background */
    gui_fill_rect(0, content_y, w, content_h, 0x00FFFFFF);

    if (loading) {
        gui_draw_text(PAD, content_y + PAD, "Loading...", 0x005F6368, 0x00FFFFFF);
        return;
    }

    if (!html_buf || html_len == 0) {
        gui_draw_text(PAD, content_y + PAD, "Enter a URL to browse the web.", 0x005F6368, 0x00FFFFFF);
        gui_draw_text(PAD, content_y + PAD + 20, "Example: http://example.com", 0x005F6368, 0x00FFFFFF);
        return;
    }

    link_count = extract_links(html_buf, links, BROWSER_LINKS, w, content_h);
    hover_link = -1;

    while (*p && y < content_y + content_h + line_h) {
        if (*p == '<') {
            in_tag = 1;
            p++;
            continue;
        }
        if (*p == '>') {
            in_tag = 0;
            p++;
            continue;
        }
        if (in_tag) {
            /* Track bold/heading for styling */
            if (b_lower(p[0]) == 'b' && (p[1] == '>' || p[1] == 'r')) {
                bold = 1;
            } else if (b_lower(p[0]) == '/' && b_lower(p[1]) == 'b') {
                bold = 0;
            } else if (b_lower(p[0]) == 'h' && p[1] >= '1' && p[1] <= '6') {
                heading = 1;
            } else if (b_lower(p[0]) == '/' && b_lower(p[1]) == 'h') {
                heading = 0;
            }
            p++;
            continue;
        }
        if (*p == '&') {
            in_entity = 1;
            p++;
            continue;
        }
        if (in_entity) {
            if (*p == ';') in_entity = 0;
            p++;
            continue;
        }
        if (*p == '\n' || *p == '\r') {
            if (li > 0) {
                line[li] = 0;
                if (y >= content_y && y < content_y + content_h) {
                    uint32_t fg = heading ? 0x001A73E8 : (bold ? 0x00202124 : 0x003C4043);
                    gui_draw_text(x, y, line, fg, 0x00FFFFFF);
                }
                li = 0;
                y += line_h;
            }
            p++;
            continue;
        }
        if (li < BROWSER_LINE_CAP - 1) {
            line[li++] = *p;
        }
        p++;
    }
    if (li > 0) {
        line[li] = 0;
        if (y >= content_y && y < content_y + content_h) {
            uint32_t fg = heading ? 0x001A73E8 : (bold ? 0x00202124 : 0x003C4043);
            gui_draw_text(x, y, line, fg, 0x00FFFFFF);
        }
    }

    max_scroll = y - content_h + line_h;
    if (max_scroll < 0) max_scroll = 0;
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

    /* Links */
    for (int i = 0; i < link_count; i++) {
        if (hit_rect(mx, my, links[i].x, links[i].y - scroll_y, links[i].w, links[i].h)) {
            navigate_to(links[i].url);
            return;
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

    html_buf = (char *)0x60000000; /* Use a fixed high address for the page buffer */

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