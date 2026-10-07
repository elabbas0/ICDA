/* Media: ICDA's player and viewer, replacing Music.
 *
 * One app, a different face per kind of file:
 *   Library  music, videos and photos found under /home, and YouTube
 *   Photos   a dark full-window viewer: fit or 1:1, previous / next in the
 *            folder, animated GIFs
 *   Music    a now-playing card: title, progress, play/pause, previous and
 *            next through the folder, the queue beside it
 *   Video    the picture with controls that hide while it plays (video.c)
 *
 * Sound goes to the kernel mixer as a PCM stream, so it plays together with
 * everything else; decoding happens a little at a time from the tick. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "libicda.h"
#include "adec.h"
#include "image.h"
#include "media.h"

#define WIN_W        980
#define WIN_H        620
#define LIB_MAX      512
#define NAME_CAP     128
#define PATH_CAP     512
#define FEED_MS      400      /* keep this much audio queued in the mixer */

enum { MODE_LIBRARY = 0, MODE_PHOTO, MODE_AUDIO, MODE_VIDEO };
enum { TAB_MUSIC = 0, TAB_VIDEOS, TAB_PHOTOS, TAB_YOUTUBE };

typedef struct {
    char path[PATH_CAP];
    char name[NAME_CAP];
    int  kind;              /* MEDIA_AUDIO / MEDIA_VIDEO / MEDIA_PHOTO */
} item_t;

static struct {
    ic_app_t *app;
    int       mode;
    int       tab;
    char      notice[160];

    /* library */
    item_t    lib[LIB_MAX];
    int       nlib;
    int       lib_scroll, lib_hover;
    int       hover_tab;

    /* the folder of the open file, for previous / next */
    item_t    queue[LIB_MAX];
    int       nqueue, qpos;

    /* photo */
    image_t   img;
    image_t  *frames;
    int      *delays;
    int       nframes, frame;
    uint32_t  frame_ms;
    int       zoom_actual;  /* 1:1 instead of fit */

    /* audio */
    adec_t   *dec;
    long      stream;
    int       paused;
    uint64_t  base_frame;   /* source frame at the stream's start */
    int       ended;
    int       hover_ctl;
    int       dragging;
} m;

/* ---- helpers ----------------------------------------------------------------- */

static int ends_with_ci(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    if (a < b) return 0;
    for (size_t i = 0; i < b; i++) {
        char c = s[a - b + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != suf[i]) return 0;
    }
    return 1;
}

int media_kind_of(const char *name) {
    static const char *const photo[] = { ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg" };
    static const char *const video[] = { ".mp4", ".mov", ".m4v", ".webm", ".mkv" };
    if (adec_is_audio_name(name)) return MEDIA_AUDIO;
    for (size_t i = 0; i < sizeof photo / sizeof photo[0]; i++)
        if (ends_with_ci(name, photo[i])) return MEDIA_PHOTO;
    for (size_t i = 0; i < sizeof video / sizeof video[0]; i++)
        if (ends_with_ci(name, video[i])) return MEDIA_VIDEO;
    return 0;
}

static const char *base_name(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* the whole file in a malloc'd buffer */
uint8_t *media_read_file(const char *path, size_t *len) {
    icda_stat_t st;
    uint8_t *buf;
    long n;
    if ((long)icda_stat(path, &st) < 0 || st.size == 0 || st.size > (512ull << 20)) return 0;
    buf = (uint8_t *)malloc(st.size + 1);
    if (!buf) return 0;
    n = (long)icda_read_file(path, (char *)buf, st.size + 1);
    if (n <= 0) {
        free(buf);
        return 0;
    }
    *len = (size_t)n;
    return buf;
}

static void fmt_time(uint64_t s, char *out, size_t cap) {
    if (s >= 3600) snprintf(out, cap, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, cap, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* ---- library ------------------------------------------------------------------- */

static void scan_dir(const char *dir, int depth) {
    static char buf[32768];
    char *list;
    long n;
    if (depth > 4 || m.nlib >= LIB_MAX) return;
    n = (long)icda_list_dir(dir, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    list = strdup(buf);              /* the static buffer is reused by recursion */
    if (!list) return;
    for (char *line = list; *line && m.nlib < LIB_MAX; ) {
        char *e = strchr(line, '\n');
        char path[PATH_CAP];
        size_t len;
        if (e) *e = 0;
        len = strlen(line);
        if (len && line[0] != '.') {
            if (line[len - 1] == '/') {
                line[len - 1] = 0;
                snprintf(path, sizeof path, "%s/%s", dir, line);
                scan_dir(path, depth + 1);
            } else {
                int k = media_kind_of(line);
                if (k) {
                    item_t *it = &m.lib[m.nlib++];
                    snprintf(it->path, sizeof it->path, "%s/%s", dir, line);
                    snprintf(it->name, sizeof it->name, "%s", line);
                    it->kind = k;
                }
            }
        }
        if (!e) break;
        line = e + 1;
    }
    free(list);
}

static void load_library(void) {
    m.nlib = 0;
    m.lib_scroll = 0;
    scan_dir("/home", 0);
}

static int tab_kind(int tab) {
    return tab == TAB_MUSIC ? MEDIA_AUDIO : tab == TAB_VIDEOS ? MEDIA_VIDEO : MEDIA_PHOTO;
}

/* items of the current tab, in order */
static int lib_visible(int *out, int cap) {
    int n = 0;
    for (int i = 0; i < m.nlib && n < cap; i++)
        if (m.lib[i].kind == tab_kind(m.tab)) out[n++] = i;
    return n;
}

/* ---- closing what is open ------------------------------------------------------- */

static void stop_audio(void) {
    if (m.stream > 0) icda_audio_stream_close(m.stream);
    m.stream = 0;
    if (m.dec) adec_close(m.dec);
    m.dec = 0;
}

static void close_photo(void) {
    image_release(&m.img);
    for (int i = 0; i < m.nframes; i++) image_release(&m.frames[i]);
    free(m.frames);
    free(m.delays);
    m.frames = 0;
    m.delays = 0;
    m.nframes = 0;
}

static void close_all(void) {
    stop_audio();
    close_photo();
    video_close();
}

/* the open file's folder, filtered to its kind, becomes the queue */
static void build_queue(const char *path, int kind) {
    char dir[PATH_CAP];
    static char buf[32768];
    long n;
    char *slash;
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    m.nqueue = 0;
    m.qpos = 0;
    if (!slash) return;
    if (slash == dir) slash[1] = 0;
    else *slash = 0;
    n = (long)icda_list_dir(dir, buf, sizeof buf - 1);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *line = buf; *line && m.nqueue < LIB_MAX; ) {
        char *e = strchr(line, '\n');
        if (e) *e = 0;
        if (*line && line[strlen(line) - 1] != '/' && media_kind_of(line) == kind) {
            item_t *it = &m.queue[m.nqueue];
            snprintf(it->path, sizeof it->path, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", line);
            snprintf(it->name, sizeof it->name, "%s", line);
            it->kind = kind;
            if (!strcmp(it->path, path)) m.qpos = m.nqueue;
            m.nqueue++;
        }
        if (!e) break;
        line = e + 1;
    }
}

/* ---- opening ----------------------------------------------------------------------- */

static int open_photo(const char *path) {
    size_t len = 0;
    uint8_t *data = media_read_file(path, &len);
    if (!data) return -1;
    if (image_decode_gif_frames(data, len, &m.frames, &m.delays, &m.nframes) == 0 && m.nframes > 1) {
        m.frame = 0;
        m.frame_ms = ic_time_ms();
    } else {
        m.nframes = 0;
        if (image_decode(data, len, &m.img) != 0) {
            free(data);
            return -1;
        }
    }
    free(data);
    m.zoom_actual = 0;
    return 0;
}

static int start_stream_at(uint64_t frame) {
    const adec_info_t *in = adec_info(m.dec);
    if (m.stream > 0) icda_audio_stream_close(m.stream);
    m.stream = icda_audio_stream_open(in->rate, (uint32_t)in->channels);
    if (m.stream <= 0) {
        m.stream = 0;
        return -1;
    }
    if (frame) adec_seek(m.dec, frame);
    m.base_frame = frame;
    m.ended = 0;
    if (m.paused) icda_audio_stream_control(m.stream, 1, 256);
    return 0;
}

static int open_audio(const char *path) {
    size_t len = 0;
    uint8_t *data = media_read_file(path, &len);
    if (!data) return -1;
    m.dec = adec_open_memory(data, len, path);
    if (!m.dec) {
        free(data);
        snprintf(m.notice, sizeof m.notice, "%s: this format or codec is not supported yet", base_name(path));
        return -1;
    }
    m.paused = 0;
    if (start_stream_at(0) != 0) {
        snprintf(m.notice, sizeof m.notice, "No sound device is available");
        return 0;     /* still show the track */
    }
    return 0;
}

static void open_path(const char *path, int rebuild_queue) {
    int kind = media_kind_of(path);
    m.notice[0] = 0;
    close_all();
    if (rebuild_queue) build_queue(path, kind);
    if (kind == MEDIA_PHOTO && open_photo(path) == 0) m.mode = MODE_PHOTO;
    else if (kind == MEDIA_AUDIO && open_audio(path) == 0) m.mode = MODE_AUDIO;
    else if (kind == MEDIA_VIDEO && video_open(path, m.notice, sizeof m.notice) == 0) m.mode = MODE_VIDEO;
    else {
        if (!m.notice[0]) snprintf(m.notice, sizeof m.notice, "%s could not be opened", base_name(path));
        m.mode = MODE_LIBRARY;
    }
    ic_app_invalidate(m.app);
}

static void step_queue(int dir) {
    if (m.nqueue < 2) return;
    m.qpos = (m.qpos + dir + m.nqueue) % m.nqueue;
    open_path(m.queue[m.qpos].path, 0);
}

/* ---- audio feeding --------------------------------------------------------------- */

static uint64_t audio_position(void) {
    long played;
    if (!m.dec) return 0;
    played = m.stream > 0 ? icda_audio_stream_position(m.stream) : 0;
    return m.base_frame + (uint64_t)(played > 0 ? played : 0);
}

static void feed_audio(void) {
    const adec_info_t *in;
    int16_t pcm[4096 * 2];
    if (!m.dec || m.stream <= 0 || m.paused || m.ended) return;
    in = adec_info(m.dec);
    for (int rounds = 0; rounds < 8; rounds++) {
        long queued = icda_audio_stream_queued(m.stream);
        long frames, sent = 0;
        if (queued < 0 || (uint64_t)queued * 1000 / in->rate >= FEED_MS) break;
        frames = adec_read(m.dec, pcm, 4096);
        if (frames <= 0) {
            m.ended = 1;
            break;
        }
        while (sent < frames) {
            long took = icda_audio_stream_write(m.stream, pcm + sent * in->channels,
                                                (uint64_t)(frames - sent) * (uint64_t)in->channels * 2);
            if (took <= 0) break;
            sent += took / (in->channels * 2);
        }
    }
}

/* ---- drawing: library ------------------------------------------------------------- */

static const char *const tab_names[4] = { "Music", "Videos", "Photos", "YouTube" };

static ic_rect_t tabs_rect(ic_app_t *app) {
    return ic_rect_make((app->width - 420) / 2, 16, 420, IC_H_CONTROL);
}

static ic_rect_t lib_list_rect(ic_app_t *app) {
    return ic_rect_make(32, 72, app->width - 64, app->height - 72 - 24);
}

#define LIB_ROW_H 46

static void draw_library(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t l = lib_list_rect(app);
    int vis[LIB_MAX], n, rows;
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    ic_ui_segmented(c, tabs_rect(app), tab_names, 4, (float)m.tab, m.hover_tab);
    if (m.tab == TAB_YOUTUBE) {
        youtube_draw(app, c, l);
        return;
    }
    n = lib_visible(vis, LIB_MAX);
    if (m.notice[0]) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(l.x, l.y - 4, l.w, 16), m.notice, p->danger,
                        IC_ALIGN_LEFT);
        l.y += 18;
        l.h -= 18;
    }
    if (!n) {
        ic_ui_empty_state(c, l, m.tab == TAB_MUSIC ? IC_SYM_MUSIC : m.tab == TAB_PHOTOS ? IC_SYM_DOCUMENT : IC_SYM_PLAY,
                          m.tab == TAB_MUSIC ? "No music yet" : m.tab == TAB_PHOTOS ? "No photos yet" : "No videos yet",
                          "Files under /home show up here. Surfer saves downloads to /home/Downloads.");
        return;
    }
    rows = l.h / LIB_ROW_H;
    if (m.lib_scroll > n - rows) m.lib_scroll = n - rows > 0 ? n - rows : 0;
    {
        ic_rect_t g = l;
        int count = n - m.lib_scroll < rows ? n - m.lib_scroll : rows;
        g.h = count * LIB_ROW_H;
        ic_ui_group(c, g);
        for (int i = 0; i < count; i++) {
            item_t *it = &m.lib[vis[m.lib_scroll + i]];
            ic_rect_t r = ic_rect_make(l.x, l.y + i * LIB_ROW_H, l.w, LIB_ROW_H);
            ic_symbol_t sym = it->kind == MEDIA_AUDIO ? IC_SYM_MUSIC : it->kind == MEDIA_VIDEO ? IC_SYM_PLAY : IC_SYM_DOCUMENT;
            ic_ui_group_row(c, r, i, count, m.lib_hover == i ? 1.0f : 0.0f);
            ic_ui_row_text(c, r, sym, p->accent, it->name, it->path);
        }
        if (n > rows) ic_ui_scrollbar(c, ic_rect_make(l.x, l.y, l.w, rows * LIB_ROW_H), m.lib_scroll * LIB_ROW_H,
                                      n * LIB_ROW_H, 1.0f);
    }
}

/* ---- drawing: photo -------------------------------------------------------------- */

static ic_rect_t photo_bar(ic_app_t *app) {
    return ic_rect_make(0, app->height - 52, app->width, 52);
}

static ic_rect_t photo_btn(ic_app_t *app, int k) {
    /* 0 library, 1 previous, 2 next, 3 zoom */
    ic_rect_t b = photo_bar(app);
    if (k == 0) return ic_rect_make(b.x + 12, b.y + 10, 32, 32);
    if (k == 3) return ic_rect_make(b.x + b.w - 44, b.y + 10, 32, 32);
    return ic_rect_make(b.x + b.w / 2 + (k == 1 ? -44 : 12), b.y + 10, 32, 32);
}

static void draw_photo(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    image_t *im = m.nframes ? &m.frames[m.frame] : &m.img;
    ic_rect_t area = ic_rect_make(0, 0, app->width, app->height - 52);
    ic_rect_t bar = photo_bar(app);
    char info[200];
    ic_gfx_fill(c, 0, 0, app->width, app->height, 0xFF101114u);
    if (im->argb && im->w > 0 && im->h > 0) {
        float k = 1.0f, kw = (float)(area.w - 32) / im->w, kh = (float)(area.h - 32) / im->h;
        int dw, dh;
        if (!m.zoom_actual) {
            k = kw < kh ? kw : kh;
            if (k > 4.0f) k = 4.0f;
        }
        dw = (int)(im->w * k);
        dh = (int)(im->h * k);
        ic_gfx_blit_scaled(c, area.x + (area.w - dw) / 2, area.y + (area.h - dh) / 2, dw, dh, im->argb, im->w,
                           im->h, im->w, 0.0f, 255);
    }
    ic_gfx_fill(c, bar.x, bar.y, bar.w, bar.h, 0xFF17181Au);
    ic_ui_icon_button(c, photo_btn(app, 0), IC_SYM_GRID, m.hover_ctl == 0 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, photo_btn(app, 1), IC_SYM_CHEVRON_LEFT,
                      m.nqueue < 2 ? IC_STATE_DISABLED : m.hover_ctl == 1 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, photo_btn(app, 2), IC_SYM_CHEVRON_RIGHT,
                      m.nqueue < 2 ? IC_STATE_DISABLED : m.hover_ctl == 2 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, photo_btn(app, 3), m.zoom_actual ? IC_SYM_RESTORE : IC_SYM_MAXIMIZE,
                      m.hover_ctl == 3 ? IC_STATE_HOVER : IC_STATE_NORMAL);
    snprintf(info, sizeof info, "%s   %d x %d%s", m.nqueue ? m.queue[m.qpos].name : "", im->w, im->h,
             m.nframes ? "   animated" : "");
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(56, bar.y, bar.w / 2 - 110, bar.h), info,
                    p->label_secondary, IC_ALIGN_LEFT);
    if (m.nqueue > 1) {
        snprintf(info, sizeof info, "%d of %d", m.qpos + 1, m.nqueue);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(bar.w / 2 + 60, bar.y, 120, bar.h), info,
                        p->label_tertiary, IC_ALIGN_LEFT);
    }
}

/* ---- drawing: music ---------------------------------------------------------------- */

static ic_rect_t card_rect(ic_app_t *app) {
    int w = app->width - 300 - 72;
    if (w < 360) w = app->width - 64;
    return ic_rect_make(32, 32, w, app->height - 64);
}

static ic_rect_t seek_rect(ic_app_t *app) {
    ic_rect_t k = card_rect(app);
    return ic_rect_make(k.x + 40, k.y + k.h - 150, k.w - 80, 20);
}

static ic_rect_t audio_btn(ic_app_t *app, int i) {
    /* 0 previous, 1 play/pause, 2 next, 3 library */
    ic_rect_t k = card_rect(app);
    int cx = k.x + k.w / 2, y = k.y + k.h - 100;
    if (i == 3) return ic_rect_make(k.x + 16, k.y + 16, 32, 32);
    if (i == 1) return ic_rect_make(cx - 32, y - 4, 64, 64);
    return ic_rect_make(cx + (i == 0 ? -110 : 70), y + 12, 40, 40);
}

static uint32_t hue_color(uint32_t h, int dark) {
    static const uint32_t pal[6][2] = { { 0x3A5BD9, 0x1B2453 }, { 0x9B4DCA, 0x2E1745 }, { 0xD9486A, 0x4A1426 },
                                        { 0x2FA37E, 0x0F3A2C }, { 0xE07B39, 0x4A2410 }, { 0x2B8FC7, 0x0E3047 } };
    return 0xFF000000u | pal[h % 6][dark];
}

static void draw_audio(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    const adec_info_t *in = m.dec ? adec_info(m.dec) : 0;
    ic_rect_t k = card_rect(app), s = seek_rect(app);
    const char *name = m.nqueue ? m.queue[m.qpos].name : "";
    uint32_t h = 0;
    char t1[16], t2[16], sub[160];
    uint64_t pos = audio_position(), total = in ? in->frames : 0;
    float frac;
    for (const char *q = name; *q; q++) h = h * 31 + (uint8_t)*q;
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    ic_gfx_rrect_gradient_v(c, k.x, k.y, k.w, k.h, 18.0f, hue_color(h, 0), hue_color(h, 1));
    /* artwork: a large soft disc with the note */
    {
        int d = k.h - 300 < 220 ? k.h - 300 : 220;
        if (d > 60) {
            int cx = k.x + k.w / 2, cy = k.y + 40 + d / 2;
            ic_gfx_rrect(c, cx - d / 2, cy - d / 2, d, d, 24.0f, 0x33FFFFFFu);
            ic_symbol_draw(c, IC_SYM_MUSIC, (float)cx, (float)cy, (float)d * 0.45f, 0xE6FFFFFFu);
        }
    }
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE1), ic_rect_make(k.x + 24, k.y + k.h - 230, k.w - 48, 32), name,
                    0xFFFFFFFFu, IC_ALIGN_CENTER);
    snprintf(sub, sizeof sub, "%s   %u Hz %s", in ? in->format : "", in ? (unsigned)in->rate : 0,
             in && in->channels == 2 ? "stereo" : "mono");
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(k.x + 24, k.y + k.h - 192, k.w - 48, 18), sub,
                    0xB3FFFFFFu, IC_ALIGN_CENTER);
    frac = total ? (float)pos / (float)total : 0;
    if (frac > 1) frac = 1;
    ic_gfx_rrect(c, s.x, s.y + 8, s.w, 4, 2.0f, 0x40FFFFFFu);
    ic_gfx_rrect(c, s.x, s.y + 8, (int)(s.w * frac), 4, 2.0f, 0xFFFFFFFFu);
    ic_gfx_rrect(c, s.x + (int)(s.w * frac) - 6, s.y + 4, 12, 12, 6.0f, 0xFFFFFFFFu);
    fmt_time(in && in->rate ? pos / in->rate : 0, t1, sizeof t1);
    fmt_time(in && in->rate ? total / in->rate : 0, t2, sizeof t2);
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(s.x, s.y + 20, 80, 16), t1, 0xB3FFFFFFu, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(s.x + s.w - 80, s.y + 20, 80, 16), t2, 0xB3FFFFFFu,
                    IC_ALIGN_RIGHT);
    {
        ic_rect_t b = audio_btn(app, 1);
        ic_gfx_rrect(c, b.x, b.y, b.w, b.h, 32.0f, m.hover_ctl == 1 ? 0xFFFFFFFFu : 0xF2FFFFFFu);
        ic_symbol_draw(c, m.paused || m.ended ? IC_SYM_PLAY : IC_SYM_PAUSE, (float)(b.x + b.w / 2), (float)(b.y + b.h / 2),
                       26.0f, hue_color(h, 1));
        for (int i = 0; i <= 2; i += 2) {
            ic_rect_t r = audio_btn(app, i);
            ic_symbol_draw(c, i == 0 ? IC_SYM_CHEVRON_LEFT : IC_SYM_CHEVRON_RIGHT, (float)(r.x + r.w / 2),
                           (float)(r.y + r.h / 2), 22.0f, m.nqueue > 1 ? (m.hover_ctl == i ? 0xFFFFFFFFu : 0xCCFFFFFFu) : 0x55FFFFFFu);
        }
        b = audio_btn(app, 3);
        ic_symbol_draw(c, IC_SYM_GRID, (float)(b.x + 16), (float)(b.y + 16), 18.0f, m.hover_ctl == 3 ? 0xFFFFFFFFu : 0xB3FFFFFFu);
    }
    /* the queue */
    if (k.w < app->width - 64) {
        ic_rect_t q = ic_rect_make(k.x + k.w + 24, 32, app->width - k.x - k.w - 56, app->height - 64);
        ic_ui_section_header(c, q.x, q.y + 4, "Up next");
        for (int i = 0; i < m.nqueue && 34 + i * 34 < q.h; i++) {
            ic_rect_t r = ic_rect_make(q.x, q.y + 28 + i * 34, q.w, 32);
            if (i == m.qpos) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 8.0f, p->fill_selected_idle);
            ic_text_draw_in(c, ic_font(i == m.qpos ? IC_FONT_BODY_EMPH : IC_FONT_BODY), ic_rect_make(r.x + 10, r.y, r.w - 20, r.h),
                            m.queue[i].name, i == m.qpos ? p->label : p->label_secondary, IC_ALIGN_LEFT);
        }
    }
    if (m.notice[0]) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(k.x + 24, k.y + 18, k.w - 48, 16), m.notice,
                        0xFFFFFFFFu, IC_ALIGN_CENTER);
    }
}

/* ---- frame ------------------------------------------------------------------------- */

static void draw(ic_app_t *app, ic_canvas_t *c) {
    if (m.mode == MODE_PHOTO) draw_photo(app, c);
    else if (m.mode == MODE_AUDIO) draw_audio(app, c);
    else if (m.mode == MODE_VIDEO) video_draw(app, c);
    else draw_library(app, c);
}

static void tick(ic_app_t *app) {
    if (m.mode == MODE_AUDIO) {
        static uint64_t last_sec = ~0ull;
        const adec_info_t *in = m.dec ? adec_info(m.dec) : 0;
        uint64_t sec = in && in->rate ? audio_position() / in->rate : 0;
        feed_audio();
        if (sec != last_sec) {
            last_sec = sec;
            ic_app_invalidate(app);
        }
        /* at the end of a track, the next one in the folder */
        if (m.ended && m.stream > 0 && icda_audio_stream_queued(m.stream) <= 1 && m.nqueue > 1 && m.qpos + 1 < m.nqueue)
            step_queue(1);
    } else if (m.mode == MODE_PHOTO && m.nframes > 1) {
        uint32_t now = ic_time_ms();
        if (now - m.frame_ms >= (uint32_t)m.delays[m.frame]) {
            m.frame = (m.frame + 1) % m.nframes;
            m.frame_ms = now;
            ic_app_invalidate(app);
        }
    } else if (m.mode == MODE_VIDEO) {
        if (video_tick(app)) ic_app_invalidate(app);
    } else if (m.mode == MODE_LIBRARY && m.tab == TAB_YOUTUBE) {
        char url[PATH_CAP];
        if (youtube_tick(app, url, sizeof url)) ic_app_invalidate(app);
        if (url[0]) {
            close_all();
            if (video_open_url(url, youtube_title(), m.notice, sizeof m.notice) == 0) m.mode = MODE_VIDEO;
            ic_app_invalidate(app);
        }
    }
}

static void seek_to_x(ic_app_t *app, int x) {
    ic_rect_t s = seek_rect(app);
    const adec_info_t *in = m.dec ? adec_info(m.dec) : 0;
    float f;
    if (!in || !in->frames) return;
    f = (float)(x - s.x) / (float)s.w;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    start_stream_at((uint64_t)(f * (float)in->frames));
}

static void toggle_pause(void) {
    if (m.mode == MODE_AUDIO) {
        if (m.ended) {
            start_stream_at(0);
            m.ended = 0;
            m.paused = 0;
            return;
        }
        m.paused = !m.paused;
        if (m.stream > 0) icda_audio_stream_control(m.stream, m.paused, 256);
    }
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    if (m.mode == MODE_VIDEO) {
        int r = video_event(app, ev);
        if (r == VIDEO_EV_LIBRARY) {
            close_all();
            m.mode = MODE_LIBRARY;
        }
        ic_app_invalidate(app);
        return;
    }
    if (m.mode == MODE_LIBRARY && m.tab == TAB_YOUTUBE && youtube_event(app, ev, lib_list_rect(app))) {
        ic_app_invalidate(app);
        if (ev->type != IC_EV_MOUSE_DOWN || !ic_ui_hit(tabs_rect(app), ev->x, ev->y)) return;
    }
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        if (m.mode == MODE_LIBRARY) {
            ic_rect_t l = lib_list_rect(app);
            m.hover_tab = ic_ui_segmented_hit(tabs_rect(app), 4, ev->x, ev->y);
            m.lib_hover = ic_ui_hit(l, ev->x, ev->y) ? (ev->y - l.y) / LIB_ROW_H : -1;
        } else if (m.mode == MODE_PHOTO) {
            m.hover_ctl = -1;
            for (int i = 0; i < 4; i++)
                if (ic_ui_hit(photo_btn(app, i), ev->x, ev->y)) m.hover_ctl = i;
        } else if (m.mode == MODE_AUDIO) {
            m.hover_ctl = -1;
            for (int i = 0; i < 4; i++)
                if (ic_ui_hit(audio_btn(app, i), ev->x, ev->y)) m.hover_ctl = i;
            if (m.dragging) seek_to_x(app, ev->x);
        }
        break;
    case IC_EV_MOUSE_DOWN:
        if (m.mode == MODE_LIBRARY) {
            int t = ic_ui_segmented_hit(tabs_rect(app), 4, ev->x, ev->y);
            ic_rect_t l = lib_list_rect(app);
            if (t >= 0) {
                m.tab = t;
                m.lib_scroll = 0;
                if (t == TAB_YOUTUBE) youtube_enter();
            } else if (ic_ui_hit(l, ev->x, ev->y)) {
                int vis[LIB_MAX], n = lib_visible(vis, LIB_MAX);
                int row = m.lib_scroll + (ev->y - l.y - (m.notice[0] ? 18 : 0)) / LIB_ROW_H;
                if (row >= 0 && row < n) open_path(m.lib[vis[row]].path, 1);
            }
        } else if (m.mode == MODE_PHOTO) {
            if (ic_ui_hit(photo_btn(app, 0), ev->x, ev->y)) {
                close_all();
                m.mode = MODE_LIBRARY;
            } else if (ic_ui_hit(photo_btn(app, 1), ev->x, ev->y)) step_queue(-1);
            else if (ic_ui_hit(photo_btn(app, 2), ev->x, ev->y)) step_queue(1);
            else if (ic_ui_hit(photo_btn(app, 3), ev->x, ev->y)) m.zoom_actual = !m.zoom_actual;
        } else if (m.mode == MODE_AUDIO) {
            ic_rect_t s = seek_rect(app);
            if (ic_ui_hit(audio_btn(app, 1), ev->x, ev->y)) toggle_pause();
            else if (ic_ui_hit(audio_btn(app, 0), ev->x, ev->y)) step_queue(-1);
            else if (ic_ui_hit(audio_btn(app, 2), ev->x, ev->y)) step_queue(1);
            else if (ic_ui_hit(audio_btn(app, 3), ev->x, ev->y)) {
                close_all();
                load_library();
                m.mode = MODE_LIBRARY;
            } else if (ic_ui_hit(ic_rect_make(s.x - 6, s.y - 6, s.w + 12, s.h + 12), ev->x, ev->y)) {
                m.dragging = 1;
                seek_to_x(app, ev->x);
            }
        }
        break;
    case IC_EV_MOUSE_UP:
        m.dragging = 0;
        break;
    case IC_EV_SCROLL:
        if (m.mode == MODE_LIBRARY) {
            m.lib_scroll += ev->wheel > 0 ? 2 : -2;
            if (m.lib_scroll < 0) m.lib_scroll = 0;
        }
        break;
    case IC_EV_KEY:
        if (ev->key == ' ') toggle_pause();
        else if (ev->key == IC_KEY_LEFT && m.mode == MODE_PHOTO) step_queue(-1);
        else if (ev->key == IC_KEY_RIGHT && m.mode == MODE_PHOTO) step_queue(1);
        else if (ev->key == IC_KEY_ESCAPE && m.mode != MODE_LIBRARY) {
            close_all();
            m.mode = MODE_LIBRARY;
        } else if (m.mode == MODE_AUDIO && (ev->key == IC_KEY_LEFT || ev->key == IC_KEY_RIGHT) && m.dec) {
            const adec_info_t *in = adec_info(m.dec);
            int64_t to = (int64_t)audio_position() + (ev->key == IC_KEY_RIGHT ? 10 : -10) * (int64_t)in->rate;
            if (to < 0) to = 0;
            start_stream_at((uint64_t)to);
        }
        break;
    default:
        break;
    }
    ic_app_invalidate(app);
}

static const char *start_path;

static void init(ic_app_t *app) {
    m.app = app;
    m.lib_hover = m.hover_tab = m.hover_ctl = -1;
    load_library();
    if (start_path && start_path[0]) open_path(start_path, 1);
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Media", WIN_W, WIN_H, init, draw, event, tick };
    start_path = argc > 1 && argv ? argv[1] : 0;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("media requires the desktop\n");
        return 1;
    }
    close_all();
    return 0;
}
