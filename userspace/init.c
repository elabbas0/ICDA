












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

    pid = icda_spawn(target);
    if ((long)pid < 0) {
        icda_write("init: spawn failed\n");
        return 1;
    }
    


    code = icda_waitpid(pid);
    return code;
}
