#ifndef USERSPACE_WM_LOGIN_H
#define USERSPACE_WM_LOGIN_H

#include "libicda.h"

/* The sign-in, first-account and lock screens (wm_login.c).  The desktop
 * runs them as a modal loop of its own: before it loads anyone's settings,
 * and over everything when the session is locked.  Accounts and passwords
 * are the kernel's (icda_account_*); nothing here keeps a password longer
 * than the screen is up. */

#define WM_LOGIN_NONE    0   /* signed in by itself: nothing to show */
#define WM_LOGIN_SETUP   1   /* no accounts yet: the owner makes the first */
#define WM_LOGIN_SIGNIN  2
#define WM_LOGIN_LOCK    3

#define WM_LOGIN_KEEP     0
#define WM_LOGIN_REDRAW   1
#define WM_LOGIN_DONE     2  /* signed in, or unlocked */
#define WM_LOGIN_SIGN_OUT 3  /* from the lock screen */

int  wm_login_begin(int mode);   /* the mode it shows (NONE: automatic sign-in done) */
void wm_login_draw(ic_canvas_t *c, int sw, int sh, uint32_t *scratch, int scratch_len);
int  wm_login_key(long key);
int  wm_login_click(int sw, int sh, int x, int y);
int  wm_login_hover(int sw, int sh, int x, int y);   /* REDRAW when what is under the pointer changed */
int  wm_login_tick(void);        /* the caret and the clock */
void wm_login_end(void);         /* wipes what was typed */

#endif
