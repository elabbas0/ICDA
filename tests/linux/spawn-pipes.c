#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static int serial, failures, checks;
static void check(int ok, const char *what) {
    checks++; if (!ok) failures++;
    dprintf(serial, "spawn-pipes: %s %s\n", ok ? "PASS" : "FAIL", what);
    if (serial != STDOUT_FILENO) printf("spawn-pipes: %s %s\n", ok ? "PASS" : "FAIL", what);
}
int main(void) {
    int p[2], status;
    pid_t child;
    char buf[64];
    serial = open("/dev/serial", O_WRONLY);
    if (serial < 0) serial = STDOUT_FILENO;
    if (pipe2(p, O_CLOEXEC)) return 2;
    child = fork();
    if (!child) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        char *args[] = {"sh", "-c", "printf child-output; exit 7", 0};
        execve("/bin/sh", args, environ);
        _exit(127);
    }
    if (child < 0) return 2;
    close(p[1]);
    struct pollfd pollfd = {.fd = p[0], .events = POLLIN | POLLHUP};
    check(poll(&pollfd, 1, 2000) == 1, "child stdout becomes readable");
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    ssize_t n = read(p[0], buf, sizeof(buf));
    check(n == 12 && !memcmp(buf, "child-output", 12), "exec preserves redirected stdout");
    int reaped = 0;
    for (int i = 0; i < 200; i++) {
        if (waitpid(child, &status, WNOHANG) == child) { reaped = 1; break; }
        usleep(10000);
    }
    check(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 7, "waitpid reports child exit");
    check(read(p[0], buf, sizeof(buf)) == 0, "last child writer closes at exit (EOF)");
    close(p[0]);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    char *missing[] = {"missing-command", 0};
    int r = posix_spawn(&child, "/definitely-missing-command", &actions, 0, missing, environ);
    check(r == ENOENT, "posix_spawn returns failed exec errno to parent");
    if (!r) {
        for (int i = 0; i < 200; i++) {
            if (waitpid(child, &status, WNOHANG) == child) break;
            usleep(10000);
        }
    }
    posix_spawn_file_actions_destroy(&actions);
    dprintf(serial, "spawn-pipes: %d/%d passed\n", checks - failures, checks);
    if (serial != STDOUT_FILENO) printf("spawn-pipes: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
