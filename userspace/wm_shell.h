








#ifndef USERSPACE_WM_SHELL_H
#define USERSPACE_WM_SHELL_H

#include "libicda.h"

#define WM_BAR_H IC_H_TASKBAR



typedef struct {
    const char *label;
    const char *icon;    
    const char *path;
} wm_app_t;

extern const wm_app_t wm_apps[];
extern const int      wm_app_count;


const ic_icon_t *wm_app_icon_for_title(const char *title);





void wm_wallpaper_paint(ic_canvas_t *c, int sw, int sh);



#define WM_DESK_CELL_W  92
#define WM_DESK_CELL_H 100
#define WM_DESK_X0      16
#define WM_DESK_Y0      18


static inline ic_rect_t wm_desk_cell_rect(int cx, int cy) {
    return ic_rect_make(WM_DESK_X0 + cx * WM_DESK_CELL_W, WM_DESK_Y0 + cy * WM_DESK_CELL_H,
                        WM_DESK_CELL_W, WM_DESK_CELL_H);
}

ic_rect_t wm_desk_icon_hit_rect(ic_rect_t cell);
void wm_desk_icon_draw(ic_canvas_t *c, ic_rect_t cell, const char *label,
                       const ic_icon_t *icon, int selected, float opacity);



typedef struct {
    const char      *title;
    const ic_icon_t *icon;
    int              focused;
    int              minimized;
} wm_task_t;

#define WM_BAR_NONE      (-1)
#define WM_BAR_LAUNCHER  (-2)
#define WM_BAR_STATUS    (-3)
#define WM_BAR_WIFI      (-4)
#define WM_BAR_UPDATE    (-5)

typedef struct {
    const wm_task_t *tasks;
    int              count;
    int              launcher_open;
    int              hover;        
    int              pressed;
    const char      *time_text;    
    const char      *date_text;    
    const char      *audio_text;   
    int              wifi_state;   /* WM_WIFI_* from wm_wifi.h */
    int              wifi_open;
    int              update_ready; /* a patch waits for a restart */
} wm_bar_t;

ic_rect_t wm_bar_rect(int sw, int sh);
ic_rect_t wm_bar_launcher_rect(int sw, int sh);
ic_rect_t wm_bar_task_rect(int sw, int sh, int count, int index);
ic_rect_t wm_bar_wifi_rect(int sw, int sh);
ic_rect_t wm_bar_update_rect(int sw, int sh);
int       wm_bar_hit(int sw, int sh, const wm_bar_t *b, int mx, int my);
void      wm_bar_draw(ic_canvas_t *c, int sw, int sh, const wm_bar_t *b,
                      uint32_t *scratch, int scratch_len);



#define WM_LAUNCH_NONE      (-1)
#define WM_LAUNCH_OUTSIDE   (-2)
#define WM_LAUNCH_SHUTDOWN  (-10)
#define WM_LAUNCH_RESTART   (-11)

#define WM_LAUNCH_QUERY_CAP 32
#define WM_LAUNCH_MAX_APPS  32
extern char wm_launch_query[WM_LAUNCH_QUERY_CAP];
int  wm_launch_visible(int *out);

ic_rect_t wm_launcher_rect(int sw, int sh);
ic_rect_t wm_launcher_item_rect(int sw, int sh, int item);


int  wm_launcher_hit(int sw, int sh, int mx, int my);
void wm_launcher_draw(ic_canvas_t *c, int sw, int sh, int hover,
                      uint32_t *scratch, int scratch_len);



void wm_rubber_band_draw(ic_canvas_t *c, int x0, int y0, int x1, int y1);


void wm_power_overlay_draw(ic_canvas_t *c, int sw, int sh, float t, int restart);

void wm_boot_overlay_draw(ic_canvas_t *c, int sw, int sh, float t);


void wm_debug_draw(ic_canvas_t *c, const char *const *lines, int count);

#endif 
