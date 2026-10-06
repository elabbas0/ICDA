/* Surfer downloads: the save sheet (file name, folder picker) and the
 * transfers themselves, pumped from the browser's tick. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "download.h"

#define DL_MAX        4
#define DL_DIR        "/home/Downloads"
#define NAME_CAP      128
#define PATH_CAP      512
#define FOLDERS_MAX   64
#define SHEET_W       460
#define SHEET_H       390
#define ROW_H         30

typedef struct {
    int         state;            /* 0 free, 1 waiting for a folder, 2 saving, 3 done, -1 failed, -2 cancelled */
    http_req_t *req;
    char        url[URL_CAP];
    char        name[NAME_CAP];
    char        path[PATH_CAP];
    uint64_t    written;          /* bytes in the file */
    int64_t     total;            /* Content-Length, -1 unknown */
    uint32_t    t_done;
} dl_t;

static dl_t dls[DL_MAX];
static char status_line[200];

/* save sheet */
static struct {
    int  open;
    int  dl;                      /* index in dls */
    char folder[PATH_CAP];
    char folders[FOLDERS_MAX][NAME_CAP];
    int  nfolders;
    int  scroll;
    int  hover;                   /* row, or -2 up, -3 cancel, -4 save */
} sh;

/* ---- helpers --------------------------------------------------------------- */

static int ieq_prefix(const char *s, const char *p) {
    for (; *p; s++, p++) {
        char a = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s;
        if (a != *p) return 0;
    }
    return 1;
}

/* the value of a response header, from the raw header block */
static int header(const http_req_t *r, const char *name, char *out, size_t cap) {
    const char *h = r->raw_headers;
    size_t nl = strlen(name);
    out[0] = 0;
    if (!h) return 0;
    while (*h) {
        const char *e = strchr(h, '\n');
        size_t len = e ? (size_t)(e - h) : strlen(h);
        if (len > nl && ieq_prefix(h, name) && h[nl] == ':') {
            const char *v = h + nl + 1;
            size_t vl;
            while (*v == ' ') v++;
            vl = (size_t)(h + len - v);
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            return 1;
        }
        if (!e) break;
        h = e + 1;
    }
    return 0;
}

static int hexv(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* keeps a file name safe for the VFS: no slashes or control characters */
static void clean_name(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|')
            continue;
        *o++ = (char)c;
    }
    *o = 0;
    while (s[0] == '.') memmove(s, s + 1, strlen(s));
    if (!s[0]) strcpy(s, "download");
}

/* Content-Disposition filename=, else the last path segment of the URL */
static void suggest_name(const http_req_t *r, const char *url, char *out) {
    char cd[300];
    out[0] = 0;
    if (r && header(r, "content-disposition", cd, sizeof cd)) {
        char *f = strstr(cd, "filename=");
        if (f) {
            f += 9;
            if (*f == '"') f++;
            snprintf(out, NAME_CAP, "%s", f);
            for (char *q = out; *q; q++)
                if (*q == '"' || *q == ';') { *q = 0; break; }
        }
    }
    if (!out[0]) {
        const char *p = url, *slash = url, *end;
        size_t o = 0;
        if (strstr(p, "://")) p = strstr(p, "://") + 3;
        for (const char *q = p; *q && *q != '?' && *q != '#'; q++)
            if (*q == '/') slash = q + 1;
        end = slash;
        while (*end && *end != '?' && *end != '#') end++;
        for (const char *q = slash; q < end && o + 1 < NAME_CAP; q++) {
            if (*q == '%' && hexv(q[1]) >= 0 && hexv(q[2]) >= 0) {
                out[o++] = (char)(hexv(q[1]) * 16 + hexv(q[2]));
                q += 2;
            } else {
                out[o++] = *q;
            }
        }
        out[o] = 0;
    }
    clean_name(out);
}

static void human(uint64_t b, char *out, size_t cap) {
    if (b >= 1024ull * 1024 * 1024) snprintf(out, cap, "%.1f GB", b / 1073741824.0);
    else if (b >= 1024 * 1024) snprintf(out, cap, "%.1f MB", b / 1048576.0);
    else if (b >= 1024) snprintf(out, cap, "%.0f KB", b / 1024.0);
    else snprintf(out, cap, "%u bytes", (unsigned)b);
}

/* ---- deciding ----------------------------------------------------------------- */

int dl_wanted(const http_req_t *r) {
    char cd[200];
    const char *t = r->content_type;
    if (header(r, "content-disposition", cd, sizeof cd) && ieq_prefix(cd, "attachment")) return 1;
    if (!t[0]) return 0;
    if (strstr(t, "html") || strstr(t, "xml") || ieq_prefix(t, "text/") || ieq_prefix(t, "image/")) return 0;
    if (strstr(t, "json") || strstr(t, "javascript")) return 0;
    return 1;            /* application/zip, octet-stream, audio, video, pdf, fonts ... */
}

/* ---- the folder picker ------------------------------------------------------------ */

static void load_folders(void) {
    static char buf[16384];
    long n = (long)icda_list_dir(sh.folder, buf, sizeof buf - 1);
    long pos = 0;
    sh.nfolders = 0;
    sh.scroll = 0;
    if (n <= 0) return;
    buf[n] = 0;
    while (pos < n && sh.nfolders < FOLDERS_MAX) {
        char *line = buf + pos;
        char *e = strchr(line, '\n');
        size_t len = e ? (size_t)(e - line) : strlen(line);
        pos += (long)len + 1;
        if (len > 1 && line[len - 1] == '/' && line[0] != '.') {
            if (len - 1 >= NAME_CAP) continue;
            memcpy(sh.folders[sh.nfolders], line, len - 1);
            sh.folders[sh.nfolders][len - 1] = 0;
            if (!strcmp(sh.folders[sh.nfolders], "dev") && !strcmp(sh.folder, "/")) continue;
            sh.nfolders++;
        }
    }
}

static void open_sheet(int i) {
    sh.open = 1;
    sh.dl = i;
    sh.hover = -1;
    (void)icda_mkdir("/home");
    (void)icda_mkdir(DL_DIR);
    snprintf(sh.folder, sizeof sh.folder, "%s", DL_DIR);
    load_folders();
}

static void enter_folder(const char *name) {
    size_t l = strlen(sh.folder);
    if (strcmp(sh.folder, "/") != 0 && l + 1 < sizeof sh.folder) strcat(sh.folder, "/");
    if (strlen(sh.folder) + strlen(name) + 1 < sizeof sh.folder) strcat(sh.folder, name);
    load_folders();
}

static void folder_up(void) {
    char *s = strrchr(sh.folder, '/');
    if (!s) return;
    if (s == sh.folder) sh.folder[1] = 0;
    else *s = 0;
    load_folders();
}

/* writes what has arrived so far to the file */
static int drain(dl_t *d) {
    http_req_t *r = d->req;
    if (!r || !r->body_len) return 0;
    if (icda_write_file_at(d->path, d->written, r->body, r->body_len) < 0) {
        d->state = -1;
        return -1;
    }
    d->written += r->body_len;
    r->body_len = 0;
    return 0;
}

static void choose_folder(void) {
    dl_t *d = &dls[sh.dl];
    char base[NAME_CAP], ext[32];
    char *dot;
    int n = 1;
    sh.open = 0;
    snprintf(base, sizeof base, "%s", d->name);
    ext[0] = 0;
    dot = strrchr(base, '.');
    if (dot && strlen(dot) < sizeof ext) {
        snprintf(ext, sizeof ext, "%s", dot);
        *dot = 0;
    }
    /* never overwrite: "name (2).zip" */
    for (;;) {
        char probe[8];
        if (n == 1) snprintf(d->path, sizeof d->path, "%s/%s%s", strcmp(sh.folder, "/") ? sh.folder : "", base, ext);
        else snprintf(d->path, sizeof d->path, "%s/%s (%d)%s", strcmp(sh.folder, "/") ? sh.folder : "", base, n, ext);
        if ((long)icda_read_file(d->path, probe, sizeof probe) < 0) break;
        n++;
    }
    if ((long)icda_write_file(d->path, "", 0) < 0) {
        d->state = -1;
        return;
    }
    d->state = 2;
    (void)drain(d);
}

static void cancel(dl_t *d) {
    if (d->req) http_free(d->req);
    d->req = 0;
    d->state = -2;
    d->t_done = ic_time_ms();
}

/* ---- starting ------------------------------------------------------------------- */

static int slot(void) {
    for (int i = 0; i < DL_MAX; i++)
        if (dls[i].state == 0 || dls[i].state == 3 || dls[i].state < 0) return i;
    return -1;
}

void dl_begin(http_req_t *r, const char *url) {
    int i = slot();
    dl_t *d;
    if (i < 0) {
        http_free(r);
        return;
    }
    d = &dls[i];
    memset(d, 0, sizeof *d);
    d->req = r;
    snprintf(d->url, sizeof d->url, "%s", url);
    suggest_name(r, url, d->name);
    d->total = r->content_length;
    d->state = 1;
    if (!sh.open) open_sheet(i);
}

void dl_begin_url(const char *url, const char *name) {
    http_req_t *r = http_open(url, "GET", 0);
    int i;
    if (!r) return;
    dl_begin(r, url);
    i = sh.dl;
    if (name && name[0] && dls[i].req == r) {
        snprintf(dls[i].name, sizeof dls[i].name, "%s", name);
        clean_name(dls[i].name);
    }
}

/* ---- running --------------------------------------------------------------------- */

int dl_busy(void) {
    for (int i = 0; i < DL_MAX; i++)
        if (dls[i].state == 1 || dls[i].state == 2) return 1;
    return 0;
}

int dl_tick(void) {
    int changed = 0;
    for (int i = 0; i < DL_MAX; i++) {
        dl_t *d = &dls[i];
        http_req_t *r = d->req;
        int st;
        if ((d->state != 1 && d->state != 2) || !r) continue;
        if (d->state == 1 && r->state == HTTP_DONE) continue;   /* complete in memory, waiting for a folder */
        st = http_poll(r, 0);
        if (r->headers_done && r->status >= 300 && r->status < 400 && r->location[0] && st == HTTP_DONE) {
            char next[URL_CAP];
            if (url_resolve(d->url, r->location, next, sizeof next) == 0) {
                http_free(r);
                d->req = http_open(next, "GET", 0);
                snprintf(d->url, sizeof d->url, "%s", next);
                changed = 1;
                continue;
            }
        }
        if (r->content_length >= 0) d->total = r->content_length;
        if (d->state == 2) {
            uint64_t before = d->written;
            if (drain(d) != 0) changed = 1;
            if (d->written != before) changed = 1;
        }
        if (st == HTTP_ERROR) {
            d->state = -1;
            d->t_done = ic_time_ms();
            changed = 1;
        } else if (st == HTTP_DONE && d->state == 2) {
            (void)drain(d);
            http_free(d->req);
            d->req = 0;
            d->state = 3;
            d->t_done = ic_time_ms();
            changed = 1;
        }
    }
    /* a sheet for the next download that is waiting */
    if (!sh.open)
        for (int i = 0; i < DL_MAX; i++)
            if (dls[i].state == 1) {
                open_sheet(i);
                changed = 1;
                break;
            }
    return changed;
}

const char *dl_status(void) {
    char a[32], b[32];
    int shown = -1;
    uint32_t now = ic_time_ms();
    for (int i = 0; i < DL_MAX; i++)
        if (dls[i].state == 2) shown = i;
    if (shown < 0)
        for (int i = 0; i < DL_MAX; i++)
            if ((dls[i].state == 3 || dls[i].state == -1) && now - dls[i].t_done < 8000) shown = i;
    if (shown < 0) return 0;
    {
        dl_t *d = &dls[shown];
        human(d->written, a, sizeof a);
        if (d->state == 3) {
            snprintf(status_line, sizeof status_line, "Saved %s to %s", d->name, d->path);
        } else if (d->state == -1) {
            snprintf(status_line, sizeof status_line, "Download of %s failed", d->name);
        } else if (d->total > 0) {
            human((uint64_t)d->total, b, sizeof b);
            snprintf(status_line, sizeof status_line, "Downloading %s  %d%%  (%s of %s)", d->name,
                     (int)(d->written * 100 / (uint64_t)d->total), a, b);
        } else {
            snprintf(status_line, sizeof status_line, "Downloading %s  (%s)", d->name, a);
        }
    }
    return status_line;
}

/* ---- the sheet --------------------------------------------------------------------- */

int dl_sheet_open(void) {
    return sh.open;
}

static ic_rect_t sheet_rect(ic_app_t *app) {
    return ic_rect_make((app->width - SHEET_W) / 2, (app->height - SHEET_H) / 2, SHEET_W, SHEET_H);
}

static ic_rect_t list_rect(ic_rect_t s) {
    return ic_rect_make(s.x + IC_SP_4, s.y + 104, s.w - 2 * IC_SP_4, SHEET_H - 104 - 64);
}

static ic_rect_t up_rect(ic_rect_t s) {
    return ic_rect_make(s.x + IC_SP_4, s.y + 66, 30, 30);
}

static ic_rect_t btn_rect(ic_rect_t s, int which) {
    int w = 110;
    return ic_rect_make(s.x + s.w - IC_SP_4 - (2 - which) * (w + IC_SP_2) + IC_SP_2, s.y + s.h - 48, w, IC_H_CONTROL);
}

void dl_draw(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t s, l;
    dl_t *d;
    char title[200], size[32];
    int rows;
    if (!sh.open) return;
    s = sheet_rect(app);
    l = list_rect(s);
    d = &dls[sh.dl];
    ic_gfx_fill(c, 0, 0, app->width, app->height, IC_BLACK_A(80));
    ic_theme_shadow(c, s.x, s.y, s.w, s.h, IC_R_PANEL, IC_ELEV_MENU);
    ic_gfx_rrect(c, s.x, s.y, s.w, s.h, IC_R_PANEL, ic_color_over(p->window, p->material_menu));
    ic_gfx_rrect_stroke(c, s.x, s.y, s.w, s.h, IC_R_PANEL, 1.0f, p->frame);
    snprintf(title, sizeof title, "Save \"%s\"", d->name);
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(s.x + IC_SP_4, s.y + IC_SP_4, s.w - 2 * IC_SP_4, 20),
                    title, p->label, IC_ALIGN_LEFT);
    if (d->total > 0) human((uint64_t)d->total, size, sizeof size);
    else snprintf(size, sizeof size, "size unknown");
    snprintf(title, sizeof title, "%s  -  %s", size, d->url);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(s.x + IC_SP_4, s.y + 40, s.w - 2 * IC_SP_4, 16),
                    title, p->label_secondary, IC_ALIGN_LEFT);
    ic_ui_icon_button(c, up_rect(s), IC_SYM_CHEVRON_LEFT, sh.hover == -2 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(s.x + IC_SP_4 + 38, s.y + 66, s.w - 2 * IC_SP_4 - 38, 30),
                    sh.folder, p->label, IC_ALIGN_LEFT);
    ic_ui_group(c, l);
    rows = l.h / ROW_H;
    if (!sh.nfolders) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), l, "No folders here", p->label_tertiary, IC_ALIGN_CENTER);
    }
    for (int i = 0; i < rows && sh.scroll + i < sh.nfolders; i++) {
        ic_rect_t r = ic_rect_make(l.x, l.y + i * ROW_H, l.w, ROW_H);
        if (sh.hover == i) ic_gfx_rrect(c, r.x + 2, r.y + 1, r.w - 4, r.h - 2, IC_R_CONTROL, p->fill_hover);
        ic_symbol_draw(c, IC_SYM_FOLDER, (float)(r.x + 18), (float)(r.y + r.h / 2), 16.0f, p->accent);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + 36, r.y, r.w - 44, r.h),
                        sh.folders[sh.scroll + i], p->label, IC_ALIGN_LEFT);
    }
    ic_ui_button(c, btn_rect(s, 0), "Cancel", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 sh.hover == -3 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_button(c, btn_rect(s, 1), "Save here", IC_SYM_NONE, IC_BUTTON_PRIMARY,
                 sh.hover == -4 ? IC_STATE_HOVER : IC_STATE_NORMAL);
}

static int hit(ic_app_t *app, int x, int y) {
    ic_rect_t s = sheet_rect(app), l = list_rect(s);
    int rows = l.h / ROW_H;
    if (ic_ui_hit(up_rect(s), x, y)) return -2;
    if (ic_ui_hit(btn_rect(s, 0), x, y)) return -3;
    if (ic_ui_hit(btn_rect(s, 1), x, y)) return -4;
    for (int i = 0; i < rows && sh.scroll + i < sh.nfolders; i++)
        if (ic_ui_hit(ic_rect_make(l.x, l.y + i * ROW_H, l.w, ROW_H), x, y)) return i;
    return -1;
}

int dl_event(ic_app_t *app, const ic_event_t *ev) {
    if (!sh.open) return 0;
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE: {
        int h = hit(app, ev->x, ev->y);
        if (h != sh.hover) sh.hover = h;
        return 1;
    }
    case IC_EV_MOUSE_DOWN: {
        int h = hit(app, ev->x, ev->y);
        if (h == -2) folder_up();
        else if (h == -3) {
            cancel(&dls[sh.dl]);
            sh.open = 0;
        } else if (h == -4) {
            choose_folder();
        } else if (h >= 0) {
            enter_folder(sh.folders[sh.scroll + h]);
        }
        sh.hover = hit(app, ev->x, ev->y);
        return 1;
    }
    case IC_EV_SCROLL: {
        ic_rect_t l = list_rect(sheet_rect(app));
        int rows = l.h / ROW_H;
        sh.scroll += ev->wheel > 0 ? 1 : -1;
        if (sh.scroll > sh.nfolders - rows) sh.scroll = sh.nfolders - rows;
        if (sh.scroll < 0) sh.scroll = 0;
        return 1;
    }
    case IC_EV_KEY:
        if (ev->key == IC_KEY_ESCAPE) {
            cancel(&dls[sh.dl]);
            sh.open = 0;
        } else if (ev->key == IC_KEY_ENTER) {
            choose_folder();
        }
        return 1;
    default:
        return 1;
    }
}
