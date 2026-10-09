#ifndef USERSPACE_WM_PANELS_H
#define USERSPACE_WM_PANELS_H

#include "libicda.h"

/* Taskbar volume, battery and notifications (wm_panels.c):
 *   - the quick settings flyout: volume, power mode, battery
 *   - the notification centre: the history kept by /dev/notify
 *   - toasts: a new notification above the taskbar for a few seconds */

#define WM_PANEL_QUICK  1
#define WM_PANEL_NOTES  2

#define WM_PANEL_KEEP    0
#define WM_PANEL_REDRAW  1
#define WM_PANEL_CLOSE   2

void wm_panels_init(void);                   /* applies the saved volume and power mode */

/* Reads battery and notifications if min_ms passed.  Bit 0: the taskbar
 * changed, bit 1: an open panel did, bit 2: the toast did. */
int  wm_panels_poll(uint32_t min_ms);

int  wm_panels_battery(int *percent, int *charging);   /* 0: no battery */
int  wm_panels_volume(int *muted);                     /* 0..100 */
int  wm_panels_unread(void);

void      wm_panels_open(int which);
ic_rect_t wm_panels_rect(int which, int sw, int sh);
void      wm_panels_draw(int which, ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len);
int       wm_panels_click(int which, int sw, int sh, int mx, int my);
int       wm_panels_drag(int which, int sw, int sh, int mx, int my);   /* button held */
int       wm_panels_release(void);                                    /* REDRAW when a drag ended */
int       wm_panels_hover(int which, int sw, int sh, int mx, int my);
int       wm_panels_wheel(int delta);                                 /* volume by wheel over the taskbar */

int       wm_toast_visible(void);
ic_rect_t wm_toast_rect(int sw, int sh);
void      wm_toast_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len);
void      wm_toast_dismiss(void);

#endif
