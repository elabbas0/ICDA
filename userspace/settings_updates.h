#ifndef USERSPACE_SETTINGS_UPDATES_H
#define USERSPACE_SETTINGS_UPDATES_H

#include "libicda.h"

/*
 * Settings > Updates: installed version, the updater's status and the
 * pending patch, on top of /dev/sysupdate and /etc/icda-release.txt.
 */
void updates_pane_enter(void);
void updates_pane_tick(ic_app_t *app);
void updates_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area);
/* Returns 1 if the event was used by the pane. */
int  updates_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area);

#endif
