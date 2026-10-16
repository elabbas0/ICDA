#ifndef USERSPACE_SETTINGS_USERS_H
#define USERSPACE_SETTINGS_USERS_H

#include "libicda.h"

/* Settings > Users: the accounts on this computer.  Administrators add and
 * remove accounts and choose who is one; everyone changes their own password
 * and whether they sign in by themselves.  Each change asks for the
 * signed-in user's password; the kernel checks it (icda_account_*). */

void users_pane_enter(void);
void users_pane_leave(void);
void users_pane_draw(ic_app_t *app, ic_canvas_t *c, ic_rect_t area);
int  users_pane_event(ic_app_t *app, const ic_event_t *ev, ic_rect_t area);   /* 1: used */
int  users_pane_modal(void);   /* a field has the keyboard */

#endif
