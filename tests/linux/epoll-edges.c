#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

static int serial, failures, checks;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) failures++;
    dprintf(serial, "epoll-edges: %s %s\n", ok ? "PASS" : "FAIL", what);
}
static int ready(int ep, unsigned bits) {
    struct epoll_event ev;
    int n = epoll_wait(ep, &ev, 1, 0);
    return n == 1 && (ev.events & bits) == bits && ev.data.u64 == 42;
}
static int modify(int ep, int fd, unsigned flags) {
    struct epoll_event ev = {.events = flags, .data.u64 = 42};
    return epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev) == 0;
}

int main(void) {
    int pair[2], ep;
    char buf[8];
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.u64 = 42};
    serial = STDOUT_FILENO;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) != 0) return 2;
    ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &ev) != 0) return 2;
    check(ready(ep, EPOLLOUT), "initial writable edge");
    check(!ready(ep, EPOLLOUT), "persistent OUT is not delivered twice");
    check(write(pair[1], "ab", 2) == 2 && ready(ep, EPOLLIN),
          "new IN edge while OUT remains ready");
    check(read(pair[0], buf, 1) == 1 && buf[0] == 'a' && !ready(ep, EPOLLIN),
          "partial read does not invent another IN edge");
    check(read(pair[0], buf, 1) == 1 && buf[0] == 'b', "exact drain");
    /* No epoll call during the unready interval: the completed read must
     * record the reset so the refill still creates a new edge. */
    check(write(pair[1], "c", 1) == 1 && ready(ep, EPOLLIN),
          "IN rearms after exact drain and immediate refill");
    check(recv(pair[0], buf, 1, MSG_PEEK) == 1 && buf[0] == 'c' && !ready(ep, EPOLLIN),
          "MSG_PEEK preserves readiness without rearming");
    check(recv(pair[0], buf, sizeof(buf), 0) == 1, "recv drains data");
    check(recv(pair[0], buf, sizeof(buf), 0) == -1 && errno == EAGAIN,
          "nonblocking recv reaches EAGAIN");
    check(send(pair[1], "d", 1, 0) == 1 && ready(ep, EPOLLIN),
          "IN rearms after EAGAIN and a new send");
    check(modify(ep, pair[0], EPOLLIN) && ready(ep, EPOLLIN) && ready(ep, EPOLLIN),
          "level-triggered readiness repeats until drained");
    check(modify(ep, pair[0], EPOLLIN | EPOLLONESHOT | EPOLLET) &&
          ready(ep, EPOLLIN) && !ready(ep, EPOLLIN), "oneshot disables after delivery");
    check(modify(ep, pair[0], EPOLLIN | EPOLLONESHOT | EPOLLET) && ready(ep, EPOLLIN),
          "MOD rearms oneshot with buffered data");
    close(ep); close(pair[0]); close(pair[1]);

    int efd = eventfd(0, EFD_NONBLOCK);
    unsigned long long value = 1;
    ep = epoll_create1(0);
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    if (efd < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev)) return 2;
    check(ready(ep, EPOLLOUT), "eventfd initial writable edge");
    check(write(efd, &value, 8) == 8 && ready(ep, EPOLLIN), "eventfd IN independent of OUT");
    check(!ready(ep, EPOLLIN), "eventfd has no new event without an operation");
    check(write(efd, &value, 8) == 8 && ready(ep, EPOLLIN | EPOLLOUT),
          "eventfd write notifies while its counter is still readable");
    unsigned long long zero = 0;
    check(write(efd, &zero, 8) == 8 && ready(ep, EPOLLIN | EPOLLOUT),
          "eventfd zero write also notifies with a readable counter");
    check(read(efd, &value, 8) == 8 && ready(ep, EPOLLOUT),
          "eventfd read notifies already writable watchers");
    check(write(efd, &value, 8) == 8 && ready(ep, EPOLLIN),
          "eventfd drain and refill rearms IN");
    close(ep); close(efd);
    dprintf(serial, "epoll-edges: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
