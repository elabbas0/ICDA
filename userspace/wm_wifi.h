#ifndef USERSPACE_WM_WIFI_H
#define USERSPACE_WM_WIFI_H

#include "libicda.h"

/* Taskbar Wi-Fi: status icon and the network flyout above the taskbar,
 * on top of /dev/wifi (the same interface as Settings > Wi-Fi). */

#define WM_WIFI_NONE      0   /* no adapter */
#define WM_WIFI_OFF       1   /* starting, failed or radio off */
#define WM_WIFI_IDLE      2   /* running, not connected */
#define WM_WIFI_BUSY      3   /* connecting or getting an address */
#define WM_WIFI_ONLINE    4

/* Reads the driver report if at least min_ms passed since the last read.
 * Returns bit 0 when the taskbar icon changed, bit 1 when the flyout did. */
int  wm_wifi_poll(uint32_t min_ms);
int  wm_wifi_bar_state(void);

void      wm_wifi_open(void);       /* resets the flyout and asks for a scan */
ic_rect_t wm_wifi_rect(int sw, int sh);
ic_rect_t wm_wifi_reach(int sw, int sh);   /* largest the flyout gets */
void      wm_wifi_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len);

#define WM_WIFI_KEEP      0
#define WM_WIFI_REDRAW    1
#define WM_WIFI_CLOSE     2

int  wm_wifi_hover(int sw, int sh, int mx, int my);   /* REDRAW when hover changed */
int  wm_wifi_click(int sw, int sh, int mx, int my);   /* inside the flyout */
int  wm_wifi_key(int key);

#endif
