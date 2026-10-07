/* IPC on ICDA's Linux personality: what WebKit's processes and GLib /
 * Wayland event loops use.  Prints "ipctest: ALL PASS" when all is well. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("PASS %s\n", what); else { printf("FAIL %s\n", what); failures++; } fflush(stdout); } while (0)

static int send_fd(int sock, int fd, const char *msg) {
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { (void *)msg, strlen(msg) };
    struct msghdr m = { 0 };
    struct cmsghdr *c;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;
    c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return (int)sendmsg(sock, &m, 0);
}

static int recv_fd(int sock, char *buf, size_t n) {
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { buf, n - 1 };
    struct msghdr m = { 0 };
    struct cmsghdr *c;
    ssize_t r;
    int fd = -1;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;
    r = recvmsg(sock, &m, 0);
    if (r < 0) return -1;
    buf[r] = 0;
    for (c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

int main(void) {
    /* 1: a stream pair across fork, passing a descriptor */
    {
        int sv[2], p[2], st = -1;
        pid_t pid;
        char buf[64];
        int ok = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0 && pipe(p) == 0;
        if (ok && (pid = fork()) == 0) {
            int got;
            close(sv[0]);
            got = recv_fd(sv[1], buf, sizeof buf);
            if (got < 0 || strcmp(buf, "here") != 0) _exit(2);
            if (write(got, "through the pipe", 16) != 16) _exit(3);
            _exit(0);
        }
        if (ok) {
            close(sv[1]);
            ok = send_fd(sv[0], p[1], "here") == 4;
            waitpid(pid, &st, 0);
            ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0 && read(p[0], buf, 16) == 16 && !memcmp(buf, "through the pipe", 16);
        }
        CHECK(ok, "socketpair + fork + SCM_RIGHTS");
    }

    /* 2: seqpacket keeps message boundaries */
    {
        int sv[2];
        char buf[32];
        int ok = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0 && write(sv[0], "one", 3) == 3 && write(sv[0], "second", 6) == 6;
        ok = ok && read(sv[1], buf, sizeof buf) == 3 && read(sv[1], buf, sizeof buf) == 6;
        CHECK(ok, "seqpacket boundaries");
        close(sv[0]);
        ok = read(sv[1], buf, sizeof buf) == 0;
        CHECK(ok, "peer close reads as end");
        close(sv[1]);
    }

    /* 3: named socket: bind / listen / connect / accept */
    {
        struct sockaddr_un a = { AF_UNIX, "\0icda-ipctest" };
        socklen_t len = offsetof(struct sockaddr_un, sun_path) + 13;
        int l = socket(AF_UNIX, SOCK_STREAM, 0), c = socket(AF_UNIX, SOCK_STREAM, 0), s;
        char buf[8];
        int ok = bind(l, (struct sockaddr *)&a, len) == 0 && listen(l, 4) == 0 && connect(c, (struct sockaddr *)&a, len) == 0;
        s = ok ? accept(l, 0, 0) : -1;
        ok = s >= 0 && write(c, "ping", 4) == 4 && read(s, buf, 4) == 4 && !memcmp(buf, "ping", 4);
        CHECK(ok, "abstract socket connect / accept");
        close(l); close(c); if (s >= 0) close(s);
    }

    /* 4: eventfd + epoll */
    {
        int e = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC), ep = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event ev = { EPOLLIN, { .u64 = 77 } }, out[4];
        uint64_t one = 1, v = 0;
        int ok = e >= 0 && ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, e, &ev) == 0 && epoll_wait(ep, out, 4, 0) == 0;
        ok = ok && write(e, &one, 8) == 8 && epoll_wait(ep, out, 4, 100) == 1 && out[0].data.u64 == 77;
        ok = ok && read(e, &v, 8) == 8 && v == 1 && epoll_wait(ep, out, 4, 0) == 0;
        CHECK(ok, "eventfd + epoll");
        close(e); close(ep);
    }

    /* 5: timerfd fires after its time */
    {
        int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        struct itimerspec its = { { 0, 0 }, { 0, 60 * 1000000L } };
        struct timespec a, b;
        uint64_t n = 0;
        int ok;
        clock_gettime(CLOCK_MONOTONIC, &a);
        ok = t >= 0 && timerfd_settime(t, 0, &its, 0) == 0 && read(t, &n, 8) == 8 && n == 1;
        clock_gettime(CLOCK_MONOTONIC, &b);
        {
            double dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
            CHECK(ok && dt > 0.05 && dt < 0.5, "timerfd 60 ms");
        }
        close(t);
    }

    /* 6: memfd shared with a child through MAP_SHARED */
    {
        int m = memfd_create("ipctest", MFD_CLOEXEC), st = -1;
        int *shared;
        pid_t pid;
        int ok = m >= 0 && ftruncate(m, 8192) == 0;
        shared = ok ? mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0) : MAP_FAILED;
        ok = shared != MAP_FAILED;
        if (ok) {
            shared[0] = 1;
            if ((pid = fork()) == 0) {
                int *again = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, m, 0);
                shared[1] = 41;
                if (again != MAP_FAILED) again[2] = 42;
                _exit(shared[0] == 1 ? 0 : 1);
            }
            waitpid(pid, &st, 0);
            ok = WIFEXITED(st) && WEXITSTATUS(st) == 0 && shared[1] == 41 && shared[2] == 42;
        }
        CHECK(ok, "memfd MAP_SHARED across fork");
    }

    /* 7: shared anonymous memory across fork */
    {
        volatile int *s = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        int st = -1;
        pid_t pid;
        int ok = s != MAP_FAILED;
        if (ok && (pid = fork()) == 0) {
            s[0] = 99;
            _exit(0);
        }
        if (ok) {
            waitpid(pid, &st, 0);
            ok = s[0] == 99;
        }
        CHECK(ok, "MAP_SHARED | MAP_ANONYMOUS across fork");
    }

    /* 8: the clock has sub-millisecond resolution */
    {
        struct timespec a, b;
        int distinct = 0;
        for (int i = 0; i < 1000 && !distinct; i++) {
            clock_gettime(CLOCK_MONOTONIC, &a);
            clock_gettime(CLOCK_MONOTONIC, &b);
            if (b.tv_nsec != a.tv_nsec && b.tv_sec == a.tv_sec && b.tv_nsec - a.tv_nsec < 1000000) distinct = 1;
        }
        CHECK(distinct, "fine-grained clock");
    }

    if (failures) printf("ipctest: %d FAILED\n", failures);
    else printf("ipctest: ALL PASS\n");
    return failures != 0;
}
