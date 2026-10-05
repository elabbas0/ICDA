#ifndef USERSPACE_SETTINGS_WIFI_H
#define USERSPACE_SETTINGS_WIFI_H

#include "libicda.h"

/*
 * Settings > Wi-Fi: talks to the kernel driver through /dev/wifi
 * (commands are written, the status/scan/log report is read back).
 */
void wifi_pane_enter(void);
void wifi_pane_tick(ic_app_t *app);
void wifi_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area);
/* Returns 1 if the event was used by the pane. */
int  wifi_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area);
/* The password sheet takes the keyboard while it is open. */
int  wifi_pane_modal(void);

#endif
