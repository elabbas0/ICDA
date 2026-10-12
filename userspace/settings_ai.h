#ifndef USERSPACE_SETTINGS_AI_H
#define USERSPACE_SETTINGS_AI_H

#include "libicda.h"

/* Settings > Assistant: the models the assistant asks, edited here instead of
 * by hand in /home/.config/ai.conf.  The file keeps exactly the format
 * assistantd parses, so the two always agree. */

void ai_pane_enter(void);
void ai_pane_leave(void);
void ai_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area);
/* Returns 1 if the event was used by the pane. */
int  ai_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area);
/* a field or the key box has the keyboard: do not steal keys for shortcuts */
int  ai_pane_modal(void);
void ai_pane_tick(ic_app_t *app);

#endif