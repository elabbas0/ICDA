












#include "libicda.h"

#define WIN_W 900
#define WIN_H 600

#define BR_URL_CAP   256
#define BR_HTML_CAP  (1024 * 1024)
#define BR_TEXT_CAP  (1024 * 1024)
#define BR_TITLE_CAP 96
#define BR_HISTORY   16
#define BR_LINKS     256
#define BR_STATUS_H  24
#define BR_ADDR_W    420



typedef struct {
    uint64_t start;
    uint64_t end;
    char     url[BR_URL_CAP];
} br_link_t;

static struct {
    char      current_url[BR_URL_CAP];
    char      address[BR_URL_CAP];
    char      title[BR_TITLE_CAP];
    char     *html;
    uint64_t  html_len;
    char     *text;
    uint64_t  text_len;
    uint64_t  html_shm;
    uint64_t  text_shm;

    char      history[BR_HISTORY][BR_URL_CAP];
    int       history_count;
    int       history_pos;

    br_link_t links[BR_LINKS];
    int       link_count;

    int  scroll;               
    int  content_h;            
    int  wrap_cols;            

    int  loading;
    int  nav_pending;
    int  load_failed;
    char nav_url[BR_URL_CAP];
    int  addr_focused;

    
    int  hover_back;
    int  hover_forward;
    int  hover_reload;
    int  hover_go;
    int  hover_link;

    

    char           addr_buf[BR_URL_CAP];
    ic_textfield_t addr;
    ic_tween_t     scrollbar;
    char           status[80];
} br;



static void strip_tags(char *dst, uint64_t dst_cap, const char *src) {
    uint64_t di = 0;
    int in_tag = 0;
    int in_entity = 0;
    if (!dst || dst_cap == 0) return;
    while (*src && di + 1 < dst_cap) {
        if (*src == '<') { in_tag = 1; src++; continue; }
        if (*src == '>') { in_tag = 0; src++; continue; }
        if (in_tag) { src++; continue; }
        if (*src == '&') { in_entity = 1; src++; continue; }
        if (in_entity) {
            if (*src == ';') in_entity = 0;
            src++;
            continue;
        }
        dst[di++] = *src++;
    }
    dst[di] = 0;
}

static void decode_entities(char *dst, uint64_t dst_cap, const char *src) {
    uint64_t di = 0;
    

    static const char ent_amp[]  = { '&', 'a', 'm', 'p', ';', 0 };
    static const char ent_lt[]   = { '&', 'l', 't', ';', 0 };
    static const char ent_gt[]   = { '&', 'g', 't', ';', 0 };
    static const char ent_quot[] = { '&', 'q', 'u', 'o', 't', ';', 0 };
    static const char ent_nbsp[] = { '&', 'n', 'b', 's', 'p', ';', 0 };
    static const char ent_num[]  = { '&', '#', 0 };
    if (!dst || dst_cap == 0) return;
    while (*src && di + 1 < dst_cap) {
        if (*src == '&' && ic_strprefix(src, ent_amp)) {
            dst[di++] = '&';
            src += 5;
        } else if (*src == '&' && ic_strprefix(src, ent_lt)) {
            dst[di++] = '<';
            src += 4;
        } else if (*src == '&' && ic_strprefix(src, ent_gt)) {
            dst[di++] = '>';
            src += 4;
        } else if (*src == '&' && ic_strprefix(src, ent_quot)) {
            dst[di++] = '"';
            src += 6;
        } else if (*src == '&' && ic_strprefix(src, ent_nbsp)) {
            dst[di++] = ' ';
            src += 6;
        } else if (*src == '&' && ic_strprefix(src, ent_num)) {
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
    ic_strcpy(title, "ICDA Browser", cap);
    while (*p) {
        if (ic_lower(p[0]) == '<' && ic_lower(p[1]) == 't' &&
            ic_lower(p[2]) == 'i' && ic_lower(p[3]) == 't' &&
            ic_lower(p[4]) == 'l' && ic_lower(p[5]) == 'e') {
            p += 6;
            while (*p && *p != '>') p++;
            if (*p == '>') p++;
            t_start = p;
            while (*p && !(*p == '<' && ic_lower(p[1]) == '/')) p++;
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



static uint64_t rt_i;            
static uint64_t rt_line_start;   
static uint64_t rt_last_space;   
static int      rt_cols;         

static int match_word(const char *p, const char *w) {
    uint64_t i = 0;
    while (w[i]) {
        if (ic_lower(p[i]) != w[i]) return 0;
        i++;
    }
    {
        char c = p[i];
        return c == 0 || c == '>' || c == '/' || c == ' ' ||
               c == '\t' || c == '\n' || c == '\r';
    }
}


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


static int tag_is_block(const char *t) {
    if (ic_lower(t[0]) == 'h' && t[1] >= '1' && t[1] <= '6') {
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
    if (rt_i == 0 || rt_i + 1 >= BR_TEXT_CAP) return;
    if (br.text[rt_i - 1] == '\n') return;      
    br.text[rt_i++] = '\n';
    rt_line_start = rt_i;
    rt_last_space = (uint64_t)-1;
}

static void rt_put(char c) {
    if (rt_i + 1 >= BR_TEXT_CAP) return;
    br.text[rt_i++] = c;
    if (rt_i - rt_line_start > (uint64_t)rt_cols &&
        rt_last_space != (uint64_t)-1 && rt_last_space >= rt_line_start) {
        br.text[rt_last_space] = '\n';
        rt_line_start = rt_last_space + 1;
        rt_last_space = (uint64_t)-1;
    }
}

static void rt_space(void) {
    if (rt_i == 0 || rt_i + 1 >= BR_TEXT_CAP) return;
    if (br.text[rt_i - 1] == ' ' || br.text[rt_i - 1] == '\n') return;
    rt_last_space = rt_i;
    rt_put(' ');
}


static void rt_emit_cp(uint32_t cp) {
    char c;
    switch (cp) {
        case 0x2013: case 0x2014: c = '-'; break;   
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
    uint64_t nlen = ic_strlen(name);
    uint64_t i = ts;
    if (cap) out[0] = 0;
    while (i + nlen + 1 < te) {
        if (ic_lower(p[i]) == ic_lower(name[0])) {
            uint64_t k = 0;
            while (k < nlen && ic_lower(p[i + k]) == ic_lower(name[k])) k++;
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


static void resolve_href(const char *href, char *out, uint64_t cap) {
    uint16_t port;
    int use_tls;
    char host[128];
    char path[256];

    if (cap) out[0] = 0;
    if (!href || !*href || cap == 0) return;
    if (ic_strprefix(href, "http://") || ic_strprefix(href, "https://")) {
        ic_strcpy(out, href, cap);
        return;
    }
    if (ic_url_split(br.current_url, host, sizeof(host),
                     &port, path, sizeof(path), &use_tls) < 0) {
        ic_strcpy(out, href, cap);
        return;
    }
    if (ic_strprefix(href, "//")) {
        ic_strcpy(out, use_tls ? "https://" : "http://", cap);
        ic_strcat(out, href, cap);
        return;
    }
    ic_strcpy(out, use_tls ? "https://" : "http://", cap);
    ic_strcat(out, host, cap);
    if (href[0] == '/') {
        ic_strcat(out, href, cap);
        return;
    }
    {
        
        uint64_t plen = ic_strlen(path);
        while (plen > 0 && path[plen - 1] != '/') plen--;
        if (plen > 1) {
            path[plen] = 0;
            ic_strcat(out, path, cap);
        } else {
            ic_strcat(out, "/", cap);
        }
        ic_strcat(out, href, cap);
    }
}


static void build_render_text(void) {
    const char *p = br.html ? br.html : "";
    uint64_t len = br.html_len;
    uint64_t i = 0;
    int skip_depth = 0;
    int cur_link = -1;

    if (!br.text) return;
    rt_i = 0;
    rt_line_start = 0;
    rt_last_space = (uint64_t)-1;
    rt_cols = br.wrap_cols;
    if (rt_cols < 20) rt_cols = 20;
    br.link_count = 0;

    while (i < len && rt_i + 1 < BR_TEXT_CAP) {
        char c = p[i];

        if (c == '<' && i + 1 < len &&
            ((p[i + 1] >= 'a' && p[i + 1] <= 'z') ||
             (p[i + 1] >= 'A' && p[i + 1] <= 'Z') ||
             p[i + 1] == '/' || p[i + 1] == '!')) {
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
                            char href[BR_URL_CAP];
                            extract_attr(p, ts, te, "href", href, sizeof(href));
                            if (href[0] && href[0] != '#' &&
                                !ic_strprefix(href, "javascript:") &&
                                br.link_count < BR_LINKS) {
                                resolve_href(href, br.links[br.link_count].url, BR_URL_CAP);
                                br.links[br.link_count].start = rt_i;
                                br.links[br.link_count].end = rt_i;
                                cur_link = (int)br.link_count++;
                            } else {
                                cur_link = -2; /* anchor without a usable href */
                            }
                        } else if (closing && match_word(tn, "a")) {
                            if (cur_link >= 0) br.links[cur_link].end = rt_i;
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
            } else if (ic_strprefix(p + i, "&amp;")) { cp = '&'; ok = 1; j = i + 5; }
            else if (ic_strprefix(p + i, "&lt;")) { cp = '<'; ok = 1; j = i + 4; }
            else if (ic_strprefix(p + i, "&gt;")) { cp = '>'; ok = 1; j = i + 4; }
            else if (ic_strprefix(p + i, "&quot;")) { cp = '"'; ok = 1; j = i + 6; }
            else if (ic_strprefix(p + i, "&apos;")) { cp = '\''; ok = 1; j = i + 6; }
            else if (ic_strprefix(p + i, "&nbsp;")) { cp = 0x00A0; ok = 1; j = i + 6; }
            if (ok) {
                rt_emit_cp(cp);
                i = j;
                continue;
            }
            rt_put('&');
            i++;
            continue;
        }

        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            rt_space();
            i++;
            continue;
        }

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
    br.text[rt_i] = 0;
    br.text_len = rt_i;
}

/* -------------------------------------------------------- page loading */

static void br_status(const char *text) {
    ic_strcpy(br.status, text, sizeof(br.status));
}

static void set_address(const char *url) {
    ic_strcpy(br.addr_buf, url, BR_URL_CAP);
    br.addr.text = br.addr_buf;
    br.addr.cursor = (int)ic_strlen(br.addr_buf);
    br.addr.sel_start = br.addr.sel_end = br.addr.cursor;
    br.addr.scroll_px = 0;
    ic_strcpy(br.address, url, BR_URL_CAP);
}

static void fetch_now(const char *url) {
    char host[128];
    char path[256];
    char out_path[64];
    uint16_t port = 80;
    int use_tls = 0;
    uint64_t bytes = 0;
    long rc;

    if (!url || !*url) return;
    /* Auto-prepend http:// when no scheme was typed. */
    if (!ic_strprefix(url, "http://") && !ic_strprefix(url, "https://")) {
        char full[BR_URL_CAP];
        ic_strcpy(full, "http://", sizeof(full));
        ic_strcat(full, url, sizeof(full));
        ic_strcpy(br.current_url, full, BR_URL_CAP);
    } else {
        ic_strcpy(br.current_url, url, BR_URL_CAP);
    }
    set_address(br.current_url);

    if (br.history_pos < 0 || !ic_streq(br.history[br.history_pos], br.current_url)) {
        if (br.history_pos + 1 < BR_HISTORY) {
            br.history_pos++;
            ic_strcpy(br.history[br.history_pos], br.current_url, BR_URL_CAP);
            br.history_count = br.history_pos + 1;
        }
    }

    br.loading = 1;
    br_status("Resolving host...");

    if (ic_url_split(br.current_url, host, sizeof(host),
                     &port, path, sizeof(path), &use_tls) < 0) {
        br_status("That address is not a valid URL");
        br.load_failed = 1;
        br.loading = 0;
        return;
    }

    br_status(use_tls ? "Connecting over TLS..." : "Connecting...");
    ic_strcpy(out_path, "/browser.page", sizeof(out_path));
    rc = ic_http_fetch_to_file(host, port, use_tls, path, out_path, &bytes);

    if (rc < 0) {
        long err = -rc;
        if (err >= 2000 && err < 3000)      br_status("The server refused the request");
        else if (err == 2)                 br_status("The host name did not resolve");
        else if (err == 3)                 br_status("The connection timed out");
        else if (err == 4)                 br_status("The connection was refused");
        else if (err == 5)                 br_status("The server sent a malformed reply");
        else if (err == 6)                 br_status("The page is too large to load");
        else if (err == 11)                br_status("The secure connection failed");
        else if (err == 12)                br_status("The secure connection dropped");
        else                               br_status("The network is unavailable");
        br.load_failed = 1;
        br.loading = 0;
        return;
    }

    if (!br.html || !br.text) {
        br_status("Not enough memory to hold the page");
        br.load_failed = 1;
        br.loading = 0;
        return;
    }
    if (bytes > BR_HTML_CAP - 1) bytes = BR_HTML_CAP - 1;
    {
        long rn = (long)icda_read_file(out_path, br.html, bytes);
        br.html_len = rn > 0 ? (uint64_t)rn : 0;
        br.html[br.html_len] = 0;
    }

    extract_title(br.html, br.title, BR_TITLE_CAP);
    build_render_text();
    br.scroll = 0;
    br.loading = 0;
    br.load_failed = 0;
    {
        char msg[80];
        char n[24];
        ic_strcpy(msg, "Loaded ", sizeof(msg));
        ic_snprintf_u64(n, sizeof(n), br.html_len);
        ic_strcat(msg, n, sizeof(msg));
        ic_strcat(msg, " bytes", sizeof(msg));
        br_status(msg);
    }
}

static void navigate_to(const char *url) {
    if (!url || !*url) return;
    ic_strcpy(br.nav_url, url, BR_URL_CAP);
    br.nav_pending = 1;
    br.loading = 1;
    br_status("Loading...");
}

static void tick(ic_app_t *app) {
    if (br.nav_pending == 1) {
        br.nav_pending = 2;
    } else if (br.nav_pending == 2) {
        br.nav_pending = 0;
        fetch_now(br.nav_url);
        ic_app_invalidate(app);
    }
}

static void go_back(void) {
    if (br.history_pos <= 0) return;
    br.history_pos--;
    ic_strcpy(br.current_url, br.history[br.history_pos], BR_URL_CAP);
    set_address(br.current_url);
    navigate_to(br.current_url);
}

static void go_forward(void) {
    if (br.history_pos + 1 >= br.history_count) return;
    br.history_pos++;
    ic_strcpy(br.current_url, br.history[br.history_pos], BR_URL_CAP);
    set_address(br.current_url);
    navigate_to(br.current_url);
}



static const ic_face_t *reading(void) { return ic_font(IC_FONT_SUBHEAD); }

static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t back_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    return ic_rect_make(b.x + IC_SP_2, (b.h - IC_H_CONTROL) / 2, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t forward_rect(ic_app_t *app) {
    ic_rect_t r = back_rect(app);
    return ic_rect_make(r.x + r.w + IC_SP_1, r.y, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t reload_rect(ic_app_t *app) {
    ic_rect_t r = forward_rect(app);
    return ic_rect_make(r.x + r.w + IC_SP_1, r.y, IC_H_CONTROL, IC_H_CONTROL);
}

static ic_rect_t addr_rect(ic_app_t *app) {
    ic_rect_t r = reload_rect(app);
    int w = app->width - r.x - r.w - IC_SP_4 - 68 - IC_SP_2;
    if (w > BR_ADDR_W) w = BR_ADDR_W;
    if (w < 80) w = 80;
    return ic_rect_make(r.x + r.w + IC_SP_2, (toolbar_rect(app).h - IC_H_CONTROL) / 2, w,
                        IC_H_CONTROL);
}

static ic_rect_t go_rect(ic_app_t *app) {
    ic_rect_t a = addr_rect(app);
    return ic_rect_make(a.x + a.w + IC_SP_2, a.y, 68, IC_H_CONTROL);
}

static ic_rect_t page_rect(ic_app_t *app) {
    int y = IC_H_TOOLBAR;
    return ic_rect_make(0, y, app->width, app->height - y - BR_STATUS_H);
}

static ic_rect_t status_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - BR_STATUS_H, app->width, BR_STATUS_H);
}



static void layout(ic_app_t *app) {
    ic_rect_t p = page_rect(app);
    const ic_face_t *f = reading();
    int col_w = p.w - 2 * IC_SP_6;
    int avg = ic_text_measure(f, "abcdefghijklmnopqrstuvwxyz ") / 27;
    int lines = 1;
    if (avg < 1) avg = 1;
    if (col_w > 720) col_w = 720;              
    br.wrap_cols = col_w / avg;
    if (br.wrap_cols < 20) br.wrap_cols = 20;
    if (br.text) {
        for (uint64_t i = 0; i < br.text_len; i++) {
            if (br.text[i] == '\n') lines++;
        }
    }
    br.content_h = lines * (f->line_h + 2) + 2 * IC_SP_5;
    if (br.scroll > br.content_h - p.h) br.scroll = br.content_h - p.h;
    if (br.scroll < 0) br.scroll = 0;
}



static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    const ic_palette_t *p = ic_palette();
    int can_back = br.history_pos > 0;
    int can_fwd = br.history_pos + 1 < br.history_count;

    ic_ui_toolbar(c, b);
    ic_ui_icon_button(c, back_rect(app), IC_SYM_CHEVRON_LEFT,
                      !can_back ? IC_STATE_DISABLED
                                : (br.hover_back ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_icon_button(c, forward_rect(app), IC_SYM_CHEVRON_RIGHT,
                      !can_fwd ? IC_STATE_DISABLED
                               : (br.hover_forward ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_icon_button(c, reload_rect(app), IC_SYM_RELOAD,
                      br.loading ? IC_STATE_PRESSED
                                 : (br.hover_reload ? IC_STATE_HOVER : IC_STATE_NORMAL));

    {
        ic_textfield_t tf = br.addr;
        tf.focused = br.addr_focused;
        tf.caret_on = ic_app_caret_visible(app);
        tf.placeholder = "Search or enter an address";
        tf.scroll_px = br.addr.scroll_px;
        ic_ui_textfield(c, addr_rect(app), &tf);
    }
    ic_ui_button(c, go_rect(app), "Go", IC_SYM_CHEVRON_RIGHT, IC_BUTTON_PRIMARY,
                 br.hover_go ? IC_STATE_HOVER : IC_STATE_NORMAL);
    (void)p;
}


static uint64_t line_offset(uint64_t row) {
    uint64_t r = 0;
    uint64_t i = 0;
    while (i < br.text_len && r < row) {
        while (i < br.text_len && br.text[i] != '\n') i++;
        if (i < br.text_len) i++;
        r++;
    }
    return i;
}

static uint64_t line_count(void) {
    uint64_t n = 1;
    for (uint64_t i = 0; i < br.text_len; i++) {
        if (br.text[i] == '\n') n++;
    }
    return n;
}

static void draw_page(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t p = page_rect(app);
    const ic_palette_t *pal = ic_palette();
    const ic_face_t *f = reading();
    int lh = f->line_h + 2;
    int col_x = p.x + (p.w - (p.w - 2 * IC_SP_6 > 720 ? 720 : p.w - 2 * IC_SP_6)) / 2;
    int col_w = p.w - 2 * IC_SP_6;
    uint64_t first, last, row;
    ic_rect_t saved;

    if (col_w > 720) col_w = 720;
    ic_gfx_fill(c, p.x, p.y, p.w, p.h, pal->content);
    ic_canvas_push_clip(c, p.x, p.y, p.w, p.h, &saved);

    if (br.loading) {
        ic_ui_empty_state(c, p, IC_SYM_RELOAD, "Loading", br.status);
        ic_canvas_pop_clip(c, &saved);
        return;
    }
    if (br.load_failed && (!br.text || br.text_len == 0)) {
        ic_ui_empty_state(c, p, IC_SYM_WARNING, "Could not open the page", br.status);
        ic_canvas_pop_clip(c, &saved);
        return;
    }
    if (!br.text || br.text_len == 0) {
        ic_ui_empty_state(c, p, IC_SYM_GLOBE, "Nothing loaded yet",
                          "Enter an address above, or pick a link from a page you have loaded.");
        ic_canvas_pop_clip(c, &saved);
        return;
    }

    first = (uint64_t)(br.scroll / lh);
    last = first + (uint64_t)(p.h / lh) + 2;
    for (row = first; row < last && row < line_count(); row++) {
        uint64_t ls = line_offset(row);
        uint64_t le = ls;
        int y = p.y + IC_SP_5 + (int)(row * (uint64_t)lh) - br.scroll;
        ic_canvas_t lc;
        while (le < br.text_len && br.text[le] != '\n') le++;
        if (le == ls) continue;
        if (y + lh <= p.y || y >= p.y + p.h) continue;
        lc = *c;
        (void)lc;
        
        ic_text_draw_n(c, f, col_x, y + ic_text_center_baseline(f, 0, f->line_h),
                       br.text + ls, (int)(le - ls), pal->label);
        for (int li = 0; li < br.link_count; li++) {
            uint64_t s = br.links[li].start;
            uint64_t e = br.links[li].end;
            uint64_t c0, c1;
            if (e <= s || s >= le || e <= ls) continue;
            c0 = s > ls ? s - ls : 0;
            c1 = e < le ? e - ls : le - ls;
            if (c1 <= c0) continue;
            {
                int x = col_x + ic_text_measure_n(f, br.text + ls, (int)c0);
                int w = ic_text_measure_n(f, br.text + ls + c0, (int)(c1 - c0));
                ic_color_t col = pal->accent;
                if (li == br.hover_link) col = pal->accent_hover;
                ic_text_draw_n(c, f, x, y + ic_text_center_baseline(f, 0, f->line_h),
                               br.text + ls + c0, (int)(c1 - c0), col);
                ic_gfx_hline(c, x, y + f->line_h + 1, w, col);
            }
        }
    }
    ic_canvas_pop_clip(c, &saved);

    if (br.content_h > p.h) {
        float a = ic_tween_value(&br.scrollbar);
        if (a > 0.01f) ic_ui_scrollbar(c, p, br.scroll, br.content_h, a);
    }
}

static void draw_status(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = status_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_ui_statusbar(c, s, br.status);
    if (br.title[0] && !br.loading) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(s.x + s.w - 320, s.y, 320 - IC_SP_3, s.h),
                        br.title, p->label_tertiary, IC_ALIGN_RIGHT);
    }
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    int scrolled;
    layout(app);
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_page(app, c);
    draw_status(app, c);

    scrolled = br.scroll > 0;
    ic_tween_to(&br.scrollbar, scrolled ? 1.0f : 0.0f, IC_DUR_FAST,
                scrolled ? IC_EASE_ENTER : IC_EASE_EXIT);
    if (ic_tween_running(&br.scrollbar)) ic_app_animate(app);
    if (br.addr_focused || br.loading) ic_app_animate(app);
}



static void event(ic_app_t *app, const ic_event_t *ev) {
    ic_rect_t a = addr_rect(app);

    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        if (br.addr_focused) {
            
            if (!ic_ui_hit(a, ev->x, ev->y)) br.addr_focused = 0;
        }
        br.hover_back = ic_ui_hit(back_rect(app), ev->x, ev->y);
        br.hover_forward = ic_ui_hit(forward_rect(app), ev->x, ev->y);
        br.hover_reload = ic_ui_hit(reload_rect(app), ev->x, ev->y);
        br.hover_go = ic_ui_hit(go_rect(app), ev->x, ev->y);
        br.hover_link = -1;
        break;
    case IC_EV_MOUSE_DOWN: {
        int i;
        if (ev->button != GUI_BTN_LEFT) break;
        if (br.hover_back) { go_back(); break; }
        if (br.hover_forward) { go_forward(); break; }
        if (br.hover_reload) { navigate_to(br.current_url); break; }
        if (br.hover_go) { br.addr_focused = 0; navigate_to(br.addr.text); break; }
        if (ic_ui_hit(a, ev->x, ev->y)) {
            br.addr_focused = 1;
            br.addr.cursor = ic_ui_textfield_index_at(a, &br.addr, ev->x);
            br.addr.sel_start = br.addr.sel_end = br.addr.cursor;
            ic_ui_textfield_scroll(a, &br.addr);
            break;
        }
        
        {
            ic_rect_t p = page_rect(app);
            const ic_face_t *f = reading();
            int lh = f->line_h + 2;
            int col_w = p.w - 2 * IC_SP_6;
            int col_x;
            int row;
            if (col_w > 720) col_w = 720;
            col_x = p.x + (p.w - col_w) / 2;
            if (ic_ui_hit(p, ev->x, ev->y) && br.text) {
                uint64_t ls, le, target;
                int col = 0;
                row = (ev->y - p.y - IC_SP_5 + br.scroll) / lh;
                if (row < 0) break;
                ls = line_offset((uint64_t)row);
                le = ls;
                while (le < br.text_len && br.text[le] != '\n') le++;
                
                {
                    int x = col_x;
                    target = le;
                    for (uint64_t k = ls; k < le; k++) {
                        int w = ic_text_measure_n(f, br.text + k, 1);
                        if (ev->x < x + w / 2) { target = k; break; }
                        x += w;
                    }
                    col = (int)(target - ls);
                }
                for (i = 0; i < br.link_count; i++) {
                    if (br.links[i].url[0] && target >= br.links[i].start &&
                        target < br.links[i].end) {
                        navigate_to(br.links[i].url);
                        break;
                    }
                }
                (void)col;
            }
        }
        break;
    }
    case IC_EV_MOUSE_LEAVE:
        br.hover_back = br.hover_forward = br.hover_reload = br.hover_go = 0;
        br.hover_link = -1;
        break;
    case IC_EV_SCROLL:
        br.scroll += ev->wheel * 3 * (reading()->line_h + 2);
        break;
    case IC_EV_KEY:
        if ((ev->mods & IC_MOD_CTRL) && ev->key == 'l') {
            br.addr_focused = 1;
            break;
        }
        if (ev->mods & IC_MOD_ALT) {
            if (ev->key == IC_KEY_LEFT) go_back();
            else if (ev->key == IC_KEY_RIGHT) go_forward();
            break;
        }
        if (br.addr_focused) {
            switch (ev->key) {
            case IC_KEY_ENTER:
                br.addr_focused = 0;
                navigate_to(br.addr.text);
                break;
            case IC_KEY_ESCAPE:
                br.addr_focused = 0;
                set_address(br.current_url);
                break;
            case IC_KEY_LEFT:
                if (br.addr.cursor > 0) br.addr.cursor--;
                ic_ui_textfield_scroll(a, &br.addr);
                break;
            case IC_KEY_RIGHT:
                if (br.addr.cursor < (int)ic_strlen(br.addr_buf)) br.addr.cursor++;
                ic_ui_textfield_scroll(a, &br.addr);
                break;
            case IC_KEY_HOME: br.addr.cursor = 0; ic_ui_textfield_scroll(a, &br.addr); break;
            case IC_KEY_END:
                br.addr.cursor = (int)ic_strlen(br.addr_buf);
                ic_ui_textfield_scroll(a, &br.addr);
                break;
            case IC_KEY_BACKSPACE: {
                int len = (int)ic_strlen(br.addr_buf);
                if (br.addr.cursor > 0) {
                    for (int k = br.addr.cursor; k < len; k++) br.addr_buf[k - 1] = br.addr_buf[k];
                    br.addr_buf[len - 1] = 0;
                    br.addr.cursor--;
                    ic_ui_textfield_scroll(a, &br.addr);
                }
                break;
            }
            case IC_KEY_DELETE: {
                int len = (int)ic_strlen(br.addr_buf);
                if (br.addr.cursor < len) {
                    for (int k = br.addr.cursor; k < len - 1; k++) br.addr_buf[k] = br.addr_buf[k + 1];
                    br.addr_buf[len - 1] = 0;
                    ic_ui_textfield_scroll(a, &br.addr);
                }
                break;
            }
            default:
                if (ev->key >= 32 && ev->key < 127 && !(ev->mods & (IC_MOD_CTRL | IC_MOD_ALT))) {
                    int len = (int)ic_strlen(br.addr_buf);
                    if (len + 1 < BR_URL_CAP) {
                        for (int k = len; k > br.addr.cursor; k--) {
                            br.addr_buf[k] = br.addr_buf[k - 1];
                        }
                        br.addr_buf[br.addr.cursor++] = (char)ev->key;
                        br.addr_buf[len + 1] = 0;
                        ic_ui_textfield_scroll(a, &br.addr);
                    }
                }
                break;
            }
            break;
        }
        switch (ev->key) {
        case IC_KEY_PAGE_DOWN: br.scroll += page_rect(app).h - IC_H_TOOLBAR; break;
        case IC_KEY_PAGE_UP:   br.scroll -= page_rect(app).h - IC_H_TOOLBAR; break;
        case IC_KEY_DOWN:      br.scroll += reading()->line_h + 2; break;
        case IC_KEY_UP:        br.scroll -= reading()->line_h + 2; break;
        case IC_KEY_HOME:      br.scroll = 0; break;
        case IC_KEY_END:       br.scroll = br.content_h; break;
        case IC_KEY_ENTER:     navigate_to(br.addr.text); break;
        case IC_KEY_ESCAPE:    br.addr_focused = 1; break;
        case 'l': case 'L':    br.addr_focused = 1; break;
        case 'r': case 'R':    navigate_to(br.current_url); break;
        case IC_KEY_LEFT:      go_back(); break;
        case IC_KEY_DELETE:    go_forward(); break;
        default: break;
        }
        break;
    case IC_EV_RESIZE:
        

        layout(app);
        if (br.html) {
            build_render_text();
            layout(app);
        }
        break;
    case IC_EV_BLUR:
        br.addr_focused = 0;
        break;
    case IC_EV_FOCUS:
    case IC_EV_APPEARANCE:
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    br.hover_back = br.hover_forward = br.hover_reload = br.hover_go = 0;
    br.hover_link = -1;
    br.addr.leading = IC_SYM_SEARCH;
    br.addr.scroll_px = 0;
    ic_tween_set(&br.scrollbar, 0.0f);
    if (app->user) {
        const char *arg = (const char *)app->user;
        if (arg[0]) {
            set_address(arg);
            navigate_to(arg);
            return;
        }
    }
    set_address("http://example.com");
    layout(app);
    br_status("Ready");
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Browser", WIN_W, WIN_H, init, draw, event, tick };
    const char *arg = (argc > 1 && argv) ? argv[1] : 0;

    

    br.html_shm = icda_shm_create(BR_HTML_CAP);
    if (br.html_shm) br.html = (char *)(uintptr_t)icda_shm_map(br.html_shm);
    br.text_shm = icda_shm_create(BR_TEXT_CAP);
    if (br.text_shm) br.text = (char *)(uintptr_t)icda_shm_map(br.text_shm);

    br.history_pos = -1;
    br.history_count = 0;
    br.scroll = 0;
    br.loading = 0;
    br.addr_focused = 0;
    ic_strcpy(br.title, "ICDA Browser", BR_TITLE_CAP);
    br_status("Ready");

    if (ic_app_run(&desc, (void *)arg) != 0) {
        icda_write("browser requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
