/* PID1 supervisor (P0 OS-ification, step 2).
 *
 * The kernel spawns /sbin/init.app once per VT era with argv[1] telling
 * which foreground app to run ("gui" -> /apps/wm.app, anything else ->
 * /apps/shell.app). Init spawns exactly that one child, waits for it,
 * and exits with its code; the kernel loop respawns init (and falls
 * back to recovery after 3 fast failures), so crash and VT-switch
 * behavior is identical to the old in-kernel loop.
 *
 * Deliberately thin: no mounts (boot already mounted), no services yet,
 * no enforcement. Identity (uid 0 / session leader) is assigned by the
 * kernel at spawn; children inherit it.
 */
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

    icda_write("init: supervising ");
    icda_write(target);
    icda_write("\n");

    pid = icda_spawn(target);
    if ((long)pid < 0) {
        icda_write("init: spawn failed\n");
        return 1;
    }
    /* Propagate the child's exit code: a clean VT-switch exit (0)
     * respawns quietly; a crash (<0) trips the kernel's retry and
     * recovery path (B1). SYS_WAITPID returns the code itself. */
    code = icda_waitpid(pid);
    return code;
}
