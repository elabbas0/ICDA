#ifndef KERNEL_VT_H
#define KERNEL_VT_H

/*
 * Virtual terminal manager.
 *
 * Ctrl+Alt+F1 runs the GUI desktop (the window manager); Ctrl+Alt+F2 and
 * the higher F-keys run a full-screen text shell, Linux-style.  The
 * keyboard driver requests a switch from IRQ context; the timer IRQ
 * applies it (vt_tick) by force-exiting the foreground user app so the
 * kernel boot loop restarts the app that belongs to the active VT.
 */

#define VT_GUI      1
#define VT_TEXT_MIN 2
#define VT_TEXT_MAX 6

/* Called from keyboard IRQ context to request a switch (1..VT_TEXT_MAX). */
void vt_request_switch(int number);

/* Called from the timer IRQ every tick; applies a pending switch. */
void vt_tick(void);

int  vt_active_number(void);
int  vt_is_gui(void);

/* Path of the app that should run on the active VT. */
const char *vt_app_path(void);

#endif /* KERNEL_VT_H */
