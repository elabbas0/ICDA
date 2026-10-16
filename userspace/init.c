












#include "icda_sys.h"

static int streq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

int init_main(int argc, char **argv) {
    const char *target;
    uint64_t pid;
    uint64_t code = 0;
    int gui = 1;

    if (argc > 1 && argv && argv[1] && streq(argv[1], "text")) {
        gui = 0;
    }
    target = gui ? "/apps/wm.app" : "/apps/shell.app";

    






    /* OTA patches in the background (absent on systems without \SYSTEM) */
    (void)icda_spawn("/sbin/updated");

    /* The session: the desktop signs a user in and ends when they sign out;
     * the next one starts at the sign-in screen.  The text shell runs once. */
    for (;;) {
        pid = icda_spawn(target);
        if ((long)pid < 0) {
            icda_write("init: spawn failed\n");
            return 1;
        }
        code = icda_waitpid(pid);
        if (!gui) return code;
        icda_sleep(50);                 /* a desktop that keeps failing still gives way */
    }
}
