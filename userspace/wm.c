














#include "libicda.h"
#include "gui_proto.h"
#include "settings_store.h"
#include "wm_frame.h"
#include "wm_shell.h"

#define MAX_WINDOWS 16
#define BACK_BUFFER_WIDTH 2560
#define BACK_BUFFER_HEIGHT 1600
#define CURSOR_W 19
#define CURSOR_H 30
#define CURSOR_SAVE_DIM 48   

#define WIN_MIN_W 320
#define WIN_MIN_H 200
#define DBLCLICK_TICKS 40





typedef enum {
    WM_ANIM_NONE = 0,
    WM_ANIM_OPEN,
    WM_ANIM_CLOSE,
    WM_ANIM_MINIMIZE,
    WM_ANIM_RESTORE,
    WM_ANIM_GEOMETRY
} wm_anim_kind_t;

typedef struct {
    int       valid;
    uint32_t  id;
    uint64_t  app_queue_handle;
    uint64_t  shm_handle;
    uint32_t *pixels;
    int       pix_w;          
    int       pix_h;
    int       x;              
    int       y;
    int       w;
    int       h;
    int       minimized;
    int       maximized;
    int       closing;
    int       anim_kind;
    uint64_t  anim_t0;
    uint32_t  anim_ms;
    ic_rect_t anim_from;
    ic_rect_t anim_to;
    int       anim_maximized; 
    int       restore_x;
    int       restore_y;
    int       restore_w;
    int       restore_h;
    wm_hit_t  hover;          
    wm_hit_t  pressed;
    int       pointer_in;     
    char      title[32];
} wm_window_t;

static wm_window_t windows[MAX_WINDOWS];
static int z_order[MAX_WINDOWS];
static int num_windows = 0;
static int task_order[MAX_WINDOWS];   
static int task_count = 0;
static int focused_window_idx = -1;







static uint32_t back_buffer[BACK_BUFFER_WIDTH * BACK_BUFFER_HEIGHT];
static uint32_t desktop_layer[BACK_BUFFER_WIDTH * BACK_BUFFER_HEIGHT];
static uint32_t layer_buffer[BACK_BUFFER_WIDTH * BACK_BUFFER_HEIGHT];
#define BLUR_SCRATCH_PX (512 * 1024)
static uint32_t blur_scratch[BLUR_SCRATCH_PX];
static uint32_t cursor_scene_save[CURSOR_SAVE_DIM * CURSOR_SAVE_DIM];
static icda_fb_info_t fb_info;
static icda_gpu_info_t gpu_info;
static uint32_t *real_fb = NULL;


static int scr_w = 0;
static int scr_h = 0;

static ic_canvas_t scene;






static int wm_flip_page = 0;   


static int mouse_x = 0;
static int mouse_y = 0;
static int prev_mouse_x = -1;
static int prev_mouse_y = -1;
static uint8_t mouse_buttons = 0;


static int drag_win = -1;           
static int drag_off_x = 0;
static int drag_off_y = 0;
static int resize_win = -1;         
static wm_hit_t resize_edge = WM_HIT_NONE;
static ic_rect_t resize_start;
static int resize_mx = 0;
static int resize_my = 0;
static int capture_win = -1;        
static int press_win = -1;          
static wm_hit_t press_hit = WM_HIT_NONE;
static int title_click_win = -1;    
static uint64_t title_click_tick = 0;




static icda_settings_t wm_settings;
static uint64_t settings_last_reload = 0;

static void build_desktop_layer(void);
static void mark_dirty_full(void);

static void settings_reload(void) {
    icda_settings_load(&wm_settings);
    if (ic_palette_reload()) {
        build_desktop_layer();
        mark_dirty_full();
    }
}

static int anim_ms(int ms) {
    return wm_settings.animations ? ms : 0;
}









#define MAX_DIRTY 32
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


static unsigned long wm_diag_composite_count = 0;
static unsigned long wm_diag_last_frame_us = 0;
static unsigned long wm_diag_max_frame_us = 0;
static unsigned long wm_diag_mouse_events = 0;
static int wm_debug_overlay = 0;

static void extend_to_materials(dirty_rect_t *r);

static void mark_dirty(int x, int y, int w, int h) {
    dirty_rect_t r;
    if (dirty_full) return;
    if (scr_w <= 0 || scr_h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return;

    r.x = x; r.y = y; r.w = w; r.h = h;
    extend_to_materials(&r);

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

static void mark_dirty_rect(ic_rect_t r) {
    mark_dirty(r.x, r.y, r.w, r.h);
}

static void mark_dirty_full(void) {
    dirty_full = 1;
    dirty_count = 0;
}


static ic_rect_t outer_of(int x, int y, int w, int h) {
    return ic_rect_make(x, y - WM_TITLE_H, w, h + WM_TITLE_H);
}

static ic_rect_t reach_of(ic_rect_t outer) {
    return ic_rect_make(outer.x - WM_SHADOW_REACH, outer.y - WM_SHADOW_REACH,
                        outer.w + 2 * WM_SHADOW_REACH, outer.h + 2 * WM_SHADOW_REACH);
}



static int win_bounds(const wm_window_t *win, ic_rect_t out[3]) {
    int n = 0;
    out[n++] = wm_frame_damage_rect(win->x, win->y, win->w, win->h);
    if (win->anim_kind == WM_ANIM_GEOMETRY) {
        out[n++] = wm_frame_damage_rect(win->anim_from.x, win->anim_from.y,
                                        win->anim_from.w, win->anim_from.h);
        out[n++] = wm_frame_damage_rect(win->anim_to.x, win->anim_to.y,
                                        win->anim_to.w, win->anim_to.h);
    } else if (win->anim_kind != WM_ANIM_NONE) {
        out[n++] = reach_of(win->anim_from);
        out[n++] = reach_of(win->anim_to);
    }
    return n;
}

static void mark_dirty_win(const wm_window_t *win) {
    ic_rect_t b[3];
    int n;
    if (!win || !win->valid) return;
    n = win_bounds(win, b);
    for (int i = 0; i < n; i++) mark_dirty_rect(b[i]);
}

static void mark_dirty_title(const wm_window_t *win) {
    if (!win || !win->valid) return;
    mark_dirty(win->x, win->y - WM_TITLE_H, win->w, WM_TITLE_H);
}

static void wm_power_sequence(int restart);




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
    int64_t t;
    int64_t cx,cy;
    if(len2==0) return (int64_t)(x-ax)*(x-ax)+(int64_t)(y-ay)*(y-ay);
    t=(int64_t)(x-ax)*dx+(int64_t)(y-ay)*dy;
    if(t<0) t=0;
    if(t>len2) t=len2;
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




static void draw_cursor_into_bb(int w, int h, int mx, int my) {
    const ic_icon_t *icon = NULL;
    int dw = CURSOR_W;
    int dh = CURSOR_H;
    int sx0, sy0, cw, ch;

    if (!cursor_icon_dims(&icon, &dw, &dh)) {
        icon = NULL;
        dw = CURSOR_W;
        dh = CURSOR_H;
    }
    if (dw > CURSOR_SAVE_DIM) dw = CURSOR_SAVE_DIM;
    if (dh > CURSOR_SAVE_DIM) dh = CURSOR_SAVE_DIM;

    sx0 = 0;
    sy0 = 0;
    if (mx < 0) { sx0 = -mx; }
    if (my < 0) { sy0 = -my; }
    cw = dw - sx0;
    ch = dh - sy0;
    if (mx + sx0 + cw > w) cw = w - mx - sx0;
    if (my + sy0 + ch > h) ch = h - my - sy0;
    if (cw <= 0 || ch <= 0) return;

    {
        int screen_x = mx + sx0;
        int screen_y = my + sy0;
        if (screen_x < 0) screen_x = 0;
        if (screen_y < 0) screen_y = 0;
        for (int y = 0; y < ch; y++) {
            copy_pixels(&cursor_scene_save[y * cw],
                        &back_buffer[(screen_y + y) * w + screen_x], cw);
        }
    }

    if (icon) {
        for (int dy = sy0; dy < sy0 + ch && dy < dh; dy++) {
            int py = my + dy;
            int sy = (int)((uint64_t)dy * icon->h / dh);
            if (py < 0 || py >= h) continue;
            if (sy >= icon->h) sy = icon->h - 1;
            for (int dx = sx0; dx < sx0 + cw && dx < dw; dx++) {
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
                back_buffer[py * w + px] =
                    p[3] == 255 ? src : blend_over(dst, src, p[3]);
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
                uint32_t src = ((uint32_t)p[0] << 16) |
                               ((uint32_t)p[1] << 8) | p[2];
                uint32_t dst = back_buffer[py * w + px];
                back_buffer[py * w + px] =
                    p[3] == 255 ? src : blend_over(dst, src, p[3]);
            }
        }
    }
}

static void restore_cursor_scene(int w, int h, int mx, int my) {
    const ic_icon_t *icon = NULL;
    int dw = CURSOR_W;
    int dh = CURSOR_H;
    int sx0, sy0, cw, ch;

    if (!cursor_icon_dims(&icon, &dw, &dh)) {
        dw = CURSOR_W;
        dh = CURSOR_H;
    }
    if (dw > CURSOR_SAVE_DIM) dw = CURSOR_SAVE_DIM;
    if (dh > CURSOR_SAVE_DIM) dh = CURSOR_SAVE_DIM;

    sx0 = 0;
    sy0 = 0;
    if (mx < 0) { sx0 = -mx; }
    if (my < 0) { sy0 = -my; }
    cw = dw - sx0;
    ch = dh - sy0;
    if (mx + sx0 + cw > w) cw = w - mx - sx0;
    if (my + sy0 + ch > h) ch = h - my - sy0;
    if (cw <= 0 || ch <= 0) return;

    {
        int screen_x = mx + sx0;
        int screen_y = my + sy0;
        if (screen_x < 0) screen_x = 0;
        if (screen_y < 0) screen_y = 0;
        for (int y = 0; y < ch; y++) {
            copy_pixels(&back_buffer[(screen_y + y) * w + screen_x],
                        &cursor_scene_save[y * cw], cw);
        }
    }
}




#define DESK_MAX_ICONS 12
#define DESK_DRAG_THRESH_PX 6
#define DESK_CFG_PATH "/cfg/desktop.cfg"

typedef struct {
    const char *label;
    const char *icon;
    const char *path;
    int pinned;     
    int cell_x;     
    int cell_y;
    int selected;
} desk_icon_t;

static desk_icon_t desk_icons[DESK_MAX_ICONS];
static int desk_icon_count = 0;
static uint64_t desk_last_click_tick = 0;
static int desk_last_click_icon = -1;

static void desk_init_registry(void) {
    

    static const char *const pinned[] = { "Explorer", "Terminal", "Browser", "Music" };
    int row = 0;
    desk_icon_count = 0;
    for (int i = 0; i < wm_app_count && desk_icon_count < DESK_MAX_ICONS; i++) {
        desk_icon_t *d = &desk_icons[desk_icon_count++];
        int pin = 0;
        for (unsigned k = 0; k < sizeof(pinned) / sizeof(pinned[0]); k++) {
            if (ic_streq(wm_apps[i].label, pinned[k])) pin = 1;
        }
        d->label = wm_apps[i].label;
        d->icon = wm_apps[i].icon;
        d->path = wm_apps[i].path;
        d->pinned = pin;
        d->cell_x = 0;
        d->cell_y = pin ? row++ : 0;
        d->selected = 0;
    }
}

static ic_rect_t desk_cell(const desk_icon_t *d) {
    return wm_desk_cell_rect(d->cell_x, d->cell_y);
}

static int desk_max_row(void) {
    int rows = (scr_h - WM_BAR_H - WM_DESK_Y0) / WM_DESK_CELL_H - 1;
    return rows < 0 ? 0 : rows;
}

static int desk_max_col(void) {
    int cols = (scr_w - WM_DESK_X0) / WM_DESK_CELL_W - 1;
    return cols < 0 ? 0 : (cols > 12 ? 12 : cols);
}


static int desk_drag_icon = -1;   
static int desk_dragging = 0;     
static int desk_press_x = 0;
static int desk_press_y = 0;
static int desk_grab_dx = 0;      
static int desk_grab_dy = 0;
static int desk_ghost_x = 0;
static int desk_ghost_y = 0;
static int desk_press_desktop = 0;
static int rubber_armed = 0;      
static int rubber_active = 0;
static int rubber_x0 = 0;
static int rubber_y0 = 0;
static int rubber_x1 = 0;
static int rubber_y1 = 0;

static void desk_paint_cell(desk_icon_t *d);
static void desk_erase_cell(int cx, int cy);
static void desk_save(void);
static void desk_load(void);

static void desk_snap_cell(int x, int y, int *cx, int *cy) {
    *cx = (x < WM_DESK_X0) ? 0 : (x - WM_DESK_X0) / WM_DESK_CELL_W;
    *cy = (y < WM_DESK_Y0) ? 0 : (y - WM_DESK_Y0) / WM_DESK_CELL_H;
    if (*cx < 0) *cx = 0;
    if (*cy < 0) *cy = 0;
    if (*cx > desk_max_col()) *cx = desk_max_col();
    if (*cy > desk_max_row()) *cy = desk_max_row();
}

static void desk_mark_ghost(void) {
    mark_dirty(desk_ghost_x - 8, desk_ghost_y - 8, WM_DESK_CELL_W + 16, WM_DESK_CELL_H + 16);
}

static void desk_mark_rubber(void) {
    int x0 = rubber_x0 < rubber_x1 ? rubber_x0 : rubber_x1;
    int y0 = rubber_y0 < rubber_y1 ? rubber_y0 : rubber_y1;
    int x1 = rubber_x0 < rubber_x1 ? rubber_x1 : rubber_x0;
    int y1 = rubber_y0 < rubber_y1 ? rubber_y1 : rubber_y0;
    mark_dirty(x0 - 2, y0 - 2, x1 - x0 + 5, y1 - y0 + 5);
}



static void desk_track_motion(int mx, int my, uint8_t buttons) {
    int dx;
    int dy;

    if (!(buttons & 1)) return;
    if (!desk_press_desktop) return;
    dx = mx - desk_press_x;
    dy = my - desk_press_y;
    if (!desk_dragging && desk_drag_icon >= 0 &&
        dx * dx + dy * dy > DESK_DRAG_THRESH_PX * DESK_DRAG_THRESH_PX) {
        desk_dragging = 1;
    }
    if (desk_dragging && desk_drag_icon >= 0 &&
        desk_drag_icon < desk_icon_count) {
        desk_mark_ghost();
        desk_ghost_x = mx - desk_grab_dx;
        desk_ghost_y = my - desk_grab_dy;
        desk_mark_ghost();
        return;
    }
    if (!rubber_active && rubber_armed &&
        dx * dx + dy * dy > DESK_DRAG_THRESH_PX * DESK_DRAG_THRESH_PX) {
        rubber_active = 1;
        rubber_x0 = desk_press_x;
        rubber_y0 = desk_press_y;
        rubber_x1 = mx;
        rubber_y1 = my;
    }
    if (rubber_active) {
        desk_mark_rubber();
        rubber_x1 = mx;
        rubber_y1 = my;
        desk_mark_rubber();
    }
}

static int desk_rects_overlap(ic_rect_t a, ic_rect_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}



#define CTX_LABEL_LEN 40

enum {
    CTX_OPEN = 1,
    CTX_TOGGLE_PIN,
    CTX_PROPS,
    CTX_ARRANGE,
    CTX_SETTINGS,
    CTX_PIN_BASE = 16
};

static int ctx_open = 0;
static int ctx_x = 0;
static int ctx_y = 0;
static int ctx_icon = -1;   
static int ctx_hover = -1;
static ic_menu_model_t ctx_model;
static int ctx_actions[IC_MENU_ITEMS_MAX];
static char ctx_labels[IC_MENU_ITEMS_MAX][CTX_LABEL_LEN];
static ic_tween_t ctx_fade;

static int props_open = 0;
static int props_icon = -1;
static int props_hover = -1;
static char props_body[160];
static ic_tween_t props_fade;
#define PROPS_W 300
#define PROPS_H 190

static ic_rect_t ctx_rect(void) {
    return ic_rect_make(ctx_x, ctx_y, ic_ui_menu_width(&ctx_model), ic_ui_menu_height(&ctx_model));
}

static ic_rect_t props_rect(void) {
    return ic_rect_make((scr_w - PROPS_W) / 2, (scr_h - WM_BAR_H - PROPS_H) / 2, PROPS_W, PROPS_H);
}

static void ctx_add(int action, const char *a, const char *b) {
    int n = ctx_model.count;
    int i = 0, j = 0;
    if (n >= IC_MENU_ITEMS_MAX) return;
    if (!a) {
        ctx_model.labels[n] = IC_MENU_SEPARATOR;
        ctx_actions[n] = 0;
        ctx_model.count++;
        return;
    }
    while (a[i] && i < CTX_LABEL_LEN - 1) {
        ctx_labels[n][i] = a[i];
        i++;
    }
    while (b && b[j] && i < CTX_LABEL_LEN - 1) ctx_labels[n][i++] = b[j++];
    ctx_labels[n][i] = '\0';
    ctx_model.labels[n] = ctx_labels[n];
    ctx_model.shortcuts[n] = 0;
    ctx_model.disabled[n] = 0;
    ctx_actions[n] = action;
    ctx_model.count++;
}

static void ctx_close(void) {
    if (ctx_open) {
        ctx_open = 0;
        ic_tween_to(&ctx_fade, 0.0f, (uint32_t)anim_ms(IC_DUR_INSTANT), IC_EASE_EXIT);
        mark_dirty_rect(reach_of(ctx_rect()));
    }
}

static void props_close(void) {
    if (props_open) {
        props_open = 0;
        ic_tween_to(&props_fade, 0.0f, (uint32_t)anim_ms(IC_DUR_FAST), IC_EASE_EXIT);
        mark_dirty_rect(reach_of(props_rect()));
    }
}

static void ctx_open_at(int x, int y, int icon) {
    int mw;
    int mh;

    ctx_icon = icon;
    for (int k = 0; k < IC_MENU_ITEMS_MAX; k++) {
        ctx_model.labels[k] = 0;
        ctx_model.shortcuts[k] = 0;
        ctx_model.disabled[k] = 0;
    }
    ctx_model.count = 0;
    ctx_model.hover = -1;
    if (icon >= 0 && icon < desk_icon_count) {
        ctx_add(CTX_OPEN, "Open", 0);
        ctx_add(0, 0, 0);
        ctx_add(CTX_TOGGLE_PIN, desk_icons[icon].pinned ? "Remove from Desktop" : "Add to Desktop", 0);
        ctx_add(CTX_PROPS, "Get Info", 0);
    } else {
        int any = 0;
        for (int j = 0; j < desk_icon_count && ctx_model.count < IC_MENU_ITEMS_MAX - 4; j++) {
            if (!desk_icons[j].pinned) {
                ctx_add(CTX_PIN_BASE + j, "Add ", desk_icons[j].label);
                any = 1;
            }
        }
        if (any) ctx_add(0, 0, 0);
        ctx_add(CTX_ARRANGE, "Clean Up", 0);
        ctx_add(CTX_SETTINGS, "Appearance\xE2\x80\xA6", 0);
    }
    if (ctx_model.count <= 0) return;
    mw = ic_ui_menu_width(&ctx_model);
    mh = ic_ui_menu_height(&ctx_model);
    if (x + mw > scr_w - 4) x = scr_w - 4 - mw;
    if (x < 4) x = 4;
    if (y + mh > scr_h - WM_BAR_H - 4) y = scr_h - WM_BAR_H - 4 - mh;
    if (y < 4) y = 4;
    ctx_x = x;
    ctx_y = y;
    ctx_hover = -1;
    ctx_open = 1;
    ic_tween_to(&ctx_fade, 1.0f, (uint32_t)anim_ms(IC_DUR_INSTANT), IC_EASE_ENTER);
    mark_dirty_rect(reach_of(ctx_rect()));
}

static void props_put(const char *s, int *pos) {
    while (s && *s && *pos < (int)sizeof(props_body) - 1) props_body[(*pos)++] = *s++;
    props_body[*pos] = '\0';
}

static void props_open_for(int icon) {
    desk_icon_t *d;
    icda_stat_t st;
    int pos = 0;

    if (icon < 0 || icon >= desk_icon_count) return;
    d = &desk_icons[icon];
    props_icon = icon;
    props_body[0] = '\0';
    props_put(d->path, &pos);
    if (icda_stat(d->path, &st) == 0) {
        char num[24];
        uint64_t kb = (st.size + 1023) / 1024;
        ic_uint_to_str(kb, num, sizeof(num));
        props_put("  \xC2\xB7  ", &pos);
        props_put(num, &pos);
        props_put(" KB", &pos);
    }
    props_hover = -1;
    props_open = 1;
    ic_tween_to(&props_fade, 1.0f, (uint32_t)anim_ms(IC_DUR_FAST), IC_EASE_ENTER);
    mark_dirty_rect(reach_of(props_rect()));
}

static void desk_arrange(void) {
    int cx = 0;
    int cy = 0;

    for (int i = 0; i < desk_icon_count; i++) {
        desk_icon_t *d = &desk_icons[i];
        if (!d->pinned) continue;
        if (cy > desk_max_row()) {
            cy = 0;
            cx++;
            if (cx > desk_max_col()) cx = desk_max_col();
        }
        d->cell_x = cx;
        d->cell_y = cy;
        d->selected = 0;
        cy++;
    }
    build_desktop_layer();
    mark_dirty_full();
}

static int desk_free_cell(int *cx, int *cy) {
    for (int x = 0; x <= desk_max_col(); x++) {
        for (int y = 0; y <= desk_max_row(); y++) {
            int taken = 0;
            for (int i = 0; i < desk_icon_count; i++) {
                if (desk_icons[i].pinned && desk_icons[i].cell_x == x &&
                    desk_icons[i].cell_y == y) {
                    taken = 1;
                    break;
                }
            }
            if (!taken) {
                *cx = x;
                *cy = y;
                return 1;
            }
        }
    }
    return 0;
}

static void desk_activate(int which) {
    int a;

    if (which < 0 || which >= ctx_model.count) {
        ctx_close();
        return;
    }
    a = ctx_actions[which];
    if (a == CTX_OPEN && ctx_icon >= 0 && ctx_icon < desk_icon_count) {
        icda_spawn(desk_icons[ctx_icon].path);
    } else if (a == CTX_TOGGLE_PIN && ctx_icon >= 0 && ctx_icon < desk_icon_count) {
        desk_icon_t *d = &desk_icons[ctx_icon];
        if (d->pinned) {
            d->pinned = 0;
            d->selected = 0;
            desk_erase_cell(d->cell_x, d->cell_y);
        } else {
            int cx;
            int cy;
            if (desk_free_cell(&cx, &cy)) {
                d->pinned = 1;
                d->cell_x = cx;
                d->cell_y = cy;
                desk_paint_cell(d);
            }
        }
        desk_save();
    } else if (a == CTX_PROPS && ctx_icon >= 0 && ctx_icon < desk_icon_count) {
        props_open_for(ctx_icon);
    } else if (a == CTX_ARRANGE) {
        desk_arrange();
        desk_save();
    } else if (a == CTX_SETTINGS) {
        icda_spawn("/apps/settings.app");
    } else if (a >= CTX_PIN_BASE && a < CTX_PIN_BASE + DESK_MAX_ICONS) {
        int j = a - CTX_PIN_BASE;
        int cx;
        int cy;
        if (j >= 0 && j < desk_icon_count && !desk_icons[j].pinned && desk_free_cell(&cx, &cy)) {
            desk_icons[j].pinned = 1;
            desk_icons[j].cell_x = cx;
            desk_icons[j].cell_y = cy;
            desk_paint_cell(&desk_icons[j]);
            desk_save();
        }
    }
    ctx_close();
}





static int desk_atoi(const char **p) {
    int v = 0;
    int neg = 0;
    if (!p || !*p) return 0;
    if (**p == '-') {
        neg = 1;
        (*p)++;
    }
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    return neg ? -v : v;
}

static void desk_put_u64(char *dst, int cap, int *pos, uint64_t v) {
    char rev[20];
    int r = 0;
    if (!dst || !pos || *pos >= cap - 1) return;
    if (v == 0) {
        dst[(*pos)++] = '0';
        dst[*pos] = '\0';
        return;
    }
    while (v > 0 && r < 19) {
        rev[r++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (int k = r - 1; k >= 0 && *pos < cap - 1; k--) dst[(*pos)++] = rev[k];
    dst[*pos] = '\0';
}

static void desk_save(void) {
    char buf[512];
    int pos = 0;

    icda_mkdir("/cfg");
    for (int i = 0; i < desk_icon_count && pos < 430; i++) {
        desk_icon_t *d = &desk_icons[i];
        const char *p;
        buf[pos++] = d->pinned ? '1' : '0';
        buf[pos++] = ' ';
        if (d->pinned) {
            desk_put_u64(buf, (int)sizeof(buf), &pos, (uint64_t)(d->cell_x < 0 ? 0 : d->cell_x));
            if (pos < 430) buf[pos++] = ' ';
            desk_put_u64(buf, (int)sizeof(buf), &pos, (uint64_t)(d->cell_y < 0 ? 0 : d->cell_y));
            if (pos < 430) buf[pos++] = ' ';
        }
        p = d->path;
        while (p && *p && pos < 430) buf[pos++] = *p++;
        if (pos < 511) buf[pos++] = '\n';
    }
    buf[pos < 512 ? pos : 511] = '\0';
    if (pos > 0) icda_write_file(DESK_CFG_PATH, buf, (uint64_t)pos);
}

static void desk_skip_line(const char *buf, int n, int *p) {
    while (*p < n && buf[*p] != '\n') (*p)++;
    if (*p < n) (*p)++;
}

static void desk_load(void) {
    char buf[512];
    long n;
    int p = 0;

    n = (long)icda_read_file(DESK_CFG_PATH, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    if (n > 511) n = 511;
    buf[n] = '\0';
    while (p < n) {
        int pinned;
        int cx = 0;
        int cy = 0;
        char path[96];
        int k = 0;
        if (buf[p] != '0' && buf[p] != '1') {
            desk_skip_line(buf, (int)n, &p);
            continue;
        }
        pinned = buf[p++] - '0';
        if (p >= n || buf[p] != ' ') {
            desk_skip_line(buf, (int)n, &p);
            continue;
        }
        p++;
        if (pinned) {
            const char *q = buf + p;
            cx = desk_atoi(&q);
            if (*q != ' ') {
                desk_skip_line(buf, (int)n, &p);
                continue;
            }
            q++;
            cy = desk_atoi(&q);
            if (*q != ' ') {
                desk_skip_line(buf, (int)n, &p);
                continue;
            }
            q++;
            p = (int)(q - buf);
            if (cx < 0 || cx > desk_max_col() || cy < 0 || cy > desk_max_row()) {
                desk_skip_line(buf, (int)n, &p);
                continue;
            }
        }
        while (p < n && buf[p] != '\n' && k < 95) path[k++] = buf[p++];
        path[k] = '\0';
        if (p < n && buf[p] == '\n') p++;
        if (k == 0) continue;
        for (int i = 0; i < desk_icon_count; i++) {
            if (ic_streq(desk_icons[i].path, path)) {
                desk_icons[i].pinned = pinned ? 1 : 0;
                if (pinned) {
                    desk_icons[i].cell_x = cx;
                    desk_icons[i].cell_y = cy;
                }
                desk_icons[i].selected = 0;
                break;
            }
        }
    }
}



static void desk_left_release(int mx, int my) {
    if (!desk_press_desktop) return;
    if (desk_dragging && desk_drag_icon >= 0 && desk_drag_icon < desk_icon_count) {
        desk_icon_t *d = &desk_icons[desk_drag_icon];
        int ocx = d->cell_x;
        int ocy = d->cell_y;
        int ncx;
        int ncy;
        desk_snap_cell(mx - desk_grab_dx + WM_DESK_CELL_W / 2,
                       my - desk_grab_dy + WM_DESK_CELL_H / 2, &ncx, &ncy);
        desk_mark_ghost();
        desk_erase_cell(ocx, ocy);
        for (int i = 0; i < desk_icon_count; i++) {
            desk_icon_t *o = &desk_icons[i];
            if (i != desk_drag_icon && o->pinned && o->cell_x == ncx && o->cell_y == ncy) {
                o->cell_x = ocx;
                o->cell_y = ocy;
                desk_paint_cell(o);
            }
        }
        d->cell_x = ncx;
        d->cell_y = ncy;
        desk_paint_cell(d);
        desk_save();
    } else if (rubber_active) {
        ic_rect_t band;
        band.x = rubber_x0 < rubber_x1 ? rubber_x0 : rubber_x1;
        band.y = rubber_y0 < rubber_y1 ? rubber_y0 : rubber_y1;
        band.w = (rubber_x0 < rubber_x1 ? rubber_x1 - rubber_x0 : rubber_x0 - rubber_x1) + 1;
        band.h = (rubber_y0 < rubber_y1 ? rubber_y1 - rubber_y0 : rubber_y0 - rubber_y1) + 1;
        desk_mark_rubber();
        for (int i = 0; i < desk_icon_count; i++) {
            desk_icon_t *d = &desk_icons[i];
            int sel = d->pinned && desk_rects_overlap(wm_desk_icon_hit_rect(desk_cell(d)), band);
            if (d->selected != sel) {
                d->selected = sel;
                desk_paint_cell(d);
            }
        }
        desk_last_click_icon = -1;
    }
    desk_drag_icon = -1;
    desk_dragging = 0;
    desk_press_desktop = 0;
    rubber_armed = 0;
    rubber_active = 0;
}

static int desk_hit(int idx, int mx, int my) {
    desk_icon_t *d;
    if (idx < 0 || idx >= desk_icon_count) return 0;
    d = &desk_icons[idx];
    if (!d->pinned) return 0;
    return ic_ui_hit(wm_desk_icon_hit_rect(desk_cell(d)), mx, my);
}

static ic_canvas_t desktop_canvas(void) {
    return ic_canvas_make(desktop_layer, scr_w, scr_h);
}


static void desk_repaint_cell_rect(ic_rect_t cell) {
    ic_canvas_t c = desktop_canvas();
    ic_canvas_set_clip(&c, cell.x, cell.y, cell.w, cell.h);
    wm_wallpaper_paint(&c, scr_w, scr_h);
    for (int i = 0; i < desk_icon_count; i++) {
        desk_icon_t *d = &desk_icons[i];
        if (d->pinned && desk_rects_overlap(desk_cell(d), cell)) {
            wm_desk_icon_draw(&c, desk_cell(d), d->label, ic_icon_builtin(d->icon), d->selected, 1.0f);
        }
    }
    mark_dirty_rect(cell);
}

static void desk_erase_cell(int cx, int cy) {
    desk_repaint_cell_rect(wm_desk_cell_rect(cx, cy));
}

static void desk_paint_cell(desk_icon_t *d) {
    desk_repaint_cell_rect(desk_cell(d));
}

static void build_desktop_layer(void) {
    ic_canvas_t c = desktop_canvas();
    if (scr_w <= 0 || scr_h <= 0) return;
    wm_wallpaper_paint(&c, scr_w, scr_h);
    for (int i = 0; i < desk_icon_count; i++) {
        desk_icon_t *d = &desk_icons[i];
        if (!d->pinned) continue;
        wm_desk_icon_draw(&c, desk_cell(d), d->label, ic_icon_builtin(d->icon), d->selected, 1.0f);
    }
}



static int launcher_open = 0;
static ic_tween_t launcher_fade;
static int launcher_hover = WM_LAUNCH_NONE;
static int bar_hover = WM_BAR_NONE;
static char clock_time[8];
static char clock_date[24];
static int clock_minute = -1;
static wm_task_t bar_tasks[MAX_WINDOWS];

static void mark_dirty_bar(void) {
    mark_dirty_rect(wm_bar_rect(scr_w, scr_h));
}

static void mark_dirty_launcher(void) {
    mark_dirty_rect(reach_of(wm_launcher_rect(scr_w, scr_h)));
}

static void launcher_set(int open) {
    if (launcher_open == open) return;
    launcher_open = open;
    launcher_hover = WM_LAUNCH_NONE;
    ic_tween_to(&launcher_fade, open ? 1.0f : 0.0f,
                (uint32_t)anim_ms(open ? IC_DUR_FAST : IC_DUR_INSTANT),
                open ? IC_EASE_ENTER : IC_EASE_EXIT);
    mark_dirty_launcher();
    mark_dirty_bar();
}

static void clock_refresh(void) {
    ic_datetime_t t;
    if (ic_wallclock(&t) != 0) {
        if (clock_minute != -2) {
            clock_time[0] = '\0';
            clock_date[0] = '\0';
            clock_minute = -2;
            mark_dirty_bar();
        }
        return;
    }
    if (t.minute != clock_minute) {
        clock_minute = t.minute;
        ic_format_hm(&t, clock_time, sizeof(clock_time));
        ic_format_day(&t, clock_date, sizeof(clock_date));
        mark_dirty_bar();
    }
}

static int bar_build(wm_bar_t *b, icda_audio_info_t *audio) {
    int n = 0;
    for (int i = 0; i < task_count; i++) {
        int idx = task_order[i];
        wm_window_t *win = &windows[idx];
        if (!win->valid || win->closing) continue;
        bar_tasks[n].title = win->title;
        bar_tasks[n].icon = wm_app_icon_for_title(win->title);
        bar_tasks[n].focused = focused_window_idx == idx && !win->minimized;
        bar_tasks[n].minimized = win->minimized;
        n++;
    }
    b->tasks = bar_tasks;
    b->count = n;
    b->launcher_open = launcher_open;
    b->hover = bar_hover;
    b->pressed = WM_BAR_NONE;
    b->time_text = clock_time[0] ? clock_time : 0;
    b->date_text = clock_date[0] ? clock_date : 0;
    b->audio_text = 0;
    if (audio && (long)icda_audio_info(audio) >= 0 && audio->active) b->audio_text = audio->name;
    return n;
}


static int bar_task_slot(int n) {
    int k = 0;
    for (int i = 0; i < task_count; i++) {
        int idx = task_order[i];
        if (!windows[idx].valid || windows[idx].closing) continue;
        if (k == n) return idx;
        k++;
    }
    return -1;
}

static ic_rect_t bar_rect_for_slot(int slot) {
    wm_bar_t b;
    int n = bar_build(&b, 0);
    for (int i = 0; i < n; i++) {
        if (bar_task_slot(i) == slot) return wm_bar_task_rect(scr_w, scr_h, n, i);
    }
    return wm_bar_launcher_rect(scr_w, scr_h);
}




static void extend_one(dirty_rect_t *r, ic_rect_t m) {
    dirty_rect_t d;
    if (m.w <= 0 || m.h <= 0) return;
    d.x = m.x; d.y = m.y; d.w = m.w; d.h = m.h;
    if (d.x < 0) { d.w += d.x; d.x = 0; }
    if (d.y < 0) { d.h += d.y; d.y = 0; }
    if (d.x + d.w > scr_w) d.w = scr_w - d.x;
    if (d.y + d.h > scr_h) d.h = scr_h - d.y;
    if (dirty_rects_intersect(r, &d)) dirty_rect_union(r, &d);
}

static void extend_to_materials(dirty_rect_t *r) {
    extend_one(r, wm_bar_rect(scr_w, scr_h));
    if (launcher_open || ic_tween_value(&launcher_fade) > 0.0f) {
        extend_one(r, reach_of(wm_launcher_rect(scr_w, scr_h)));
    }
    if (ctx_open || ic_tween_value(&ctx_fade) > 0.0f) {
        extend_one(r, reach_of(ctx_rect()));
    }
}



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
    if (idx < 0 || idx >= MAX_WINDOWS) return;
    if (old >= 0 && old < MAX_WINDOWS && windows[old].valid) mark_dirty_win(&windows[old]);
    focused_window_idx = idx;
    if (idx != old && windows[idx].valid) mark_dirty_win(&windows[idx]);
    mark_dirty_bar();
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
        if (idx < 0 || idx >= MAX_WINDOWS) continue;
        if (windows[idx].valid && !windows[idx].minimized && !windows[idx].closing) {
            found = idx;
            break;
        }
    }
    if (found >= 0) {
        set_focus(found);
    } else if (old != -1) {
        focused_window_idx = -1;
        notify_focus_change(old, -1);
        mark_dirty_bar();
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
    for (int i = 0; i < task_count; i++) {
        if (task_order[i] == win_idx) {
            for (int k = i; k < task_count - 1; k++) task_order[k] = task_order[k + 1];
            task_count--;
            break;
        }
    }
    if (drag_win == win_idx) drag_win = -1;
    if (resize_win == win_idx) resize_win = -1;
    if (capture_win == win_idx) capture_win = -1;
    if (press_win == win_idx) press_win = -1;
    if (focused_window_idx == win_idx) focus_top_visible();
    mark_dirty_bar();
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

static int send_maybe(uint64_t q, gui_msg_t *m) {
    if (icda_msg_poll(q) > 32) return -1;
    return icda_msg_send(q, m);
}

static void send_close_to_app(wm_window_t *win) {
    gui_msg_t close_msg;
    clear_msg(&close_msg);
    close_msg.type = GUI_MSG_CLOSE_WINDOW;
    close_msg.window_id = win->id;
    
    if (icda_msg_poll(win->app_queue_handle) > 32) return;
    icda_msg_send(win->app_queue_handle, &close_msg);
}

static void send_mouse(wm_window_t *win, int x, int y, uint8_t buttons) {
    gui_msg_t m;
    clear_msg(&m);
    m.type = GUI_MSG_MOUSE_EVENT;
    m.window_id = win->id;
    m.mouse.x = x;
    m.mouse.y = y;
    m.mouse.buttons = buttons;
    send_maybe(win->app_queue_handle, &m);
}



static ic_rect_t work_area(void) {
    return ic_rect_make(0, 0, scr_w, scr_h - WM_BAR_H);
}

static void clamp_window(wm_window_t *win) {
    ic_rect_t wa = work_area();
    if (win->w > wa.w) win->w = wa.w;
    if (win->h > wa.h - WM_TITLE_H) win->h = wa.h - WM_TITLE_H;
    
    if (win->y < WM_TITLE_H) win->y = WM_TITLE_H;
    if (win->y > wa.y + wa.h - 8) win->y = wa.y + wa.h - 8;
    if (win->x + win->w < 80) win->x = 80 - win->w;
    if (win->x > wa.w - 80) win->x = wa.w - 80;
}

static float anim_progress(const wm_window_t *win) {
    uint64_t elapsed;
    if (win->anim_kind == WM_ANIM_NONE || win->anim_ms == 0) return 1.0f;
    elapsed = ic_time_ns() - win->anim_t0;
    return ic_clampf((float)elapsed / ((float)win->anim_ms * 1e6f), 0.0f, 1.0f);
}

static ic_rect_t rect_lerp(ic_rect_t a, ic_rect_t b, float t) {
    ic_rect_t r;
    r.x = (int)ic_lerpf((float)a.x, (float)b.x, t);
    r.y = (int)ic_lerpf((float)a.y, (float)b.y, t);
    r.w = (int)ic_lerpf((float)a.w, (float)b.w, t);
    r.h = (int)ic_lerpf((float)a.h, (float)b.h, t);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

static ic_rect_t rect_scaled_about_center(ic_rect_t r, float s) {
    ic_rect_t o;
    o.w = (int)((float)r.w * s);
    o.h = (int)((float)r.h * s);
    o.x = r.x + (r.w - o.w) / 2;
    o.y = r.y + (r.h - o.h) / 2;
    return o;
}



static void anim_state(const wm_window_t *win, ic_rect_t *rect, float *opacity) {
    float t = anim_progress(win);
    float e;
    switch (win->anim_kind) {
    case WM_ANIM_OPEN:
        e = ic_ease(IC_EASE_DECELERATE, t);
        *rect = rect_lerp(win->anim_from, win->anim_to, e);
        *opacity = ic_clampf(t * 1.8f, 0.0f, 1.0f);
        break;
    case WM_ANIM_CLOSE:
        e = ic_ease(IC_EASE_ACCELERATE, t);
        *rect = rect_lerp(win->anim_from, win->anim_to, e);
        *opacity = 1.0f - e;
        break;
    case WM_ANIM_MINIMIZE:
        e = ic_ease(IC_EASE_EMPHASIZED, t);
        *rect = rect_lerp(win->anim_from, win->anim_to, e);
        *opacity = 1.0f - ic_clampf((t - 0.45f) / 0.55f, 0.0f, 1.0f);
        break;
    case WM_ANIM_RESTORE:
        e = ic_ease(IC_EASE_EMPHASIZED, t);
        *rect = rect_lerp(win->anim_from, win->anim_to, e);
        *opacity = ic_clampf(t * 2.2f, 0.0f, 1.0f);
        break;
    case WM_ANIM_GEOMETRY:
        e = ic_ease(IC_EASE_EMPHASIZED, t);
        *rect = rect_lerp(win->anim_from, win->anim_to, e);
        *opacity = 1.0f;
        break;
    default:
        *rect = ic_rect_make(win->x, win->y, win->w, win->h);
        *opacity = 1.0f;
        break;
    }
}

static void anim_start(wm_window_t *win, int kind, ic_rect_t from, ic_rect_t to, int ms) {
    mark_dirty_win(win);
    win->anim_kind = kind;
    win->anim_from = from;
    win->anim_to = to;
    win->anim_t0 = ic_time_ns();
    win->anim_ms = (uint32_t)anim_ms(ms);
    mark_dirty_win(win);
}





static void win_resize_buffer(wm_window_t *win, int nw, int nh) {
    uint64_t shm;
    uint64_t addr;
    uint32_t *px;
    gui_msg_t m;
    uint32_t bg = ic_palette()->window & 0xFFFFFFu;
    if (!win || !win->valid || nw <= 0 || nh <= 0) return;
    if (nw == win->pix_w && nh == win->pix_h) return;
    shm = icda_shm_create((uint64_t)nw * (uint64_t)nh * 4ULL);
    if (!shm) return;
    addr = icda_shm_map(shm);
    if (!addr) {
        icda_shm_close(shm);
        return;
    }
    px = (uint32_t *)addr;
    for (int y = 0; y < nh; y++) {
        uint32_t *row = px + (int64_t)y * nw;
        int copy = 0;
        if (y < win->pix_h && win->pixels) {
            copy = nw < win->pix_w ? nw : win->pix_w;
            copy_pixels(row, win->pixels + (int64_t)y * win->pix_w, copy);
        }
        for (int x = copy; x < nw; x++) row[x] = bg;
    }
    icda_shm_unmap(win->shm_handle);
    win->shm_handle = shm;
    win->pixels = px;
    win->pix_w = nw;
    win->pix_h = nh;
    clear_msg(&m);
    m.type = GUI_MSG_RESIZE;
    m.window_id = win->id;
    m.resize.shm_handle = shm;
    m.resize.w = nw;
    m.resize.h = nh;
    send_maybe(win->app_queue_handle, &m);
    mark_dirty_win(win);
}

static void start_close(wm_window_t *win) {
    ic_rect_t outer;
    if (!win || !win->valid || win->closing) return;
    win->closing = 1;
    outer = outer_of(win->x, win->y, win->w, win->h);
    if (win->minimized) {
        win->anim_kind = WM_ANIM_NONE;
        win->anim_ms = 0;
        return;
    }
    anim_start(win, WM_ANIM_CLOSE, outer, rect_scaled_about_center(outer, 0.94f), IC_DUR_FAST + 20);
}

static void finish_close(int slot) {
    wm_window_t *win;
    if (slot < 0 || slot >= MAX_WINDOWS) return;
    win = &windows[slot];
    if (!win->valid) return;
    mark_dirty_win(win);
    icda_shm_unmap(win->shm_handle);
    icda_shm_close(win->shm_handle);
    win->valid = 0;
    win->closing = 0;
    win->anim_kind = WM_ANIM_NONE;
    remove_window(slot);
}

static void minimize_window(int idx) {
    wm_window_t *win = &windows[idx];
    if (!win->valid || win->minimized || win->closing) return;
    anim_start(win, WM_ANIM_MINIMIZE, outer_of(win->x, win->y, win->w, win->h),
               bar_rect_for_slot(idx), IC_DUR_SLOW);
    win->minimized = 1;
    if (capture_win == idx) capture_win = -1;
    focus_top_visible();
    mark_dirty_bar();
}

static void restore_window(int idx) {
    wm_window_t *win = &windows[idx];
    if (!win->valid || !win->minimized) return;
    win->minimized = 0;
    anim_start(win, WM_ANIM_RESTORE, bar_rect_for_slot(idx),
               outer_of(win->x, win->y, win->w, win->h), IC_DUR_SLOW);
}

static void toggle_maximize(int idx) {
    wm_window_t *win = &windows[idx];
    ic_rect_t to;
    if (!win->valid || win->closing) return;
    if (!win->maximized) {
        ic_rect_t wa = work_area();
        win->restore_x = win->x;
        win->restore_y = win->y;
        win->restore_w = win->w;
        win->restore_h = win->h;
        to = ic_rect_make(wa.x, wa.y + WM_TITLE_H, wa.w, wa.h - WM_TITLE_H);
        win->anim_maximized = 1;
    } else {
        to = ic_rect_make(win->restore_x, win->restore_y, win->restore_w, win->restore_h);
        win->anim_maximized = 0;
        win->maximized = 0;
    }
    anim_start(win, WM_ANIM_GEOMETRY, ic_rect_make(win->x, win->y, win->w, win->h), to,
               IC_DUR_BASE + 40);
    if (win->anim_ms == 0) {
        win->x = to.x;
        win->y = to.y;
        win->w = to.w;
        win->h = to.h;
        win->maximized = win->anim_maximized;
        win->anim_kind = WM_ANIM_NONE;
        win_resize_buffer(win, win->w, win->h);
    }
}


static int animations_tick(void) {
    int running = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *win = &windows[i];
        if (!win->valid) continue;
        if (win->closing && win->anim_kind == WM_ANIM_NONE) {
            finish_close(i);
            continue;
        }
        if (win->anim_kind == WM_ANIM_NONE) continue;
        mark_dirty_win(win);
        if (anim_progress(win) < 1.0f) {
            running = 1;
            continue;
        }
        switch (win->anim_kind) {
        case WM_ANIM_CLOSE:
            win->anim_kind = WM_ANIM_NONE;
            finish_close(i);
            continue;
        case WM_ANIM_GEOMETRY:
            win->x = win->anim_to.x;
            win->y = win->anim_to.y;
            win->w = win->anim_to.w;
            win->h = win->anim_to.h;
            win->maximized = win->anim_maximized;
            win->anim_kind = WM_ANIM_NONE;
            win_resize_buffer(win, win->w, win->h);
            break;
        default:
            win->anim_kind = WM_ANIM_NONE;
            break;
        }
        mark_dirty_win(win);
    }
    if (ic_tween_running(&launcher_fade)) {
        mark_dirty_launcher();
        running = 1;
    }
    if (ic_tween_running(&ctx_fade)) {
        mark_dirty_rect(reach_of(ctx_rect()));
        running = 1;
    }
    if (ic_tween_running(&props_fade)) {
        mark_dirty_rect(reach_of(props_rect()));
        running = 1;
    }
    return running;
}

static void open_window_from_msg(gui_msg_t *msg, uint64_t wm_queue, uint32_t *next_win_id) {
    int slot = alloc_window_slot();
    gui_msg_t reply;
    ic_rect_t wa = work_area();
    int win_w = msg->open_req.w;
    int win_h = msg->open_req.h;
    uint64_t shm_hnd;
    uint64_t map_addr;
    wm_window_t *win;

    if (slot == -1) {
        clear_msg(&reply);
        reply.type = GUI_MSG_OPEN_FAIL;
        icda_msg_send(msg->window_id, &reply);
        return;
    }
    if (win_w < 160) win_w = 160;
    if (win_h < 120) win_h = 120;
    if (win_w > wa.w - 24) win_w = wa.w - 24;
    if (win_h > wa.h - WM_TITLE_H - 24) win_h = wa.h - WM_TITLE_H - 24;

    shm_hnd = icda_shm_create((uint64_t)win_w * win_h * 4);
    if (!shm_hnd) {
        clear_msg(&reply);
        reply.type = GUI_MSG_OPEN_FAIL;
        icda_msg_send(msg->window_id, &reply);
        return;
    }
    map_addr = icda_shm_map(shm_hnd);
    if (!map_addr) {
        icda_shm_close(shm_hnd);
        clear_msg(&reply);
        reply.type = GUI_MSG_OPEN_FAIL;
        icda_msg_send(msg->window_id, &reply);
        return;
    }

    win = &windows[slot];
    for (int i = 0; i < (int)sizeof(*win); i++) ((uint8_t *)win)[i] = 0;
    win->valid = 1;
    win->id = (*next_win_id)++;
    win->app_queue_handle = msg->window_id;
    win->shm_handle = shm_hnd;
    win->pixels = (uint32_t*)map_addr;
    win->pix_w = win_w;
    win->pix_h = win_h;
    win->w = win_w;
    win->h = win_h;
    

    {
        int cascade = (num_windows % 6) * 28;
        win->x = (wa.w - win_w) / 2 - 80 + cascade;
        win->y = WM_TITLE_H + (wa.h - WM_TITLE_H - win_h) / 3 + cascade;
        if (win->x < 16) win->x = 16 + cascade;
    }
    win->hover = WM_HIT_NONE;
    win->pressed = WM_HIT_NONE;
    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = win_w;
    win->restore_h = win_h;
    {
        uint32_t bg = ic_palette()->window & 0xFFFFFFu;
        for (int i = 0; i < win_w * win_h; i++) win->pixels[i] = bg;
    }
    {
        int ti = 0;
        while (msg->open_req.title[ti] && ti < 31) {
            win->title[ti] = msg->open_req.title[ti];
            ti++;
        }
        win->title[ti] = 0;
    }
    clamp_window(win);

    z_order[num_windows++] = slot;
    task_order[task_count++] = slot;

    clear_msg(&reply);
    reply.type = GUI_MSG_OPEN_OK;
    reply.window_id = win->id;
    reply.open_ok.shm_handle = shm_hnd;
    reply.open_ok.w = win_w;
    reply.open_ok.h = win_h;
    reply.open_ok.reply_queue = wm_queue;
    icda_msg_send(win->app_queue_handle, &reply);

    {
        ic_rect_t outer = outer_of(win->x, win->y, win->w, win->h);
        anim_start(win, WM_ANIM_OPEN, rect_scaled_about_center(outer, 0.94f), outer, IC_DUR_BASE);
    }

    


    set_focus(slot);
    mark_dirty_bar();
}



static void win_frame(const wm_window_t *win, int idx, wm_frame_t *f) {
    f->x = win->x;
    f->y = win->y;
    f->w = win->w;
    f->h = win->h;
    f->title = win->title;
    f->focused = focused_window_idx == idx && !win->minimized;
    f->maximized = win->maximized;
    f->hover = win->hover;
    f->pressed = win->pressed;
}




static void composite_window_zoom(wm_window_t *win, int idx, ic_rect_t outer, float opacity) {
    wm_frame_t f;
    ic_canvas_t lc = ic_canvas_make(layer_buffer, scr_w, scr_h);
    int nw, nh;
    float sx, sy;
    win_frame(win, idx, &f);
    f.hover = WM_HIT_NONE;
    f.pressed = WM_HIT_NONE;
    f.x = 1;
    f.y = 1 + WM_TITLE_H;
    nw = f.w + 2;
    nh = f.h + WM_TITLE_H + 2;
    if (nw > scr_w || nh > scr_h || outer.w <= 0 || outer.h <= 0) return;
    ic_canvas_set_clip(&lc, 0, 0, nw, nh);
    ic_gfx_fill(&lc, 0, 0, nw, nh, IC_RGB(0x000000));
    wm_frame_draw(&lc, &f, win->pixels, win->pix_w, win->pix_h);

    sx = (float)outer.w / (float)(nw - 2);
    sy = (float)outer.h / (float)(nh - 2);
    ic_theme_shadow_faded(&scene, outer.x, outer.y, outer.w, outer.h, IC_R_WINDOW * sx,
                          f.focused ? IC_ELEV_WINDOW : IC_ELEV_WINDOW_IDLE, opacity);
    ic_gfx_blit_scaled(&scene, outer.x - (int)(sx + 0.5f), outer.y - (int)(sy + 0.5f),
                       outer.w + 2 * (int)(sx + 0.5f), outer.h + 2 * (int)(sy + 0.5f),
                       layer_buffer, nw, nh, scr_w, (IC_R_WINDOW + 1.0f) * sx,
                       (uint32_t)(opacity * 255.0f + 0.5f));
}

static void composite_window(wm_window_t *win, int idx) {
    wm_frame_t f;
    if (!win || !win->valid) return;
    if (win->minimized && win->anim_kind != WM_ANIM_MINIMIZE) return;
    if (win->anim_kind == WM_ANIM_OPEN || win->anim_kind == WM_ANIM_CLOSE ||
        win->anim_kind == WM_ANIM_MINIMIZE || win->anim_kind == WM_ANIM_RESTORE) {
        ic_rect_t r;
        float op;
        anim_state(win, &r, &op);
        composite_window_zoom(win, idx, r, op);
        return;
    }
    win_frame(win, idx, &f);
    if (win->anim_kind == WM_ANIM_GEOMETRY) {
        ic_rect_t r;
        float op;
        anim_state(win, &r, &op);
        f.x = r.x;
        f.y = r.y;
        f.w = r.w;
        f.h = r.h;
        wm_frame_draw_shadow(&scene, &f, 1.0f);
        
        wm_frame_draw(&scene, &f, 0, 0, 0);
        ic_gfx_blit_scaled4(&scene, f.x, f.y, f.w, f.h, win->pixels, win->pix_w, win->pix_h,
                            win->pix_w, 0.0f, 0.0f, wm_frame_radius(&f), wm_frame_radius(&f), 255);
        return;
    }
    wm_frame_draw_shadow(&scene, &f, 1.0f);
    wm_frame_draw(&scene, &f, win->pixels, win->pix_w, win->pix_h);
}



typedef void (*layer_draw_fn)(ic_canvas_t *c);

static void composite_faded(ic_rect_t area, float opacity, layer_draw_fn draw) {
    ic_canvas_t lc;
    int x0, y0, x1, y1;
    if (opacity <= 0.0f) return;
    if (opacity >= 1.0f) {
        draw(&scene);
        return;
    }
    lc = ic_canvas_make(layer_buffer, scr_w, scr_h);
    if (!ic_canvas_bounds(&scene, &x0, &y0, &x1, &y1)) return;
    ic_canvas_set_clip(&lc, x0, y0, x1 - x0, y1 - y0);
    ic_canvas_push_clip(&lc, area.x, area.y, area.w, area.h, 0);
    if (!ic_canvas_bounds(&lc, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++) {
        copy_pixels(layer_buffer + (int64_t)y * scr_w + x0, back_buffer + (int64_t)y * scr_w + x0,
                    x1 - x0);
    }
    draw(&lc);
    ic_gfx_blit(&scene, x0, y0, layer_buffer + (int64_t)y0 * scr_w + x0, x1 - x0, y1 - y0, scr_w,
                (uint32_t)(opacity * 255.0f + 0.5f));
}

static void draw_launcher_layer(ic_canvas_t *c) {
    wm_launcher_draw(c, scr_w, scr_h, launcher_hover, blur_scratch, BLUR_SCRATCH_PX);
}

static void draw_ctx_layer(ic_canvas_t *c) {
    ctx_model.hover = ctx_hover;
    ic_ui_menu(c, &ctx_model, ctx_x, ctx_y, blur_scratch, BLUR_SCRATCH_PX);
}

static void draw_props_layer(ic_canvas_t *c) {
    static const char *const buttons[1] = { "Done" };
    ic_rect_t r = props_rect();
    const char *title = (props_icon >= 0 && props_icon < desk_icon_count) ?
                        desk_icons[props_icon].label : "Info";
    ic_ui_alert(c, r, IC_SYM_INFO, title, props_body, buttons, 1, props_hover, 0);
}

static void draw_bar(void) {
    wm_bar_t b;
    icda_audio_info_t audio;
    bar_build(&b, &audio);
    wm_bar_draw(&scene, scr_w, scr_h, &b, blur_scratch, BLUR_SCRATCH_PX);
}

static void draw_overlays(void) {
    if (rubber_active) wm_rubber_band_draw(&scene, rubber_x0, rubber_y0, rubber_x1, rubber_y1);
    if (desk_dragging && desk_drag_icon >= 0 && desk_drag_icon < desk_icon_count) {
        desk_icon_t *d = &desk_icons[desk_drag_icon];
        wm_desk_icon_draw(&scene, ic_rect_make(desk_ghost_x, desk_ghost_y, WM_DESK_CELL_W, WM_DESK_CELL_H),
                          d->label, ic_icon_builtin(d->icon), 0, 0.7f);
    }
    if (ic_tween_value(&launcher_fade) > 0.0f) {
        composite_faded(reach_of(wm_launcher_rect(scr_w, scr_h)), ic_tween_value(&launcher_fade),
                        draw_launcher_layer);
    }
    if (ctx_model.count > 0 && ic_tween_value(&ctx_fade) > 0.0f) {
        composite_faded(reach_of(ctx_rect()), ic_tween_value(&ctx_fade), draw_ctx_layer);
    }
    if (ic_tween_value(&props_fade) > 0.0f) {
        composite_faded(reach_of(props_rect()), ic_tween_value(&props_fade), draw_props_layer);
    }
}

static void draw_debug_overlay(void) {
    char l[5][48];
    const char *lines[5];
    int pos;
    if (!wm_debug_overlay) return;
    pos = 0;
    l[0][0] = 0;
    ic_strcat(l[0], "ICDA ", sizeof(l[0]));
    ic_strcat(l[0], IC_VERSION_STRING, sizeof(l[0]));
    {
        char a[12], b[12];
        ic_uint_to_str((uint64_t)fb_info.width, a, sizeof(a));
        ic_uint_to_str((uint64_t)fb_info.height, b, sizeof(b));
        l[1][0] = 0;
        ic_strcat(l[1], "display  ", sizeof(l[1]));
        ic_strcat(l[1], a, sizeof(l[1]));
        ic_strcat(l[1], "x", sizeof(l[1]));
        ic_strcat(l[1], b, sizeof(l[1]));
        ic_strcat(l[1], gpu_info.flip_active ? "  flip" : (gpu_info.needs_present ? "  present" : "  fbdev"),
                  sizeof(l[1]));
    }
    {
        char a[20], b[20];
        ic_uint_to_str(wm_diag_last_frame_us / 100, a, sizeof(a));
        ic_uint_to_str(wm_diag_max_frame_us / 100, b, sizeof(b));
        l[2][0] = 0;
        ic_strcat(l[2], "frame    ", sizeof(l[2]));
        ic_strcat(l[2], a, sizeof(l[2]));
        ic_strcat(l[2], "/10 ms  max ", sizeof(l[2]));
        ic_strcat(l[2], b, sizeof(l[2]));
        ic_strcat(l[2], "/10", sizeof(l[2]));
    }
    {
        char a[20];
        ic_uint_to_str(wm_diag_composite_count, a, sizeof(a));
        l[3][0] = 0;
        ic_strcat(l[3], "frames   ", sizeof(l[3]));
        ic_strcat(l[3], a, sizeof(l[3]));
    }
    {
        char a[20];
        ic_uint_to_str(wm_diag_mouse_events, a, sizeof(a));
        l[4][0] = 0;
        ic_strcat(l[4], "pointer  ", sizeof(l[4]));
        ic_strcat(l[4], a, sizeof(l[4]));
    }
    (void)pos;
    for (int i = 0; i < 5; i++) lines[i] = l[i];
    wm_debug_draw(&scene, lines, 5);
}


static void blit_row_24(uint8_t *dst, const uint32_t *src, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t c = src[i];
        dst[i * 3 + 0] = (uint8_t)c;
        dst[i * 3 + 1] = (uint8_t)(c >> 8);
        dst[i * 3 + 2] = (uint8_t)(c >> 16);
    }
}

static uint32_t fb_pitch_pixels(void) {
    return fb_info.pitch ? fb_info.pitch / 4 : (uint32_t)fb_info.width;
}


static void blit_region(int x, int y, int rw, int rh) {
    uint32_t pitch = fb_pitch_pixels();
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > scr_w) rw = scr_w - x;
    if (y + rh > scr_h) rh = scr_h - y;
    if (rw <= 0 || rh <= 0) return;
    if (fb_info.bpp != 32) {
        for (int yy = y; yy < y + rh; yy++) {
            blit_row_24((uint8_t *)real_fb + (uint64_t)yy * fb_info.pitch + (uint64_t)x * 3,
                        back_buffer + (uint64_t)yy * scr_w + x, rw);
        }
        return;
    }
    for (int yy = y; yy < y + rh; yy++) {
        copy_pixels(real_fb + (uint64_t)yy * pitch + x, back_buffer + (uint64_t)yy * scr_w + x, rw);
    }
}

static int rect_hit(int ax, int ay, int aw, int ah, ic_rect_t b) {
    return ax < b.x + b.w && b.x < ax + aw && ay < b.y + b.h && b.y < ay + ah;
}


static void composite_region(int x, int y, int rw, int rh) {
    ic_canvas_set_clip(&scene, x, y, rw, rh);
    for (int yy = y; yy < y + rh; yy++) {
        copy_pixels(back_buffer + (uint64_t)yy * scr_w + x, desktop_layer + (uint64_t)yy * scr_w + x, rw);
    }
    for (int zi = 0; zi < num_windows; zi++) {
        int idx = z_order[zi];
        wm_window_t *win = &windows[idx];
        if (!win->valid) continue;
        if (win->minimized && win->anim_kind != WM_ANIM_MINIMIZE) continue;
        {
            ic_rect_t b[3];
            int n = win_bounds(win, b), touches = 0;
            for (int k = 0; k < n && !touches; k++) touches = rect_hit(x, y, rw, rh, b[k]);
            if (!touches) continue;
        }
        composite_window(win, idx);
    }
    if (rect_hit(x, y, rw, rh, wm_bar_rect(scr_w, scr_h))) draw_bar();
    draw_overlays();
    draw_debug_overlay();
    ic_canvas_clear_clip(&scene);
}

static void cursor_bbox(int mx, int my, int pmx, int pmy, int *ox, int *oy, int *ow, int *oh) {
    int cw, ch;
    cursor_dims(&cw, &ch);
    if (scr_w <= 0 || scr_h <= 0) { *ox = *oy = *ow = *oh = 0; return; }
    if (mx < 0) mx = 0; else if (mx >= scr_w) mx = scr_w - 1;
    if (my < 0) my = 0; else if (my >= scr_h) my = scr_h - 1;
    if (pmx < 0) pmx = 0; else if (pmx >= scr_w) pmx = scr_w - 1;
    if (pmy < 0) pmy = 0; else if (pmy >= scr_h) pmy = scr_h - 1;
    *ox = pmx < mx ? pmx : mx;
    *oy = pmy < my ? pmy : my;
    *ow = (pmx > mx ? pmx : mx) + cw + 2 - *ox;
    *oh = (pmy > my ? pmy : my) + ch + 2 - *oy;
    if (*ox + *ow > scr_w) *ow = scr_w - *ox;
    if (*oy + *oh > scr_h) *oh = scr_h - *oy;
}



static void composite_dirty(int mx, int my, int pmx, int pmy) {
    if (dirty_full) {
        composite_region(0, 0, scr_w, scr_h);
        draw_cursor_into_bb(scr_w, scr_h, mx, my);
        blit_region(0, 0, scr_w, scr_h);
        restore_cursor_scene(scr_w, scr_h, mx, my);
        dirty_full = 0;
        dirty_count = 0;
        return;
    }
    for (int i = 0; i < dirty_count; i++) {
        dirty_rect_t *d = &dirty_rects[i];
        composite_region(d->x, d->y, d->w, d->h);
        blit_region(d->x, d->y, d->w, d->h);
    }
    dirty_count = 0;
    draw_cursor_into_bb(scr_w, scr_h, mx, my);
    {
        int cx0, cy0, cbw, cbh;
        cursor_bbox(mx, my, pmx, pmy, &cx0, &cy0, &cbw, &cbh);
        if (cbw > 0 && cbh > 0) blit_region(cx0, cy0, cbw, cbh);
    }
    restore_cursor_scene(scr_w, scr_h, mx, my);
}


static void composite_cursor_only(int mx, int my, int pmx, int pmy) {
    int x0, y0, bw, bh;
    cursor_bbox(mx, my, pmx, pmy, &x0, &y0, &bw, &bh);
    if (bw <= 0 || bh <= 0) return;
    draw_cursor_into_bb(scr_w, scr_h, mx, my);
    blit_region(x0, y0, bw, bh);
    restore_cursor_scene(scr_w, scr_h, mx, my);
}


static void present_frame(int do_present, int full, int cursor_only) {
    uint32_t *saved_fb = real_fb;
    uint64_t t0 = ic_time_us();
    if (gpu_info.flip_active) {
        real_fb = (uint32_t *)((uint8_t *)saved_fb +
            (uint64_t)wm_flip_page * (uint64_t)fb_info.pitch * (uint64_t)fb_info.height);
        full = 1;
    }
    if (full) mark_dirty_full();
    if (cursor_only && !full) {
        composite_cursor_only(mouse_x, mouse_y, prev_mouse_x, prev_mouse_y);
    } else {
        composite_dirty(mouse_x, mouse_y, prev_mouse_x, prev_mouse_y);
    }
    if (do_present) icda_gpu_present_flags(wm_settings.vsync ? 1ULL : 0ULL);
    if (gpu_info.flip_active) {
        wm_flip_page ^= 1;
        real_fb = saved_fb;
    }
    wm_diag_composite_count++;
    wm_diag_last_frame_us = (unsigned long)(ic_time_us() - t0);
    if (wm_diag_last_frame_us > wm_diag_max_frame_us) wm_diag_max_frame_us = wm_diag_last_frame_us;
}



static void wm_power_sequence(int restart) {
    uint64_t t0;
    uint32_t dur = wm_settings.boot_anim ? 700u : 0u;
    launcher_set(0);
    if (dur == 0 || scr_w < 320 || scr_h < 240) {
        icda_power(restart ? 1U : 0U);
        return;
    }
    composite_region(0, 0, scr_w, scr_h);
    copy_pixels(layer_buffer, back_buffer, scr_w * scr_h);
    t0 = ic_time_ns();
    for (;;) {
        float t = (float)(ic_time_ns() - t0) / ((float)dur * 1e6f);
        uint32_t *saved_fb = real_fb;
        if (t > 1.0f) t = 1.0f;
        copy_pixels(back_buffer, layer_buffer, scr_w * scr_h);
        wm_power_overlay_draw(&scene, scr_w, scr_h, ic_ease(IC_EASE_STANDARD, t), restart);
        if (gpu_info.flip_active) {
            real_fb = (uint32_t *)((uint8_t *)saved_fb +
                (uint64_t)wm_flip_page * (uint64_t)fb_info.pitch * (uint64_t)fb_info.height);
        }
        blit_region(0, 0, scr_w, scr_h);
        icda_gpu_present_flags(wm_settings.vsync ? 1ULL : 0ULL);
        if (gpu_info.flip_active) {
            wm_flip_page ^= 1;
            real_fb = saved_fb;
        }
        if (t >= 1.0f) break;
        icda_sleep(1);
    }
    icda_power(restart ? 1U : 0U);
}




static int window_at(int mx, int my, wm_hit_t *hit_out) {
    for (int i = num_windows - 1; i >= 0; i--) {
        int idx = z_order[i];
        wm_window_t *win;
        wm_frame_t f;
        wm_hit_t hit;
        if (idx < 0 || idx >= MAX_WINDOWS) continue;
        win = &windows[idx];
        if (!win->valid || win->minimized || win->closing) continue;
        win_frame(win, idx, &f);
        hit = wm_frame_hit(&f, mx, my);
        if (hit != WM_HIT_NONE) {
            if (hit_out) *hit_out = hit;
            return idx;
        }
    }
    if (hit_out) *hit_out = WM_HIT_NONE;
    return -1;
}

static int handle_desktop_icon_click(int mx, int my) {
    for (int i = 0; i < desk_icon_count; i++) {
        if (desk_hit(i, mx, my)) {
            uint64_t now = icda_ticks();
            for (int j = 0; j < desk_icon_count; j++) {
                int sel = (j == i);
                if (desk_icons[j].selected != sel) {
                    desk_icons[j].selected = sel;
                    desk_paint_cell(&desk_icons[j]);
                }
            }
            if (desk_last_click_icon == i && now - desk_last_click_tick < DBLCLICK_TICKS) {
                desk_last_click_icon = -1;
                icda_spawn(desk_icons[i].path);
            } else {
                desk_last_click_icon = i;
                desk_last_click_tick = now;
            }
            desk_drag_icon = i;
            desk_dragging = 0;
            desk_press_x = mx;
            desk_press_y = my;
            desk_grab_dx = mx - desk_cell(&desk_icons[i]).x;
            desk_grab_dy = my - desk_cell(&desk_icons[i]).y;
            desk_ghost_x = desk_cell(&desk_icons[i]).x;
            desk_ghost_y = desk_cell(&desk_icons[i]).y;
            desk_press_desktop = 1;
            rubber_armed = 0;
            rubber_active = 0;
            return 1;
        }
    }
    for (int j = 0; j < desk_icon_count; j++) {
        if (desk_icons[j].selected) {
            desk_icons[j].selected = 0;
            desk_paint_cell(&desk_icons[j]);
        }
    }
    desk_last_click_icon = -1;
    desk_drag_icon = -1;
    desk_dragging = 0;
    desk_press_x = mx;
    desk_press_y = my;
    desk_press_desktop = 1;
    rubber_armed = 1;
    rubber_active = 0;
    return 0;
}

static void handle_bar_click(int hit) {
    if (hit == WM_BAR_LAUNCHER) {
        launcher_set(!launcher_open);
        return;
    }
    launcher_set(0);
    if (hit >= 0) {
        int idx = bar_task_slot(hit);
        if (idx < 0) return;
        if (focused_window_idx == idx && !windows[idx].minimized) {
            minimize_window(idx);
        } else {
            if (windows[idx].minimized) restore_window(idx);
            bring_to_front(idx);
        }
    }
}

static void handle_launcher_click(int hit) {
    if (hit >= 0 && hit < wm_app_count) {
        icda_spawn(wm_apps[hit].path);
        launcher_set(0);
    } else if (hit == WM_LAUNCH_SHUTDOWN || hit == WM_LAUNCH_RESTART) {
        wm_power_sequence(hit == WM_LAUNCH_RESTART);
    }
}

static void begin_resize(int idx, wm_hit_t edge) {
    wm_window_t *win = &windows[idx];
    resize_win = idx;
    resize_edge = edge;
    resize_start = ic_rect_make(win->x, win->y, win->w, win->h);
    resize_mx = mouse_x;
    resize_my = mouse_y;
}

static void update_resize(void) {
    wm_window_t *win;
    int dx, dy;
    ic_rect_t r;
    if (resize_win < 0) return;
    win = &windows[resize_win];
    if (!win->valid) {
        resize_win = -1;
        return;
    }
    dx = mouse_x - resize_mx;
    dy = mouse_y - resize_my;
    r = resize_start;
    if (resize_edge == WM_HIT_RESIZE_R || resize_edge == WM_HIT_RESIZE_BR) r.w += dx;
    if (resize_edge == WM_HIT_RESIZE_L || resize_edge == WM_HIT_RESIZE_BL) {
        r.x += dx;
        r.w -= dx;
    }
    if (resize_edge == WM_HIT_RESIZE_B || resize_edge == WM_HIT_RESIZE_BR ||
        resize_edge == WM_HIT_RESIZE_BL) {
        r.h += dy;
    }
    if (r.w < WIN_MIN_W) {
        if (resize_edge == WM_HIT_RESIZE_L || resize_edge == WM_HIT_RESIZE_BL) r.x -= WIN_MIN_W - r.w;
        r.w = WIN_MIN_W;
    }
    if (r.h < WIN_MIN_H) r.h = WIN_MIN_H;
    if (r.y + r.h > scr_h - WM_BAR_H) r.h = scr_h - WM_BAR_H - r.y;
    if (r.w > scr_w) r.w = scr_w;
    if (r.x == win->x && r.w == win->w && r.h == win->h) return;
    mark_dirty_win(win);
    win->x = r.x;
    win->w = r.w;
    win->h = r.h;
    mark_dirty_win(win);
}

static void end_resize(void) {
    if (resize_win >= 0 && windows[resize_win].valid) {
        wm_window_t *win = &windows[resize_win];
        win_resize_buffer(win, win->w, win->h);
    }
    resize_win = -1;
    resize_edge = WM_HIT_NONE;
}

static void set_caption_hover(int idx, wm_hit_t hit) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *win = &windows[i];
        wm_hit_t want = (i == idx && (hit == WM_HIT_CLOSE || hit == WM_HIT_MINIMIZE ||
                                      hit == WM_HIT_MAXIMIZE)) ? hit : WM_HIT_NONE;
        if (!win->valid) continue;
        if (win->hover != want) {
            win->hover = want;
            mark_dirty_title(win);
        }
    }
}

static void left_press(void) {
    int idx;
    wm_hit_t hit;

    if (props_open) {
        ic_rect_t r = props_rect();
        
        if (!ic_ui_hit(r, mouse_x, mouse_y) || mouse_y >= r.y + r.h - IC_SP_4 - IC_H_CONTROL) {
            props_close();
        }
        return;
    }
    if (ctx_open) {
        int which = ic_ui_menu_hit(&ctx_model, ctx_x, ctx_y, mouse_x, mouse_y);
        if (which >= 0) desk_activate(which);
        else ctx_close();
        return;
    }
    if (launcher_open) {
        int lh = wm_launcher_hit(scr_w, scr_h, mouse_x, mouse_y);
        if (lh != WM_LAUNCH_OUTSIDE) {
            handle_launcher_click(lh);
            return;
        }
        if (wm_bar_hit(scr_w, scr_h, 0, mouse_x, mouse_y) == WM_BAR_LAUNCHER) {
            launcher_set(0);
            return;
        }
        launcher_set(0);
    }
    if (mouse_y >= scr_h - WM_BAR_H) {
        wm_bar_t b;
        bar_build(&b, 0);
        handle_bar_click(wm_bar_hit(scr_w, scr_h, &b, mouse_x, mouse_y));
        return;
    }

    idx = window_at(mouse_x, mouse_y, &hit);
    if (idx >= 0) {
        wm_window_t *win = &windows[idx];
        bring_to_front(idx);
        if (hit == WM_HIT_CLOSE || hit == WM_HIT_MINIMIZE || hit == WM_HIT_MAXIMIZE) {
            press_win = idx;
            press_hit = hit;
            win->pressed = hit;
            mark_dirty_title(win);
        } else if (hit == WM_HIT_TITLE) {
            uint64_t now = icda_ticks();
            if (title_click_win == idx && now - title_click_tick < DBLCLICK_TICKS) {
                title_click_win = -1;
                toggle_maximize(idx);
                return;
            }
            title_click_win = idx;
            title_click_tick = now;
            drag_win = idx;
            drag_off_x = mouse_x - win->x;
            drag_off_y = mouse_y - win->y;
        } else if (wm_hit_is_resize(hit)) {
            begin_resize(idx, hit);
        } else if (hit == WM_HIT_CLIENT) {
            capture_win = idx;
            send_mouse(win, mouse_x - win->x, mouse_y - win->y, mouse_buttons);
        }
        return;
    }

    if (!handle_desktop_icon_click(mouse_x, mouse_y)) {
        if (focused_window_idx != -1) {
            int old = focused_window_idx;
            focused_window_idx = -1;
            mark_dirty_win(&windows[old]);
            mark_dirty_bar();
            notify_focus_change(old, -1);
        }
    }
}

static void left_release(void) {
    if (press_win >= 0) {
        wm_window_t *win = &windows[press_win];
        int idx = press_win;
        wm_hit_t hit = WM_HIT_NONE;
        press_win = -1;
        if (win->valid) {
            wm_frame_t f;
            win->pressed = WM_HIT_NONE;
            mark_dirty_title(win);
            win_frame(win, idx, &f);
            hit = wm_frame_hit(&f, mouse_x, mouse_y);
            if (hit == press_hit) {
                if (hit == WM_HIT_CLOSE) {
                    send_close_to_app(win);
                    start_close(win);
                } else if (hit == WM_HIT_MINIMIZE) {
                    minimize_window(idx);
                } else if (hit == WM_HIT_MAXIMIZE) {
                    toggle_maximize(idx);
                }
            }
        }
        press_hit = WM_HIT_NONE;
    }
    if (drag_win >= 0) drag_win = -1;
    if (resize_win >= 0) end_resize();
    if (capture_win >= 0) {
        wm_window_t *win = &windows[capture_win];
        if (win->valid && !win->closing) {
            send_mouse(win, mouse_x - win->x, mouse_y - win->y, mouse_buttons);
        }
        capture_win = -1;
    }
    desk_left_release(mouse_x, mouse_y);
}



static void right_edge(uint8_t prev_buttons) {
    int pressed = (mouse_buttons & 2) && !(prev_buttons & 2);
    int released = !(mouse_buttons & 2) && (prev_buttons & 2);
    int idx;
    wm_hit_t hit;

    if (!pressed && !released) return;
    if (pressed && (ctx_open || props_open || launcher_open)) {
        ctx_close();
        props_close();
        launcher_set(0);
        return;
    }
    idx = window_at(mouse_x, mouse_y, &hit);
    if (idx >= 0) {
        if (hit == WM_HIT_CLIENT) {
            wm_window_t *win = &windows[idx];
            if (pressed) bring_to_front(idx);
            send_mouse(win, mouse_x - win->x, mouse_y - win->y, mouse_buttons);
        }
        return;
    }
    if (pressed && mouse_y < scr_h - WM_BAR_H) {
        int icon = -1;
        for (int i = 0; i < desk_icon_count; i++) {
            if (desk_hit(i, mouse_x, mouse_y)) {
                icon = i;
                break;
            }
        }
        if (icon >= 0) {
            for (int j = 0; j < desk_icon_count; j++) {
                int sel = (j == icon);
                if (desk_icons[j].selected != sel) {
                    desk_icons[j].selected = sel;
                    desk_paint_cell(&desk_icons[j]);
                }
            }
        }
        ctx_open_at(mouse_x, mouse_y, icon);
    }
}



static void pointer_moved(void) {
    int idx;
    wm_hit_t hit;

    desk_track_motion(mouse_x, mouse_y, mouse_buttons);

    if (drag_win >= 0) {
        wm_window_t *win = &windows[drag_win];
        if (!win->valid || win->closing) {
            drag_win = -1;
        } else {
            if (win->maximized) {
                

                int rw = win->restore_w;
                mark_dirty_win(win);
                win->maximized = 0;
                drag_off_x = rw * drag_off_x / (win->w > 0 ? win->w : 1);
                win->w = rw;
                win->h = win->restore_h;
                win_resize_buffer(win, win->w, win->h);
            }
            mark_dirty_win(win);
            win->x = mouse_x - drag_off_x;
            win->y = mouse_y - drag_off_y;
            clamp_window(win);
            mark_dirty_win(win);
        }
        return;
    }
    if (resize_win >= 0) {
        update_resize();
        return;
    }

    
    idx = window_at(mouse_x, mouse_y, &hit);
    set_caption_hover(press_win >= 0 ? press_win : idx, press_win >= 0 ? press_hit : hit);
    {
        wm_bar_t b;
        int bh;
        bar_build(&b, 0);
        bh = wm_bar_hit(scr_w, scr_h, &b, mouse_x, mouse_y);
        if (bh != bar_hover) {
            bar_hover = bh;
            mark_dirty_bar();
        }
    }
    if (launcher_open) {
        int lh = wm_launcher_hit(scr_w, scr_h, mouse_x, mouse_y);
        if (lh != launcher_hover) {
            launcher_hover = lh;
            mark_dirty_launcher();
        }
    }
    if (ctx_open) {
        int h = ic_ui_menu_hit(&ctx_model, ctx_x, ctx_y, mouse_x, mouse_y);
        if (h != ctx_hover) {
            ctx_hover = h;
            mark_dirty_rect(ctx_rect());
        }
    }
    if (props_open) {
        ic_rect_t r = props_rect();
        int h = (ic_ui_hit(r, mouse_x, mouse_y) &&
                 mouse_y >= r.y + r.h - IC_SP_4 - IC_H_CONTROL) ? 0 : -1;
        if (h != props_hover) {
            props_hover = h;
            mark_dirty_rect(r);
        }
    }

    


    if (capture_win >= 0) {
        wm_window_t *win = &windows[capture_win];
        if (win->valid && !win->closing) {
            send_mouse(win, mouse_x - win->x, mouse_y - win->y, mouse_buttons);
        }
        return;
    }
    for (int i = 0; i < MAX_WINDOWS; i++) {
        wm_window_t *win = &windows[i];
        int inside;
        if (!win->valid || win->closing || win->minimized) {
            win->pointer_in = 0;
            continue;
        }
        inside = (i == idx && hit == WM_HIT_CLIENT);
        if (inside) {
            send_mouse(win, mouse_x - win->x, mouse_y - win->y, mouse_buttons);
        } else if (win->pointer_in) {
            send_mouse(win, -1, -1, mouse_buttons);
        }
        win->pointer_in = inside;
    }
}

static void handle_key(long key) {
    if (key == 0x80) {
        
        wm_debug_overlay = !wm_debug_overlay;
        mark_dirty(0, 0, 340, 140);
        return;
    }
    if (key == 27 && (ctx_open || props_open || launcher_open)) {
        ctx_close();
        props_close();
        launcher_set(0);
        return;
    }
    if (props_open && key == 13) {
        props_close();
        return;
    }
    if (focused_window_idx != -1) {
        wm_window_t *win = &windows[focused_window_idx];
        if (win->valid && !win->minimized && !win->closing) {
            gui_msg_t kmsg;
            clear_msg(&kmsg);
            kmsg.type = GUI_MSG_KEY_EVENT;
            kmsg.window_id = win->id;
            kmsg.key.keycode = (uint32_t)key;
            kmsg.key.pressed = 1;
            send_maybe(win->app_queue_handle, &kmsg);
        }
    }
}

int main(int argc, char **argv) {
    uint64_t addr;
    uint64_t wm_queue;
    uint32_t next_win_id = 1;
    uint64_t last_composite_tick = 0;
    int do_present;

    (void)argc;
    (void)argv;

    build_cursor_sprite();
    ic_time_init();

    

    ic_icon_load_folder("/usr/share/icons");

    addr = icda_map_framebuffer(&fb_info);
    if (!addr) return -1;
    real_fb = (uint32_t*)addr;

    if (icda_gpu_query(&gpu_info) != 0) gpu_info.flip_active = 0;
    
    wm_flip_page = gpu_info.flip_active ? 1 : 0;
    
    do_present = gpu_info.flip_active || gpu_info.needs_present;

    scr_w = fb_info.width;
    scr_h = fb_info.height;
    if (scr_w > BACK_BUFFER_WIDTH) scr_w = BACK_BUFFER_WIDTH;
    if (scr_h > BACK_BUFFER_HEIGHT) scr_h = BACK_BUFFER_HEIGHT;
    if (scr_w < 320 || scr_h < 240) return -1;
    scene = ic_canvas_make(back_buffer, scr_w, scr_h);

    wm_queue = icda_msg_open(WM_QUEUE_NAME);
    if (!wm_queue) return -1;

    mouse_x = scr_w / 2;
    mouse_y = scr_h / 2;
    prev_mouse_x = mouse_x;
    prev_mouse_y = mouse_y;

    icda_settings_load(&wm_settings);
    ic_palette_reload();
    settings_last_reload = icda_ticks();
    ic_tween_set(&launcher_fade, 0.0f);
    ic_tween_set(&ctx_fade, 0.0f);
    ic_tween_set(&props_fade, 0.0f);

    desk_init_registry();
    desk_load();
    build_desktop_layer();
    clock_refresh();
    mark_dirty_full();

    for (;;) {
        int need_frame = 0;
        int mouse_moved = 0;
        uint64_t now = icda_ticks();

        if (now - settings_last_reload >= 100) {
            settings_last_reload = now;
            settings_reload();
            clock_refresh();
        }

        while (icda_msg_poll(wm_queue) > 0) {
            gui_msg_t msg;
            if (icda_msg_recv(wm_queue, &msg, 0) != 0) continue;
            if (msg.type == GUI_MSG_OPEN_WINDOW) {
                open_window_from_msg(&msg, wm_queue, &next_win_id);
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                int slot = find_window_by_id(msg.window_id);
                if (slot != -1 && !windows[slot].closing) {
                    send_close_to_app(&windows[slot]);
                    start_close(&windows[slot]);
                }
            } else if (msg.type == GUI_MSG_FLUSH) {
                int slot = find_window_by_id(msg.window_id);
                if (slot != -1) {
                    wm_window_t *win = &windows[slot];
                    mark_dirty(win->x, win->y, win->w, win->h);
                }
            }
        }

        {
            


            icda_mouse_event_t mev;
            while (icda_input_read_mouse(&mev) == 0) {
                uint8_t prev_btn = mouse_buttons;
                mouse_moved = 1;
                wm_diag_mouse_events++;
                mouse_x = mev.abs_x;
                mouse_y = mev.abs_y;
                mouse_buttons = mev.buttons;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_x >= scr_w) mouse_x = scr_w - 1;
                if (mouse_y >= scr_h) mouse_y = scr_h - 1;
                right_edge(prev_btn);
                if ((mouse_buttons & 1) && !(prev_btn & 1)) left_press();
                else if (!(mouse_buttons & 1) && (prev_btn & 1)) left_release();
            }
            if (mouse_moved && (mouse_x != prev_mouse_x || mouse_y != prev_mouse_y)) {
                pointer_moved();
            }
        }

        {
            long key = icda_read_char_timeout(1);
            if (key >= 0) handle_key(key);
        }

        if (animations_tick()) need_frame = 1;

        

        now = icda_ticks();
        if (!wm_settings.vsync || now != last_composite_tick) {
            int moved = mouse_x != prev_mouse_x || mouse_y != prev_mouse_y;
            if (dirty_full || dirty_count > 0 || need_frame) {
                present_frame(do_present, gpu_info.flip_active, 0);
                last_composite_tick = now;
                prev_mouse_x = mouse_x;
                prev_mouse_y = mouse_y;
            } else if (moved) {
                present_frame(do_present, gpu_info.flip_active, 1);
                last_composite_tick = now;
                prev_mouse_x = mouse_x;
                prev_mouse_y = mouse_y;
            }
        }
    }
}
