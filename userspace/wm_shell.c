



#include "wm_shell.h"
#include "kernel/diag/boot_logo.h"
#include "kernel/diag/boot_layout.h"



const wm_app_t wm_apps[] = {
    { "Explorer",     "explorer", "/apps/desktop.app" },
    { "Terminal",     "terminal", "/apps/terminal.app" },
    { "Surfer",       "browser",  "/apps/browser.app" },
    { "Editor",       "editor",   "/apps/editor.app" },
    { "Music",        "music",    "/apps/audioplay.app" },
    { "Disk Utility", "disk",     "/apps/diskman.app" },
    { "Activity",     "taskman",  "/apps/taskman.app" },
    { "Settings",     "settings", "/apps/settings.app" },
};
const int wm_app_count = (int)(sizeof(wm_apps) / sizeof(wm_apps[0]));

static int wm_prefix_ci(const char *s, const char *prefix) {
    while (*prefix) {
        if (ic_lower(*s) != ic_lower(*prefix)) return 0;
        s++;
        prefix++;
    }
    return 1;
}

const ic_icon_t *wm_app_icon_for_title(const char *title) {
    const ic_icon_t *icon = 0;
    if (title) {
        for (int i = 0; i < wm_app_count; i++) {
            if (wm_prefix_ci(title, wm_apps[i].label)) {
                icon = ic_icon_builtin(wm_apps[i].icon);
                break;
            }
        }
    }
    return icon ? icon : ic_icon_builtin("app");
}



typedef struct {
    float cx, cy;       
    float radius;       
    uint32_t rgb;
    float strength;
} wm_glow_t;

static const wm_glow_t wm_glows_dark[] = {
    { 0.18f, 0.20f, 0.55f, 0x5A5FE0, 0.42f },
    { 0.86f, 0.78f, 0.60f, 0x1F9E94, 0.30f },
    { 0.74f, 0.10f, 0.40f, 0xB24FD0, 0.20f },
    { 0.40f, 1.05f, 0.45f, 0x2D6BD8, 0.22f },
};

static const wm_glow_t wm_glows_light[] = {
    { 0.16f, 0.18f, 0.55f, 0x8FAEF5, 0.55f },
    { 0.88f, 0.80f, 0.60f, 0x9ED9C8, 0.45f },
    { 0.76f, 0.08f, 0.40f, 0xF2C2AE, 0.45f },
    { 0.40f, 1.05f, 0.45f, 0xC6B7F2, 0.35f },
};

static inline float wm_chan(uint32_t rgb, int shift) {
    return (float)((rgb >> shift) & 0xFF);
}

void wm_wallpaper_paint(ic_canvas_t *c, int sw, int sh) {
    const ic_palette_t *p = ic_palette();
    const wm_glow_t *glows = p->dark ? wm_glows_dark : wm_glows_light;
    int nglow = 4;
    uint32_t top = p->dark ? 0x161B30 : 0xDDE4F2;
    uint32_t bottom = p->dark ? 0x0B0F1C : 0xEEF0F6;
    float diag = ic_sqrtf((float)sw * (float)sw + (float)sh * (float)sh);
    int x0, y0, x1, y1;
    if (c->scale > 1) {
        sw *= c->scale;
        sh *= c->scale;
        diag *= (float)c->scale;
    }
    if (!ic_canvas_bounds(c, &x0, &y0, &x1, &y1) || sw <= 0 || sh <= 0) return;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->px + (int64_t)y * c->w;
        float ty = (float)y / (float)sh;
        float br = wm_chan(top, 16) + (wm_chan(bottom, 16) - wm_chan(top, 16)) * ty;
        float bg = wm_chan(top, 8) + (wm_chan(bottom, 8) - wm_chan(top, 8)) * ty;
        float bb = wm_chan(top, 0) + (wm_chan(bottom, 0) - wm_chan(top, 0)) * ty;
        for (int x = x0; x < x1; x++) {
            float r = br, g = bg, b = bb;
            uint32_t hash;
            float n;
            for (int i = 0; i < nglow; i++) {
                float dx = (float)x - glows[i].cx * (float)sw;
                float dy = (float)y - glows[i].cy * (float)sh;
                float rad = glows[i].radius * diag;
                float d2 = (dx * dx + dy * dy) / (rad * rad);
                if (d2 < 1.0f) {
                    float k = (1.0f - d2);
                    k = k * k * glows[i].strength;
                    r += (wm_chan(glows[i].rgb, 16) - r) * k;
                    g += (wm_chan(glows[i].rgb, 8) - g) * k;
                    b += (wm_chan(glows[i].rgb, 0) - b) * k;
                }
            }
            
            hash = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u;
            hash ^= hash >> 15;
            hash *= 0x2C1B3C6Du;
            hash ^= hash >> 12;
            n = (float)(hash & 0xFF) / 255.0f - 0.5f;
            r += n; g += n; b += n;
            row[x] = ((uint32_t)ic_clampf(r, 0.0f, 255.0f) << 16) |
                     ((uint32_t)ic_clampf(g, 0.0f, 255.0f) << 8) |
                     (uint32_t)ic_clampf(b, 0.0f, 255.0f);
        }
    }
}



#define WM_DESK_ICON     48
#define WM_DESK_ICON_Y    8
#define WM_DESK_LABEL_Y  64

ic_rect_t wm_desk_icon_hit_rect(ic_rect_t cell) {
    return ic_rect_make(cell.x + 6, cell.y + 2, cell.w - 12, WM_DESK_LABEL_Y + 22);
}

void wm_desk_icon_draw(ic_canvas_t *c, ic_rect_t cell, const char *label,
                       const ic_icon_t *icon, int selected, float opacity) {
    const ic_palette_t *p = ic_palette();
    const ic_face_t *f = ic_font(IC_FONT_FOOTNOTE);
    int ix = cell.x + (cell.w - WM_DESK_ICON) / 2;
    int iy = cell.y + WM_DESK_ICON_Y;
    uint32_t op = (uint32_t)(ic_clampf(opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (selected) {
        ic_gfx_rrect(c, ix - 7, iy - 5, WM_DESK_ICON + 14, WM_DESK_ICON + 10, IC_R_TILE,
                     p->fill_pressed);
    }
    if (icon && ic_icon_valid(icon)) {
        ic_gfx_image_rgba(c, ix, iy, WM_DESK_ICON, WM_DESK_ICON, icon->rgba, icon->w, icon->h, op);
    }
    if (label) {
        int tw = ic_text_measure(f, label);
        int maxw = cell.w - 8;
        int lw = tw < maxw ? tw : maxw;
        int lx = cell.x + (cell.w - lw) / 2;
        int ly = cell.y + WM_DESK_LABEL_Y;
        ic_rect_t lr = ic_rect_make(lx, ly, lw, 18);
        if (selected) {
            ic_gfx_rrect(c, lx - 6, ly, lw + 12, 18, 5.0f, p->accent);
            ic_text_draw_in(c, f, lr, label, p->label_on_accent, IC_ALIGN_LEFT);
        } else {
            

            if (p->dark) {
                ic_rect_t sr = lr;
                sr.y += 1;
                ic_text_draw_in(c, f, sr, label, IC_RGBA(0x000000, 0x8C), IC_ALIGN_LEFT);
            }
            ic_text_draw_in(c, f, lr, label, p->label, IC_ALIGN_LEFT);
        }
    }
}



#define WM_BAR_PAD        8
#define WM_BAR_BTN_H     36
#define WM_BAR_TASK_MAX 188
#define WM_BAR_TASK_MIN  44
#define WM_BAR_STATUS_W 150
#define WM_BAR_ICON      22

ic_rect_t wm_bar_rect(int sw, int sh) {
    return ic_rect_make(0, sh - WM_BAR_H, sw, WM_BAR_H);
}

ic_rect_t wm_bar_launcher_rect(int sw, int sh) {
    ic_rect_t b = wm_bar_rect(sw, sh);
    return ic_rect_make(WM_BAR_PAD, b.y + (WM_BAR_H - WM_BAR_BTN_H) / 2, WM_BAR_BTN_H + 4,
                        WM_BAR_BTN_H);
}

static int wm_bar_task_w(int sw, int count) {
    int avail = sw - (WM_BAR_PAD * 3 + WM_BAR_BTN_H + 4) - WM_BAR_STATUS_W - WM_BAR_PAD;
    int w;
    if (count <= 0) return WM_BAR_TASK_MAX;
    w = avail / count - 4;
    if (w > WM_BAR_TASK_MAX) w = WM_BAR_TASK_MAX;
    if (w < WM_BAR_TASK_MIN) w = WM_BAR_TASK_MIN;
    return w;
}

ic_rect_t wm_bar_task_rect(int sw, int sh, int count, int index) {
    ic_rect_t l = wm_bar_launcher_rect(sw, sh);
    int w = wm_bar_task_w(sw, count);
    return ic_rect_make(l.x + l.w + WM_BAR_PAD + index * (w + 4), l.y, w, WM_BAR_BTN_H);
}

static ic_rect_t wm_bar_status_rect(int sw, int sh) {
    ic_rect_t b = wm_bar_rect(sw, sh);
    return ic_rect_make(sw - WM_BAR_STATUS_W - WM_BAR_PAD, b.y, WM_BAR_STATUS_W, WM_BAR_H);
}

int wm_bar_hit(int sw, int sh, const wm_bar_t *b, int mx, int my) {
    if (!ic_ui_hit(wm_bar_rect(sw, sh), mx, my)) return WM_BAR_NONE;
    if (ic_ui_hit(wm_bar_launcher_rect(sw, sh), mx, my)) return WM_BAR_LAUNCHER;
    for (int i = 0; b && i < b->count; i++) {
        ic_rect_t r = wm_bar_task_rect(sw, sh, b->count, i);
        if (r.x + r.w > wm_bar_status_rect(sw, sh).x) break;
        if (ic_ui_hit(r, mx, my)) return i;
    }
    if (ic_ui_hit(wm_bar_status_rect(sw, sh), mx, my)) return WM_BAR_STATUS;
    return WM_BAR_NONE;
}

void wm_bar_draw(ic_canvas_t *c, int sw, int sh, const wm_bar_t *b,
                 uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t bar = wm_bar_rect(sw, sh);
    ic_rect_t launch = wm_bar_launcher_rect(sw, sh);
    ic_rect_t status = wm_bar_status_rect(sw, sh);

    ic_gfx_backdrop(c, bar.x, bar.y, bar.w, bar.h, 0.0f, 30, p->material_bar, scratch, scratch_len);
    ic_gfx_hline(c, bar.x, bar.y, bar.w, p->bar_edge);
    ic_gfx_hline(c, bar.x, bar.y + 1, bar.w, p->highlight);

    
    {
        int open = b && b->launcher_open;
        int hover = b && b->hover == WM_BAR_LAUNCHER;
        if (open) ic_gfx_rrect(c, launch.x, launch.y, launch.w, launch.h, IC_R_CONTROL + 2.0f,
                               p->fill_selected_idle);
        else if (hover) ic_gfx_rrect(c, launch.x, launch.y, launch.w, launch.h, IC_R_CONTROL + 2.0f,
                                     p->fill_hover);
        ic_symbol_draw(c, IC_SYM_GRID, (float)launch.x + (float)launch.w * 0.5f,
                       (float)launch.y + (float)launch.h * 0.5f, 20.0f,
                       open ? p->accent : p->label);
    }

    
    for (int i = 0; b && i < b->count; i++) {
        const wm_task_t *t = &b->tasks[i];
        ic_rect_t r = wm_bar_task_rect(sw, sh, b->count, i);
        int hover = b->hover == i;
        int ix = r.x + (r.w >= 100 ? 10 : (r.w - WM_BAR_ICON) / 2);
        ic_rect_t saved;
        if (r.x + r.w > status.x) break;
        if (t->focused) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL + 2.0f, p->fill_selected_idle);
        else if (hover) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_CONTROL + 2.0f, p->fill_hover);
        if (t->icon && ic_icon_valid(t->icon)) {
            ic_gfx_image_rgba(c, ix, r.y + (r.h - WM_BAR_ICON) / 2, WM_BAR_ICON, WM_BAR_ICON,
                              t->icon->rgba, t->icon->w, t->icon->h, t->minimized ? 150u : 255u);
        }
        if (r.w >= 100 && t->title) {
            ic_canvas_push_clip(c, r.x, r.y, r.w, r.h, &saved);
            ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH),
                            ic_rect_make(ix + WM_BAR_ICON + 8, r.y, r.x + r.w - (ix + WM_BAR_ICON + 8) - 8, r.h),
                            t->title, t->minimized ? p->label_secondary : p->label, IC_ALIGN_LEFT);
            ic_canvas_pop_clip(c, &saved);
        }
        {
            int iw = t->focused ? 16 : 6;
            ic_gfx_rrect(c, r.x + (r.w - iw) / 2, bar.y + bar.h - 5, iw, 3, 1.5f,
                         t->focused ? p->accent : p->label_tertiary);
        }
    }

    
    {
        int right = status.x + status.w;
        const ic_face_t *ft = ic_font(IC_FONT_BODY_EMPH);
        const ic_face_t *fd = ic_font(IC_FONT_CAPTION);
        int clock_w = 0;
        if (b && b->time_text) {
            int tw = ic_text_measure(ft, b->time_text);
            int dw = b->date_text ? ic_text_measure(fd, b->date_text) : 0;
            int block = ft->cap_h + 6 + fd->cap_h;
            int top = bar.y + (bar.h - block) / 2;
            clock_w = tw > dw ? tw : dw;
            ic_text_draw(c, ft, right - tw, top + ft->cap_h, b->time_text, p->label);
            if (b->date_text) {
                ic_text_draw(c, fd, right - dw, top + ft->cap_h + 6 + fd->cap_h, b->date_text,
                             p->label_secondary);
            }
        }
        if (b && b->audio_text && b->audio_text[0]) {
            int ax1 = right - clock_w - IC_SP_4;
            int ax0 = status.x;
            ic_symbol_draw(c, IC_SYM_SPEAKER, (float)ax0 + 9.0f, (float)bar.y + (float)bar.h * 0.5f,
                           16.0f, p->label_secondary);
            ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                            ic_rect_make(ax0 + 22, bar.y, ax1 - ax0 - 22, bar.h),
                            b->audio_text, p->label_secondary, IC_ALIGN_LEFT);
        }
    }
}



#define WM_LAUNCH_COLS     4
#define WM_LAUNCH_TILE_W  88
#define WM_LAUNCH_TILE_H  90
#define WM_LAUNCH_PAD     IC_SP_4
#define WM_LAUNCH_TITLE_H 40
#define WM_LAUNCH_FOOT_H  52
#define WM_LAUNCH_ICON    44

char wm_launch_query[WM_LAUNCH_QUERY_CAP];

static int wm_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int wm_contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] && wm_lower(hay[i + k]) == wm_lower(needle[k])) k++;
        if (!needle[k]) return 1;
    }
    return 0;
}

int wm_launch_visible(int *out) {
    int n = 0;
    for (int i = 0; i < wm_app_count; i++) {
        if (wm_contains_ci(wm_apps[i].label, wm_launch_query)) out[n++] = i;
    }
    return n;
}

static int wm_launch_rows(void) {
    return (wm_app_count + WM_LAUNCH_COLS - 1) / WM_LAUNCH_COLS;
}

ic_rect_t wm_launcher_rect(int sw, int sh) {
    int w = WM_LAUNCH_PAD * 2 + WM_LAUNCH_COLS * WM_LAUNCH_TILE_W;
    int h = WM_LAUNCH_PAD + WM_LAUNCH_TITLE_H + wm_launch_rows() * WM_LAUNCH_TILE_H +
            IC_SP_2 + WM_LAUNCH_FOOT_H;
    (void)sw;
    return ic_rect_make(WM_BAR_PAD, sh - WM_BAR_H - IC_SP_2 - h, w, h);
}

static ic_rect_t wm_launch_tile(ic_rect_t panel, int i) {
    int col = i % WM_LAUNCH_COLS, row = i / WM_LAUNCH_COLS;
    return ic_rect_make(panel.x + WM_LAUNCH_PAD + col * WM_LAUNCH_TILE_W,
                        panel.y + WM_LAUNCH_PAD + WM_LAUNCH_TITLE_H + row * WM_LAUNCH_TILE_H,
                        WM_LAUNCH_TILE_W, WM_LAUNCH_TILE_H);
}

static ic_rect_t wm_launch_footer(ic_rect_t panel) {
    return ic_rect_make(panel.x, panel.y + panel.h - WM_LAUNCH_FOOT_H, panel.w, WM_LAUNCH_FOOT_H);
}

static ic_rect_t wm_launch_power_button(ic_rect_t panel, int restart) {
    ic_rect_t foot = wm_launch_footer(panel);
    int size = 32;
    int x = foot.x + foot.w - WM_LAUNCH_PAD - size - (restart ? size + IC_SP_1 : 0);
    return ic_rect_make(x, foot.y + (foot.h - size) / 2, size, size);
}

int wm_launcher_hit(int sw, int sh, int mx, int my) {
    ic_rect_t panel = wm_launcher_rect(sw, sh);
    if (!ic_ui_hit(panel, mx, my)) return WM_LAUNCH_OUTSIDE;
    {
        int vis[WM_LAUNCH_MAX_APPS];
        int n = wm_launch_visible(vis);
        for (int k = 0; k < n; k++) {
            if (ic_ui_hit(ic_rect_inset(wm_launch_tile(panel, k), 4, 2), mx, my)) return vis[k];
        }
    }
    if (ic_ui_hit(wm_launch_power_button(panel, 1), mx, my)) return WM_LAUNCH_RESTART;
    if (ic_ui_hit(wm_launch_power_button(panel, 0), mx, my)) return WM_LAUNCH_SHUTDOWN;
    return WM_LAUNCH_NONE;
}

void wm_launcher_draw(ic_canvas_t *c, int sw, int sh, int hover,
                      uint32_t *scratch, int scratch_len) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t panel = wm_launcher_rect(sw, sh);
    ic_rect_t foot = wm_launch_footer(panel);
    const ic_face_t *ft = ic_font(IC_FONT_TITLE3);
    const ic_face_t *fl = ic_font(IC_FONT_FOOTNOTE);

    ic_ui_panel(c, panel, IC_R_PANEL, IC_ELEV_MENU, scratch, scratch_len);
    {
        ic_textfield_t tf;
        int len = 0;
        while (wm_launch_query[len]) len++;
        tf.text = wm_launch_query;
        tf.cursor = len;
        tf.sel_start = tf.sel_end = len;
        tf.focused = 1;
        tf.caret_on = 1;
        tf.placeholder = "Search applications";
        tf.leading = IC_SYM_SEARCH;
        tf.scroll_px = 0;
        ic_ui_textfield(c, ic_rect_make(panel.x + WM_LAUNCH_PAD, panel.y + WM_LAUNCH_PAD,
                                        panel.w - 2 * WM_LAUNCH_PAD, IC_H_CONTROL), &tf);
    }
    (void)ft;

    {
    int vis[WM_LAUNCH_MAX_APPS];
    int nvis = wm_launch_visible(vis);
    if (nvis == 0) {
        ic_rect_t area = ic_rect_make(panel.x, panel.y + WM_LAUNCH_PAD + WM_LAUNCH_TITLE_H, panel.w,
                                      foot.y - panel.y - WM_LAUNCH_PAD - WM_LAUNCH_TITLE_H);
        ic_text_draw_in(c, fl, area, "No matching applications", p->label_secondary, IC_ALIGN_CENTER);
    }
    for (int k = 0; k < nvis; k++) {
        int i = vis[k];
        ic_rect_t tile = wm_launch_tile(panel, k);
        ic_rect_t hit = ic_rect_inset(tile, 4, 2);
        const ic_icon_t *icon = ic_icon_builtin(wm_apps[i].icon);
        if (!icon) icon = ic_icon_builtin("app");
        if (hover == i) ic_gfx_rrect(c, hit.x, hit.y, hit.w, hit.h, IC_R_TILE, p->fill_hover);
        if (icon && ic_icon_valid(icon)) {
            ic_gfx_image_rgba(c, tile.x + (tile.w - WM_LAUNCH_ICON) / 2, tile.y + 10,
                              WM_LAUNCH_ICON, WM_LAUNCH_ICON, icon->rgba, icon->w, icon->h, 255);
        }
        ic_text_draw_in(c, fl, ic_rect_make(tile.x + 4, tile.y + 60, tile.w - 8, 20),
                        wm_apps[i].label, p->label, IC_ALIGN_CENTER);
    }
    }

    ic_gfx_hline(c, foot.x + WM_LAUNCH_PAD, foot.y, foot.w - 2 * WM_LAUNCH_PAD, p->separator);
    {
        const ic_face_t *fh = ic_font(IC_FONT_HEADLINE);
        const ic_face_t *fc = ic_font(IC_FONT_CAPTION);
        int block = fh->cap_h + 6 + fc->cap_h;
        int top = foot.y + (foot.h - block) / 2;
        int x = foot.x + WM_LAUNCH_PAD + 6;
        ic_text_draw(c, fh, x, top + fh->cap_h, "ICDA", p->label);
        ic_text_draw(c, fc, x, top + fh->cap_h + 6 + fc->cap_h, "Version " IC_VERSION_STRING,
                     p->label_secondary);
    }
    {
        ic_rect_t rb = wm_launch_power_button(panel, 1);
        ic_rect_t pb = wm_launch_power_button(panel, 0);
        ic_ui_icon_button(c, rb, IC_SYM_RESTART,
                          hover == WM_LAUNCH_RESTART ? IC_STATE_HOVER : IC_STATE_NORMAL);
        ic_ui_icon_button(c, pb, IC_SYM_POWER,
                          hover == WM_LAUNCH_SHUTDOWN ? IC_STATE_HOVER : IC_STATE_NORMAL);
    }
}



void wm_rubber_band_draw(ic_canvas_t *c, int x0, int y0, int x1, int y1) {
    const ic_palette_t *p = ic_palette();
    int x = x0 < x1 ? x0 : x1, y = y0 < y1 ? y0 : y1;
    int w = (x0 < x1 ? x1 - x0 : x0 - x1) + 1, h = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    ic_gfx_rrect(c, x, y, w, h, 3.0f, ic_color_with_alpha(p->accent, 0x2A));
    ic_gfx_rrect_stroke(c, x, y, w, h, 3.0f, 1.0f, ic_color_with_alpha(p->accent, 0xB4));
}

void wm_power_overlay_draw(ic_canvas_t *c, int sw, int sh, float t, int restart) {
    float k = ic_clampf(t, 0.0f, 1.0f);
    float content = ic_clampf((k - 0.35f) / 0.4f, 0.0f, 1.0f);
    ic_color_t fg = ic_color_fade(IC_RGB(0xF2F2F5), content);
    const char *msg = restart ? "Restarting\xE2\x80\xA6" : "Shutting Down\xE2\x80\xA6";
    ic_gfx_fill(c, 0, 0, sw, sh, IC_RGBA(0x000000, (uint32_t)(k * 255.0f)));
    if (content > 0.0f) {
        ic_symbol_draw(c, restart ? IC_SYM_RESTART : IC_SYM_POWER, (float)sw * 0.5f,
                       (float)sh * 0.5f - 22.0f, 40.0f, fg);
        ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(0, sh / 2 + 10, sw, 24), msg, fg,
                        IC_ALIGN_CENTER);
    }
}

void wm_debug_draw(ic_canvas_t *c, const char *const *lines, int count) {
    const ic_palette_t *p = ic_palette();
    const ic_face_t *f = ic_font(IC_FONT_MONO_SMALL);
    int w = 300, h = IC_SP_3 * 2 + count * f->line_h;
    ic_rect_t r = ic_rect_make(IC_SP_3, IC_SP_3, w, h);
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_MENU, IC_RGBA(0x000000, 0xC8));
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_MENU, 1.0f, IC_RGBA(0xFFFFFF, 0x24));
    for (int i = 0; i < count; i++) {
        ic_text_draw(c, f, r.x + IC_SP_3, r.y + IC_SP_3 + i * f->line_h + f->ascent, lines[i],
                     i == 0 ? p->accent : IC_RGB(0xE6E6EA));
    }
}

void wm_boot_overlay_draw(ic_canvas_t *c, int sw, int sh, float t) {
    float k = ic_clampf(t, 0.0f, 1.0f);
    float veil = 1.0f - ic_ease(IC_EASE_STANDARD, ic_clampf((k - 0.25f) / 0.75f, 0.0f, 1.0f));
    float ink = 1.0f - ic_ease(IC_EASE_STANDARD, ic_clampf(k / 0.45f, 0.0f, 1.0f));
    int lx = sw / 2 - BOOT_LOGO_W / 2;
    int ly = sh / 2 + BOOT_LOGO_CENTER_DY - BOOT_LOGO_H_PX / 2;
    int bx = sw / 2 - BOOT_BAR_W / 2;
    int by = sh / 2 + BOOT_BAR_TOP_DY;
    if (veil > 0.0f) ic_gfx_fill(c, 0, 0, sw, sh, IC_RGBA(0x000000, (uint32_t)(veil * 255.0f + 0.5f)));
    if (ink > 0.0f) {
        ic_gfx_mask(c, lx, ly, boot_logo_alpha, BOOT_LOGO_W, BOOT_LOGO_H_PX, BOOT_LOGO_W,
                    ic_color_fade(IC_RGB(0xF2F2F5), ink));
        ic_gfx_rrect(c, bx, by, BOOT_BAR_W, BOOT_BAR_H, BOOT_BAR_H * 0.5f, ic_color_fade(IC_RGB(0xF2F2F5), ink));
    }
}
