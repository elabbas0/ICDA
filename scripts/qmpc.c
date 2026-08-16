/* Tiny QMP client over a unix socket.  Usage:
 *   qmpc <sock> screendump <ppm-path>
 *   qmpc <sock> key <qcode>
 *   qmpc <sock> keydown <qcode>
 *   qmpc <sock> keyup <qcode>
 *   qmpc <sock> mousemove <x> <y>
 *   qmpc <sock> click
 *   qmpc <sock> mousedown
 *   qmpc <sock> mouseup
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int sfd;

static void send_cmd(const char *json) {
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", json);
    if (write(sfd, buf, (size_t)n) != n) {
        perror("write");
        exit(1);
    }
}

static void read_until_return(void) {
    char buf[8192];
    size_t used = 0;
    for (;;) {
        ssize_t n = read(sfd, buf + used, sizeof(buf) - used - 1);
        if (n <= 0) break;
        used += (size_t)n;
        buf[used] = 0;
        if (strstr(buf, "\r\n")) break;
    }
    printf("%s", buf);
}

static void cmd(const char *json) {
    send_cmd(json);
    read_until_return();
}

static void qmp_key(const char *qcode, int down) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
             "{\"type\":\"key\",\"data\":{\"down\":%s,\"key\":{\"type\":\"qcode\",\"data\":\"%s\"}}}]}}",
             down ? "true" : "false", qcode);
    cmd(buf);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: qmpc <sock> <op> [args]\n");
        return 1;
    }
    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, argv[1], sizeof(addr.sun_path) - 1);
    if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }
    /* QMP sends a greeting immediately on connect; consume it, then
     * negotiate capabilities and consume that reply too. */
    read_until_return();
    send_cmd("{\"execute\":\"qmp_capabilities\"}");
    read_until_return();

    const char *op = argv[2];
    if (strcmp(op, "screendump") == 0) {
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\"execute\":\"screendump\",\"arguments\":{\"filename\":\"%s\"}}", argv[3]);
        cmd(buf);
    } else if (strcmp(op, "key") == 0) {
        qmp_key(argv[3], 1);
        usleep(50000);
        qmp_key(argv[3], 0);
    } else if (strcmp(op, "hmp") == 0) {
        char buf[1024];
        snprintf(buf, sizeof(buf),
                 "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"%s\"}}", argv[3]);
        cmd(buf);
    } else if (strcmp(op, "keydown") == 0) {
        qmp_key(argv[3], 1);
    } else if (strcmp(op, "keyup") == 0) {
        qmp_key(argv[3], 0);
    } else if (strcmp(op, "mousemove") == 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
                 "{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":%d}},"
                 "{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":%d}}]}}",
                 atoi(argv[3]), atoi(argv[4]));
        cmd(buf);
    } else if (strcmp(op, "click") == 0) {
        cmd("{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
            "{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"left\"}},"
            "{\"type\":\"btn\",\"data\":{\"down\":false,\"button\":\"left\"}}]}}");
    } else if (strcmp(op, "mousedown") == 0) {
        cmd("{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
            "{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"left\"}}]}}");
    } else if (strcmp(op, "mouseup") == 0) {
        cmd("{\"execute\":\"input-send-event\",\"arguments\":{\"events\":["
            "{\"type\":\"btn\",\"data\":{\"down\":false,\"button\":\"left\"}}]}}");
    } else {
        fprintf(stderr, "unknown op\n");
        return 1;
    }
    close(sfd);
    return 0;
}
