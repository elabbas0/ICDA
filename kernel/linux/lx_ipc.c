/* Linux personality IPC and event files: Unix domain sockets (with
 * descriptor passing), eventfd, timerfd, epoll and memfd.  Multi-process
 * programs (WebKit's UI, web and network processes, GLib and Wayland event
 * loops) are built on these.
 *
 * Blocking calls wait the way the rest of the personality does: a tick's
 * sleep at a time, checking for signals. */
#include "lx_internal.h"
#include "../memory/heap.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../proc/sched.h"
#include "../syscall/uaccess.h"

#define EPERM        1
#define ENOENT       2
#define EINTR        4
#define EBADF        9
#define EAGAIN       11
#define ENOMEM       12
#define EFAULT       14
#define EINVAL       22
#define EMFILE       24
#define ENOTSOCK     88
#define EMSGSIZE     90
#define EPROTOTYPE   91
#define ENOPROTOOPT  92
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define EADDRINUSE   98
#define ENOTCONN     107
#define ECONNREFUSED 111
#define EPIPE        32
#define EISCONN      106

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  04000
#define SOCK_CLOEXEC   02000000
#define MSG_PEEK       0x2
#define MSG_TRUNC      0x20
#define MSG_DONTWAIT   0x40
#define MSG_NOSIGNAL   0x4000
#define MSG_CMSG_CLOEXEC 0x40000000
#define MSG_CTRUNC     0x8

static void *zalloc(uint64_t n) {
    uint8_t *p = (uint8_t *)kmalloc(n);
    if (p)
        for (uint64_t i = 0; i < n; i++) p[i] = 0;
    return p;
}

static void bcopy_k(void *d, const void *s, uint64_t n) {
    uint8_t *dd = (uint8_t *)d;
    const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
}

static int nonblock(lx_file_t *f, int flags) {
    return (f->flags & LX_O_NONBLOCK) || (flags & MSG_DONTWAIT);
}

/* ==== Unix domain sockets ===================================================== */

typedef struct chunk {
    struct chunk *next;
    uint64_t      len, off;
    lx_file_t   **fds;
    int           nfds;
    uint8_t       data[];
} chunk_t;

typedef struct usock {
    int            type;
    int            state;          /* 0 new, 1 listening, 2 connected, 3 peer gone */
    struct usock  *peer;
    chunk_t       *rx, *rx_tail;
    uint64_t       rx_bytes;
    int            shut_rd, shut_wr;
    char           name[108];      /* bound name; name[0] == 0 with name_len > 0: abstract */
    uint32_t       name_len;
    struct usock  *pending;        /* listening: connections waiting for accept */
    struct usock  *next_pending;
    int            backlog, npending;
    uint64_t       owner_pid;
    struct usock  *next_bound;
} usock_t;

#define USOCK_RX_MAX (4u << 20)

static usock_t *bound;             /* sockets with a name, for connect */

static const lx_fops_t usock_ops;

static usock_t *sock_of(int64_t fd, lx_file_t **fp) {
    lx_file_t *f = lxi_fd_file(fd);
    if (fp) *fp = f;
    if (!f || f->kind != LXF_OBJ || f->ops != &usock_ops) return 0;
    return (usock_t *)f->obj;
}

static void chunk_free(chunk_t *c) {
    for (int i = 0; i < c->nfds; i++) lxi_file_unref(c->fds[i]);
    if (c->fds) kfree(c->fds);
    kfree(c);
}

static void usock_release(void *obj) {
    usock_t *s = (usock_t *)obj;
    chunk_t *c = s->rx;
    if (s->peer) {
        s->peer->peer = 0;
        s->peer->state = 3;
    }
    while (c) {
        chunk_t *n = c->next;
        chunk_free(c);
        c = n;
    }
    for (usock_t **pp = &bound; *pp; pp = &(*pp)->next_bound)
        if (*pp == s) {
            *pp = s->next_bound;
            break;
        }
    while (s->pending) {   /* connections nobody accepted */
        usock_t *p = s->pending;
        s->pending = p->next_pending;
        usock_release(p);
        kfree(p);
    }
    kfree(s);
}

static int usock_ready(lx_file_t *f, int want_write) {
    usock_t *s = (usock_t *)f->obj;
    int r = 0;
    if (s->state == 1) return s->pending ? 1 : 0;
    if (s->rx_bytes || s->rx) r |= 1;
    if (s->state == 3 || s->shut_rd) r |= 1 | 0x10;
    if (want_write) {
        if (s->state == 2 && s->peer && s->peer->rx_bytes < USOCK_RX_MAX) r |= 4;
        if (s->state == 3) r |= 8;
    }
    return r;
}

/* queues len bytes from the user (plus files) on the peer */
static int64_t usock_send(lx_file_t *f, usock_t *s, const uint64_t *iov, uint64_t niov, lx_file_t **fds, int nfds,
                          int flags) {
    uint64_t total = 0, done = 0;
    chunk_t *c;
    for (uint64_t i = 0; i < niov; i++) total += iov[2 * i + 1];
    if (s->state != 2 && s->state != 3) return -ENOTCONN;
    for (;;) {
        if (s->state == 3 || !s->peer || s->shut_wr || s->peer->shut_rd) return -EPIPE;
        if (s->peer->rx_bytes + total <= USOCK_RX_MAX || s->peer->rx_bytes == 0) break;
        if (nonblock(f, flags)) return -EAGAIN;
        if (lxi_interrupted()) return -EINTR;
        sched_sleep(1);
    }
    c = (chunk_t *)kmalloc(sizeof(chunk_t) + total);
    if (!c) return -ENOMEM;
    c->next = 0;
    c->len = total;
    c->off = 0;
    c->fds = 0;
    c->nfds = 0;
    for (uint64_t i = 0; i < niov; i++) {
        uint64_t n = iov[2 * i + 1];
        if (n && copy_from_user(c->data + done, (const void *)iov[2 * i], n) != 0) {
            kfree(c);
            return -EFAULT;
        }
        done += n;
    }
    if (nfds) {
        c->fds = (lx_file_t **)kmalloc(sizeof(lx_file_t *) * (uint64_t)nfds);
        if (!c->fds) {
            kfree(c);
            return -ENOMEM;
        }
        for (int i = 0; i < nfds; i++) {
            c->fds[i] = fds[i];
            lxi_file_ref(fds[i]);
        }
        c->nfds = nfds;
    }
    if (s->peer->rx_tail) s->peer->rx_tail->next = c;
    else s->peer->rx = c;
    s->peer->rx_tail = c;
    s->peer->rx_bytes += total;
    return (int64_t)total;
}

/* takes queued bytes (and the files that came with them) */
static int64_t usock_recv(lx_file_t *f, usock_t *s, const uint64_t *iov, uint64_t niov, lx_file_t ***fds_out,
                          int *nfds_out, int flags, int *truncated) {
    uint64_t want = 0, got = 0, iov_i = 0, iov_off = 0;
    int stream = s->type == SOCK_STREAM;
    for (uint64_t i = 0; i < niov; i++) want += iov[2 * i + 1];
    *nfds_out = 0;
    *fds_out = 0;
    *truncated = 0;
    if (s->state != 2 && s->state != 3) return -ENOTCONN;
    while (!s->rx) {
        if (s->state == 3 || s->shut_rd) return 0;
        if (nonblock(f, flags)) return -EAGAIN;
        if (lxi_interrupted()) return -EINTR;
        sched_sleep(1);
    }
    while (s->rx && (got < want || (!want && !got))) {
        chunk_t *c = s->rx;
        uint64_t avail = c->len - c->off, take;
        if (got && c->nfds) break;                       /* files arrive with their own bytes */
        if (c->nfds && !*nfds_out) {
            *fds_out = c->fds;
            *nfds_out = c->nfds;
            if (!(flags & MSG_PEEK)) {
                c->fds = 0;
                c->nfds = 0;
            }
        }
        take = want - got < avail ? want - got : avail;
        while (take) {
            uint64_t room = iov[2 * iov_i + 1] - iov_off, n = take < room ? take : room;
            if (n && copy_to_user((uint8_t *)iov[2 * iov_i] + iov_off, c->data + c->off, n) != 0) return got ? (int64_t)got : -EFAULT;
            if (!(flags & MSG_PEEK)) c->off += n;
            got += n;
            take -= n;
            iov_off += n;
            if (iov_off == iov[2 * iov_i + 1]) {
                iov_i++;
                iov_off = 0;
            }
        }
        if (flags & MSG_PEEK) break;
        if (!stream && c->off < c->len) {                /* datagrams: the rest is lost */
            *truncated = 1;
            c->off = c->len;
        }
        if (c->off >= c->len) {
            s->rx = c->next;
            if (!s->rx) s->rx_tail = 0;
            s->rx_bytes -= c->len;
            chunk_free(c);
        }
        if (!stream) break;
        if (!want) break;
    }
    return (int64_t)got;
}

static int64_t usock_read(lx_file_t *f, char *ubuf, uint64_t count) {
    uint64_t iov[2] = { (uint64_t)ubuf, count };
    lx_file_t **fds;
    int nfds, trunc;
    int64_t r = usock_recv(f, (usock_t *)f->obj, iov, 1, &fds, &nfds, 0, &trunc);
    for (int i = 0; i < nfds; i++) lxi_file_unref(fds[i]);   /* read() cannot take files: they are closed */
    if (fds) kfree(fds);
    return r;
}

static int64_t usock_write(lx_file_t *f, const char *ubuf, uint64_t count) {
    uint64_t iov[2] = { (uint64_t)ubuf, count };
    return usock_send(f, (usock_t *)f->obj, iov, 1, 0, 0, 0);
}

static const lx_fops_t usock_ops = {
    "socket", usock_read, usock_write, usock_ready, usock_release, 0, 0, 0140000, 0
};

static usock_t *usock_new(int type) {
    usock_t *s = (usock_t *)zalloc(sizeof(usock_t));
    process_t *p = sched_current_process();
    if (!s) return 0;
    s->type = type;
    s->owner_pid = p ? p->pid : 0;
    return s;
}

static int64_t install_sock(usock_t *s, int type_flags) {
    lx_file_t *f = lxi_file_new(&usock_ops, s, 2 | ((type_flags & SOCK_NONBLOCK) ? LX_O_NONBLOCK : 0));
    int64_t fd;
    if (!f) {
        kfree(s);
        return -ENOMEM;
    }
    fd = lxi_fd_install(f, (type_flags & SOCK_CLOEXEC) != 0);
    if (fd < 0) lxi_file_unref(f);
    return fd;
}

/* sockets the personality does not have yet (AF_INET ...) are refused */
int64_t lxi_socket(int domain, int type, int proto) {
    int t = type & 0xF;
    usock_t *s;
    (void)proto;
    if (domain != 1) return -EAFNOSUPPORT;               /* AF_UNIX only for now */
    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_SEQPACKET) return -EPROTOTYPE;
    s = usock_new(t);
    if (!s) return -ENOMEM;
    return install_sock(s, type);
}

int64_t lxi_socketpair(int domain, int type, int proto, int32_t *usv) {
    int t = type & 0xF;
    usock_t *a, *b;
    int64_t fa, fb;
    int32_t sv[2];
    (void)proto;
    if (domain != 1) return -EAFNOSUPPORT;
    if (t != SOCK_STREAM && t != SOCK_DGRAM && t != SOCK_SEQPACKET) return -EPROTOTYPE;
    a = usock_new(t);
    b = usock_new(t);
    if (!a || !b) {
        if (a) kfree(a);
        if (b) kfree(b);
        return -ENOMEM;
    }
    a->peer = b;
    b->peer = a;
    a->state = b->state = 2;
    fa = install_sock(a, type);
    if (fa < 0) {
        kfree(b);
        return fa;
    }
    fb = install_sock(b, type);
    if (fb < 0) return fb;
    sv[0] = (int32_t)fa;
    sv[1] = (int32_t)fb;
    return copy_to_user(usv, sv, sizeof(sv)) ? -EFAULT : 0;
}

/* sockaddr_un -> name / name_len (path names and abstract names alike) */
static int64_t read_name(const void *uaddr, uint64_t len, char *name, uint32_t *name_len) {
    uint8_t buf[110];
    uint32_t n;
    if (len < 3 || len > sizeof(buf)) return -EINVAL;
    if (copy_from_user(buf, uaddr, len) != 0) return -EFAULT;
    if (*(uint16_t *)buf != 1) return -EAFNOSUPPORT;
    n = (uint32_t)len - 2;
    if (buf[2]) {                                      /* a path: up to its terminator */
        uint32_t k = 0;
        while (k < n && buf[2 + k]) k++;
        n = k;
    }
    bcopy_k(name, buf + 2, n);
    *name_len = n;
    return 0;
}

static usock_t *find_bound(const char *name, uint32_t len) {
    for (usock_t *s = bound; s; s = s->next_bound) {
        int same = s->name_len == len;
        for (uint32_t i = 0; same && i < len; i++) same = s->name[i] == name[i];
        if (same) return s;
    }
    return 0;
}

int64_t lxi_bind(int64_t fd, const void *uaddr, uint64_t len) {
    usock_t *s = sock_of(fd, 0);
    int64_t r;
    if (!s) return lxi_fd_file(fd) ? -ENOTSOCK : -EBADF;
    if (s->name_len) return -EINVAL;
    r = read_name(uaddr, len, s->name, &s->name_len);
    if (r < 0) return r;
    if (find_bound(s->name, s->name_len)) {
        s->name_len = 0;
        return -EADDRINUSE;
    }
    s->next_bound = bound;
    bound = s;
    return 0;
}

int64_t lxi_listen(int64_t fd, int backlog) {
    usock_t *s = sock_of(fd, 0);
    if (!s) return lxi_fd_file(fd) ? -ENOTSOCK : -EBADF;
    if (s->type == SOCK_DGRAM) return -EOPNOTSUPP;
    s->state = 1;
    s->backlog = backlog > 0 ? backlog : 16;
    return 0;
}

int64_t lxi_connect(int64_t fd, const void *uaddr, uint64_t len) {
    usock_t *s = sock_of(fd, 0), *l, *server;
    char name[108];
    uint32_t nlen;
    int64_t r;
    if (!s) return lxi_fd_file(fd) ? -ENOTSOCK : -EBADF;
    if (s->state == 2) return -EISCONN;
    r = read_name(uaddr, len, name, &nlen);
    if (r < 0) return r;
    l = find_bound(name, nlen);
    if (!l || l->state != 1) return l ? -ECONNREFUSED : -ENOENT;
    if (l->npending >= l->backlog + 4) return -EAGAIN;
    server = usock_new(s->type);
    if (!server) return -ENOMEM;
    bcopy_k(server->name, l->name, l->name_len);
    server->name_len = l->name_len;
    server->peer = s;
    s->peer = server;
    server->state = s->state = 2;
    server->next_pending = 0;
    {
        usock_t **pp = &l->pending;
        while (*pp) pp = &(*pp)->next_pending;
        *pp = server;
    }
    l->npending++;
    return 0;
}

static int64_t put_name(const usock_t *s, void *uaddr, uint32_t *ulen) {
    uint8_t buf[110];
    uint32_t have, n = 2 + s->name_len + (s->name_len && s->name[0] ? 1 : 0);
    if (!uaddr || !ulen) return 0;
    if (copy_from_user(&have, ulen, 4) != 0) return -EFAULT;
    *(uint16_t *)buf = 1;
    bcopy_k(buf + 2, s->name, s->name_len);
    if (s->name_len && s->name[0]) buf[2 + s->name_len] = 0;
    if (copy_to_user(uaddr, buf, have < n ? have : n) != 0) return -EFAULT;
    return copy_to_user(ulen, &n, 4) ? -EFAULT : 0;
}

int64_t lxi_accept(int64_t fd, void *uaddr, uint32_t *ulen, int flags) {
    lx_file_t *f;
    usock_t *l = sock_of(fd, &f), *c;
    if (!l) return f ? -ENOTSOCK : -EBADF;
    if (l->state != 1) return -EINVAL;
    while (!l->pending) {
        if (f->flags & LX_O_NONBLOCK) return -EAGAIN;
        if (lxi_interrupted()) return -EINTR;
        sched_sleep(1);
    }
    c = l->pending;
    l->pending = c->next_pending;
    l->npending--;
    if (uaddr && ulen) {
        uint32_t two = 2;
        uint16_t fam = 1;
        if (copy_to_user(uaddr, &fam, 2) || copy_to_user(ulen, &two, 4)) return -EFAULT;
    }
    return install_sock(c, flags);
}

/* struct msghdr: name, namelen, iov, iovlen, control, controllen, flags */
typedef struct {
    uint64_t name;
    uint32_t namelen, pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    int32_t  flags, pad1;
} lx_msghdr_t;

#define IOV_MAX_K 64

int64_t lxi_sendmsg(int64_t fd, const void *umsg, int flags) {
    lx_file_t *f, *files[64];
    usock_t *s = sock_of(fd, &f);
    lx_msghdr_t m;
    uint64_t iov[2 * IOV_MAX_K];
    int nfiles = 0;
    int64_t r;
    if (!s) return f ? -ENOTSOCK : -EBADF;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) return -EFAULT;
    if (m.iovlen > IOV_MAX_K) return -EMSGSIZE;
    if (m.iovlen && copy_from_user(iov, (const void *)m.iov, m.iovlen * 16) != 0) return -EFAULT;
    if (m.control && m.controllen >= 16) {
        uint8_t ctl[512];
        uint64_t off = 0, clen = m.controllen < sizeof(ctl) ? m.controllen : sizeof(ctl);
        if (copy_from_user(ctl, (const void *)m.control, clen) != 0) return -EFAULT;
        while (off + 16 <= clen) {
            uint64_t len = *(uint64_t *)(ctl + off);
            int level = *(int32_t *)(ctl + off + 8), type = *(int32_t *)(ctl + off + 12);
            if (len < 16 || off + len > clen) break;
            if (level == 1 && type == 1) {               /* SOL_SOCKET, SCM_RIGHTS */
                for (uint64_t i = 16; i + 4 <= len && nfiles < 64; i += 4) {
                    lx_file_t *x = lxi_fd_file(*(int32_t *)(ctl + off + i));
                    if (!x) return -EBADF;
                    files[nfiles++] = x;
                }
            }
            off += (len + 7) & ~7ULL;
        }
    }
    r = usock_send(f, s, iov, m.iovlen, files, nfiles, flags);
    return r;
}

int64_t lxi_recvmsg(int64_t fd, void *umsg, int flags) {
    lx_file_t *f, **fds;
    usock_t *s = sock_of(fd, &f);
    lx_msghdr_t m;
    uint64_t iov[2 * IOV_MAX_K];
    int nfds, trunc, out_flags = 0;
    int64_t r;
    if (!s) return f ? -ENOTSOCK : -EBADF;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) return -EFAULT;
    if (m.iovlen > IOV_MAX_K) return -EMSGSIZE;
    if (m.iovlen && copy_from_user(iov, (const void *)m.iov, m.iovlen * 16) != 0) return -EFAULT;
    r = usock_recv(f, s, iov, m.iovlen, &fds, &nfds, flags, &trunc);
    if (r < 0) return r;
    if (trunc) out_flags |= MSG_TRUNC;
    {
        uint64_t used = 0;
        if (nfds) {
            uint8_t ctl[16 + 4 * 64];
            uint64_t room = m.controllen >= 16 ? (m.controllen - 16) / 4 : 0;
            int give = nfds < (int)room ? nfds : (int)room;
            if (give < nfds) out_flags |= MSG_CTRUNC;
            for (int i = 0; i < nfds; i++) {
                if (i < give && !(flags & MSG_PEEK)) {
                    int64_t nfd = lxi_fd_install(fds[i], (flags & MSG_CMSG_CLOEXEC) != 0);
                    if (nfd < 0) {
                        lxi_file_unref(fds[i]);
                        nfd = -1;
                    }
                    *(int32_t *)(ctl + 16 + 4 * i) = (int32_t)nfd;
                } else if (!(flags & MSG_PEEK)) {
                    lxi_file_unref(fds[i]);
                }
            }
            if (give) {
                *(uint64_t *)ctl = 16 + 4 * (uint64_t)give;
                *(int32_t *)(ctl + 8) = 1;
                *(int32_t *)(ctl + 12) = 1;
                used = (16 + 4 * (uint64_t)give + 7) & ~7ULL;
                if (used > m.controllen) used = m.controllen;
                if (copy_to_user((void *)m.control, ctl, 16 + 4 * (uint64_t)give) != 0) return -EFAULT;
            }
            if (fds && !(flags & MSG_PEEK)) kfree(fds);
        }
        m.controllen = used;
        m.flags = out_flags;
        m.namelen = 0;
        if (copy_to_user((uint8_t *)umsg + 40, &m.controllen, 8) || copy_to_user((uint8_t *)umsg + 48, &m.flags, 4) ||
            copy_to_user((uint8_t *)umsg + 8, &m.namelen, 4))
            return -EFAULT;
    }
    return r;
}

int64_t lxi_sendto(int64_t fd, const void *ubuf, uint64_t len, int flags, const void *uaddr, uint64_t alen) {
    lx_file_t *f;
    usock_t *s = sock_of(fd, &f);
    uint64_t iov[2] = { (uint64_t)ubuf, len };
    (void)uaddr; (void)alen;
    if (!s) return f ? -ENOTSOCK : -EBADF;
    return usock_send(f, s, iov, 1, 0, 0, flags);
}

int64_t lxi_recvfrom(int64_t fd, void *ubuf, uint64_t len, int flags, void *uaddr, uint32_t *ualen) {
    lx_file_t *f, **fds;
    usock_t *s = sock_of(fd, &f);
    uint64_t iov[2] = { (uint64_t)ubuf, len };
    int nfds, trunc;
    int64_t r;
    if (!s) return f ? -ENOTSOCK : -EBADF;
    r = usock_recv(f, s, iov, 1, &fds, &nfds, flags, &trunc);
    if (!(flags & MSG_PEEK)) {
        for (int i = 0; i < nfds; i++) lxi_file_unref(fds[i]);
        if (fds) kfree(fds);
    }
    if (r >= 0 && uaddr && ualen) {
        uint32_t zero = 0;
        (void)copy_to_user(ualen, &zero, 4);
    }
    return r;
}

int64_t lxi_shutdown(int64_t fd, int how) {
    usock_t *s = sock_of(fd, 0);
    if (!s) return lxi_fd_file(fd) ? -ENOTSOCK : -EBADF;
    if (how == 0 || how == 2) s->shut_rd = 1;
    if (how == 1 || how == 2) {
        s->shut_wr = 1;
        if (s->peer) s->peer->shut_rd = 1;
    }
    return 0;
}

int64_t lxi_getsockopt(int64_t fd, int level, int opt, void *uval, uint32_t *ulen) {
    usock_t *s = sock_of(fd, 0);
    uint32_t have, n = 4;
    int32_t v[3] = { 0, 0, 0 };
    if (!s) return lxi_fd_file(fd) ? -ENOTSOCK : -EBADF;
    if (copy_from_user(&have, ulen, 4) != 0) return -EFAULT;
    if (level != 1) return -ENOPROTOOPT;
    switch (opt) {
    case 3: v[0] = s->type; break;                       /* SO_TYPE */
    case 4: v[0] = 0; break;                             /* SO_ERROR */
    case 7: case 8: v[0] = 212992; break;                /* SO_SNDBUF, SO_RCVBUF */
    case 39: v[0] = 1; break;                            /* SO_DOMAIN */
    case 30: v[0] = s->state == 1; break;                /* SO_ACCEPTCONN */
    case 17: {                                           /* SO_PEERCRED: pid, uid, gid */
        process_t *p = sched_current_process();
        v[0] = (int32_t)(s->peer ? s->peer->owner_pid : (p ? p->pid : 0));
        n = 12;
        break;
    }
    default: v[0] = 0; break;
    }
    if (have < n) n = have;
    if (copy_to_user(uval, v, n) || copy_to_user(ulen, &n, 4)) return -EFAULT;
    return 0;
}

int64_t lxi_setsockopt(int64_t fd, int level, int opt, const void *uval, uint64_t len) {
    (void)level; (void)opt; (void)uval; (void)len;
    if (!sock_of(fd, 0)) return lxi_fd_file(fd) ? -ENOTSOCK : -EBADF;
    return 0;
}

int64_t lxi_getsockname(int64_t fd, void *uaddr, uint32_t *ulen, int peer) {
    usock_t *s = sock_of(fd, 0);
    if (!s) return lxi_fd_file(fd) ? -ENOTSOCK : -EBADF;
    if (peer) {
        if (!s->peer) return -ENOTCONN;
        return put_name(s->peer, uaddr, ulen);
    }
    return put_name(s, uaddr, ulen);
}

/* ==== eventfd ======================================================================= */

typedef struct {
    uint64_t count;
    int      semaphore;
} evfd_t;

static int64_t evfd_read(lx_file_t *f, char *ubuf, uint64_t count) {
    evfd_t *e = (evfd_t *)f->obj;
    uint64_t v;
    if (count < 8) return -EINVAL;
    while (!e->count) {
        if (f->flags & LX_O_NONBLOCK) return -EAGAIN;
        if (lxi_interrupted()) return -EINTR;
        sched_sleep(1);
    }
    v = e->semaphore ? 1 : e->count;
    e->count -= v;
    return copy_to_user(ubuf, &v, 8) ? -EFAULT : 8;
}

static int64_t evfd_write(lx_file_t *f, const char *ubuf, uint64_t count) {
    evfd_t *e = (evfd_t *)f->obj;
    uint64_t v;
    if (count < 8) return -EINVAL;
    if (copy_from_user(&v, ubuf, 8) != 0) return -EFAULT;
    if (v == ~0ULL) return -EINVAL;
    while (e->count + v < e->count || e->count + v == ~0ULL) {
        if (f->flags & LX_O_NONBLOCK) return -EAGAIN;
        if (lxi_interrupted()) return -EINTR;
        sched_sleep(1);
    }
    e->count += v;
    return 8;
}

static int evfd_ready(lx_file_t *f, int want_write) {
    evfd_t *e = (evfd_t *)f->obj;
    return (e->count ? 1 : 0) | (want_write && e->count < ~0ULL - 1 ? 4 : 0);
}

static void free_obj(void *obj) { kfree(obj); }

static const lx_fops_t evfd_ops = { "anon_inode:[eventfd]", evfd_read, evfd_write, evfd_ready, free_obj, 0, 0, 0, 0 };

int64_t lxi_eventfd(uint64_t initval, int flags) {
    evfd_t *e = (evfd_t *)zalloc(sizeof(evfd_t));
    lx_file_t *f;
    int64_t fd;
    if (!e) return -ENOMEM;
    e->count = (uint32_t)initval;
    e->semaphore = (flags & 1) != 0;                     /* EFD_SEMAPHORE */
    f = lxi_file_new(&evfd_ops, e, 2 | ((flags & 04000) ? LX_O_NONBLOCK : 0));
    if (!f) {
        kfree(e);
        return -ENOMEM;
    }
    fd = lxi_fd_install(f, (flags & 02000000) != 0);
    if (fd < 0) lxi_file_unref(f);
    return fd;
}

/* ==== timerfd ======================================================================= */

typedef struct {
    int      realtime;
    uint64_t next_ns;      /* 0: disarmed (monotonic / realtime as the clock) */
    uint64_t interval_ns;
} tfd_t;

static uint64_t tfd_now(const tfd_t *t) {
    return t->realtime ? lxi_realtime_ns() : lxi_now_ns();
}

/* expirations so far (and moves the timer on) */
static uint64_t tfd_take(tfd_t *t) {
    uint64_t now = tfd_now(t), n;
    if (!t->next_ns || now < t->next_ns) return 0;
    if (!t->interval_ns) {
        t->next_ns = 0;
        return 1;
    }
    n = (now - t->next_ns) / t->interval_ns + 1;
    t->next_ns += n * t->interval_ns;
    return n;
}

static int64_t tfd_read(lx_file_t *f, char *ubuf, uint64_t count) {
    tfd_t *t = (tfd_t *)f->obj;
    uint64_t n;
    if (count < 8) return -EINVAL;
    while (!(n = tfd_take(t))) {
        if (f->flags & LX_O_NONBLOCK) return -EAGAIN;
        if (lxi_interrupted()) return -EINTR;
        sched_sleep(1);
    }
    return copy_to_user(ubuf, &n, 8) ? -EFAULT : 8;
}

static int tfd_ready(lx_file_t *f, int want_write) {
    tfd_t *t = (tfd_t *)f->obj;
    (void)want_write;
    return t->next_ns && tfd_now(t) >= t->next_ns ? 1 : 0;
}

static const lx_fops_t tfd_ops = { "anon_inode:[timerfd]", tfd_read, 0, tfd_ready, free_obj, 0, 0, 0, 0 };

int64_t lxi_timerfd_create(int clockid, int flags) {
    tfd_t *t = (tfd_t *)zalloc(sizeof(tfd_t));
    lx_file_t *f;
    int64_t fd;
    if (!t) return -ENOMEM;
    t->realtime = clockid == 0 || clockid == 8;          /* CLOCK_REALTIME(_ALARM) */
    f = lxi_file_new(&tfd_ops, t, ((flags & 04000) ? LX_O_NONBLOCK : 0));
    if (!f) {
        kfree(t);
        return -ENOMEM;
    }
    fd = lxi_fd_install(f, (flags & 02000000) != 0);
    if (fd < 0) lxi_file_unref(f);
    return fd;
}

static tfd_t *tfd_of(int64_t fd) {
    lx_file_t *f = lxi_fd_file(fd);
    return f && f->kind == LXF_OBJ && f->ops == &tfd_ops ? (tfd_t *)f->obj : 0;
}

static void ns_to_ts(uint64_t ns, uint64_t *ts) {
    ts[0] = ns / 1000000000ULL;
    ts[1] = ns % 1000000000ULL;
}

int64_t lxi_timerfd_gettime(int64_t fd, void *ucur) {
    tfd_t *t = tfd_of(fd);
    uint64_t v[4] = { 0, 0, 0, 0 }, now;
    if (!t) return -EINVAL;
    now = tfd_now(t);
    ns_to_ts(t->interval_ns, v);
    if (t->next_ns) ns_to_ts(t->next_ns > now ? t->next_ns - now : 1, v + 2);
    return copy_to_user(ucur, v, sizeof(v)) ? -EFAULT : 0;
}

int64_t lxi_timerfd_settime(int64_t fd, int flags, const void *unew, void *uold) {
    tfd_t *t = tfd_of(fd);
    uint64_t v[4], first;
    if (!t) return -EINVAL;
    if (uold && lxi_timerfd_gettime(fd, uold) < 0) return -EFAULT;
    if (copy_from_user(v, unew, sizeof(v)) != 0) return -EFAULT;
    t->interval_ns = v[0] * 1000000000ULL + v[1];
    first = v[2] * 1000000000ULL + v[3];
    if (!first) t->next_ns = 0;
    else if (flags & 1) t->next_ns = first;              /* TFD_TIMER_ABSTIME */
    else t->next_ns = tfd_now(t) + first;
    return 0;
}

/* ==== epoll ========================================================================== */

typedef struct {
    int32_t    fd;
    lx_file_t *file;        /* not a reference: an entry goes when the descriptor is closed */
    uint32_t   events;
    uint64_t   data;
    int        was_ready;   /* EPOLLET: report only when it becomes ready */
    int        disabled;    /* EPOLLONESHOT after a report */
} epent_t;

typedef struct {
    epent_t *e;
    int      n, cap;
} epoll_t;

#define EPOLLIN      0x001
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDHUP   0x2000
#define EPOLLONESHOT (1u << 30)
#define EPOLLET      (1u << 31)

static void epoll_release(void *obj) {
    epoll_t *ep = (epoll_t *)obj;
    if (ep->e) kfree(ep->e);
    kfree(ep);
}

/* entries whose descriptor is gone (closed, or now another file) drop out */
static void epoll_prune(epoll_t *ep) {
    for (int i = 0; i < ep->n;) {
        if (lxi_fd_file(ep->e[i].fd) != ep->e[i].file) {
            ep->e[i] = ep->e[--ep->n];
            continue;
        }
        i++;
    }
}

static uint32_t ep_poll_bits(epent_t *x) {
    int r = lxi_file_ready(x->file, (x->events & EPOLLOUT) != 0);
    uint32_t got = 0;
    if (r & 1) got |= EPOLLIN;
    if (r & 4) got |= EPOLLOUT;
    if (r & 8) got |= EPOLLERR;
    if (r & 0x10) got |= EPOLLHUP | (x->events & EPOLLRDHUP);
    return got & (x->events | EPOLLERR | EPOLLHUP);
}

static int epoll_ready(lx_file_t *f, int want_write) {
    epoll_t *ep = (epoll_t *)f->obj;
    (void)want_write;
    epoll_prune(ep);
    for (int i = 0; i < ep->n; i++)
        if (!ep->e[i].disabled && ep_poll_bits(&ep->e[i])) return 1;
    return 0;
}

static const lx_fops_t epoll_ops = { "anon_inode:[eventpoll]", 0, 0, epoll_ready, epoll_release, 0, 0, 0, 0 };

int64_t lxi_epoll_create(int flags) {
    epoll_t *ep = (epoll_t *)zalloc(sizeof(epoll_t));
    lx_file_t *f;
    int64_t fd;
    if (!ep) return -ENOMEM;
    f = lxi_file_new(&epoll_ops, ep, 0);
    if (!f) {
        kfree(ep);
        return -ENOMEM;
    }
    fd = lxi_fd_install(f, (flags & 02000000) != 0);
    if (fd < 0) lxi_file_unref(f);
    return fd;
}

int64_t lxi_epoll_ctl(int64_t epfd, int op, int64_t fd, const void *uev) {
    lx_file_t *ef = lxi_fd_file(epfd), *tf = lxi_fd_file(fd);
    epoll_t *ep;
    uint8_t ev[12];
    int at = -1;
    if (!ef || !tf) return -EBADF;
    if (ef->kind != LXF_OBJ || ef->ops != &epoll_ops || ef == tf) return -EINVAL;
    ep = (epoll_t *)ef->obj;
    epoll_prune(ep);
    for (int i = 0; i < ep->n; i++)
        if (ep->e[i].fd == fd) at = i;
    if (op != 2 && copy_from_user(ev, uev, 12) != 0) return -EFAULT;
    switch (op) {
    case 1:                                              /* EPOLL_CTL_ADD */
        if (at >= 0) return -17;                         /* EEXIST */
        if (ep->n == ep->cap) {
            int nc = ep->cap ? ep->cap * 2 : 16;
            epent_t *ne = (epent_t *)kmalloc(sizeof(epent_t) * (uint64_t)nc);
            if (!ne) return -ENOMEM;
            if (ep->e) {
                bcopy_k(ne, ep->e, sizeof(epent_t) * (uint64_t)ep->n);
                kfree(ep->e);
            }
            ep->e = ne;
            ep->cap = nc;
        }
        at = ep->n++;
        ep->e[at].fd = (int32_t)fd;
        ep->e[at].file = tf;
        /* fall through */
    case 3:                                              /* EPOLL_CTL_MOD */
        if (at < 0) return -ENOENT;
        ep->e[at].events = *(uint32_t *)ev;
        ep->e[at].data = *(uint64_t *)(ev + 4);
        ep->e[at].was_ready = 0;
        ep->e[at].disabled = 0;
        return 0;
    case 2:                                              /* EPOLL_CTL_DEL */
        if (at < 0) return -ENOENT;
        ep->e[at] = ep->e[--ep->n];
        return 0;
    }
    return -EINVAL;
}

int64_t lxi_epoll_wait(int64_t epfd, void *uevs, int maxevents, int64_t timeout_ms) {
    lx_file_t *ef = lxi_fd_file(epfd);
    epoll_t *ep;
    uint64_t start = sched_ticks();
    if (!ef) return -EBADF;
    if (ef->kind != LXF_OBJ || ef->ops != &epoll_ops || maxevents <= 0) return -EINVAL;
    ep = (epoll_t *)ef->obj;
    for (;;) {
        int got = 0;
        epoll_prune(ep);
        for (int i = 0; i < ep->n && got < maxevents; i++) {
            epent_t *x = &ep->e[i];
            uint32_t bits;
            uint8_t out[12];
            if (x->disabled) continue;
            bits = ep_poll_bits(x);
            if (!bits) {
                x->was_ready = 0;
                continue;
            }
            if ((x->events & EPOLLET) && x->was_ready) continue;
            x->was_ready = 1;
            if (x->events & EPOLLONESHOT) x->disabled = 1;
            *(uint32_t *)out = bits;
            *(uint64_t *)(out + 4) = x->data;
            if (copy_to_user((uint8_t *)uevs + 12 * got, out, 12) != 0) return got ? got : -EFAULT;
            got++;
        }
        if (got) return got;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && (sched_ticks() - start) * 10 >= (uint64_t)timeout_ms) return 0;
        if (lxi_interrupted()) return -EINTR;
        sched_sleep(1);
    }
}

/* ==== memfd: memory files (shared when mapped MAP_SHARED) ============================== */

typedef struct {
    uint64_t  size;
    uint64_t *frames;       /* physical frames, 0 until used */
    uint64_t  nframes;      /* entries in frames */
} memobj_t;

static int mem_grow(memobj_t *m, uint64_t pages) {
    uint64_t *nf;
    if (pages <= m->nframes) return 0;
    nf = (uint64_t *)zalloc(pages * 8);
    if (!nf) return -1;
    if (m->frames) {
        bcopy_k(nf, m->frames, m->nframes * 8);
        kfree(m->frames);
    }
    m->frames = nf;
    m->nframes = pages;
    return 0;
}

static uint64_t mem_frame(void *obj, uint64_t off) {
    memobj_t *m = (memobj_t *)obj;
    uint64_t idx = off / PAGE_SIZE_4K;
    if (off >= ((m->size + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1))) return 0;
    if (mem_grow(m, idx + 1) != 0) return 0;
    if (!m->frames[idx]) {
        uint64_t phys = pmm_alloc();
        uint64_t *z;
        if (!phys) return 0;
        z = (uint64_t *)PHYS_TO_VIRT(phys);
        for (int i = 0; i < 512; i++) z[i] = 0;
        m->frames[idx] = phys;
    }
    return m->frames[idx];
}

static void mem_release(void *obj) {
    memobj_t *m = (memobj_t *)obj;
    for (uint64_t i = 0; i < m->nframes; i++)
        if (m->frames[i]) pmm_free(m->frames[i]);   /* mappings hold their own references */
    if (m->frames) kfree(m->frames);
    kfree(m);
}

static uint64_t mem_size(void *obj) { return ((memobj_t *)obj)->size; }

static int64_t mem_truncate(void *obj, uint64_t len) {
    memobj_t *m = (memobj_t *)obj;
    uint64_t keep = (len + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (uint64_t i = keep; i < m->nframes; i++)
        if (m->frames[i]) {
            pmm_free(m->frames[i]);
            m->frames[i] = 0;
        }
    if (len % PAGE_SIZE_4K && keep - 1 < m->nframes && m->frames[keep - 1]) {   /* clear past the end */
        uint8_t *p = (uint8_t *)PHYS_TO_VIRT(m->frames[keep - 1]);
        for (uint64_t i = len % PAGE_SIZE_4K; i < PAGE_SIZE_4K; i++) p[i] = 0;
    }
    m->size = len;
    return 0;
}

static int64_t mem_rw(lx_file_t *f, char *ubuf, uint64_t count, int write) {
    memobj_t *m = (memobj_t *)f->obj;
    uint64_t done = 0, off = f->off;
    if (!write) {
        if (off >= m->size) return 0;
        if (count > m->size - off) count = m->size - off;
    } else if (off + count > m->size) {
        m->size = off + count;
    }
    while (done < count) {
        uint64_t phys = mem_frame(m, off + done), in = (off + done) % PAGE_SIZE_4K;
        uint64_t n = PAGE_SIZE_4K - in;
        uint8_t *p;
        if (!phys) return done ? (int64_t)done : -ENOMEM;
        if (n > count - done) n = count - done;
        p = (uint8_t *)PHYS_TO_VIRT(phys) + in;
        if (write ? copy_from_user(p, ubuf + done, n) : copy_to_user(ubuf + done, p, n)) return done ? (int64_t)done : -EFAULT;
        done += n;
    }
    f->off = off + done;
    return (int64_t)done;
}

static int64_t mem_read(lx_file_t *f, char *ubuf, uint64_t count) { return mem_rw(f, ubuf, count, 0); }
static int64_t mem_write(lx_file_t *f, const char *ubuf, uint64_t count) { return mem_rw(f, (char *)ubuf, count, 1); }
static int mem_ready(lx_file_t *f, int want_write) { (void)f; return 1 | (want_write ? 4 : 0); }

static const lx_fops_t mem_ops = { "memfd", mem_read, mem_write, mem_ready, mem_release, mem_size, mem_truncate, 0100000, mem_frame };

int64_t lxi_memfd_create(const char *uname, unsigned flags) {
    memobj_t *m = (memobj_t *)zalloc(sizeof(memobj_t));
    lx_file_t *f;
    int64_t fd;
    (void)uname;
    if (!m) return -ENOMEM;
    f = lxi_file_new(&mem_ops, m, 2);
    if (!f) {
        kfree(m);
        return -ENOMEM;
    }
    fd = lxi_fd_install(f, (flags & 1) != 0);              /* MFD_CLOEXEC */
    if (fd < 0) lxi_file_unref(f);
    return fd;
}

/* MAP_SHARED | MAP_ANONYMOUS: a nameless memory object behind the mapping,
 * so a fork's child shares the pages */
lx_file_t *lxi_shared_anon(uint64_t len) {
    memobj_t *m = (memobj_t *)zalloc(sizeof(memobj_t));
    lx_file_t *f;
    if (!m) return 0;
    m->size = len;
    f = lxi_file_new(&mem_ops, m, 2);
    if (!f) kfree(m);
    return f;
}
