#ifndef USERSPACE_WM_DIALOG_H
#define USERSPACE_WM_DIALOG_H

#include <stdint.h>
#include "ic_gfx.h"
#include "ic_ui.h"

/* Dialogs (wm_dialog.c): what programs and the kernel ask for through
 * /dev/dialog.  A program's dialog sits on its window, which takes no input
 * until the dialog is answered; the kernel's (a program that stopped) and
 * those of programs without a window sit in the middle of the screen.
 *
 * window_id 0 below means those screen dialogs. */

/* exists: whether a window id is one of the window manager's (a request
 * naming another is shown on the screen instead) */
void wm_dialog_init(int (*exists)(uint32_t window_id));

/* Reads /dev/dialog - at once when now is set (a program said it asked),
 * else every 250 ms.  Returns 1 when dialogs came or went (redraw all);
 * *focus_window then names a window whose new dialog wants the focus (0:
 * none). */
int  wm_dialog_poll(int now, uint32_t *focus_window);

int  wm_dialog_has(uint32_t window_id);

/* where the dialog sits: over the window's content (screen coordinates),
 * or in the middle of the screen for window_id 0 */
ic_rect_t wm_dialog_rect(uint32_t window_id, ic_rect_t content, int sw, int sh);
/* the rect and its shadow: what to redraw */
ic_rect_t wm_dialog_reach(ic_rect_t r);

/* draws it: a window's dims the window's content (corner: its bottom
 * corners' radius) and sits over it */
void wm_dialog_draw(ic_canvas_t *c, uint32_t window_id, ic_rect_t content, float corner, int sw, int sh);

/* pointer: returns 1 when the dialog needs a redraw */
int  wm_dialog_hover(uint32_t window_id, ic_rect_t r, int x, int y);
int  wm_dialog_press(uint32_t window_id, ic_rect_t r, int x, int y);
/* release: 1 redraw, 2 answered (it is gone: redraw all) */
int  wm_dialog_release(uint32_t window_id, ic_rect_t r, int x, int y);

/* a key for the dialog: 1 redraw, 2 answered */
int  wm_dialog_key(uint32_t window_id, long key);
/* call every pass: Escape on its own (not the start of an arrow key's
 * sequence) cancels once no other key followed; 2 when that answered */
int  wm_dialog_tick(void);
/* the window whose dialog the last wm_dialog_key/tick returning 2 answered */
uint32_t wm_dialog_last_window(void);

/* the window closed: its dialog is answered with no button */
void wm_dialog_window_gone(uint32_t window_id);

/* a dialog the window manager itself shows (no program waits) */
void wm_dialog_local(const char *kind, const char *title, const char *message);

/* who to tell that their dialog was answered: set by wm.c (sends
 * GUI_MSG_DIALOG to the window's program) */
typedef void (*wm_dialog_answered_fn)(uint32_t window_id, uint32_t dialog_id, int button);
void wm_dialog_on_answer(wm_dialog_answered_fn fn);

/* the kernel reports a program that stopped (its pid): wm.c removes its
 * windows and gives the name its window had (1), for the dialog's title */
typedef int (*wm_dialog_stopped_fn)(uint32_t pid, char *label, int cap);
void wm_dialog_on_stopped(wm_dialog_stopped_fn fn);

#endif
