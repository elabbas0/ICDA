#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(ok, what) do { checks++; if (!(ok)) { failures++; printf("tcp-loopback: FAIL %s (errno %d)\n", what, errno); } else printf("tcp-loopback: PASS %s\n", what); } while (0)
int main(void) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(1455), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}, peer;
    socklen_t n = sizeof(peer);
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(listener >= 0 && bind(listener, (void *)&a, sizeof(a)) == 0, "bind OAuth callback port");
    CHECK(getsockname(listener, (void *)&peer, &n) == 0 && peer.sin_port == a.sin_port && peer.sin_addr.s_addr == a.sin_addr.s_addr, "bound address and port");
    CHECK(listen(listener, 2) == 0, "listen");
    int duplicate = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(bind(duplicate, (void *)&a, sizeof(a)) == -1 && errno == EADDRINUSE, "duplicate bind refused");
    close(duplicate);
    CHECK(accept4(listener, 0, 0, SOCK_NONBLOCK) == -1 && errno == EAGAIN, "empty nonblocking accept");
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.u64 = 1455};
    CHECK(ep >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, listener, &ev) == 0 && epoll_wait(ep, &ev, 1, 0) == 0, "listener initially not readable");
    int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, (void *)&a, sizeof(a)) == 0, "local connect");
    CHECK(epoll_wait(ep, &ev, 1, 1000) == 1 && ev.data.u64 == 1455 && (ev.events & EPOLLIN), "epoll callback readiness");
    n = sizeof(peer);
    int server = accept4(listener, (void *)&peer, &n, SOCK_NONBLOCK | SOCK_CLOEXEC);
    CHECK(server >= 0 && peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK) && peer.sin_port, "accept peer address");
    CHECK((fcntl(server, F_GETFL) & O_NONBLOCK) && (fcntl(server, F_GETFD) & FD_CLOEXEC), "accept descriptor flags");
    const char request[] = "GET /auth/callback?error=access_denied HTTP/1.1\r\nHost: localhost:1455\r\n\r\n";
    const char response[] = "HTTP/1.1 200 OK\r\n\r\nOK";
    char buf[256] = {0};
    CHECK(send(client, request, sizeof(request)-1, 0) == sizeof(request)-1 && recv(server, buf, sizeof(buf), 0) == sizeof(request)-1 && !strcmp(buf, request), "HTTP callback request");
    CHECK(send(server, response, sizeof(response)-1, 0) == sizeof(response)-1 && recv(client, buf, sizeof(buf), 0) == sizeof(response)-1 && !memcmp(buf, response, sizeof(response)-1), "HTTP callback response");
    CHECK(shutdown(client, SHUT_WR) == 0 && recv(server, buf, sizeof(buf), 0) == 0, "half-close EOF");
    CHECK(send(server, "after EOF", 9, 0) == 9 && recv(client, buf, sizeof(buf), 0) == 9, "half-close reverse direction");
    close(server); close(client); close(ep); close(listener);
    listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    CHECK(bind(listener, (void *)&a, sizeof(a)) == 0, "port reusable after close");
    CHECK(listen(listener, 1) == 0, "listen again");
    client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, (void *)&a, sizeof(a)) == 0, "pending connection");
    close(listener);
    int ended = recv(client, buf, sizeof(buf), 0);
    CHECK(ended == 0 || (ended == -1 && errno == ECONNRESET), "listener close cleans pending peer");
    close(client);
    client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(client, (void *)&a, sizeof(a)) == -1 && errno == ECONNREFUSED, "unused local port refused");
    close(client);
    printf("tcp-loopback: %d/%d passed\n", checks-failures, checks);
    return failures ? 1 : 0;
}
