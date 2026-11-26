/* ws2_32 (Winsock) on Unix sockets.
 *
 * A SOCKET is the descriptor.  What differs is translated: AF_INET6 is 23
 * on Windows, the socket-level options have other numbers, fd_set is a
 * list, timeval and hostent have 32-bit longs and shorts, addrinfo has its
 * fields in another order, and errors are WSA codes from WSAGetLastError. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef uintptr_t SOCKET;
#define INVALID_SOCKET ((SOCKET)~0ull)
#define SOCKET_ERROR (-1)
#define WAF_INET6 23

static __thread int wsa_error;

static int wsa_of(int e) {
    switch (e) {
    case EWOULDBLOCK: return 10035;
    /* a non-blocking connect under way: Windows says WSAEWOULDBLOCK (its
     * WSAEINPROGRESS is Winsock 1's blocking call in progress) */
    case EINPROGRESS: return 10035;
    case EALREADY: return 10037;
    case ENOTSOCK: case EBADF: return 10038;
    case EDESTADDRREQ: return 10039;
    case EMSGSIZE: return 10040;
    case EPROTOTYPE: return 10041;
    case ENOPROTOOPT: return 10042;
    case EPROTONOSUPPORT: return 10043;
    case EOPNOTSUPP: return 10045;
    case EAFNOSUPPORT: return 10047;
    case EADDRINUSE: return 10048;
    case EADDRNOTAVAIL: return 10049;
    case ENETDOWN: return 10050;
    case ENETUNREACH: return 10051;
    case ECONNABORTED: return 10053;
    case ECONNRESET: return 10054;
    case ENOBUFS: return 10055;
    case EISCONN: return 10056;
    case ENOTCONN: return 10057;
    case ETIMEDOUT: return 10060;
    case ECONNREFUSED: return 10061;
    case EHOSTUNREACH: return 10065;
    case EINTR: return 10004;
    case EACCES: return 10013;
    case EFAULT: return 10014;
    case EINVAL: return 10022;
    case EMFILE: return 10024;
    default: return 10022;
    }
}

static int fail(void) {
    wsa_error = wsa_of(errno);
    return SOCKET_ERROR;
}

/* addresses: only the family field differs */
static socklen_t addr_in(const void *w, int len, struct sockaddr_storage *u) {
    if (!w || len <= 0) return 0;
    if ((size_t)len > sizeof(*u)) len = sizeof(*u);
    memcpy(u, w, (size_t)len);
    if (u->ss_family == WAF_INET6) u->ss_family = AF_INET6;
    return (socklen_t)len;
}

static void addr_out(const struct sockaddr_storage *u, socklen_t ulen, void *w, int *wlen) {
    socklen_t n;
    if (!w || !wlen) return;
    n = ulen < (socklen_t)*wlen ? ulen : (socklen_t)*wlen;
    memcpy(w, u, n);
    if (n >= 2 && u->ss_family == AF_INET6) *(uint16_t *)w = WAF_INET6;
    *wlen = (int)ulen;
}

static int af_in(int af) { return af == WAF_INET6 ? AF_INET6 : af; }

typedef struct {
    WORD  wVersion, wHighVersion;
    USHORT iMaxSockets, iMaxUdpDg;
    char *lpVendorInfo;
    char  szDescription[257], szSystemStatus[129];
} WSADATA;

static int WINAPI WSAStartup(WORD ver, WSADATA *d) {
    if (d) {
        memset(d, 0, sizeof(*d));
        d->wVersion = ver;
        d->wHighVersion = 0x0202;
        snprintf(d->szDescription, sizeof(d->szDescription), "WinSock 2.0");
        snprintf(d->szSystemStatus, sizeof(d->szSystemStatus), "Running");
    }
    return 0;
}
static int WINAPI WSACleanup(void) { return 0; }
static int WINAPI WSAGetLastError(void) { return wsa_error; }
static void WINAPI WSASetLastError(int e) { wsa_error = e; }

static SOCKET WINAPI w_socket(int af, int type, int proto) {
    int s = socket(af_in(af), type | SOCK_CLOEXEC, proto);
    if (s < 0) {
        fail();
        return INVALID_SOCKET;
    }
    return (SOCKET)s;
}

static SOCKET WINAPI WSASocketW(int af, int type, int proto, void *info, unsigned g, DWORD flags) {
    (void)info, (void)g, (void)flags;
    return w_socket(af, type, proto);
}

/* WSAPROTOCOL_INFOA differs from the W struct only in its last field, the
 * protocol's name, which WSASocketW does not read */
static SOCKET WINAPI WSASocketA(int af, int type, int proto, void *info, unsigned g, DWORD flags) {
    return WSASocketW(af, type, proto, info, g, flags);
}

static int WINAPI w_closesocket(SOCKET s) { return close((int)s) == 0 ? 0 : fail(); }

static int WINAPI w_bind(SOCKET s, const void *a, int len) {
    struct sockaddr_storage u;
    socklen_t n = addr_in(a, len, &u);
    return bind((int)s, (struct sockaddr *)&u, n) == 0 ? 0 : fail();
}

static int WINAPI w_connect(SOCKET s, const void *a, int len) {
    struct sockaddr_storage u;
    socklen_t n = addr_in(a, len, &u);
    return connect((int)s, (struct sockaddr *)&u, n) == 0 ? 0 : fail();
}

static int WINAPI w_listen(SOCKET s, int n) { return listen((int)s, n) == 0 ? 0 : fail(); }

static SOCKET WINAPI w_accept(SOCKET s, void *a, int *len) {
    struct sockaddr_storage u;
    socklen_t ul = sizeof(u);
    int r = accept4((int)s, (struct sockaddr *)&u, &ul, SOCK_CLOEXEC);
    if (r < 0) {
        fail();
        return INVALID_SOCKET;
    }
    addr_out(&u, ul, a, len);
    return (SOCKET)r;
}

static int WINAPI w_send(SOCKET s, const char *b, int n, int f) {
    ssize_t r = send((int)s, b, (size_t)n, (f & 1 ? MSG_OOB : 0) | MSG_NOSIGNAL);
    return r < 0 ? fail() : (int)r;
}

static int WINAPI w_recv(SOCKET s, char *b, int n, int f) {
    ssize_t r = recv((int)s, b, (size_t)n, (f & 1 ? MSG_OOB : 0) | (f & 2 ? MSG_PEEK : 0));
    return r < 0 ? fail() : (int)r;
}

static int WINAPI w_sendto(SOCKET s, const char *b, int n, int f, const void *a, int len) {
    struct sockaddr_storage u;
    socklen_t ul = addr_in(a, len, &u);
    ssize_t r = sendto((int)s, b, (size_t)n, (f & 1 ? MSG_OOB : 0) | MSG_NOSIGNAL, ul ? (struct sockaddr *)&u : 0, ul);
    return r < 0 ? fail() : (int)r;
}

static int WINAPI w_recvfrom(SOCKET s, char *b, int n, int f, void *a, int *len) {
    struct sockaddr_storage u;
    socklen_t ul = sizeof(u);
    ssize_t r = recvfrom((int)s, b, (size_t)n, (f & 2 ? MSG_PEEK : 0), (struct sockaddr *)&u, &ul);
    if (r < 0) {
        /* Windows reports a too-big datagram as WSAEMSGSIZE; ICMP errors on UDP as WSAECONNRESET */
        return fail();
    }
    addr_out(&u, ul, a, len);
    return (int)r;
}

static int WINAPI w_shutdown(SOCKET s, int how) { return shutdown((int)s, how) == 0 ? 0 : fail(); }

static int WINAPI w_getsockname(SOCKET s, void *a, int *len) {
    struct sockaddr_storage u;
    socklen_t ul = sizeof(u);
    if (getsockname((int)s, (struct sockaddr *)&u, &ul) != 0) return fail();
    addr_out(&u, ul, a, len);
    return 0;
}

static int WINAPI w_getpeername(SOCKET s, void *a, int *len) {
    struct sockaddr_storage u;
    socklen_t ul = sizeof(u);
    if (getpeername((int)s, (struct sockaddr *)&u, &ul) != 0) return fail();
    addr_out(&u, ul, a, len);
    return 0;
}

/* SOL_SOCKET options: Windows numbers to Unix ones */
static int sockopt_in(int level, int name, int *ulevel, int *uname) {
    *ulevel = level, *uname = name;
    if (level == 0xFFFF) {
        *ulevel = SOL_SOCKET;
        switch (name) {
        case 0x4: *uname = SO_REUSEADDR; break;
        case 0x8: *uname = SO_KEEPALIVE; break;
        case 0x20: *uname = SO_BROADCAST; break;
        case 0x80: *uname = SO_LINGER; break;
        case 0x1001: *uname = SO_SNDBUF; break;
        case 0x1002: *uname = SO_RCVBUF; break;
        case 0x1005: *uname = SO_SNDTIMEO; break;
        case 0x1006: *uname = SO_RCVTIMEO; break;
        case 0x1007: *uname = SO_ERROR; break;
        case 0x1008: *uname = SO_TYPE; break;
        case ~0x4: *uname = SO_REUSEADDR; break;    /* SO_EXCLUSIVEADDRUSE: near enough */
        default: return -1;
        }
    } else if (level == 0) {     /* IPPROTO_IP */
        switch (name) {
        case 4: *uname = IP_TTL; break;
        case 9: *uname = IP_MULTICAST_IF; break;
        case 10: *uname = IP_MULTICAST_TTL; break;
        case 11: *uname = IP_MULTICAST_LOOP; break;
        case 12: *uname = IP_ADD_MEMBERSHIP; break;
        case 13: *uname = IP_DROP_MEMBERSHIP; break;
        case 3: *uname = IP_TOS; break;
        default: return -1;
        }
    } else if (level == 41) {    /* IPPROTO_IPV6 */
        if (name == 27) *uname = IPV6_V6ONLY;
        else return -1;
    }
    return 0;
}

static int WINAPI w_setsockopt(SOCKET s, int level, int name, const char *v, int len) {
    int ul, un;
    if (sockopt_in(level, name, &ul, &un) != 0) return 0;     /* unknown options: accepted, as harmless */
    if (ul == SOL_SOCKET && (un == SO_RCVTIMEO || un == SO_SNDTIMEO) && len >= 4) {
        DWORD ms = *(const DWORD *)v;
        struct timeval tv = { ms / 1000, (long)(ms % 1000) * 1000 };
        return setsockopt((int)s, ul, un, &tv, sizeof(tv)) == 0 ? 0 : fail();
    }
    if (ul == SOL_SOCKET && un == SO_LINGER && len >= 4) {
        struct linger l = { ((const uint16_t *)v)[0], ((const uint16_t *)v)[1] };
        return setsockopt((int)s, ul, un, &l, sizeof(l)) == 0 ? 0 : fail();
    }
    {
        int iv = len >= 4 ? *(const int *)v : (len >= 1 ? (uint8_t)*v : 0);
        if (ul == SOL_SOCKET || (ul == IPPROTO_TCP && un == TCP_NODELAY) || (ul == IPPROTO_IP && (un == IP_TTL || un == IP_TOS || un == IP_MULTICAST_TTL || un == IP_MULTICAST_LOOP)) || ul == IPPROTO_IPV6)
            return setsockopt((int)s, ul, un, &iv, sizeof(iv)) == 0 ? 0 : fail();
    }
    return setsockopt((int)s, ul, un, v, (socklen_t)len) == 0 ? 0 : fail();
}

static int WINAPI w_getsockopt(SOCKET s, int level, int name, char *v, int *len) {
    int ul, un;
    socklen_t l;
    if (sockopt_in(level, name, &ul, &un) != 0) {
        wsa_error = 10042;
        return SOCKET_ERROR;
    }
    if (ul == SOL_SOCKET && un == SO_ERROR) {
        int e = 0;
        l = sizeof(e);
        if (getsockopt((int)s, ul, un, &e, &l) != 0) return fail();
        *(int *)v = e ? wsa_of(e) : 0;
        *len = 4;
        return 0;
    }
    l = (socklen_t)*len;
    if (getsockopt((int)s, ul, un, v, &l) != 0) return fail();
    *len = (int)l;
    return 0;
}

#define FIONBIO_W  0x8004667E
#define FIONREAD_W 0x4004667F

static int WINAPI w_ioctlsocket(SOCKET s, LONG cmd, ULONG *arg) {
    if ((ULONG)cmd == FIONBIO_W) {
        int fl = fcntl((int)s, F_GETFL);
        if (fl < 0) return fail();
        fl = *arg ? fl | O_NONBLOCK : fl & ~O_NONBLOCK;
        return fcntl((int)s, F_SETFL, fl) == 0 ? 0 : fail();
    }
    if ((ULONG)cmd == FIONREAD_W) {
        int n = 0;
        if (ioctl((int)s, FIONREAD, &n) != 0) return fail();
        *arg = (ULONG)n;
        return 0;
    }
    wsa_error = 10045;
    return SOCKET_ERROR;
}

/* fd_set: a count and up to 64 sockets */
typedef struct {
    UINT   fd_count;
    SOCKET fd_array[64];
} wfd_set;

typedef struct {
    LONG tv_sec, tv_usec;
} wtimeval;

static int WINAPI w_select(int nfds, wfd_set *r, wfd_set *w, wfd_set *e, const wtimeval *tv) {
    struct pollfd p[192];
    int np = 0, rc, timeout = -1;
    wfd_set *sets[3] = { r, w, e };
    (void)nfds;
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        for (UINT i = 0; i < sets[k]->fd_count && np < 192; i++) {
            int fd = (int)sets[k]->fd_array[i], j;
            for (j = 0; j < np; j++)
                if (p[j].fd == fd) break;
            if (j == np) p[np].fd = fd, p[np].events = 0, p[np].revents = 0, np++;
            p[j].events |= k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI;
        }
    }
    if (tv) timeout = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    if (np == 0) {
        if (timeout > 0) poll(0, 0, timeout);
        return 0;
    }
    rc = poll(p, (nfds_t)np, timeout);
    if (rc < 0) return fail();
    rc = 0;
    for (int k = 0; k < 3; k++) {
        UINT o = 0;
        if (!sets[k]) continue;
        for (UINT i = 0; i < sets[k]->fd_count; i++) {
            int fd = (int)sets[k]->fd_array[i];
            for (int j = 0; j < np; j++) {
                short want = k == 0 ? (POLLIN | POLLHUP | POLLERR) : k == 1 ? (POLLOUT | POLLERR) : POLLPRI;
                if (p[j].fd == fd && (p[j].revents & want)) {
                    sets[k]->fd_array[o++] = (SOCKET)fd;
                    rc++;
                    break;
                }
            }
        }
        sets[k]->fd_count = o;
    }
    return rc;
}

static int WINAPI w___WSAFDIsSet(SOCKET s, wfd_set *set) {
    for (UINT i = 0; i < set->fd_count; i++)
        if (set->fd_array[i] == s) return 1;
    return 0;
}

/* names */
typedef struct {
    char  *h_name;
    char **h_aliases;
    short  h_addrtype, h_length;
    char **h_addr_list;
} whostent;

static __thread whostent host_out;
static __thread char host_name[256];
static __thread char *host_aliases[1];
static __thread char *host_addrs[17];
static __thread uint8_t host_addr_data[16][16];

static whostent *host_from(struct hostent *h) {
    int i;
    if (!h) {
        wsa_error = 11001;    /* WSAHOST_NOT_FOUND */
        return 0;
    }
    snprintf(host_name, sizeof(host_name), "%s", h->h_name ? h->h_name : "");
    host_out.h_name = host_name;
    host_aliases[0] = 0;
    host_out.h_aliases = host_aliases;
    host_out.h_addrtype = (short)(h->h_addrtype == AF_INET6 ? WAF_INET6 : h->h_addrtype);
    host_out.h_length = (short)h->h_length;
    for (i = 0; i < 16 && h->h_addr_list[i]; i++) {
        memcpy(host_addr_data[i], h->h_addr_list[i], (size_t)(h->h_length > 16 ? 16 : h->h_length));
        host_addrs[i] = (char *)host_addr_data[i];
    }
    host_addrs[i] = 0;
    host_out.h_addr_list = host_addrs;
    return &host_out;
}

static whostent *WINAPI w_gethostbyname(const char *name) { return host_from(gethostbyname(name)); }

static whostent *WINAPI w_gethostbyaddr(const char *a, int len, int type) {
    return host_from(gethostbyaddr(a, (socklen_t)len, af_in(type)));
}

static int WINAPI w_gethostname(char *b, int n) { return gethostname(b, (size_t)n) == 0 ? 0 : fail(); }

static ULONG WINAPI w_inet_addr(const char *s) {
    struct in_addr a;
    if (!inet_aton(s, &a)) return 0xFFFFFFFF;
    return a.s_addr;
}

static char *WINAPI w_inet_ntoa(struct in_addr a) {
    static __thread char buf[20];
    snprintf(buf, sizeof(buf), "%s", inet_ntoa(a));
    return buf;
}

static int WINAPI w_inet_pton(int af, const char *s, void *out) { return inet_pton(af_in(af), s, out); }
static const char *WINAPI w_inet_ntop(int af, const void *a, char *b, size_t n) { return inet_ntop(af_in(af), a, b, (socklen_t)n); }

static uint32_t WINAPI w_htonl(uint32_t v) { return htonl(v); }
static uint16_t WINAPI w_htons(uint16_t v) { return htons(v); }
static uint32_t WINAPI w_ntohl(uint32_t v) { return ntohl(v); }
static uint16_t WINAPI w_ntohs(uint16_t v) { return ntohs(v); }

/* addrinfo: Windows has ai_canonname before ai_addr, and a size_t length */
typedef struct waddrinfo {
    int    ai_flags, ai_family, ai_socktype, ai_protocol;
    size_t ai_addrlen;
    char  *ai_canonname;
    void  *ai_addr;
    struct waddrinfo *ai_next;
} waddrinfo;

static int gai_err(int e) {
    switch (e) {
    case EAI_AGAIN: return 11002;      /* WSATRY_AGAIN */
    case EAI_NONAME: return 11001;     /* WSAHOST_NOT_FOUND */
    case EAI_FAIL: return 11003;       /* WSANO_RECOVERY */
    case EAI_MEMORY: return 8;
    case EAI_FAMILY: return 10047;
    case EAI_SOCKTYPE: return 10044;
    case EAI_SERVICE: return 10109;    /* WSATYPE_NOT_FOUND */
    default: return 11001;
    }
}

static int WINAPI w_getaddrinfo(const char *node, const char *serv, const waddrinfo *hints, waddrinfo **res) {
    struct addrinfo h, *r = 0, *p;
    waddrinfo *head = 0, **tail = &head;
    int e;
    memset(&h, 0, sizeof(h));
    if (hints) {
        h.ai_flags = hints->ai_flags & (AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST);
        h.ai_family = af_in(hints->ai_family);
        h.ai_socktype = hints->ai_socktype;
        h.ai_protocol = hints->ai_protocol;
    }
    e = getaddrinfo(node, serv, hints ? &h : 0, &r);
    if (e) {
        *res = 0;
        wsa_error = gai_err(e);
        return wsa_error;
    }
    for (p = r; p; p = p->ai_next) {
        waddrinfo *w = calloc(1, sizeof(*w) + sizeof(struct sockaddr_storage));
        struct sockaddr_storage *a = (struct sockaddr_storage *)(w + 1);
        w->ai_flags = p->ai_flags;
        w->ai_family = p->ai_family == AF_INET6 ? WAF_INET6 : p->ai_family;
        w->ai_socktype = p->ai_socktype;
        w->ai_protocol = p->ai_protocol;
        w->ai_addrlen = p->ai_addrlen;
        memcpy(a, p->ai_addr, p->ai_addrlen);
        if (a->ss_family == AF_INET6) a->ss_family = WAF_INET6;
        w->ai_addr = a;
        w->ai_canonname = p->ai_canonname ? strdup(p->ai_canonname) : 0;
        *tail = w;
        tail = &w->ai_next;
    }
    freeaddrinfo(r);
    *res = head;
    return 0;
}

static void WINAPI w_freeaddrinfo(waddrinfo *a) {
    while (a) {
        waddrinfo *n = a->ai_next;
        free(a->ai_canonname);
        free(a);
        a = n;
    }
}

static int WINAPI GetAddrInfoW(const WCHAR *node, const WCHAR *serv, const waddrinfo *hints, waddrinfo **res) {
    char *n = node ? w_to_utf8_dup(node) : 0, *s = serv ? w_to_utf8_dup(serv) : 0;
    int r = w_getaddrinfo(n, s, hints, res);
    free(n), free(s);
    return r;
}

static void WINAPI FreeAddrInfoW(waddrinfo *a) { w_freeaddrinfo(a); }

static int WINAPI w_getnameinfo(const void *a, int len, char *host, DWORD hl, char *serv, DWORD sl, int flags) {
    struct sockaddr_storage u;
    socklen_t ul = addr_in(a, len, &u);
    int e = getnameinfo((struct sockaddr *)&u, ul, host, hl, serv, sl, flags & (NI_NUMERICHOST | NI_NUMERICSERV));
    return e ? gai_err(e) : 0;
}

static void *WINAPI w_getservbyname(const char *n, const char *p) {
    (void)n, (void)p;
    return 0;
}
static void *WINAPI w_getprotobyname(const char *n) {
    (void)n;
    return 0;
}

/* WSA events: libcurl's waiting.  Events are kernel32 events; sockets are
 * polled while waiting */
HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name);
BOOL WINAPI SetEvent(HANDLE h);
BOOL WINAPI ResetEvent(HANDLE h);
BOOL WINAPI CloseHandle(HANDLE h);
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);

typedef struct {
    SOCKET s;
    HANDLE ev;
    LONG   mask;
} wsa_sel_t;

static wsa_sel_t sels[64];
static int nsels;

static HANDLE WINAPI WSACreateEvent(void) { return CreateEventW(0, TRUE, FALSE, 0); }
static BOOL WINAPI WSACloseEvent(HANDLE e) { return CloseHandle(e); }
static BOOL WINAPI WSASetEvent(HANDLE e) { return SetEvent(e); }
static BOOL WINAPI WSAResetEvent(HANDLE e) { return ResetEvent(e); }

static int WINAPI WSAEventSelect(SOCKET s, HANDLE ev, LONG mask) {
    int i;
    for (i = 0; i < nsels; i++)
        if (sels[i].s == s) break;
    if (i == nsels) {
        if (nsels >= 64) {
            wsa_error = 10055;
            return SOCKET_ERROR;
        }
        nsels++;
    }
    sels[i].s = s, sels[i].ev = ev, sels[i].mask = mask;
    if (!mask || !ev) sels[i] = sels[--nsels];
    {
        int fl = fcntl((int)s, F_GETFL);
        if (fl >= 0) fcntl((int)s, F_SETFL, fl | O_NONBLOCK);
    }
    return 0;
}

/* sockets ready now: their events are set */
static void wsa_poll(int ms) {
    struct pollfd p[64];
    for (int i = 0; i < nsels; i++) {
        p[i].fd = (int)sels[i].s;
        p[i].events = (short)(((sels[i].mask & (0x1 | 0x8 | 0x20)) ? POLLIN : 0) | ((sels[i].mask & (0x2 | 0x10)) ? POLLOUT : 0));
        p[i].revents = 0;
    }
    if (poll(p, (nfds_t)nsels, ms) > 0)
        for (int i = 0; i < nsels; i++)
            if (p[i].revents) SetEvent(sels[i].ev);
}

static DWORD WINAPI WSAWaitForMultipleEvents(DWORD n, const HANDLE *evs, BOOL all, DWORD ms, BOOL alertable) {
    uint64_t end = mono_ns() + (ms == INFINITE ? (uint64_t)1 << 62 : (uint64_t)ms * 1000000ull);
    (void)alertable;
    for (;;) {
        DWORD r = WaitForMultipleObjects(n, evs, all, 0);
        uint64_t now;
        if (r != WAIT_TIMEOUT) return r;
        now = mono_ns();
        if (now >= end) return WAIT_TIMEOUT;
        wsa_poll((int)((end - now) / 1000000ull > 10 ? 10 : (end - now) / 1000000ull));
    }
}

typedef struct {
    LONG lNetworkEvents;
    int  iErrorCode[10];
} WSANETWORKEVENTS;

static int WINAPI WSAEnumNetworkEvents(SOCKET s, HANDLE ev, WSANETWORKEVENTS *ne) {
    struct pollfd p = { (int)s, POLLIN | POLLOUT, 0 };
    LONG mask = 0x3F;
    for (int i = 0; i < nsels; i++)
        if (sels[i].s == s) mask = sels[i].mask;
    memset(ne, 0, sizeof(*ne));
    if (poll(&p, 1, 0) > 0) {
        if (p.revents & POLLIN) ne->lNetworkEvents |= mask & (0x1 | 0x8);   /* FD_READ, FD_ACCEPT */
        if (p.revents & POLLOUT) ne->lNetworkEvents |= mask & (0x2 | 0x10); /* FD_WRITE, FD_CONNECT */
        if (p.revents & POLLHUP) ne->lNetworkEvents |= mask & 0x20;         /* FD_CLOSE */
    }
    if (ev) ResetEvent(ev);
    return 0;
}

static int WINAPI WSAIoctl(SOCKET s, DWORD code, void *in, DWORD nin, void *out, DWORD nout, DWORD *ret, void *ov, void *cb) {
    (void)s, (void)code, (void)in, (void)nin, (void)out, (void)nout, (void)ov, (void)cb;
    if (ret) *ret = 0;
    wsa_error = 10045;
    return SOCKET_ERROR;
}

static int WINAPI WSASendNotSupported(void) {
    wsa_error = 10045;
    return SOCKET_ERROR;
}

static const wexport_t ws2[] = {
    WEXPO("accept", w_accept, 1), WEXPO("bind", w_bind, 2), WEXPO("closesocket", w_closesocket, 3),
    WEXPO("connect", w_connect, 4), WEXPO("getpeername", w_getpeername, 5), WEXPO("getsockname", w_getsockname, 6),
    WEXPO("getsockopt", w_getsockopt, 7), WEXPO("htonl", w_htonl, 8), WEXPO("htons", w_htons, 9),
    WEXPO("ioctlsocket", w_ioctlsocket, 10), WEXPO("inet_addr", w_inet_addr, 11), WEXPO("inet_ntoa", w_inet_ntoa, 12),
    WEXPO("listen", w_listen, 13), WEXPO("ntohl", w_ntohl, 14), WEXPO("ntohs", w_ntohs, 15), WEXPO("recv", w_recv, 16),
    WEXPO("recvfrom", w_recvfrom, 17), WEXPO("select", w_select, 18), WEXPO("send", w_send, 19),
    WEXPO("sendto", w_sendto, 20), WEXPO("setsockopt", w_setsockopt, 21), WEXPO("shutdown", w_shutdown, 22),
    WEXPO("socket", w_socket, 23), WEXPO("gethostbyaddr", w_gethostbyaddr, 51),
    WEXPO("gethostbyname", w_gethostbyname, 52), WEXPO("getprotobyname", w_getprotobyname, 53),
    WEXPO("getservbyname", w_getservbyname, 55), WEXPO("gethostname", w_gethostname, 57),
    WEXPO("WSAGetLastError", WSAGetLastError, 111), WEXPO("WSASetLastError", WSASetLastError, 112),
    WEXPO("WSAStartup", WSAStartup, 115), WEXPO("WSACleanup", WSACleanup, 116),
    WEXPO("__WSAFDIsSet", w___WSAFDIsSet, 151), WEXP(WSASocketW), WEXP(WSASocketA), WEXPN("getaddrinfo", w_getaddrinfo),
    WEXPN("freeaddrinfo", w_freeaddrinfo), WEXP(GetAddrInfoW), WEXP(FreeAddrInfoW), WEXPN("getnameinfo", w_getnameinfo),
    WEXPN("inet_pton", w_inet_pton), WEXPN("inet_ntop", w_inet_ntop), WEXP(WSACreateEvent), WEXP(WSACloseEvent),
    WEXP(WSASetEvent), WEXP(WSAResetEvent), WEXP(WSAEventSelect), WEXP(WSAWaitForMultipleEvents),
    WEXP(WSAEnumNetworkEvents), WEXP(WSAIoctl), WEXPN("WSASend", WSASendNotSupported),
    WEXPN("WSARecv", WSASendNotSupported),
};
const wdll_t dll_ws2_32 = { "ws2_32.dll", ws2, sizeof(ws2) / sizeof(ws2[0]) };
