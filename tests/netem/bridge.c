/* QEMU "-netdev socket" (4-byte big-endian length + Ethernet frame over TCP)
 * <-> a Linux TAP device, so a guest talks to this container's real kernel. */
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
static int full(int fd, void *b, size_t n) {
    for (size_t d = 0; d < n; ) { ssize_t k = read(fd, (char *)b + d, n - d); if (k <= 0) return -1; d += k; }
    return 0;
}
int main(void) {
    struct ifreq ifr = {0};
    int tap = open("/dev/net/tun", O_RDWR), ls = socket(AF_INET, SOCK_STREAM, 0), one = 1, c;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(9999) };
    unsigned char buf[65536 + 4];
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strcpy(ifr.ifr_name, "tap0");
    if (tap < 0 || ioctl(tap, TUNSETIFF, &ifr) < 0) { perror("tap"); return 1; }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(ls, (void *)&a, sizeof a) || listen(ls, 1)) { perror("listen"); return 1; }
    printf("bridge ready\n"); fflush(stdout);
    c = accept(ls, 0, 0);
    setsockopt(c, IPPROTO_TCP, 1 /* TCP_NODELAY */, &one, sizeof one);
    printf("qemu connected\n"); fflush(stdout);
    for (;;) {
        struct pollfd p[2] = { { c, POLLIN, 0 }, { tap, POLLIN, 0 } };
        poll(p, 2, -1);
        if (p[0].revents) {
            unsigned n;
            if (full(c, buf, 4)) return 0;
            n = (unsigned)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
            if (n > 65536 || full(c, buf, n)) return 0;
            if (n > 54 && buf[12] == 8 && buf[13] == 0 && buf[23] == 6) {
                unsigned char *t = buf + 14 + (buf[14] & 15) * 4;
                if (t[13] & 2) { int h = (t[12] >> 4) * 4; printf("SYN from guest, %d header bytes, options:", h); for (int i = 20; i < h; i++) printf(" %d", t[i]); printf("\n"); fflush(stdout); }
            }
            write(tap, buf, n);
        }
        if (p[1].revents) {
            ssize_t n = read(tap, buf + 4, 65536);
            if (n <= 0) return 0;
            if (n > 54 && buf[16] == 8 && buf[17] == 0 && buf[27] == 6) {
                unsigned char *t = buf + 18 + (buf[18] & 15) * 4;
                if (t[13] & 2) { int h = (t[12] >> 4) * 4; printf("SYN-ACK from server, %d header bytes, options:", h); for (int i = 20; i < h; i++) printf(" %d", t[i]); printf("\n"); fflush(stdout); }
            }
            buf[0] = n >> 24; buf[1] = n >> 16; buf[2] = n >> 8; buf[3] = n;
            if (write(c, buf, n + 4) != n + 4) return 0;
        }
    }
}
