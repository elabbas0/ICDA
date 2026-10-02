#include "vt.h"

#include "../drivers/console/console.h"
#include "../drivers/device.h"
#include "../drivers/serial/serial.h"
#include "../proc/sched.h"





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

    




    if (target == VT_GUI) {
        console_mute_fb(1);
    } else {
        console_mute_fb(0);
    }
    console_clear();

    





    sched_force_exit_all_user_processes(0);
}
