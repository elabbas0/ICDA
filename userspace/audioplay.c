/*
 * audioplay.app - ICDA Music.
 *
 * The list + now-playing bar from docs/DESIGN.md: a source list of
 * tracks found in the media folders, a transport row, and a now-playing
 * bar pinned to the bottom with real progress from icda_audio_info.
 *
 * Discovery walks /usr/share/audio and /home for .wav files, plus any
 * path passed on the command line (the Explorer "Open With" path).  The
 * `audio` setting is a master switch: with it off, Play explains itself
 * instead of failing silently.
 */
#include "libicda.h"
#include "settings_store.h"

#define WIN_W 560
#define WIN_H 420

#define AP_MAX_TRACKS  64
#define AP_NAME_CAP    96
#define AP_PATH_CAP    192
#define AP_STATUS_CAP  128
#define AP_STATUS_H    24
#define AP_NOWPLAY_H   64

typedef struct {
    char name[AP_NAME_CAP];
    char path[AP_PATH_CAP];
} ap_track_t;

static struct {
    ap_track_t tracks[AP_MAX_TRACKS];
    int        count;
    int        selected;
    int        scroll;

    /* view */
    int rows;
    int first_row;
    int last_row;

    /* pointer */
    int hover_row;
    int hover_play;
    int hover_stop;
    int hover_refresh;
    int list_focused;

    char status[AP_STATUS_CAP];
    int  playing;              /* the kernel reports an active stream */
} ap;

/* ------------------------------------------------------------- layout */

static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t list_rect(ic_app_t *app) {
    int y = IC_H_TOOLBAR;
    return ic_rect_make(IC_SP_4, y, app->width - 2 * IC_SP_4,
                        app->height - y - AP_NOWPLAY_H - AP_STATUS_H);
}

static ic_rect_t row_rect(ic_app_t *app, int row) {
    ic_rect_t l = list_rect(app);
    return ic_rect_make(l.x, l.y + row * IC_H_ROW, l.w, IC_H_ROW);
}

static ic_rect_t status_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - AP_STATUS_H, app->width, AP_STATUS_H);
}

static ic_rect_t nowplaying_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - AP_STATUS_H - AP_NOWPLAY_H, app->width,
                        AP_NOWPLAY_H);
}

static ic_rect_t play_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    int w = ic_ui_button_width("Play", IC_SYM_PLAY);
    return ic_rect_make(b.x + IC_SP_4, (b.h - IC_H_CONTROL_SM) / 2, w, IC_H_CONTROL_SM);
}

static ic_rect_t stop_rect(ic_app_t *app) {
    ic_rect_t r = play_rect(app);
    int w = ic_ui_button_width("Stop", IC_SYM_STOP);
    return ic_rect_make(r.x + r.w + IC_SP_2, r.y, w, IC_H_CONTROL_SM);
}

static ic_rect_t refresh_rect(ic_app_t *app) {
    ic_rect_t r = stop_rect(app);
    int w = ic_ui_button_width("Refresh", IC_SYM_RELOAD);
    return ic_rect_make(r.x + r.w + IC_SP_2, r.y, w, IC_H_CONTROL_SM);
}

static void layout(ic_app_t *app) {
    ic_rect_t l = list_rect(app);
    int rows = l.h / IC_H_ROW;
    if (rows < 1) rows = 1;
    ap.rows = rows;
    if (ap.selected < ap.scroll) ap.scroll = ap.selected;
    if (ap.selected >= ap.scroll + rows) ap.scroll = ap.selected - rows + 1;
    if (ap.scroll < 0) ap.scroll = 0;
    if (ap.scroll > ap.count - rows) ap.scroll = ap.count - rows;
    if (ap.scroll < 0) ap.scroll = 0;
    ap.first_row = ap.scroll;
    ap.last_row = ap.scroll + rows;
    if (ap.last_row > ap.count) ap.last_row = ap.count;
}

/* ------------------------------------------------------------ helpers */

static void ap_status(const char *text) {
    ic_strcpy(ap.status, text, AP_STATUS_CAP);
}

static int has_wav_suffix(const char *name) {
    static const char suffix[] = ".wav";
    uint64_t len = ic_strlen(name);
    for (int i = 0; i < 4; i++) {
        if (ic_lower(name[len - 4 + i]) != suffix[i]) return 0;
    }
    return 1;
}

static void add_track(const char *name, const char *path) {
    if (ap.count >= AP_MAX_TRACKS) return;
    for (int i = 0; i < ap.count; i++) {
        if (ic_streq(ap.tracks[i].name, name)) return;
    }
    ic_strcpy(ap.tracks[ap.count].name, name, AP_NAME_CAP);
    ic_strcpy(ap.tracks[ap.count].path, path, AP_PATH_CAP);
    ap.count++;
}

static void add_dir(const char *dir) {
    char buf[4096];
    long rc;
    uint64_t pos = 0;

    if (!dir || !*dir) return;
    rc = (long)icda_list_dir(dir, buf, sizeof(buf));
    if (rc < 0) return;

    while (pos < (uint64_t)rc && ap.count < AP_MAX_TRACKS) {
        char entry[AP_NAME_CAP];
        uint64_t ei = 0;
        char path[AP_PATH_CAP];

        while (pos < (uint64_t)rc && buf[pos] != '\n' && ei + 1 < sizeof(entry)) {
            entry[ei++] = buf[pos++];
        }
        while (pos < (uint64_t)rc && buf[pos] != '\n') pos++;
        if (pos < (uint64_t)rc) pos++;
        entry[ei] = 0;

        if (ei == 0 || entry[ei - 1] == '/') continue;
        if (!has_wav_suffix(entry)) continue;

        path[0] = 0;
        ic_strlcat(path, dir, sizeof(path));
        if (path[0] && path[ic_strlen(path) - 1] != '/') ic_strlcat(path, "/", sizeof(path));
        ic_strlcat(path, entry, sizeof(path));
        add_track(entry, path);
    }
}

static void scan(void) {
    char keep[AP_PATH_CAP];
    keep[0] = 0;
    if (ap.selected >= 0 && ap.selected < ap.count) {
        ic_strcpy(keep, ap.tracks[ap.selected].path, sizeof(keep));
    }
    ap.count = 0;
    add_dir("/usr/share/audio");
    add_dir("/home");
    ap.selected = 0;
    /* Keep pointing at the same file across a rescan. */
    if (keep[0]) {
        for (int i = 0; i < ap.count; i++) {
            if (ic_streq(ap.tracks[i].path, keep)) { ap.selected = i; break; }
        }
    }
    if (ap.count == 0) ap_status("No .wav files found in the media folders");
    else ap_status("Select a track, then press Play");
}

/* ------------------------------------------------------------- actions */

static void play_selected(void) {
    icda_settings_t opt;
    if (ap.selected < 0 || ap.selected >= ap.count) {
        ap_status("No track selected");
        return;
    }
    icda_settings_load(&opt);
    if (!opt.audio) {
        ap_status("Sound is off. Turn it on in Settings.");
        return;
    }
    if ((long)icda_play_audio_file(ap.tracks[ap.selected].path) < 0) {
        ap_status("This track could not be played");
        return;
    }
    ap_status("Playing");
}

static void stop_playback(void) {
    icda_stop_audio();
    ap_status("Stopped");
}

/* Progress of the active stream, 0..1, or -1 when nothing is playing. */
static float playback_progress(char *name, int name_cap, uint64_t *seconds_left) {
    icda_audio_info_t info;
    name[0] = 0;
    *seconds_left = 0;
    if ((long)icda_audio_info(&info) < 0 || !info.active) return -1.0f;
    ic_strncpy(name, info.name, (uint64_t)name_cap, (uint64_t)name_cap);
    *seconds_left = info.seconds_left;
    if (info.total_seconds == 0) return -1.0f;
    {
        uint64_t left = info.seconds_left;
        if (left > info.total_seconds) return 0.0f;
        return 1.0f - (float)left / (float)info.total_seconds;
    }
}

static void refresh_status(void) {
    char name[AP_NAME_CAP];
    uint64_t left = 0;
    float p = playback_progress(name, AP_NAME_CAP, &left);
    ap.playing = p >= 0.0f;
}

/* ------------------------------------------------------------ drawing */

static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    const ic_palette_t *p = ic_palette();
    int enabled = ap.selected >= 0 && ap.selected < ap.count;

    ic_ui_toolbar(c, b);
    ic_ui_button(c, play_rect(app), "Play", IC_SYM_PLAY,
                 ap.playing ? IC_BUTTON_DEFAULT : IC_BUTTON_PRIMARY,
                 !enabled ? IC_STATE_DISABLED
                          : (ap.hover_play ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_button(c, stop_rect(app), "Stop", IC_SYM_STOP, IC_BUTTON_DEFAULT,
                 !ap.playing ? IC_STATE_DISABLED
                             : (ap.hover_stop ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_button(c, refresh_rect(app), "Refresh", IC_SYM_RELOAD, IC_BUTTON_DEFAULT,
                 ap.hover_refresh ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                    ic_rect_make(refresh_rect(app).x + refresh_rect(app).w + IC_SP_3, 0,
                                 b.w - refresh_rect(app).w - IC_SP_3, b.h),
                    ap.playing ? "Playing" : "Stopped", p->label_secondary, IC_ALIGN_LEFT);
}

static void draw_list(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t l = list_rect(app);
    const ic_face_t *body = ic_font(IC_FONT_BODY);

    layout(app);
    ic_gfx_fill(c, l.x, l.y, l.w, l.h, ic_palette()->content);

    for (int i = ap.first_row; i < ap.last_row; i++) {
        ic_rect_t r = row_rect(app, i - ap.scroll);
        ic_color_t text = ic_ui_list_row(c, r, i == ap.selected, ap.list_focused,
                                         i == ap.hover_row ? 1.0f : 0.0f);
        ic_symbol_draw(c, IC_SYM_MUSIC, (float)(r.x + IC_SP_3 + 8), (float)(r.y + r.h / 2),
                       16.0f, text);
        ic_text_draw_in(c, body,
                        ic_rect_make(r.x + IC_SP_3 + 20, r.y, r.w - IC_SP_3 - 24, r.h),
                        ap.tracks[i].name, text, IC_ALIGN_LEFT);
    }

    if (ap.count == 0) {
        ic_ui_empty_state(c, l, IC_SYM_MUSIC, "No music found",
                          "Add .wav files to the media folder, then press Refresh.");
    } else if (ap.count > ap.rows) {
        ic_ui_scrollbar(c, l, ap.scroll, ap.count, 1.0f);
    }
}

static void draw_now_playing(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t r = nowplaying_rect(app);
    const ic_palette_t *p = ic_palette();
    const ic_face_t *title = ic_font(IC_FONT_HEADLINE);
    const ic_face_t *sub = ic_font(IC_FONT_FOOTNOTE);
    char name[AP_NAME_CAP];
    char time[32];
    uint64_t left = 0;
    float progress = playback_progress(name, AP_NAME_CAP, &left);
    ic_rect_t bar;

    ic_ui_group(c, ic_rect_make(r.x + IC_SP_4, r.y + IC_SP_2,
                                r.w - 2 * IC_SP_4, r.h - 2 * IC_SP_2));

    if (progress < 0.0f) {
        ic_symbol_draw(c, IC_SYM_MUSIC, (float)(r.x + IC_SP_6 + 12),
                       (float)(r.y + IC_SP_4 + 14), 24.0f, p->label_tertiary);
        ic_text_draw_in(c, title, ic_rect_make(r.x + IC_SP_6 + 32, r.y + IC_SP_3 + 2,
                                               r.w - IC_SP_8 - 32, 18),
                        "Nothing playing", p->label_secondary, IC_ALIGN_LEFT);
        ic_text_draw_in(c, sub, ic_rect_make(r.x + IC_SP_6 + 32, r.y + IC_SP_3 + 20,
                                             r.w - IC_SP_8 - 32, 16),
                        "Choose a track and press Play", p->label_tertiary, IC_ALIGN_LEFT);
        return;
    }

    ic_symbol_draw(c, IC_SYM_PLAY, (float)(r.x + IC_SP_6 + 12),
                   (float)(r.y + IC_SP_4 + 14), 24.0f, p->accent);
    ic_text_draw_in(c, title, ic_rect_make(r.x + IC_SP_6 + 32, r.y + IC_SP_3 + 2,
                                           r.w - IC_SP_8 - 32, 18),
                    name[0] ? name : "Unknown track", p->label, IC_ALIGN_LEFT);

    time[0] = 0;
    ic_strlcat(time, "0:", sizeof(time));
    {
        char n[24];
        ic_snprintf_u64(n, sizeof(n), left);
        ic_strlcat(time, n, sizeof(time));
        ic_strlcat(time, " left", sizeof(time));
    }
    ic_text_draw_in(c, sub, ic_rect_make(r.x + IC_SP_6 + 32, r.y + IC_SP_3 + 20,
                                         r.w - IC_SP_8 - 32, 16),
                    time, p->label_tertiary, IC_ALIGN_LEFT);

    bar = ic_rect_make(r.x + IC_SP_6, r.y + r.h - IC_SP_3 - 4,
                       r.w - 2 * (IC_SP_6 + IC_SP_2), 4);
    ic_ui_progress(c, bar, progress, p->accent);
}

static void draw_status(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = status_rect(app);
    ic_ui_statusbar(c, s, ap.status);
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_list(app, c);
    draw_now_playing(app, c);
    draw_status(app, c);
    /* The progress bar moves while a track plays. */
    if (ap.playing || app->focused) ic_app_animate(app);
}

/* -------------------------------------------------------------- events */

static int row_at(ic_app_t *app, int x, int y) {
    ic_rect_t l = list_rect(app);
    int i;
    if (!ic_ui_hit(l, x, y)) return -1;
    i = ap.scroll + (y - l.y) / IC_H_ROW;
    return (i >= 0 && i < ap.count) ? i : -1;
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        ap.hover_play = ic_ui_hit(play_rect(app), ev->x, ev->y);
        ap.hover_stop = ic_ui_hit(stop_rect(app), ev->x, ev->y);
        ap.hover_refresh = ic_ui_hit(refresh_rect(app), ev->x, ev->y);
        ap.hover_row = row_at(app, ev->x, ev->y);
        break;
    case IC_EV_MOUSE_DOWN: {
        int i;
        if (ev->button != GUI_BTN_LEFT) break;
        if (ap.hover_play) { play_selected(); break; }
        if (ap.hover_stop) { stop_playback(); break; }
        if (ap.hover_refresh) { scan(); break; }
        i = row_at(app, ev->x, ev->y);
        if (i >= 0) {
            ap.selected = i;
            ap.list_focused = 1;
            /* A plain click plays, like every other music player. */
            play_selected();
        } else {
            ap.list_focused = 0;
        }
        break;
    }
    case IC_EV_MOUSE_LEAVE:
        ap.hover_row = -1;
        ap.hover_play = ap.hover_stop = ap.hover_refresh = 0;
        break;
    case IC_EV_KEY:
        switch (ev->key) {
        case IC_KEY_UP:   if (ap.selected > 0) ap.selected--; break;
        case IC_KEY_DOWN: if (ap.selected + 1 < ap.count) ap.selected++; break;
        case IC_KEY_HOME: ap.selected = 0; break;
        case IC_KEY_END:  ap.selected = ap.count > 0 ? ap.count - 1 : 0; break;
        case IC_KEY_ENTER:
        case ' ':
            play_selected();
            break;
        case IC_KEY_ESCAPE: stop_playback(); break;
        case 's': case 'S': stop_playback(); break;
        case 'r': case 'R': scan(); break;
        default: break;
        }
        break;
    case IC_EV_RESIZE:
        layout(app);
        break;
    case IC_EV_BLUR:
        ap.list_focused = 0;
        break;
    case IC_EV_APPEARANCE:
    case IC_EV_FOCUS:
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void tick(ic_app_t *app) {
    int was = ap.playing;
    refresh_status();
    if (ap.playing != was) ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    ap.count = 0;
    ap.selected = 0;
    ap.scroll = 0;
    ap.hover_row = -1;
    ap.hover_play = ap.hover_stop = ap.hover_refresh = 0;
    ap.list_focused = 1;
    ap.playing = 0;
    ap.status[0] = 0;
    scan();
    /* `user` carries argv[1] when the shell opened a specific file. */
    if (app->user) {
        const char *arg = (const char *)app->user;
        if (arg[0]) {
            add_track(arg, arg);
            for (int i = 0; i < ap.count; i++) {
                if (ic_streq(ap.tracks[i].path, arg)) ap.selected = i;
            }
        }
    }
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Music", WIN_W, WIN_H, init, draw, event, tick };
    const char *arg = (argc > 1 && argv) ? argv[1] : 0;
    if (ic_app_run(&desc, (void *)arg) != 0) {
        icda_write("music requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
