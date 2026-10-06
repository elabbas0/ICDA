#ifndef USERSPACE_SETTINGS_KEYS_H
#define USERSPACE_SETTINGS_KEYS_H

#include "libicda.h"

/* Settings > Keyboard: edits the global shortcuts (shortcuts.h). */
void keys_pane_enter(void);
void keys_pane_leave(void);
void keys_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area);
/* Returns 1 if the event was used by the pane. */
int  keys_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area);
/* recording a shortcut: the pane takes the keyboard */
int  keys_pane_modal(void);

#endif
