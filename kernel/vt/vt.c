#include "vt.h"

#include "../drivers/console/console.h"
#include "../drivers/device.h"
#include "../drivers/serial/serial.h"
#include "../proc/sched.h"

/*
 * The active VT number (VT_GUI = desktop, VT_TEXT_MIN.. = text shell) and
 * a pending switch request set by the keyboard driver from IRQ context.
 */
static volatile int vt_active = VT_GUI;
static volatile int vt_pending = 0;

void vt_request_switch(int number) {
    if (number < VT_GUI || number > VT_TEXT_MAX) {
        return;
    }
    vt_pending = number;
}

int vt_active_number(void) {
    return vt_active;
}

int vt_is_gui(void) {
    return vt_active == VT_GUI;
}

const char *vt_app_path(void) {
    return vt_active == VT_GUI ? "/apps/wm.app" : "/apps/shell.app";
}

void vt_tick(void) {
    int target;

    if (!vt_pending) {
        return;
    }

    target = vt_pending;
    vt_pending = 0;
    if (target == vt_active) {
        return;
    }

    vt_active = target;

    /* Wipe the screen so the next app starts clean.  The GUI repaints
     * everything itself and the text shell clears again on startup. */
    console_clear();

    /* Force-exit every user process (the foreground app and whatever it
     * spawned).  The timer IRQ that gets us here may have fired while the
     * app was blocked - with the idle thread current - so exit by walking
     * the process list rather than by touching the current thread.  The
     * kernel boot loop's user_run_path() then returns and restarts the
     * app for the newly active VT. */
    sched_force_exit_all_user_processes(0);
}
