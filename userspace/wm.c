/*
 * wm.c - the ICDA window manager / desktop engine host.
 *
 * The WM owns the physical framebuffer and composites every window.
 * Apps never touch the screen: they draw into shared-memory window
 * buffers (gui_pixel_buffer) and the WM blits them into the scene.
 * All chrome/theme/icon rendering comes from libicda so the desktop
 * and apps share one look.
 */
#include "libicda.h"
#include "gui_proto.h"

#define MAX_WINDOWS 16
#define BACK_BUFFER_WIDTH 2560
#define BACK_BUFFER_HEIGHT 1600
#define TASKBAR_H 42
#define CURSOR_W 19
#define CURSOR_H 30

#define WM_ANIM_NONE        0
#define WM_ANIM_OPEN        1
#define WM_ANIM_MINIMIZE    2
#define WM_ANIM_RESTORE     3
#define WM_ANIM_MAXIMIZE    4
#define WM_ANIM_UNMAXIMIZE  5

typedef struct {
    int      valid;
    uint32_t id;
    uint64_t app_queue_handle;
    uint64_t shm_handle;
    uint32_t *pixels;
    int      x;
    int      y;
    int      w;
    int      h;
    int      minimized;
    int      maximized;
    int      anim;
    int      anim_kind;
    int      anim_from_x;
    int      anim_from_y;
    int      anim_from_w;
    int      anim_from_h;
    int      anim_to_x;
    int      anim_to_y;
    int      anim_to_w;
    int      anim_to_h;
    int      restore_x;
    int      restore_y;
    int      restore_w;
    int      restore_h;

    char     title[32];
} wm_window_t;

static wm_window_t windows[MAX_WINDOWS];
static int z_order[MAX_WINDOWS];
static int num_windows = 0;
static int focused_window_idx = -1;
static int start_menu_open = 0;

/* back_buffer holds the scene without the cursor; the cursor is blitted
 * straight to the real framebuffer so a mouse move never forces a full
 * frame rebuild.  desktop_layer is the static wallpaper (gradient + icon
 * tiles), pre-rendered once and copied out as the base of every frame. */
static uint32_t back_buffer[BACK_BUFFER_WIDTH * BACK_BUFFER_HEIGHT];
static uint32_t desktop_layer[BACK_BUFFER_WIDTH * BACK_BUFFER_HEIGHT];
static icda_fb_info_t fb_info;
static uint32_t *real_fb = NULL;
static const ic_theme_t *theme;

/* Mouse position lives in file-scope state, not registers: the compiled
 * main loop has been observed losing its register-tracked coordinates
 * when message/click handling clobbers the loop registers, which then
 * fed garbage (screen dims leaking in as coordinates) into the cursor
 * refresh and faulted the framebuffer blit.  Memory-backed state cannot
 * be clobbered by the generated code. */
static int mouse_x = 0;
static int mouse_y = 0;
static int prev_mouse_x = -1;
static int prev_mouse_y = -1;
static uint8_t mouse_buttons = 0;

/* ---- damage tracking -------------------------------------------------
 *
 * Compositors repaint only what changed.  Every mutation of the scene
 * (window open/close/move/resize, focus change, taskbar, start menu)
 * calls mark_dirty() with the affected rectangle; composite_dirty() then
 * restores those rectangles from the wallpaper layer, redraws the
 * windows that intersect them, and blits only those rectangles to the
 * real framebuffer - instead of rebuilding and blitting the whole
 * 1920x1080 frame on every event (which is what made real hardware
 * crawl).  A pure mouse move still takes the tiny cursor-only path. */
#define MAX_DIRTY 32
#define SHADOW_MARGIN 2
typedef struct { int x, y, w, h; } dirty_rect_t;
static dirty_rect_t dirty_rects[MAX_DIRTY];
static int dirty_count = 0;
static int dirty_full = 0;

static void dirty_rect_union(dirty_rect_t *a, const dirty_rect_t *b) {
    int x0 = a->x < b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y;
    int x1 = (a->x + a->w) > (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y1 = (a->y + a->h) > (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
    a->x = x0;
    a->y = y0;
    a->w = x1 - x0;
    a->h = y1 - y0;
}

static int dirty_rects_intersect(const dirty_rect_t *a, const dirty_rect_t *b) {
    return a->x < b->x + b->w && b->x < a->x + a->w &&
           a->y < b->y + b->h && b->y < a->y + a->h;
}

static void mark_dirty(int x, int y, int w, int h) {
    dirty_rect_t r;
    int sw = (int)fb_info.width;
    int sh = (int)fb_info.height;

    if (dirty_full) return;
    if (sw <= 0 || sh <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;

    r.x = x; r.y = y; r.w = w; r.h = h;

    for (int i = 0; i < dirty_count; i++) {
        if (dirty_rects_intersect(&dirty_rects[i], &r)) {
            dirty_rect_union(&dirty_rects[i], &r);
            return;
        }
    }
    if (dirty_count < MAX_DIRTY) {
        dirty_rects[dirty_count++] = r;
    } else {
        dirty_full = 1;
        dirty_count = 0;
    }
}

/* The chrome + drop shadow around a window, in screen space. */
static void mark_dirty_frame_rect(int x, int y, int w, int h) {
    mark_dirty(x - SHADOW_MARGIN, y - IC_TITLE_H - SHADOW_MARGIN,
               w + SHADOW_MARGIN * 2, h + IC_TITLE_H + SHADOW_MARGIN * 2);
}

static void mark_dirty_win(const wm_window_t *win) {
    if (!win || !win->valid) return;
    mark_dirty_frame_rect(win->x, win->y, win->w, win->h);
    if (win->anim_kind != WM_ANIM_NONE) {
        mark_dirty_frame_rect(win->anim_from_x, win->anim_from_y,
                              win->anim_from_w, win->anim_from_h);
        mark_dirty_frame_rect(win->anim_to_x, win->anim_to_y,
                              win->anim_to_w, win->anim_to_h);
    }
}

static void mark_dirty_full(void) {
    dirty_full = 1;
    dirty_count = 0;
}

/* Windows-style AA arrow, 19×30, hotspot 0,0 */
typedef struct { int n; int x[8]; int y[8]; } cursor_poly_t;
static const cursor_poly_t cursor_outline = {7, {0,0,5,9,14,10,18}, {0,26,21,29,27,18,18}};
static uint8_t cursor_rgba[CURSOR_H][CURSOR_W][4];
static uint16_t cursor_poly_x[8];
static uint16_t cursor_poly_y[8];
static int cursor_point_in_poly(int x, int y) {
    int inside=0;
    for(int i=0,j=cursor_outline.n-1;i<cursor_outline.n;j=i++){
        int xi=cursor_poly_x[i], yi=cursor_poly_y[i];
        int xj=cursor_poly_x[j], yj=cursor_poly_y[j];
        if(((yi>y)!=(yj>y)) && (x < (int)((int64_t)(xj-xi)*(y-yi)/(yj-yi))+xi)) inside=!inside;
    }
    return inside;
}
static int64_t cursor_seg_dist2(int x,int y,int ax,int ay,int bx,int by){
    int dx=bx-ax, dy=by-ay;
    int64_t len2=(int64_t)dx*dx+(int64_t)dy*dy;
    if(len2==0) return (int64_t)(x-ax)*(x-ax)+(int64_t)(y-ay)*(y-ay);
    int64_t t=(int64_t)(x-ax)*dx+(int64_t)(y-ay)*dy;
    int64_t cx,cy;
    if(t<0) t=0; if(t>len2) t=len2;
    cx=ax+t*dx/len2; cy=ay+t*dy/len2;
    return (int64_t)(x-cx)*(x-cx)+(int64_t)(y-cy)*(y-cy);
}
#define CURSOR_RIM_R2 ((int64_t)22*22)
static void build_cursor_sprite(void){
    static const int sub[4]={2,6,10,14};
    for(int i=0;i<cursor_outline.n;i++){cursor_poly_x[i]=cursor_outline.x[i]*16; cursor_poly_y[i]=cursor_outline.y[i]*16;}
    for(int py=0;py<CURSOR_H;py++) for(int px=0;px<CURSOR_W;px++){
        int w_hits=0,b_hits=0;
        for(int sy=0;sy<4;sy++) for(int sx=0;sx<4;sx++){
            int ix=px*16+sub[sx], iy=py*16+sub[sy];
            if(!cursor_point_in_poly(ix,iy)) continue;
            int64_t d2=-1;
            for(int e=0,j=cursor_outline.n-1;e<cursor_outline.n;j=e++){
                int64_t dd=cursor_seg_dist2(ix,iy,cursor_poly_x[j],cursor_poly_y[j],cursor_poly_x[e],cursor_poly_y[e]);
                if(d2<0||dd<d2) d2=dd;
            }
            if(d2<=CURSOR_RIM_R2) b_hits++; else w_hits++;
        }
        int total=w_hits+b_hits;
        uint8_t *out=cursor_rgba[py][px];
        if(total==0) out[0]=out[1]=out[2]=out[3]=0;
        else{
            int w_share=w_hits*255/total, b_share=255-w_share;
            out[0]=(255*w_share+32*b_share)/255;
            out[1]=(255*w_share+32*b_share)/255;
            out[2]=(255*w_share+38*b_share)/255;
            out[3]=total*255/16;
        }
    }
}

static void clear_msg(gui_msg_t *msg) {
    for (int i = 0; i < 64; i++) ((uint8_t*)msg)[i] = 0;
}

/* Native-format pixel write to the real framebuffer (see definition below). */
static void fb_write_px(int x, int y, uint32_t color);

static int cursor_icon_dims(const ic_icon_t **icon_out, int *w_out, int *h_out) {
    const ic_icon_t *icon = ic_icon_builtin("cursor");
    int w;
    int h;

    if (!icon) icon = ic_icon_builtin("mouse");
    if (!icon || !ic_icon_valid(icon)) return 0;

    w = icon->w;
    h = icon->h;
    if (w <= 0 || h <= 0) return 0;
    if (w > 48 || h > 48) {
        if (w >= h) {
            h = h * 48 / w;
            w = 48;
        } else {
            w = w * 48 / h;
            h = 48;
        }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
    }
    if (icon_out) *icon_out = icon;
    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
    return 1;
}

static void cursor_dims(int *w_out, int *h_out) {
    int cw = CURSOR_W;
    int ch = CURSOR_H;
    if (!cursor_icon_dims(0, &cw, &ch)) {
        cw = CURSOR_W;
        ch = CURSOR_H;
    }
    if (w_out) *w_out = cw;
    if (h_out) *h_out = ch;
}

static ic_canvas_t bb_canvas(int w, int h) {
    ic_canvas_t c;
    c.px = back_buffer;
    c.w = w;
    c.h = h;
    return c;
}

static ic_canvas_t layer_canvas(int w, int h) {
    ic_canvas_t c;
    c.px = desktop_layer;
    c.w = w;
    c.h = h;
    return c;
}

/* Tight 64-bit copy: the full-frame blit at 1920x1080 is the hot path
 * on real hardware, and a plain per-pixel 32-bit loop at -O0 is slow. */
static void copy_pixels(uint32_t *dst, const uint32_t *src, int count) {
    uint64_t *d = (uint64_t *)dst;
    const uint64_t *s = (const uint64_t *)src;
    int n = count >> 1;
    int i = 0;
    for (; i + 3 < n; i += 4) {
        d[i] = s[i];
        d[i + 1] = s[i + 1];
        d[i + 2] = s[i + 2];
        d[i + 3] = s[i + 3];
    }
    for (; i < n; i++) d[i] = s[i];
    if (count & 1) dst[count - 1] = src[count - 1];
}

/* Copy one row while dimming it toward the wallpaper (unfocused windows):
 * Windows/Linux keep the active window vivid and mute the others; a flat
 * 60/40 blend gives the same depth cue at row-copy speed. */
#define DIM_NUM 60
#define DIM_DEN 100
static void copy_pixels_dim(uint32_t *dst, const uint32_t *src, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t c = src[i];
        uint32_t r = ((c >> 16) & 0xFF) * DIM_NUM / DIM_DEN;
        uint32_t g = ((c >> 8) & 0xFF) * DIM_NUM / DIM_DEN;
        uint32_t b = (c & 0xFF) * DIM_NUM / DIM_DEN;
        dst[i] = (r << 16) | (g << 8) | b;
    }
}

static uint32_t blend_over(uint32_t dst, uint32_t src, int alpha) {
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = src & 0xFF;
    int inv = 255 - alpha;
    return (((sr * (uint32_t)alpha + dr * (uint32_t)inv) / 255) << 16) |
           (((sg * (uint32_t)alpha + dg * (uint32_t)inv) / 255) << 8) |
           ((sb * (uint32_t)alpha + db * (uint32_t)inv) / 255);
}

/* Cursor is blitted straight onto the real framebuffer, above the scene
 * buffer, so a mouse move never rebuilds the frame underneath. */
static void draw_cursor_at(int w, int h, int mx, int my) {
    const ic_icon_t *icon;
    int dw;
    int dh;

    if (cursor_icon_dims(&icon, &dw, &dh)) {
        for (int dy = 0; dy < dh; dy++) {
            int py = my + dy;
            int sy = (int)((uint64_t)dy * icon->h / dh);
            if (py < 0 || py >= h) continue;
            if (sy >= icon->h) sy = icon->h - 1;
            for (int dx = 0; dx < dw; dx++) {
                int px = mx + dx;
                int sx = (int)((uint64_t)dx * icon->w / dw);
                const uint8_t *p;
                uint32_t src;
                uint32_t dst;
                if (px < 0 || px >= w) continue;
                if (sx >= icon->w) sx = icon->w - 1;
                p = icon->rgba + (uint64_t)(sy * icon->w + sx) * 4;
                if (p[3] == 0) continue;
                src = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                dst = back_buffer[py * w + px];
                fb_write_px(px, py, p[3] == 255 ? src : blend_over(dst, src, p[3]));
            }
        }
        return;
    }

    for (int cy = 0; cy < CURSOR_H; cy++) {
        int py = my + cy;
        if (py < 0 || py >= h) continue;
        for (int cx = 0; cx < CURSOR_W; cx++) {
            const uint8_t *p = cursor_rgba[cy][cx];
            int px = mx + cx;
            if (px < 0 || px >= w) continue;
            if (p[3] == 0) continue;
            {
                uint32_t src = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
                uint32_t dst = back_buffer[py * w + px];
                fb_write_px(px, py, p[3] == 255 ? src : blend_over(dst, src, p[3]));
            }
        }
    }
}

#define WALL_TOP    IC_WALL_TOP
#define WALL_BOTTOM IC_WALL_BOTTOM

/* Modern flat wallpaper: a calm blue vertical gradient, rendered once
 * into desktop_layer.  No per-pixel hills or per-row re-blends at
 * runtime - this is the base every frame is copied from. */
static void draw_desktop_icon_layer(int w, int h, int x, int y,
                                    const char *label, const char *icon_name) {
    ic_canvas_t c = layer_canvas(w, h);
    const ic_icon_t *icon = ic_icon_builtin(icon_name);
    uint32_t label_bg = ic_blend(WALL_TOP, WALL_BOTTOM, y + 60, h);
    if (icon) {
        ic_icon_draw(&c, x + 13, y + 8, 48, 48, icon);
    }
    ic_text_clip(&c, x + 4, y + 60, label, 0x00FFFFFF, label_bg, 66);
}

static void build_desktop_layer(void) {
    /* The compiled main loop has been observed passing corrupted w/h here
     * (the local that main reads from fb_info gets disturbed before the
     * call, and the height arrives as BACK_BUFFER_HEIGHT), which walks
     * ic_rect past the desktop_layer buffer and panics the kernel.  Never
     * trust stack-carried dimensions: the kernel-reported fb_info struct
     * in BSS is provably correct at this point, so derive everything from
     * it and clamp to the back buffer. */
    int w = (int)fb_info.width;
    int h = (int)fb_info.height;
    if (w <= 0 || h <= 0) return;
    if (w > BACK_BUFFER_WIDTH) w = BACK_BUFFER_WIDTH;
    if (h > BACK_BUFFER_HEIGHT) h = BACK_BUFFER_HEIGHT;
    ic_canvas_t c = layer_canvas(w, h);
    for (int y = 0; y < h; y++) {
        ic_rect(&c, 0, y, w, 1, ic_blend(WALL_TOP, WALL_BOTTOM, y, h));
    }
    draw_desktop_icon_layer(w, h, 22, 64, "Explorer", "folder");
    draw_desktop_icon_layer(w, h, 22, 154, "Terminal", "terminal");
    draw_desktop_icon_layer(w, h, 22, 244, "Music", "music");
    draw_desktop_icon_layer(w, h, 22, 334, "Browser", "app");
}

static void draw_start_button(int w, int h, int active) {
    ic_canvas_t c = bb_canvas(w, h);
    int y = h - TASKBAR_H + 6;
    uint32_t fill = active ? theme->accent : 0x001E293B;
    uint32_t fg = active ? theme->text_on_accent : 0x00F1F5F9;
    const ic_icon_t *icon = ic_icon_builtin("app");
    ic_rect_r(&c, 6, y, 94, 30, IC_RADIUS_BUTTON, fill);
    if (!active) ic_outline_r(&c, 6,y,94,30,IC_RADIUS_BUTTON,0x00334155);
    if (icon) ic_icon_draw(&c, 12, y + 4, 22, 22, icon);
    ic_text(&c, 40, y + 7, "Start", fg, fill);
}

static const char *window_icon_name(const char *title) {
    char t[40];
    ic_strcpy(t, title, sizeof(t));
    for (int i = 0; t[i]; i++) t[i] = ic_lower(t[i]);
    if (ic_strprefix(t, "terminal")) return "terminal";
    if (ic_strprefix(t, "icda explorer")) return "folder";
    if (ic_strprefix(t, "disk")) return "disk";
    if (ic_strprefix(t, "editor") || ic_strprefix(t, "notepad")) return "editor";
    if (ic_strprefix(t, "icda demo")) return "app";
    if (ic_strprefix(t, "icda browser")) return "app";
    return "app";
}

static void draw_taskbar(int w, int h) {
    ic_canvas_t c = bb_canvas(w, h);
    int y = h - TASKBAR_H;
    int tx = 112;
    icda_audio_info_t audio;
    ic_rect(&c, 0, y, w, TASKBAR_H, theme->taskbar_top);
    ic_hline(&c, 0, y, w, 0x00334155);
    draw_start_button(w, h, start_menu_open);
    for (int i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        int idx = z_order[i];
        wm_window_t *win = &windows[idx];
        const ic_icon_t *icon;
        if (!win->valid) continue;
        {
            int focused = focused_window_idx == idx && !win->minimized;
            uint32_t fill = focused ? theme->accent : 0x001E293B;
            uint32_t fg = focused ? theme->text_on_accent : 0x00F1F5F9;
            ic_rect_r(&c, tx, y + 7, 136, 28, IC_RADIUS_BUTTON, fill);
            if (!focused) ic_outline_r(&c, tx, y+7, 136, 28, IC_RADIUS_BUTTON, 0x00334155);
            if (focused) ic_rect(&c, tx+16, y+30, 104, 2, 0x00FFFFFF);
            icon = ic_icon_builtin(window_icon_name(win->title));
            if (icon) ic_icon_draw(&c, tx + 6, y + 11, 20, 20, icon);
            ic_text_clip(&c, tx + 30, y + 13, win->title, fg, fill, 100);
            if (win->minimized) ic_rect(&c, tx + 122, y + 26, 8, 2, fg);
        }
        tx += 142;
    }
    if ((long)icda_audio_info(&audio) >= 0 && audio.active) {
        ic_text_clip(&c, w - 176, y + 14, "Audio:", 0x0094A3B8, theme->taskbar_top, 56);
        ic_text_clip(&c, w - 120, y + 14, audio.name, 0x00F1F5F9, theme->taskbar_top, 104);
    } else {
        char clk[16];
        uint64_t t = icda_ticks();
        uint64_t secs = t/100;
        uint64_t mins = (secs/60)%60;
        uint64_t hrs = (secs/3600)%24;
        char hbuf[8], mbuf[8];
        ic_uint_to_str(hrs, hbuf, sizeof(hbuf));
        ic_uint_to_str(mins, mbuf, sizeof(mbuf));
        clk[0]=0;
        if (hrs<10) ic_strcat(clk,"0",sizeof(clk));
        ic_strcat(clk,hbuf,sizeof(clk));
        ic_strcat(clk,":",sizeof(clk));
        if (mins<10) ic_strcat(clk,"0",sizeof(clk));
        ic_strcat(clk,mbuf,sizeof(clk));
        ic_text_clip(&c, w - 68, y+14, clk, 0x00F1F5F9, theme->taskbar_top, 48);
        ic_rect_r(&c, w-108, y+18, 4,4,2, 0x0038BDF8);
        ic_rect_r(&c, w-100, y+18, 4,4,2, 0x0094A3B8);
        ic_rect_r(&c, w-92, y+18, 4,4,2, 0x00475569);
    }
}

static void draw_start_row(int sw, int sh, int x, int y, int w, int h,
                           const char *icon_name, const char *label, int hover) {
    ic_canvas_t c = bb_canvas(sw, sh);
    const ic_icon_t *icon = ic_icon_builtin(icon_name);
    uint32_t fill = hover ? theme->accent : 0x001E293B;
    uint32_t fg = hover ? theme->text_on_accent : 0x00F1F5F9;
    ic_rect_r(&c, x, y, w, h, IC_RADIUS_BUTTON, fill);
    if (icon) ic_icon_draw(&c, x + 6, y + 4, 22, 22, icon);
    ic_text(&c, x + 34, y + 7, label, fg, fill);
}

/* Start menu layout: apps at the top, system actions (task manager +
 * power) pinned at the bottom under a divider - the modern pattern. */
#define START_MENU_W 270
#define START_MENU_H 384
#define START_ROW_H 30
#define START_ROW_GAP 32

static void draw_start_menu(int w, int h, int mx, int my) {
    ic_canvas_t c = bb_canvas(w, h);
    int x = 6;
    int y = h - TASKBAR_H - START_MENU_H;
    if (!start_menu_open) return;
    ic_draw_shadow(&c, x, y, START_MENU_W, START_MENU_H, 8, IC_SHADOW_COLOR);
    ic_rect_r(&c, x, y, START_MENU_W, START_MENU_H, IC_RADIUS_PANEL, 0x001E293B);
    ic_outline_r(&c, x, y, START_MENU_W, START_MENU_H, IC_RADIUS_PANEL, 0x00334155);
    ic_text(&c, x + 16, y + 13, "ICDA Desktop", 0x00F1F5F9, 0x001E293B);
    ic_hline(&c, x + 12, y + 44, START_MENU_W - 24, 0x00334155);

    draw_start_row(w, h, x + 12, y + 56, 246, START_ROW_H, "folder", "Explorer",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 56, 246, START_ROW_H}));
    draw_start_row(w, h, x + 12, y + 88, 246, START_ROW_H, "terminal", "Terminal",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 88, 246, START_ROW_H}));
    draw_start_row(w, h, x + 12, y + 120, 246, START_ROW_H, "disk", "Disk Manager",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 120, 246, START_ROW_H}));
    draw_start_row(w, h, x + 12, y + 152, 246, START_ROW_H, "audio", "Audio Player",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 152, 246, START_ROW_H}));
    draw_start_row(w, h, x + 12, y + 184, 246, START_ROW_H, "app", "Browser",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 184, 246, START_ROW_H}));

    /* System section */
    ic_hline(&c, x + 12, y + 224, START_MENU_W - 24, 0x003C4043);
    draw_start_row(w, h, x + 12, y + 232, 246, START_ROW_H, "gear", "Task Manager",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 232, 246, START_ROW_H}));
    draw_start_row(w, h, x + 12, y + 264, 246, START_ROW_H, "gear", "Shutdown",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 264, 246, START_ROW_H}));
    draw_start_row(w, h, x + 12, y + 296, 246, START_ROW_H, "gear", "Restart",
                   ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 296, 246, START_ROW_H}));
}

/* ---- focus notifications ------------------------------------------------- */

static void notify_focus_change(int old_idx, int new_idx) {
    if (old_idx == new_idx) return;
    if (old_idx >= 0 && old_idx < MAX_WINDOWS && windows[old_idx].valid) {
        gui_msg_t m;
        clear_msg(&m);
        m.type = GUI_MSG_FOCUS;
        m.window_id = windows[old_idx].id;
        m.focus.focused = 0;
        icda_msg_send(windows[old_idx].app_queue_handle, &m);
    }
    if (new_idx >= 0 && new_idx < MAX_WINDOWS && windows[new_idx].valid) {
        gui_msg_t m;
        clear_msg(&m);
        m.type = GUI_MSG_FOCUS;
        m.window_id = windows[new_idx].id;
        m.focus.focused = 1;
        icda_msg_send(windows[new_idx].app_queue_handle, &m);
    }
}

static void set_focus(int idx) {
    int old = focused_window_idx;
    /* The compiled main loop has been observed passing a garbage idx into
     * the focus path (register-carried loop state disturbed by message
     * handling); windows[idx] would then index anywhere in the address
     * space and fault the kernel.  Never dereference without a bounds +
     * validity check. */
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    if (old >= 0 && old < MAX_WINDOWS && windows[old].valid) {
        mark_dirty_win(&windows[old]);
    }
    focused_window_idx = idx;
    /* Do the windows[] access BEFORE notify_focus_change's icda_msg_send:
     * the int 0x80 syscall path has been observed clobbering RDX (it comes
     * back holding a kernel direct-map pointer), and the compiler keeps
     * &windows[idx] live across the call to write focus_glow.  Order the
     * write first so no user pointer is dereferenced after a syscall. */
    if (idx != old && windows[idx].valid) {
        mark_dirty_win(&windows[idx]);
    }
    notify_focus_change(old, focused_window_idx);
}

static void bring_to_front(int win_idx) {
    int z_idx = -1;
    if (win_idx < 0 || win_idx >= MAX_WINDOWS || !windows[win_idx].valid) return;
    for (int i = 0; i < num_windows; i++) {
        if (z_order[i] == win_idx) {
            z_idx = i;
            break;
        }
    }
    if (z_idx != -1) {
        for (int i = z_idx; i < num_windows - 1; i++) z_order[i] = z_order[i + 1];
        z_order[num_windows - 1] = win_idx;
    }
    windows[win_idx].minimized = 0;
    mark_dirty_win(&windows[win_idx]);
    set_focus(win_idx);
}

static void focus_top_visible(void) {
    int old = focused_window_idx;
    int found = -1;
    for (int i = num_windows - 1; i >= 0; i--) {
        int idx = z_order[i];
        if (windows[idx].valid && !windows[idx].minimized) {
            found = idx;
            break;
        }
    }
    if (found >= 0) {
        set_focus(found);
    } else if (old != -1) {
        focused_window_idx = -1;
        notify_focus_change(old, -1);
    } else {
        focused_window_idx = -1;
    }
}

static void remove_window(int win_idx) {
    int z_idx = -1;
    for (int i = 0; i < num_windows; i++) {
        if (z_order[i] == win_idx) {
            z_idx = i;
            break;
        }
    }
    if (z_idx != -1) {
        for (int i = z_idx; i < num_windows - 1; i++) z_order[i] = z_order[i + 1];
        num_windows--;
    }
    if (focused_window_idx == win_idx) focus_top_visible();
}

static int alloc_window_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].valid) return i;
    }
    return -1;
}

static int find_window_by_id(uint32_t id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].valid && windows[i].id == id) return i;
    }
    return -1;
}

static void send_close_to_app(wm_window_t *win) {
    gui_msg_t close_msg;
    clear_msg(&close_msg);
    close_msg.type = GUI_MSG_CLOSE_WINDOW;
    close_msg.window_id = win->id;
    // Real HW: app queue may be full if app is busy listing / decoding Papirus 32px icons → drop instead of blocking WM
    if (icda_msg_poll(win->app_queue_handle) > 32) return;
    icda_msg_send(win->app_queue_handle, &close_msg);
}
static int send_maybe(uint64_t q, gui_msg_t *m){
    if (icda_msg_poll(q) > 32) return -1;
    return icda_msg_send(q, m);
}

static void clamp_window(wm_window_t *win, int w, int h) {
    if (win->x < 4) win->x = 4;
    if (win->y < IC_TITLE_H + 4) win->y = IC_TITLE_H + 4;
    if (win->x + win->w > w - 4) win->x = w - win->w - 4;
    if (win->y + win->h > h - TASKBAR_H - 4) win->y = h - TASKBAR_H - win->h - 4;
    if (win->x < 4) win->x = 4;
    if (win->y < IC_TITLE_H + 4) win->y = IC_TITLE_H + 4;
}

/* Start a window geometry animation.  The window's current position/size
 * is the "from" rect; the target is the "to" rect.  The compositor
 * interpolates between them over IC_ANIM_MAX ticks. */
static void start_anim(wm_window_t *win, int kind,
                       int from_x, int from_y, int from_w, int from_h,
                       int to_x, int to_y, int to_w, int to_h) {
    if (!win || !win->valid) return;
    win->anim_kind = kind;
    win->anim_from_x = from_x;
    win->anim_from_y = from_y;
    win->anim_from_w = from_w;
    win->anim_from_h = from_h;
    win->anim_to_x = to_x;
    win->anim_to_y = to_y;
    win->anim_to_w = to_w;
    win->anim_to_h = to_h;
    win->anim = 0;
    mark_dirty_win(win);
}

/* Animate a window minimizing down to its taskbar button. */
static void animate_minimize(wm_window_t *win, int w, int h) {
    int tx = 112;
    int ty = h - TASKBAR_H + 7;
    int i;
    if (!win || !win->valid) return;
    for (i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        if (z_order[i] == (int)(win - windows)) break;
        tx += 142;
    }
    start_anim(win, WM_ANIM_MINIMIZE,
               win->x, win->y, win->w, win->h,
               tx, ty, 136, 28);
}

/* Animate a window restoring from its taskbar button back to its
 * previous position/size. */
static void animate_restore(wm_window_t *win, int w, int h) {
    int tx = 112;
    int ty = h - TASKBAR_H + 7;
    int i;
    if (!win || !win->valid) return;
    for (i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        if (z_order[i] == (int)(win - windows)) break;
        tx += 142;
    }
    start_anim(win, WM_ANIM_RESTORE,
               tx, ty, 136, 28,
               win->x, win->y, win->w, win->h);
}

/* Animate a window maximizing to fill the screen above the taskbar. */
static void animate_maximize(wm_window_t *win, int w, int h) {
    if (!win || !win->valid) return;
    start_anim(win, WM_ANIM_MAXIMIZE,
               win->x, win->y, win->w, win->h,
               4, IC_TITLE_H + 4, w - 8, h - TASKBAR_H - IC_TITLE_H - 8);
}

/* Animate a window un-maximizing back to its restore rect. */
static void animate_unmaximize(wm_window_t *win) {
    if (!win || !win->valid) return;
    start_anim(win, WM_ANIM_UNMAXIMIZE,
               win->x, win->y, win->w, win->h,
               win->restore_x, win->restore_y, win->restore_w, win->restore_h);
}

/* Toggle maximize on a window.  Saves the restore rect on first
 * maximize; restores it on un-maximize. */
static void toggle_maximize(wm_window_t *win, int w, int h) {
    if (!win || !win->valid) return;
    if (!win->maximized) {
        win->restore_x = win->x;
        win->restore_y = win->y;
        win->restore_w = win->w;
        win->restore_h = win->h;
        win->maximized = 1;
        animate_maximize(win, w, h);
    } else {
        win->maximized = 0;
        animate_unmaximize(win);
    }
}

static void open_window_from_msg(gui_msg_t *msg, uint64_t wm_queue, int w, int h, uint32_t *next_win_id) {
    int slot = alloc_window_slot();
    gui_msg_t reply;
    if (slot == -1) {
        clear_msg(&reply);
        reply.type = GUI_MSG_OPEN_FAIL;
        icda_msg_send(msg->window_id, &reply);
        return;
    }

    {
        int win_w = msg->open_req.w;
        int win_h = msg->open_req.h;
        if (win_w < 160) win_w = 160;
        if (win_h < 120) win_h = 120;
        if (win_w > w - 40) win_w = w - 40;
        if (win_h > h - TASKBAR_H - 58) win_h = h - TASKBAR_H - 58;

        {
            uint64_t shm_hnd = icda_shm_create((uint64_t)win_w * win_h * 4);
            if (!shm_hnd) {
                clear_msg(&reply);
                reply.type = GUI_MSG_OPEN_FAIL;
                icda_msg_send(msg->window_id, &reply);
                return;
            }
            {
                uint64_t map_addr = icda_shm_map(shm_hnd);
                if (!map_addr) {
                    icda_shm_close(shm_hnd);
                    clear_msg(&reply);
                    reply.type = GUI_MSG_OPEN_FAIL;
                    icda_msg_send(msg->window_id, &reply);
                    return;
                }

                {
                    wm_window_t *win = &windows[slot];
                    int range_x = w - win_w - 120;
                    int range_y = h - win_h - TASKBAR_H - 110;
                    if (range_x < 1) range_x = 1;
                    if (range_y < 1) range_y = 1;

                    win->valid = 1;
                    win->id = (*next_win_id)++;
                    win->app_queue_handle = msg->window_id;
                    win->shm_handle = shm_hnd;
                    win->pixels = (uint32_t*)map_addr;
                    win->w = win_w;
                    win->h = win_h;
                    win->x = 92 + (slot * 38) % range_x;
                    win->y = 78 + (slot * 32) % range_y;
                    win->minimized = 0;
                    win->maximized = 0;
                    win->anim = 0;
                    win->anim_kind = WM_ANIM_NONE;
                    win->restore_x = win->x;
                    win->restore_y = win->y;
                    win->restore_w = win_w;
                    win->restore_h = win_h;
                    for (int i = 0; i < win_w * win_h; i++) win->pixels[i] = 0x00FFFFFF;
                    {
                        int ti = 0;
                        while (msg->open_req.title[ti] && ti < 31) {
                            win->title[ti] = msg->open_req.title[ti];
                            ti++;
                        }
                        win->title[ti] = 0;
                    }
                    clamp_window(win, w, h);

                    z_order[num_windows++] = slot;

                    clear_msg(&reply);
                    reply.type = GUI_MSG_OPEN_OK;
                    reply.window_id = win->id;
                    reply.open_ok.shm_handle = shm_hnd;
                    reply.open_ok.w = win_w;
                    reply.open_ok.h = win_h;
                    reply.open_ok.reply_queue = wm_queue;
                    icda_msg_send(win->app_queue_handle, &reply);

                    mark_dirty_win(win);

                    /* Notify focus only after the open handshake completes:
                     * the app is blocked in recv() waiting for OPEN_OK, so a
                     * FOCUS message sent first would be mistaken for the
                     * reply and abort the window open. */
                    {
                        set_focus(slot);
                    }
                }
            }
        }
    }
}

static int handle_taskbar_click(int mx, int my, int w, int h) {
    int task_y = h - TASKBAR_H;
    int tx = 112;
    (void)w;
    if (ic_hit_rect(mx, my, (ic_rect_t){6, task_y + 6, 94, 30})) {
        start_menu_open = !start_menu_open;
        mark_dirty(6, h - TASKBAR_H - START_MENU_H, START_MENU_W, START_MENU_H + TASKBAR_H);
        return 1;
    }
    for (int i = 0; i < num_windows && tx + 118 < w - 180; i++) {
        int idx = z_order[i];
        if (windows[idx].valid && ic_hit_rect(mx, my, (ic_rect_t){tx, task_y + 7, 136, 28})) {
            if (focused_window_idx == idx && !windows[idx].minimized) {
                animate_minimize(&windows[idx], w, h);
                windows[idx].minimized = 1;
                focus_top_visible();
            } else {
                if (windows[idx].minimized) {
                    animate_restore(&windows[idx], w, h);
                }
                bring_to_front(idx);
            }
            start_menu_open = 0;
            return 1;
        }
        tx += 142;
    }
    return 0;
}

static int handle_start_menu_click(int mx, int my, int w, int h) {
    int x = 6;
    int y = h - TASKBAR_H - START_MENU_H;
    (void)w;
    if (!start_menu_open) return 0;
    if (!ic_hit_rect(mx, my, (ic_rect_t){x, y, START_MENU_W, START_MENU_H})) {
        /* Outside the menu: dismiss it. Returning 1 here (instead of 0)
         * stops the taskbar click handler from immediately re-opening it
         * when the user clicks the Start button to close the menu. */
        start_menu_open = 0;
        mark_dirty(x, y, START_MENU_W, START_MENU_H);
        return 1;
    }
    if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 56, 246, 30})) icda_spawn("/apps/desktop.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 88, 246, 30})) icda_spawn("/apps/terminal.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 120, 246, 30})) icda_spawn("/apps/diskman.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 152, 246, 30})) icda_spawn("/apps/audioplay.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 184, 246, 30})) icda_spawn("/apps/browser.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 232, 246, 30})) icda_spawn("/apps/taskman.app");
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 264, 246, 30})) icda_power(0);
    else if (ic_hit_rect(mx, my, (ic_rect_t){x + 12, y + 296, 246, 30})) icda_power(1);
    start_menu_open = 0;
    mark_dirty(x, y, START_MENU_W, START_MENU_H);
    return 1;
}

static int handle_desktop_icon_click(int mx, int my) {
    if (ic_hit_rect(mx, my, (ic_rect_t){22, 64, 74, 74})) {
        icda_spawn("/apps/desktop.app");
        return 1;
    }
    if (ic_hit_rect(mx, my, (ic_rect_t){22, 154, 74, 74})) {
        icda_spawn("/apps/terminal.app");
        return 1;
    }
    if (ic_hit_rect(mx, my, (ic_rect_t){22, 244, 74, 74})) {
        icda_spawn("/apps/audioplay.app");
        return 1;
    }
    if (ic_hit_rect(mx, my, (ic_rect_t){22, 334, 74, 74})) {
        icda_spawn("/apps/browser.app");
        return 1;
    }
    return 0;
}

static void composite_window(wm_window_t *win, int idx, int w, int h, int mx, int my) {
    ic_canvas_t c = bb_canvas(w, h);
    int active;
    int anim;
    int wx;
    int wy;
    int cw;
    int ch;

    if (!win->valid || win->minimized) return;
    active = focused_window_idx == idx;

    anim = win->anim;
    if (anim < 0) anim = 0;
    if (anim > IC_ANIM_MAX) anim = IC_ANIM_MAX;

    /* Interpolate the window rect between anim_from and anim_to while
     * a geometry animation (minimize/restore/maximize/unmaximize) runs. */
    if (win->anim_kind != WM_ANIM_NONE && win->anim < IC_ANIM_MAX) {
        int t = anim;
        int d = IC_ANIM_MAX;
        wx = win->anim_from_x + (win->anim_to_x - win->anim_from_x) * t / d;
        wy = win->anim_from_y + (win->anim_to_y - win->anim_from_y) * t / d;
        cw = win->anim_from_w + (win->anim_to_w - win->anim_from_w) * t / d;
        ch = win->anim_from_h + (win->anim_to_h - win->anim_from_h) * t / d;
        if (cw < 1) cw = 1;
        if (ch < 1) ch = 1;
    } else {
        wx = win->x;
        wy = win->y;
        cw = win->w;
        ch = win->h;
    }

    {
        ic_window_t iw;
        ic_theme_t t = *theme;
        iw.x = wx;
        iw.y = wy;
        iw.w = cw;
        iw.h = ch;
        iw.focused = active;
        iw.minimized = 0;
        iw.anim = win->anim;
        iw.title = win->title;
        iw.hover_close = ic_hit_close(&iw, mx, my);
        iw.hover_min = ic_hit_minimize(&iw, mx, my);
        iw.hover_max = ic_hit_maximize(&iw, mx, my);
        ic_draw_chrome(&c, &t, &iw, ic_icon_builtin("close"), ic_icon_builtin("min"),
                       ic_icon_builtin("max"));
    }

    /* client pixels.  Unfocused windows are dimmed so the active one
     * reads as the foreground (the same depth cue Windows/Linux use). */
    if (wx >= 0 && wy >= 0 && wx + cw <= w && wy + ch <= h) {
        /* Fully visible window: tight row copies instead of per-pixel
         * bounds checks - the hot path when windows sit on screen. */
        if (active) {
            for (int cy = 0; cy < ch; cy++) {
                copy_pixels(back_buffer + (wy + cy) * w + wx,
                            win->pixels + cy * win->w, cw);
            }
        } else {
            for (int cy = 0; cy < ch; cy++) {
                copy_pixels_dim(back_buffer + (wy + cy) * w + wx,
                                win->pixels + cy * win->w, cw);
            }
        }
    } else {
        for (int cy = 0; cy < ch; cy++) {
            int py = wy + cy;
            if (py < 0 || py >= h) continue;
            for (int cx = 0; cx < cw; cx++) {
                int px = wx + cx;
                if (px < 0 || px >= w) continue;
                {
                    uint32_t c = win->pixels[cy * win->w + cx];
                    if (!active) {
                        uint32_t r = ((c >> 16) & 0xFF) * DIM_NUM / DIM_DEN;
                        uint32_t g = ((c >> 8) & 0xFF) * DIM_NUM / DIM_DEN;
                        uint32_t b = (c & 0xFF) * DIM_NUM / DIM_DEN;
                        c = (r << 16) | (g << 8) | b;
                    }
                    back_buffer[py * w + px] = c;
                }
            }
        }
    }

    /* Advance the animation.  When it completes, snap the window to the
     * target rect and clear the animation state. */
    if (win->anim_kind != WM_ANIM_NONE) {
        if (win->anim >= IC_ANIM_MAX) {
            win->x = win->anim_to_x;
            win->y = win->anim_to_y;
            win->w = win->anim_to_w;
            win->h = win->anim_to_h;
            win->anim_kind = WM_ANIM_NONE;
            win->anim = 0;
        } else {
            win->anim++;
        }
    }

}

static uint32_t fb_pitch_pixels(void) {
    return fb_info.pitch ? fb_info.pitch / 4 : (uint32_t)fb_info.width;
}

/* Write one pixel to the real framebuffer in its native format.  32bpp is
 * the fast path (real GPUs); 24bpp is what GRUB/QEMU hand out in
 * fallback modes - the scene buffer is 32bpp ARGB, so blitting it as-is
 * would both mangle colors and, worse, walk past the end of the mapped
 * region (pitch 2400 vs 3200 bytes/row). */
static void fb_write_px(int x, int y, uint32_t color) {
    /* Hard-clamp against the kernel-reported mapping (BSS state, never
     * disturbed); a garbage coordinate here writes past the mapping and
     * panics the kernel. */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)fb_info.width) x = (int)fb_info.width - 1;
    if (y >= (int)fb_info.height) y = (int)fb_info.height - 1;
    if (fb_info.bpp == 32) {
        real_fb[y * fb_pitch_pixels() + x] = color;
    } else {
        uint8_t *p = (uint8_t *)real_fb + (uint64_t)y * fb_info.pitch + (uint64_t)x * 3;
        p[0] = (uint8_t)(color >> 16);
        p[1] = (uint8_t)(color >> 8);
        p[2] = (uint8_t)color;
    }
}

static void blit_row_24(uint8_t *dst, const uint32_t *src, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t c = src[i];
        dst[i * 3 + 0] = (uint8_t)(c >> 16);
        dst[i * 3 + 1] = (uint8_t)(c >> 8);
        dst[i * 3 + 2] = (uint8_t)c;
    }
}

static void blit_to_screen(int w, int h) {
    uint32_t pitch = fb_pitch_pixels();
    /* Never blit more than the kernel actually mapped: the w/h params
     * come from the caller's stack and can be disturbed, while
     * fb_info (BSS) always holds the true framebuffer dimensions. */
    if (w <= 0 || h <= 0) return;
    if (w > (int)fb_info.width) w = (int)fb_info.width;
    if (h > (int)fb_info.height) h = (int)fb_info.height;
    if (fb_info.bpp != 32) {
        for (int y = 0; y < h; y++) {
            blit_row_24((uint8_t *)real_fb + (uint64_t)y * fb_info.pitch,
                        back_buffer + (uint64_t)y * w, w);
        }
        return;
    }
    if (pitch == (uint32_t)w) {
        copy_pixels(real_fb, back_buffer, w * h);
    } else {
        for (int y = 0; y < h; y++) {
            copy_pixels(real_fb + y * pitch, back_buffer + y * w, w);
        }
    }
}

/* Restore the cursor area of the real framebuffer from the scene buffer.
 * Called before any full/dirty composite so a stale cursor from the
 * previous frame cannot ghost on screen when the cursor sits outside
 * the damaged rectangles. */
static void restore_cursor_area(int mx, int my) {
    int sw = (int)fb_info.width;
    int sh = (int)fb_info.height;
    int cw, ch, x0, y0, x1, y1;

    if (sw <= 0 || sh <= 0) return;
    cursor_dims(&cw, &ch);
    if (mx < 0) mx = 0; else if (mx >= sw) mx = sw - 1;
    if (my < 0) my = 0; else if (my >= sh) my = sh - 1;

    x0 = mx;
    y0 = my;
    x1 = mx + cw + 2;
    y1 = my + ch + 2;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    if (x1 <= x0 || y1 <= y0) return;

    if (fb_info.bpp == 32) {
        uint32_t pitch = fb_pitch_pixels();
        for (int y = y0; y < y1; y++) {
            copy_pixels(real_fb + y * pitch + x0, back_buffer + y * sw + x0, x1 - x0);
        }
    } else {
        for (int y = y0; y < y1; y++) {
            uint8_t *dst = (uint8_t *)real_fb + (uint64_t)y * fb_info.pitch;
            const uint32_t *src = back_buffer + (uint64_t)y * sw;
            for (int x = x0; x < x1; x++) {
                uint32_t c = src[x];
                uint8_t *p = dst + (uint64_t)x * 3;
                p[0] = (uint8_t)(c >> 16);
                p[1] = (uint8_t)(c >> 8);
                p[2] = (uint8_t)c;
            }
        }
    }
}

static void composite_screen(int w, int h, int mouse_x, int mouse_y) {
    /* Trust fb_info for the copy bounds too: desktop_layer is sized to
     * the back buffer, and a corrupted w/h would copy past it. */
    if (w > (int)fb_info.width) w = (int)fb_info.width;
    if (h > (int)fb_info.height) h = (int)fb_info.height;
    if (w <= 0 || h <= 0) return;
    /* Erase the previous frame's cursor from the real framebuffer before
     * the full rebuild, or it ghosts on screen. */
    restore_cursor_area(mouse_x, mouse_y);
    copy_pixels(back_buffer, desktop_layer, w * h);
    for (int i = 0; i < num_windows; i++) {
        int idx = z_order[i];
        composite_window(&windows[idx], idx, w, h, mouse_x, mouse_y);
    }
    draw_taskbar(w, h);
    draw_start_menu(w, h, mouse_x, mouse_y);
    blit_to_screen(w, h);
    draw_cursor_at(w, h, mouse_x, mouse_y);
}

/* Blit one rectangle of the scene buffer to the real framebuffer. */
static void blit_region(int x, int y, int rw, int rh, int w) {
    uint32_t pitch = fb_pitch_pixels();
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > (int)fb_info.width) rw = (int)fb_info.width - x;
    if (y + rh > (int)fb_info.height) rh = (int)fb_info.height - y;
    if (rw <= 0 || rh <= 0) return;
    if (fb_info.bpp != 32) {
        for (int yy = y; yy < y + rh; yy++) {
            blit_row_24((uint8_t *)real_fb + (uint64_t)yy * fb_info.pitch + (uint64_t)x * 3,
                        back_buffer + (uint64_t)yy * w + x, rw);
        }
        return;
    }
    for (int yy = y; yy < y + rh; yy++) {
        copy_pixels(real_fb + (uint64_t)yy * pitch + x,
                    back_buffer + (uint64_t)yy * w + x, rw);
    }
}

static int rect_hit(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Rebuild only the damaged rectangles: restore them from the wallpaper
 * layer, redraw the windows/taskbar/menu that intersect them, and blit
 * just those rectangles to the screen.  This is the damage-tracking
 * compositor core - the full-frame path is only for boot and for
 * changes so large they cover the screen anyway. */
static void composite_dirty(int w, int h, int mx, int my, int pmx, int pmy) {
    if (dirty_full || dirty_count == 0) {
        composite_screen(w, h, mx, my);
        dirty_count = 0;
        dirty_full = 0;
        return;
    }

    for (int i = 0; i < dirty_count; i++) {
        dirty_rect_t *d = &dirty_rects[i];
        int rw = d->w;
        int rh = d->h;
        if (d->x + rw > (int)fb_info.width) rw = (int)fb_info.width - d->x;
        if (d->y + rh > (int)fb_info.height) rh = (int)fb_info.height - d->y;
        if (rw <= 0 || rh <= 0) continue;

        /* 1. Restore the region from the pre-rendered wallpaper. */
        for (int yy = d->y; yy < d->y + rh; yy++) {
            copy_pixels(back_buffer + (uint64_t)yy * w + d->x,
                        desktop_layer + (uint64_t)yy * w + d->x, rw);
        }

        /* 2. Redraw windows intersecting the region, bottom-up. */
        for (int zi = 0; zi < num_windows; zi++) {
            int idx = z_order[zi];
            wm_window_t *win = &windows[idx];
            if (!win->valid || win->minimized) continue;
            if (rect_hit(d->x, d->y, rw, rh,
                         win->x - SHADOW_MARGIN, win->y - IC_TITLE_H - SHADOW_MARGIN,
                         win->w + SHADOW_MARGIN * 2, win->h + IC_TITLE_H + SHADOW_MARGIN * 2)) {
                composite_window(win, idx, w, h, mx, my);
            }
        }

        /* 3. Taskbar and start menu, if the region touches them. */
        if (d->y + rh > h - TASKBAR_H) draw_taskbar(w, h);
        if (start_menu_open &&
            rect_hit(d->x, d->y, rw, rh, 6, h - TASKBAR_H - START_MENU_H, START_MENU_W, START_MENU_H)) {
            draw_start_menu(w, h, mx, my);
        }

        /* 4. Blit the region to the real framebuffer. */
        blit_region(d->x, d->y, rw, rh, w);
    }

    /* Erase the OLD cursor position AFTER dirty-rect blits (which may
     * have naturally overwritten parts of it) but BEFORE drawing the
     * new cursor.  This eliminates both ghost cursors and the flash
     * that occurred when erasing at the top of the function. */
    restore_cursor_area(pmx, pmy);
    if (mx != pmx || my != pmy)
        restore_cursor_area(mx, my);

    dirty_count = 0;
    draw_cursor_at(w, h, mx, my);
}

/* Mouse moved and nothing else changed: erase the old pointer from the
 * real framebuffer using the scene buffer and redraw it at the new spot,
 * touching only the tiny cursor rectangle instead of the whole frame. */
/* Compute the bounding box that covers two cursor positions (old + new)
 * using the actual cursor dimensions (icon cursors can be larger than
 * the built-in 12x19 bitmap). */
static void cursor_bbox(int mx, int my, int pmx, int pmy,
                        int *ox, int *oy, int *ow, int *oh) {
    int sw = (int)fb_info.width;
    int sh = (int)fb_info.height;
    int cw, ch;
    cursor_dims(&cw, &ch);
    if (sw <= 0 || sh <= 0) { *ox = *oy = *ow = *oh = 0; return; }
    if (mx < 0) mx = 0; else if (mx >= sw) mx = sw - 1;
    if (my < 0) my = 0; else if (my >= sh) my = sh - 1;
    if (pmx < 0) pmx = 0; else if (pmx >= sw) pmx = sw - 1;
    if (pmy < 0) pmy = 0; else if (pmy >= sh) pmy = sh - 1;
    *ox = pmx < mx ? pmx : mx;
    *oy = pmy < my ? pmy : my;
    *ow = (pmx > mx ? pmx : mx) + cw + 2 - *ox;
    *oh = (pmy > my ? pmy : my) + ch + 2 - *oy;
    if (*ox + *ow > sw) *ow = sw - *ox;
    if (*oy + *oh > sh) *oh = sh - *oy;
}

static void composite_cursor_only(int w, int h, int mx, int my, int pmx, int pmy) {
    int sw, sh, x0, y0, bw, bh;

    (void)w;
    (void)h;

    sw = fb_info.width;
    sh = fb_info.height;
    if (sw <= 0 || sh <= 0) return;
    cursor_bbox(mx, my, pmx, pmy, &x0, &y0, &bw, &bh);
    if (bw <= 0 || bh <= 0) return;
    if (x0 + bw > sw) bw = sw - x0;
    if (y0 + bh > sh) bh = sh - y0;
    if (bw <= 0 || bh <= 0) return;

    if (fb_info.bpp == 32) {
        uint32_t pitch = fb_pitch_pixels();
        for (int y = y0; y < y0 + bh; y++) {
            copy_pixels(real_fb + y * pitch + x0, back_buffer + y * sw + x0, bw);
        }
    } else {
        for (int y = y0; y < y0 + bh; y++) {
            uint8_t *dst = (uint8_t *)real_fb + (uint64_t)y * fb_info.pitch;
            const uint32_t *src = back_buffer + (uint64_t)y * sw;
            for (int x = x0; x < x0 + bw; x++) {
                uint32_t c = src[x];
                uint8_t *p = dst + (uint64_t)x * 3;
                p[0] = (uint8_t)(c >> 16);
                p[1] = (uint8_t)(c >> 8);
                p[2] = (uint8_t)c;
            }
        }
    }
    draw_cursor_at(sw, sh, mx, my);
}

int main(int argc, char **argv) {
    uint64_t addr;
    int w;
    int h;
    uint64_t wm_queue;
    int dragging_win_idx = -1;
    int drag_off_x = 0;
    int drag_off_y = 0;
    uint32_t next_win_id = 1;

    (void)argc;
    (void)argv;

    theme = ic_theme_default();
    build_cursor_sprite();

    /* Replace the stock icon set with whatever the user dropped into
     * /usr/share/icons as .ico files (folder.ico, terminal.ico, ...). */
    ic_icon_load_folder("/usr/share/icons");

    addr = icda_map_framebuffer(&fb_info);
    if (!addr) return -1;
    real_fb = (uint32_t*)addr;

    w = fb_info.width;
    h = fb_info.height;
    if (w > BACK_BUFFER_WIDTH) w = BACK_BUFFER_WIDTH;
    if (h > BACK_BUFFER_HEIGHT) h = BACK_BUFFER_HEIGHT;
    if (w < 320 || h < 240) return -1;

    wm_queue = icda_msg_open(WM_QUEUE_NAME);
    if (!wm_queue) return -1;

    /* No app is auto-opened: the desktop shows the wallpaper, icons and
     * taskbar only; apps launch from icons / the start menu. */

    mouse_x = w / 2;
    mouse_y = h / 2;

    /* Pre-render the static wallpaper + desktop icons once; every frame
     * is copied out of this layer instead of recomputed.  The layer
     * derives its own dimensions from fb_info (the stack w/h above can
     * be disturbed by the time this runs). */
    build_desktop_layer();

    /* First present covers the whole screen. */
    mark_dirty_full();

    {
        prev_mouse_x = mouse_x;
        prev_mouse_y = mouse_y;
        uint64_t last_composite_tick = 0;

        for (;;) {
        int need_redraw = 0;
        int mouse_moved = 0;
        while (icda_msg_poll(wm_queue) > 0) {
            gui_msg_t msg;
            if (icda_msg_recv(wm_queue, &msg, 0) != 0) continue;
            need_redraw = 1;
            if (msg.type == GUI_MSG_OPEN_WINDOW) {
                open_window_from_msg(&msg, wm_queue, w, h, &next_win_id);
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                int slot = find_window_by_id(msg.window_id);
                if (slot != -1) {
                    wm_window_t *win = &windows[slot];
                    mark_dirty_win(win);
                    icda_shm_unmap(win->shm_handle);
                    icda_shm_close(win->shm_handle);
                    win->valid = 0;
                    remove_window(slot);
                }
            } else if (msg.type == GUI_MSG_FLUSH) {
                int slot = find_window_by_id(msg.window_id);
                if (slot != -1) {
                    mark_dirty_win(&windows[slot]);
                }
            }
        }

        {
            /* Drain every queued mouse event in one pass and handle each in
             * order.  Reading a single event per loop pass let the kernel
             * ring buffer back up while a frame composited, so the cursor
             * trailed the hand and kept gliding after the mouse stopped.
             * Processing the whole batch keeps press/release edges (clicks)
             * and drags correct while tracking the live position, and only
             * the final composite runs once the batch is consumed. */
            icda_mouse_event_t mev;
            while (icda_input_read_mouse(&mev) == 0) {
                static uint64_t move_cnt = 0;
                if ((++move_cnt % 50) == 0) {
                    icda_write("WM mouse move\n");
                }
                int prev_mx = mouse_x;
                int prev_my = mouse_y;
                uint8_t prev_btn = mouse_buttons;
                mouse_moved = 1;
                mouse_x = mev.abs_x;
                mouse_y = mev.abs_y;
                mouse_buttons = mev.buttons;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_x >= w) mouse_x = w - 1;
                if (mouse_y >= h) mouse_y = h - 1;

                {
                    int left_clicked = (mouse_buttons & 1) && !(prev_btn & 1);
                    int left_released = !(mouse_buttons & 1) && (prev_btn & 1);

                    if (left_clicked || left_released) {
                        need_redraw = 1;
                    }
                    if (left_clicked) {
                        int handled = 0;
                        if (start_menu_open) handled = handle_start_menu_click(mouse_x, mouse_y, w, h);
                        if (!handled && mouse_y >= h - TASKBAR_H) handled = handle_taskbar_click(mouse_x, mouse_y, w, h);

                        if (!handled) {
                            int hit = -1;
                            start_menu_open = 0;
                            for (int i = num_windows - 1; i >= 0; i--) {
                                int idx = z_order[i];
                                if (idx < 0 || idx >= MAX_WINDOWS) continue;
                                wm_window_t *win = &windows[idx];
                                ic_window_t iw;

                                if (!win->valid || win->minimized) continue;
                                iw.x = win->x;
                                iw.y = win->y;
                                iw.w = win->w;
                                iw.h = win->h;
                                iw.title = win->title;

                                if (ic_hit_close(&iw, mouse_x, mouse_y)) {
                                    send_close_to_app(win);
                                    hit = idx;
                                    break;
                                }
                                if (ic_hit_maximize(&iw, mouse_x, mouse_y)) {
                                    toggle_maximize(win, w, h);
                                    hit = idx;
                                    break;
                                }
                                if (ic_hit_minimize(&iw, mouse_x, mouse_y)) {
                                    animate_minimize(win, w, h);
                                    win->minimized = 1;
                                    focus_top_visible();
                                    hit = idx;
                                    break;
                                }
                                if (ic_hit_title(&iw, mouse_x, mouse_y)) {
                                    bring_to_front(idx);
                                    dragging_win_idx = idx;
                                    drag_off_x = mouse_x - win->x;
                                    drag_off_y = mouse_y - win->y;
                                    hit = idx;
                                    break;
                                }
                                if (ic_hit_client(&iw, mouse_x, mouse_y)) {
                                    bring_to_front(idx);
                                    {
                                        gui_msg_t click_msg;
                                        clear_msg(&click_msg);
                                        click_msg.type = GUI_MSG_MOUSE_EVENT;
                                        click_msg.window_id = win->id;
                                        click_msg.mouse.x = mouse_x - win->x;
                                        click_msg.mouse.y = mouse_y - win->y;
                                        click_msg.mouse.buttons = mouse_buttons;
                                        send_maybe(win->app_queue_handle, &click_msg);
                                    }
                                    hit = idx;
                                    break;
                                }
                            }
                            if (hit == -1) {
                                if (!handle_desktop_icon_click(mouse_x, mouse_y)) {
                                    if (focused_window_idx != -1) {
                                        int old = focused_window_idx;
                                        focused_window_idx = -1;
                                        notify_focus_change(old, -1);
                                    }
                                }
                            }
                        }
                    } else if (left_released) {
                        dragging_win_idx = -1;
                    } else if (dragging_win_idx != -1) {
                        need_redraw = 1;
                        wm_window_t *win = &windows[dragging_win_idx];
                        if (mouse_x != prev_mx || mouse_y != prev_my) {
                            mark_dirty_win(win);
                            win->x = mouse_x - drag_off_x;
                            win->y = mouse_y - drag_off_y;
                            clamp_window(win, w, h);
                            mark_dirty_win(win);
                        }
                    } else if (focused_window_idx != -1 && (mouse_x != prev_mx || mouse_y != prev_my)) {
                        wm_window_t *win = &windows[focused_window_idx];
                        if (win->valid && !win->minimized) {
                            ic_window_t iw;
                            iw.x = win->x;
                            iw.y = win->y;
                            iw.w = win->w;
                            iw.h = win->h;
                            iw.focused = 0;
                            iw.minimized = 0;
                            iw.anim = 0;
                            iw.title = win->title;
                            iw.hover_close = 0;
                            iw.hover_min = 0;
                            iw.hover_max = 0;
                            if (ic_hit_client(&iw, mouse_x, mouse_y)) {
                                gui_msg_t motion_msg;
                                clear_msg(&motion_msg);
                                motion_msg.type = GUI_MSG_MOUSE_EVENT;
                                motion_msg.window_id = win->id;
                                motion_msg.mouse.x = mouse_x - win->x;
                                motion_msg.mouse.y = mouse_y - win->y;
                                motion_msg.mouse.buttons = mouse_buttons;
                                send_maybe(win->app_queue_handle, &motion_msg);
                            }
                        }
                    }
                }
            }
        }

        {
            long key = icda_read_char_timeout(1);
            if (key >= 0) {
                need_redraw = 1;
                if (focused_window_idx != -1) {
                    wm_window_t *win = &windows[focused_window_idx];
                    if (win->valid && !win->minimized) {
                        gui_msg_t kmsg;
                        clear_msg(&kmsg);
                        kmsg.type = GUI_MSG_KEY_EVENT;
                        kmsg.window_id = win->id;
                        kmsg.key.keycode = (uint32_t)key;
                        kmsg.key.pressed = 1;
                        send_maybe(win->app_queue_handle, &kmsg);
                        mark_dirty_win(win);
                    }
                }
            }
        }

        /* Start-menu hover needs frame updates while the pointer moves
         * over it (the cursor-only path skips the menu). */
        if (start_menu_open && mouse_moved) {
            need_redraw = 1;
            mark_dirty(6, h - TASKBAR_H - START_MENU_H, START_MENU_W, START_MENU_H);
        }

        {
            uint64_t now = icda_ticks();
            int animating = 0;
            for (int i = 0; i < num_windows; i++) {
                if (windows[i].valid &&
                    (windows[i].anim < IC_ANIM_MAX ||
                     windows[i].anim_kind != WM_ANIM_NONE)) {
                    animating = 1;
                    mark_dirty_win(&windows[i]);
                }
            }
            /* Composite at most once per tick - the vsync pacing point:
             * the frame is presented to the screen at the tick boundary
             * (like waiting on vblank), so repaints never tear mid-frame
             * and the display can never run faster than the panel.
             * Full rebuild only for the initial frame or screen-wide
             * damage; everything else goes through the dirty-rect path. */
            if (now != last_composite_tick) {
                if (need_redraw || animating || dragging_win_idx != -1 ||
                    dirty_full || dirty_count > 0) {
                    composite_dirty(w, h, mouse_x, mouse_y,
                                    prev_mouse_x, prev_mouse_y);
                    last_composite_tick = now;
                } else if (mouse_moved &&
                           (mouse_x != prev_mouse_x || mouse_y != prev_mouse_y)) {
                    composite_cursor_only(w, h, mouse_x, mouse_y,
                                          prev_mouse_x, prev_mouse_y);
                    last_composite_tick = now;
                }
            }
            prev_mouse_x = mouse_x;
            prev_mouse_y = mouse_y;
            {
                static uint64_t last_hb = 0;
                if (now - last_hb > 200) {
                    last_hb = now;
                    icda_write("WM HB\n");
                }
            }
        }

        /* Event-driven pacing: the icda_read_char_timeout(1) above is the
         * only blocking point.  Keyboard and mouse IRQs both wake it, so
         * input is handled the moment it lands instead of on a fixed
         * 10ms poll; when nothing happens it sleeps one tick, which also
         * paces window animations at the tick rate. */
        }
    }
}