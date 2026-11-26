/* kernel32: files, consoles, memory, time, the system, the environment,
 * text conversion, locks and modules.
 *
 * Files are Unix files under the drive mapping of path.c; a file handle is
 * a descriptor.  Text in "ANSI" functions is UTF-8 (the code page programs
 * see is 65001, as on Windows with UTF-8 turned on), so names round-trip
 * with the Unix side unchanged. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <pthread.h>
#include <malloc.h>
#include <sched.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <linux/futex.h>
#include <cpuid.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>

#define MAP_FIXED_NOREPLACE_COMPAT 0x100000

extern char **environ;

/* ---- files ------------------------------------------------------------------------------ */

typedef struct {
    int  fd;
    int  is_dir;
    int  append;
    char path[1024];
    int  stream;           /* a pipe or socket: no file offsets */
    int  delete_on_close;  /* FILE_FLAG_DELETE_ON_CLOSE, FileDispositionInfo */
    int  listen_fd;        /* a named pipe's server end: the socket it accepts on, -1 */
    char pipe_name[256];   /* a named pipe's Unix socket path */
    void *iocp;            /* the completion port it is associated with, or 0 */
    ULONG_PTR iocp_key;
    UCHAR iocp_modes;      /* SetFileCompletionNotificationModes */
} wfileobj_t;

#define GENERIC_READ    0x80000000U
#define GENERIC_WRITE   0x40000000U
#define FILE_APPEND_DATA 0x4

#define CREATE_NEW        1
#define CREATE_ALWAYS     2
#define OPEN_EXISTING     3
#define OPEN_ALWAYS       4
#define TRUNCATE_EXISTING 5

#define FILE_ATTRIBUTE_READONLY  0x1
#define FILE_ATTRIBUTE_HIDDEN    0x2
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_ARCHIVE   0x20
#define FILE_ATTRIBUTE_NORMAL    0x80
#define INVALID_FILE_ATTRIBUTES  0xFFFFFFFFU
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000U

static void set_errno_error(void) { wset_last_error(errno_to_win(errno)); }

/* the Unix path for a program's path, found case-blind when the exact one
 * is not there (Windows names are case-blind; Unix ones are not) */
int path_find(const char *wpath, char *out, size_t cap) {
    struct stat st;
    if (path_to_unix(wpath, out, cap) != 0) return -1;
    if (lstat(out, &st) == 0) return 0;
    {
        /* walk from the root, matching each part case-blind */
        char work[1024], *p, *save = 0;
        size_t o;
        snprintf(work, sizeof(work), "%s", out);
        o = 0;
        if (work[0] == '/') out[o++] = '/';
        else {
            out[o++] = '.';
        }
        out[o] = 0;
        for (p = strtok_r(work, "/", &save); p; p = strtok_r(0, "/", &save)) {
            char cand[1024];
            DIR *d;
            struct dirent *e;
            int found = 0;
            snprintf(cand, sizeof(cand), "%s%s%s", out, o && out[o - 1] == '/' ? "" : "/", p);
            if (lstat(cand, &st) == 0) {
                snprintf(out, cap, "%s", cand);
                o = strlen(out);
                continue;
            }
            d = opendir(out[0] ? out : ".");
            if (d) {
                while ((e = readdir(d))) {
                    if (!a_icmp(e->d_name, p)) {
                        snprintf(cand, sizeof(cand), "%s%s%s", out, o && out[o - 1] == '/' ? "" : "/", e->d_name);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found) {
                /* not there: the rest as given, so a new file is made with its name */
                snprintf(cand, sizeof(cand), "%s%s%s", out, o && out[o - 1] == '/' ? "" : "/", p);
                snprintf(out, cap, "%s", cand);
                o = strlen(out);
                for (p = strtok_r(0, "/", &save); p; p = strtok_r(0, "/", &save)) {
                    snprintf(cand, sizeof(cand), "%s/%s", out, p);
                    snprintf(out, cap, "%s", cand);
                }
                return 0;
            }
            snprintf(out, cap, "%s", cand);
            o = strlen(out);
        }
        if (out[0] == '.' && out[1] == '/') memmove(out, out + 2, strlen(out + 2) + 1);
    }
    return 0;
}

static HANDLE file_handle(int fd, const char *path, int is_dir) {
    wfileobj_t *f = calloc(1, sizeof(*f));
    HANDLE h;
    if (!f) return INVALID_HANDLE_VALUE;
    f->fd = fd;
    f->is_dir = is_dir;
    f->listen_fd = -1;
    if (path) snprintf(f->path, sizeof(f->path), "%s", path);
    h = h_new(H_FILE, f);
    if (!h) {
        free(f);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* the descriptor of a file handle, -1 if it is not one (consoles too) */
int file_fd(HANDLE h) {
    whandle_t *w = h_get(h, 0);
    if (!w) return -1;
    if (w->kind == H_FILE) return ((wfileobj_t *)w->obj)->fd;
    if (w->kind == H_CONSOLE) return (int)(intptr_t)w->obj;
    return -1;
}

HANDLE file_from_fd(int fd) { return file_handle(fd, 0, 0); }

/* ---- named pipes ------------------------------------------------------------------------
 * \\.\pipe\NAME is a Unix socket, /tmp/.icda-pipes/<hash of the name, case-blind>:
 * the server end listens there and accepts a client for each ConnectNamedPipe;
 * a client opens it with CreateFile.  Message pipes are SOCK_SEQPACKET, which
 * keeps each write a message. */

static int pipe_path(const char *wname, char *out, size_t cap) {
    const char *n = wname;
    uint64_t h = 1469598103934665603ull;
    if (!strncasecmp(n, "\\\\.\\pipe\\", 9)) n += 9;
    else if (!strncasecmp(n, "\\\\?\\pipe\\", 9)) n += 9;
    else return -1;
    for (; *n; n++) h = (h ^ (uint8_t)(*n >= 'A' && *n <= 'Z' ? *n + 32 : *n)) * 1099511628211ull;
    mkdir("/tmp/.icda-pipes", 01777);
    snprintf(out, cap, "/tmp/.icda-pipes/%016llx", (unsigned long long)h);
    return 0;
}

static int pipe_connect(const char *upath) {
    struct sockaddr_un a;
    int types[2] = { SOCK_SEQPACKET, SOCK_STREAM };
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", upath);
    for (int i = 0; i < 2; i++) {
        int fd = socket(AF_UNIX, types[i] | SOCK_CLOEXEC, 0);
        if (fd < 0) continue;
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
        close(fd);
        if (errno != EPROTOTYPE) break;      /* the other kind is the one that listens */
    }
    return -1;
}

HANDLE WINAPI CreateNamedPipeW(const WCHAR *name, DWORD om, DWORD pm, DWORD inst, DWORD ob, DWORD ib, DWORD to, void *sa) {
    char wn[512], up[256];
    struct sockaddr_un a;
    int lfd, type = (pm & 4) ? SOCK_SEQPACKET : SOCK_STREAM;     /* PIPE_TYPE_MESSAGE */
    HANDLE h;
    whandle_t *w;
    (void)inst, (void)ob, (void)ib, (void)to, (void)sa;
    w_to_utf8(name, -1, wn, sizeof(wn));
    if (pipe_path(wn, up, sizeof(up)) != 0) {
        wset_last_error(ERROR_INVALID_NAME);
        return INVALID_HANDLE_VALUE;
    }
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", up);
    /* another server of the same name, still there? */
    {
        int probe = pipe_connect(up);
        if (probe >= 0) {
            close(probe);
            if (om & 0x80000) {                     /* FILE_FLAG_FIRST_PIPE_INSTANCE */
                wset_last_error(ERROR_ACCESS_DENIED);
                return INVALID_HANDLE_VALUE;
            }
            /* more instances of a live pipe: a listener of our own is not possible
             * on the same path, so the new instance shares nothing (rare) */
        }
        unlink(up);
    }
    lfd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(lfd, 16) != 0) {
        if (lfd >= 0) close(lfd);
        set_errno_error();
        return INVALID_HANDLE_VALUE;
    }
    h = file_handle(-1, 0, 0);
    w = h_get(h, H_FILE);
    if (!w) {
        close(lfd);
        return INVALID_HANDLE_VALUE;
    }
    ((wfileobj_t *)w->obj)->listen_fd = lfd;
    ((wfileobj_t *)w->obj)->stream = 1;
    snprintf(((wfileobj_t *)w->obj)->pipe_name, sizeof(((wfileobj_t *)w->obj)->pipe_name), "%s", up);
    if (wtrace) wlog("named pipe %s", wn);
    return h;
}

HANDLE WINAPI CreateNamedPipeA(const char *name, DWORD om, DWORD pm, DWORD inst, DWORD ob, DWORD ib, DWORD to, void *sa) {
    WCHAR *w = utf8_to_w_dup(name);
    HANDLE h = CreateNamedPipeW(w, om, pm, inst, ob, ib, to, sa);
    free(w);
    return h;
}

typedef struct {
    int       lfd;
    wfileobj_t *f;
    void     *ov;
} accept_job_t;

BOOL WINAPI SetEvent(HANDLE h);

static void *accept_thread(void *arg) {
    accept_job_t *j = arg;
    int fd = accept4(j->lfd, 0, 0, SOCK_CLOEXEC);
    ULONG_PTR *ov = j->ov;
    if (fd >= 0) j->f->fd = fd;
    if (ov) {
        ov[0] = fd >= 0 ? 0 : (ULONG_PTR)0xC0000001;   /* Internal: the status */
        ov[1] = 0;
        if (ov[3]) SetEvent((HANDLE)ov[3]);
    }
    free(j);
    return 0;
}

BOOL WINAPI ConnectNamedPipe(HANDLE h, void *ov) {
    whandle_t *w = h_get(h, H_FILE);
    wfileobj_t *f = w ? w->obj : 0;
    if (!f || f->listen_fd < 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (f->fd >= 0) {
        wset_last_error(535);                 /* ERROR_PIPE_CONNECTED */
        return FALSE;
    }
    if (ov) {
        /* overlapped: accepted by a thread of ours, the event says when */
        accept_job_t *j = malloc(sizeof(*j));
        pthread_t th;
        j->lfd = f->listen_fd;
        j->f = f;
        j->ov = ov;
        ((ULONG_PTR *)ov)[0] = 0x103;         /* STATUS_PENDING */
        pthread_create(&th, 0, accept_thread, j);
        pthread_detach(th);
        wset_last_error(997);                 /* ERROR_IO_PENDING */
        return FALSE;
    }
    f->fd = accept4(f->listen_fd, 0, 0, SOCK_CLOEXEC);
    if (f->fd < 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DisconnectNamedPipe(HANDLE h) {
    whandle_t *w = h_get(h, H_FILE);
    wfileobj_t *f = w ? w->obj : 0;
    if (!f) return FALSE;
    if (f->fd >= 0) close(f->fd);
    f->fd = -1;
    return TRUE;
}

BOOL WINAPI WaitNamedPipeW(const WCHAR *name, DWORD to) {
    char wn[512], up[256];
    uint64_t end = mono_ns() + (uint64_t)(to == 0 ? 50 : to == INFINITE ? 1u << 30 : to) * 1000000ull;
    w_to_utf8(name, -1, wn, sizeof(wn));
    if (pipe_path(wn, up, sizeof(up)) != 0) return FALSE;
    while (mono_ns() < end) {
        struct stat st;
        if (stat(up, &st) == 0) return TRUE;
        usleep(2000);
    }
    wset_last_error(ERROR_TIMEOUT);
    return FALSE;
}

BOOL WINAPI SetNamedPipeHandleState(HANDLE h, DWORD *mode, DWORD *max, DWORD *to) {
    (void)h, (void)mode, (void)max, (void)to;
    return TRUE;
}

BOOL WINAPI GetNamedPipeInfo(HANDLE h, DWORD *flags, DWORD *ob, DWORD *ib, DWORD *inst) {
    whandle_t *w = h_get(h, H_FILE);
    wfileobj_t *f = w ? w->obj : 0;
    if (flags) *flags = (f && f->listen_fd >= 0) ? 1 : 0;      /* PIPE_SERVER_END */
    if (ob) *ob = 65536;
    if (ib) *ib = 65536;
    if (inst) *inst = 255;
    return f != 0;
}

BOOL WINAPI GetNamedPipeClientProcessId(HANDLE h, ULONG *pid) {
    int fd = file_fd(h);
    struct ucred c;
    socklen_t n = sizeof(c);
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &c, &n) != 0) return FALSE;
    *pid = (ULONG)c.pid;
    return TRUE;
}

BOOL WINAPI ImpersonateNamedPipeClient(HANDLE h) {
    (void)h;
    return TRUE;
}

BOOL WINAPI TransactNamedPipe(HANDLE h, void *in, DWORD nin, void *out, DWORD nout, DWORD *got, void *ov) {
    int fd = file_fd(h);
    ssize_t r;
    (void)ov;
    if (fd < 0 || write(fd, in, nin) != (ssize_t)nin) {
        set_errno_error();
        return FALSE;
    }
    r = read(fd, out, nout);
    if (r < 0) {
        set_errno_error();
        return FALSE;
    }
    if (got) *got = (DWORD)r;
    return TRUE;
}

BOOL WINAPI CallNamedPipeW(const WCHAR *name, void *in, DWORD nin, void *out, DWORD nout, DWORD *got, DWORD to) {
    char wn[512], up[256];
    int fd;
    ssize_t r;
    (void)to;
    w_to_utf8(name, -1, wn, sizeof(wn));
    if (pipe_path(wn, up, sizeof(up)) != 0 || (fd = pipe_connect(up)) < 0) {
        wset_last_error(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    if (write(fd, in, nin) != (ssize_t)nin) {
        close(fd);
        return FALSE;
    }
    r = read(fd, out, nout);
    close(fd);
    if (r < 0) return FALSE;
    *got = (DWORD)r;
    return TRUE;
}

HANDLE WINAPI CreateFileW(const WCHAR *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    char wp[1024], up[1024];
    int oflags = O_CLOEXEC, fd;
    struct stat st;
    int existed;
    (void)share, (void)sa, (void)tmpl;
    if (!name) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    w_to_utf8(name, -1, wp, sizeof(wp));
    if (!a_icmp(wp, "CONOUT$") || !a_icmp(wp, "CONIN$")) return h_std(wp[3] == 'O' || wp[3] == 'o' ? 1 : 0);
    if (!strncasecmp(wp, "\\\\.\\pipe\\", 9) || !strncasecmp(wp, "\\\\?\\pipe\\", 9)) {
        char pp[256];
        int fd;
        HANDLE h;
        whandle_t *w;
        if (pipe_path(wp, pp, sizeof(pp)) != 0 || (fd = pipe_connect(pp)) < 0) {
            wset_last_error(231);                 /* ERROR_PIPE_BUSY or not there: FILE_NOT_FOUND below */
            wset_last_error(ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE_VALUE;
        }
        h = file_handle(fd, 0, 0);
        w = h_get(h, H_FILE);
        if (w) ((wfileobj_t *)w->obj)->stream = 1;
        return h;
    }
    if (!strncmp(wp, "\\\\.\\", 4)) {          /* other devices: none */
        wset_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (path_find(wp, up, sizeof(up)) != 0) {
        wset_last_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    existed = stat(up, &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
            wset_last_error(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        fd = open(up, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            set_errno_error();
            return INVALID_HANDLE_VALUE;
        }
        return file_handle(fd, up, 1);
    }
    if ((access & (GENERIC_WRITE | 0x10000000U /* GENERIC_ALL */ | 0x2 /* FILE_WRITE_DATA */ | FILE_APPEND_DATA)) &&
        (access & (GENERIC_READ | 0x10000000U | 0x1)))
        oflags |= O_RDWR;
    else if (access & (GENERIC_WRITE | 0x2 | FILE_APPEND_DATA))
        oflags |= O_WRONLY;
    else
        oflags |= O_RDONLY;
    switch (disp) {
    case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: oflags |= O_CREAT; break;
    case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
    default: break;
    }
    if ((oflags & O_TRUNC) && (oflags & O_ACCMODE) == O_RDONLY) oflags = (oflags & ~O_ACCMODE) | O_RDWR;
    fd = open(up, oflags, 0644);
    if (fd < 0) {
        set_errno_error();
        if (errno == EEXIST) wset_last_error(ERROR_FILE_EXISTS);
        return INVALID_HANDLE_VALUE;
    }
    /* CREATE_ALWAYS and OPEN_ALWAYS say whether the file was there */
    wset_last_error(((disp == CREATE_ALWAYS || disp == OPEN_ALWAYS) && existed) ? ERROR_ALREADY_EXISTS : 0);
    {
        HANDLE h = file_handle(fd, up, 0);
        whandle_t *w = h_get(h, H_FILE);
        if (w && (access & FILE_APPEND_DATA) && !(access & (GENERIC_WRITE | 0x2))) ((wfileobj_t *)w->obj)->append = 1;
        if (w && (flags & 0x04000000)) ((wfileobj_t *)w->obj)->delete_on_close = 1;     /* FILE_FLAG_DELETE_ON_CLOSE */
        return h;
    }
}

HANDLE WINAPI CreateFileA(const char *name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl) {
    WCHAR *w = utf8_to_w_dup(name);
    HANDLE h = CreateFileW(w, access, share, sa, disp, flags, tmpl);
    free(w);
    return h;
}

HANDLE WINAPI CreateFile2(const WCHAR *name, DWORD access, DWORD share, DWORD disp, void *params) {
    (void)params;
    return CreateFileW(name, access, share, 0, disp, 0, 0);
}

typedef struct {
    ULONG_PTR Internal, InternalHigh;
    DWORD     Offset, OffsetHigh;
    HANDLE    hEvent;
} OVERLAPPED;

BOOL WINAPI SetEvent(HANDLE h);
static void file_completed(whandle_t *w, OVERLAPPED *ov, DWORD bytes);

BOOL WINAPI ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, OVERLAPPED *ov) {
    int fd = file_fd(h);
    whandle_t *w = h_get(h, H_FILE);
    ssize_t r;
    if (got) *got = 0;
    if (fd < 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (ov && !(w && ((wfileobj_t *)w->obj)->stream)) r = pread(fd, buf, n, (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset));
    else r = read(fd, buf, n);
    if (r < 0) {
        set_errno_error();
        return FALSE;
    }
    if (got) *got = (DWORD)r;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = (ULONG_PTR)r;
        if (ov->hEvent) SetEvent(ov->hEvent);
        file_completed(w, ov, (DWORD)r);
    }
    return TRUE;
}

BOOL WINAPI WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *put, OVERLAPPED *ov) {
    int fd = file_fd(h);
    ssize_t r;
    whandle_t *w = h_get(h, H_FILE);
    if (put) *put = 0;
    if (fd < 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (w && ((wfileobj_t *)w->obj)->append) lseek(fd, 0, SEEK_END);
    if (ov && !(ov->Offset == 0xFFFFFFFF && ov->OffsetHigh == 0xFFFFFFFF) && !(w && ((wfileobj_t *)w->obj)->stream))
        r = pwrite(fd, buf, n, (off_t)((uint64_t)ov->OffsetHigh << 32 | ov->Offset));
    else r = write(fd, buf, n);
    if (r < 0) {
        set_errno_error();
        return FALSE;
    }
    if (put) *put = (DWORD)r;
    if (ov) {
        ov->Internal = 0;
        ov->InternalHigh = (ULONG_PTR)r;
        if (ov->hEvent) SetEvent(ov->hEvent);
        file_completed(w, ov, (DWORD)r);
    }
    return TRUE;
}

/* ---- I/O completion ports --------------------------------------------------------------------
 *
 * A port is a queue of completion packets.  ICDA's file I/O is done before
 * ReadFile or WriteFile returns, so a file associated with a port posts its
 * packet then (unless the program asked for none on success); the program
 * posts its own.  Waits for packets use the waitable objects' one lock. */
typedef struct {
    ULONG_PTR   key;
    OVERLAPPED *ov;
    DWORD       bytes, status;
} iopkt_t;

typedef struct {
    int      magic;
    iopkt_t *q;
    int      head, n, cap;
} iocp_t;

#define IOCP_MAGIC 0x494F4350

static iocp_t *iocp_of(HANDLE h) {
    whandle_t *w = h_get(h, H_OTHER);
    iocp_t *p = w ? w->obj : 0;
    return p && p->magic == IOCP_MAGIC ? p : 0;
}

static void iocp_post(iocp_t *p, ULONG_PTR key, OVERLAPPED *ov, DWORD bytes, DWORD status) {
    sync_lock();
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        iopkt_t *nq = malloc((size_t)nc * sizeof(*nq));
        for (int i = 0; i < p->n; i++) nq[i] = p->q[(p->head + i) % p->cap];
        free(p->q);
        p->q = nq, p->cap = nc, p->head = 0;
    }
    p->q[(p->head + p->n) % p->cap] = (iopkt_t){ key, ov, bytes, status };
    p->n++;
    sync_broadcast();
    sync_unlock();
}

static void file_completed(whandle_t *w, OVERLAPPED *ov, DWORD bytes) {
    wfileobj_t *f = w ? w->obj : 0;
    if (f && f->iocp && !(f->iocp_modes & 1))          /* FILE_SKIP_COMPLETION_PORT_ON_SUCCESS */
        iocp_post(f->iocp, f->iocp_key, ov, bytes, 0);
}

HANDLE WINAPI CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD threads) {
    iocp_t *p = existing ? iocp_of(existing) : 0;
    HANDLE port = existing;
    (void)threads;
    if (existing && !p) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!p) {
        p = calloc(1, sizeof(*p));
        p->magic = IOCP_MAGIC;
        port = h_new(H_OTHER, p);
    }
    if (file && file != INVALID_HANDLE_VALUE) {
        whandle_t *w = h_get(file, H_FILE);
        if (w) {
            ((wfileobj_t *)w->obj)->iocp = p;
            ((wfileobj_t *)w->obj)->iocp_key = key;
        }
    }
    return port;
}

BOOL WINAPI PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, OVERLAPPED *ov) {
    iocp_t *p = iocp_of(port);
    if (!p) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    iocp_post(p, key, ov, bytes, 0);
    return TRUE;
}

/* waits until the port has packets (under the sync lock): 0, or WAIT_TIMEOUT */
static DWORD iocp_wait(iocp_t *p, DWORD ms) {
    if (ms != INFINITE) sync_deadline(ms);
    while (!p->n) {
        if (ms == 0 || sync_wait_ms(ms) == ETIMEDOUT) return WAIT_TIMEOUT;
    }
    return 0;
}

BOOL WINAPI GetQueuedCompletionStatus(HANDLE port, DWORD *bytes, ULONG_PTR *key, OVERLAPPED **ov, DWORD ms) {
    iocp_t *p = iocp_of(port);
    iopkt_t k;
    if (ov) *ov = 0;
    if (!p) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    sync_lock();
    if (iocp_wait(p, ms)) {
        sync_unlock();
        wset_last_error(WAIT_TIMEOUT);
        return FALSE;
    }
    k = p->q[p->head];
    p->head = (p->head + 1) % p->cap;
    p->n--;
    sync_unlock();
    if (bytes) *bytes = k.bytes;
    if (key) *key = k.key;
    if (ov) *ov = k.ov;
    if (k.status) {
        wset_last_error(k.status);
        return FALSE;
    }
    return TRUE;
}

typedef struct {
    ULONG_PTR   key;
    OVERLAPPED *ov;
    ULONG_PTR   internal;
    DWORD       bytes;
} OVERLAPPED_ENTRY;

BOOL WINAPI GetQueuedCompletionStatusEx(HANDLE port, OVERLAPPED_ENTRY *e, ULONG count, ULONG *removed, DWORD ms, BOOL alertable) {
    iocp_t *p = iocp_of(port);
    ULONG got = 0;
    (void)alertable;
    if (removed) *removed = 0;
    if (!p || !e || !count) {
        wset_last_error(p ? ERROR_INVALID_PARAMETER : ERROR_INVALID_HANDLE);
        return FALSE;
    }
    sync_lock();
    if (iocp_wait(p, ms)) {
        sync_unlock();
        wset_last_error(WAIT_TIMEOUT);
        return FALSE;
    }
    while (p->n && got < count) {
        iopkt_t k = p->q[p->head];
        p->head = (p->head + 1) % p->cap;
        p->n--;
        e[got].key = k.key, e[got].ov = k.ov, e[got].internal = k.status, e[got].bytes = k.bytes;
        got++;
    }
    sync_unlock();
    if (removed) *removed = got;
    return TRUE;
}

BOOL WINAPI SetFileCompletionNotificationModes(HANDLE h, UCHAR modes) {
    whandle_t *w = h_get(h, H_FILE);
    if (w) ((wfileobj_t *)w->obj)->iocp_modes = modes;
    return TRUE;
}

/* ---- waits of the thread pool (RegisterWaitForSingleObject) -------------------------------------
 * Each registered wait is a thread of its own, waiting for the object or a
 * cancel event, and calling the program back. */
typedef void (WCALLBACK *WAITORTIMERCALLBACK)(void *, BOOLEAN);
typedef struct {
    int                 magic;
    HANDLE              obj, cancel, done;
    WAITORTIMERCALLBACK cb;
    void               *ctx;
    DWORD               ms;
    ULONG               flags;
    pthread_t           th;
    DWORD               tid;
} rwait_t;

#define RWAIT_MAGIC 0x52574149
HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name);
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms);

static void *rwait_thread(void *arg) {
    rwait_t *r = arg;
    win_thread_setup();
    r->tid = thread_id();
    for (;;) {
        HANDLE hs[2] = { r->cancel, r->obj };
        DWORD got = WaitForMultipleObjects(2, hs, FALSE, r->ms);
        if (got == WAIT_OBJECT_0 || got == WAIT_FAILED) break;
        r->cb(r->ctx, got == WAIT_TIMEOUT);
        if (r->flags & 8) break;                    /* WT_EXECUTEONLYONCE */
        if (WaitForSingleObject(r->cancel, 0) == WAIT_OBJECT_0) break;
    }
    if (r->done) SetEvent(r->done);
    return 0;
}

BOOL WINAPI RegisterWaitForSingleObject(HANDLE *out, HANDLE obj, WAITORTIMERCALLBACK cb, void *ctx, ULONG ms, ULONG flags) {
    rwait_t *r = calloc(1, sizeof(*r));
    pthread_attr_t a;
    r->magic = RWAIT_MAGIC;
    r->obj = obj, r->cb = cb, r->ctx = ctx, r->ms = ms, r->flags = flags;
    r->cancel = CreateEventW(0, TRUE, FALSE, 0);
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1u << 20);
    if (pthread_create(&r->th, &a, rwait_thread, r) != 0) {
        free(r);
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    *out = h_new(H_OTHER, r);
    return TRUE;
}

BOOL WINAPI UnregisterWaitEx(HANDLE h, HANDLE completion) {
    whandle_t *w = h_get(h, H_OTHER);
    rwait_t *r = w ? w->obj : 0;
    if (!r || r->magic != RWAIT_MAGIC) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (completion && completion != INVALID_HANDLE_VALUE) r->done = completion;
    SetEvent(r->cancel);
    if (completion == INVALID_HANDLE_VALUE && r->tid != thread_id()) pthread_join(r->th, 0);   /* its callbacks are over */
    else pthread_detach(r->th);
    h_close(h);
    return TRUE;
}

BOOL WINAPI UnregisterWait(HANDLE h) { return UnregisterWaitEx(h, 0); }

BOOL WINAPI GetOverlappedResult(HANDLE h, OVERLAPPED *ov, DWORD *n, BOOL wait) {
    (void)h, (void)wait;
    if (n) *n = ov ? (DWORD)ov->InternalHigh : 0;
    return TRUE;
}

BOOL WINAPI CancelIo(HANDLE h) {
    (void)h;
    return TRUE;
}

BOOL WINAPI CancelIoEx(HANDLE h, OVERLAPPED *ov) {
    (void)h, (void)ov;
    return TRUE;
}

BOOL WINAPI FlushFileBuffers(HANDLE h) {
    int fd = file_fd(h);
    if (fd >= 0) fsync(fd);
    return TRUE;
}

BOOL WINAPI GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    struct stat st;
    int fd = file_fd(h);
    if (fd < 0 || fstat(fd, &st) != 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    size->QuadPart = st.st_size;
    return TRUE;
}

DWORD WINAPI GetFileSize(HANDLE h, DWORD *high) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) return 0xFFFFFFFF;
    if (high) *high = (DWORD)(s.QuadPart >> 32);
    return (DWORD)s.QuadPart;
}

BOOL WINAPI SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, LARGE_INTEGER *newpos, DWORD method) {
    int fd = file_fd(h);
    off_t r;
    if (fd < 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    r = lseek(fd, (off_t)dist.QuadPart, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) {
        set_errno_error();
        return FALSE;
    }
    if (newpos) newpos->QuadPart = r;
    return TRUE;
}

DWORD WINAPI SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD method) {
    LARGE_INTEGER d, n;
    d.QuadPart = hi ? ((int64_t)*hi << 32) | (uint32_t)lo : lo;
    if (!SetFilePointerEx(h, d, &n, method)) return 0xFFFFFFFF;
    if (hi) *hi = (LONG)(n.QuadPart >> 32);
    wset_last_error(0);
    return (DWORD)n.QuadPart;
}

BOOL WINAPI SetEndOfFile(HANDLE h) {
    int fd = file_fd(h);
    off_t pos;
    if (fd < 0) return FALSE;
    pos = lseek(fd, 0, SEEK_CUR);
    return ftruncate(fd, pos) == 0;
}

DWORD WINAPI GetFileType(HANDLE h) {
    struct stat st;
    int fd = file_fd(h);
    whandle_t *w = h_get(h, 0);
    if (w && w->kind == H_CONSOLE) return isatty(fd) ? 2 : (fstat(fd, &st) == 0 && S_ISFIFO(st.st_mode) ? 3 : 1);
    if (fd < 0 || fstat(fd, &st) != 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    if (S_ISCHR(st.st_mode)) return 2;   /* FILE_TYPE_CHAR */
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) return 3;    /* FILE_TYPE_PIPE */
    return 1;                            /* FILE_TYPE_DISK */
}

BOOL WINAPI GetFileTime(HANDLE h, FILETIME *c, FILETIME *a, FILETIME *m) {
    struct stat st;
    int fd = file_fd(h);
    if (fd < 0 || fstat(fd, &st) != 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (c) unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec, c);
    if (a) unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec, a);
    if (m) unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, m);
    return TRUE;
}

BOOL WINAPI SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a, const FILETIME *m) {
    (void)h, (void)c, (void)a, (void)m;
    return TRUE;
}

static DWORD attrs_of(const struct stat *st, const char *path) {
    DWORD a = 0;
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    if (S_ISDIR(st->st_mode)) a |= FILE_ATTRIBUTE_DIRECTORY;
    else a |= FILE_ATTRIBUTE_ARCHIVE;
    if (!(st->st_mode & 0222)) a |= FILE_ATTRIBUTE_READONLY;
    if (b[0] == '.' && b[1] && strcmp(b, "..")) a |= FILE_ATTRIBUTE_HIDDEN;
    return a;
}

DWORD WINAPI GetFileAttributesW(const WCHAR *name) {
    char wp[1024], up[1024];
    struct stat st;
    w_to_utf8(name, -1, wp, sizeof(wp));
    if (path_find(wp, up, sizeof(up)) != 0 || stat(up, &st) != 0) {
        wset_last_error(errno == ENOTDIR ? ERROR_PATH_NOT_FOUND : ERROR_FILE_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    return attrs_of(&st, up);
}

DWORD WINAPI GetFileAttributesA(const char *name) {
    WCHAR *w = utf8_to_w_dup(name);
    DWORD r = GetFileAttributesW(w);
    free(w);
    return r;
}

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

BOOL WINAPI GetFileAttributesExW(const WCHAR *name, int level, WIN32_FILE_ATTRIBUTE_DATA *d) {
    char wp[1024], up[1024];
    struct stat st;
    (void)level;
    w_to_utf8(name, -1, wp, sizeof(wp));
    if (path_find(wp, up, sizeof(up)) != 0 || stat(up, &st) != 0) {
        wset_last_error(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    d->dwFileAttributes = attrs_of(&st, up);
    unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec, &d->ftCreationTime);
    unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec, &d->ftLastAccessTime);
    unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, &d->ftLastWriteTime);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}

BOOL WINAPI GetFileAttributesExA(const char *name, int level, WIN32_FILE_ATTRIBUTE_DATA *d) {
    WCHAR *w = utf8_to_w_dup(name);
    BOOL r = GetFileAttributesExW(w, level, d);
    free(w);
    return r;
}

BOOL WINAPI SetFileAttributesW(const WCHAR *name, DWORD a) {
    (void)name, (void)a;
    return TRUE;
}

/* as Windows lays it out: the volume's serial number comes before the size */
typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION;
_Static_assert(sizeof(BY_HANDLE_FILE_INFORMATION) == 52, "BY_HANDLE_FILE_INFORMATION is 52 bytes");

BOOL WINAPI GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *i) {
    struct stat st;
    int fd = file_fd(h);
    whandle_t *w = h_get(h, H_FILE);
    if (fd < 0 || fstat(fd, &st) != 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    memset(i, 0, sizeof(*i));
    i->dwFileAttributes = attrs_of(&st, w ? ((wfileobj_t *)w->obj)->path : "");
    unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec, &i->ftCreationTime);
    unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec, &i->ftLastAccessTime);
    unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, &i->ftLastWriteTime);
    i->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    i->nFileSizeLow = (DWORD)st.st_size;
    i->dwVolumeSerialNumber = (DWORD)st.st_dev;
    i->nNumberOfLinks = (DWORD)st.st_nlink;
    i->nFileIndexHigh = (DWORD)((uint64_t)st.st_ino >> 32);
    i->nFileIndexLow = (DWORD)st.st_ino;
    return TRUE;
}

BOOL WINAPI GetFileInformationByHandleEx(HANDLE h, int cls, void *buf, DWORD size) {
    struct stat st;
    int fd = file_fd(h);
    if (fd < 0 || fstat(fd, &st) != 0) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (cls == 1 && size >= 24) {             /* FileStandardInfo */
        uint8_t *b = buf;
        memset(b, 0, 24);
        *(int64_t *)b = ((int64_t)st.st_blocks * 512);
        *(int64_t *)(b + 8) = st.st_size;
        *(DWORD *)(b + 16) = (DWORD)st.st_nlink;
        b[21] = S_ISDIR(st.st_mode);
        return TRUE;
    }
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI DeleteFileW(const WCHAR *name) {
    char wp[1024], up[1024];
    w_to_utf8(name, -1, wp, sizeof(wp));
    if (path_find(wp, up, sizeof(up)) != 0 || unlink(up) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DeleteFileA(const char *name) {
    WCHAR *w = utf8_to_w_dup(name);
    BOOL r = DeleteFileW(w);
    free(w);
    return r;
}

BOOL WINAPI MoveFileExW(const WCHAR *from, const WCHAR *to, DWORD flags) {
    char a[1024], b[1024], ua[1024], ub[1024];
    struct stat st;
    w_to_utf8(from, -1, a, sizeof(a));
    w_to_utf8(to, -1, b, sizeof(b));
    if (path_find(a, ua, sizeof(ua)) != 0 || path_find(b, ub, sizeof(ub)) != 0) {
        wset_last_error(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (!(flags & 1) && stat(ub, &st) == 0) {      /* MOVEFILE_REPLACE_EXISTING */
        wset_last_error(ERROR_ALREADY_EXISTS);
        return FALSE;
    }
    if (rename(ua, ub) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI MoveFileW(const WCHAR *from, const WCHAR *to) { return MoveFileExW(from, to, 0); }

BOOL WINAPI MoveFileA(const char *from, const char *to) {
    WCHAR *a = utf8_to_w_dup(from), *b = utf8_to_w_dup(to);
    BOOL r = MoveFileExW(a, b, 0);
    free(a), free(b);
    return r;
}

BOOL WINAPI CopyFileW(const WCHAR *from, const WCHAR *to, BOOL fail_if_exists) {
    char a[1024], b[1024], ua[1024], ub[1024], buf[65536];
    int in, out;
    ssize_t n;
    w_to_utf8(from, -1, a, sizeof(a));
    w_to_utf8(to, -1, b, sizeof(b));
    if (path_find(a, ua, sizeof(ua)) != 0 || path_find(b, ub, sizeof(ub)) != 0) return FALSE;
    in = open(ua, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        set_errno_error();
        return FALSE;
    }
    out = open(ub, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) {
        set_errno_error();
        close(in);
        return FALSE;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, (size_t)n) != n) break;
    close(in);
    close(out);
    return n == 0;
}

BOOL WINAPI CreateDirectoryW(const WCHAR *name, void *sa) {
    char wp[1024], up[1024];
    (void)sa;
    w_to_utf8(name, -1, wp, sizeof(wp));
    if (path_find(wp, up, sizeof(up)) != 0 || mkdir(up, 0755) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI CreateDirectoryA(const char *name, void *sa) {
    WCHAR *w = utf8_to_w_dup(name);
    BOOL r = CreateDirectoryW(w, sa);
    free(w);
    return r;
}

BOOL WINAPI RemoveDirectoryW(const WCHAR *name) {
    char wp[1024], up[1024];
    w_to_utf8(name, -1, wp, sizeof(wp));
    if (path_find(wp, up, sizeof(up)) != 0 || rmdir(up) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI RemoveDirectoryA(const char *name) {
    WCHAR *w = utf8_to_w_dup(name);
    BOOL r = RemoveDirectoryW(w);
    free(w);
    return r;
}

/* ---- finding files ----------------------------------------------------------------------- */

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR    cFileName[MAX_PATH];
    WCHAR    cAlternateFileName[14];
} WIN32_FIND_DATAW;

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    char     cFileName[MAX_PATH];
    char     cAlternateFileName[14];
} WIN32_FIND_DATAA;

typedef struct {
    DIR  *d;
    char  dir[1024];
    char  pattern[256];
} wfind_t;

static int find_next(wfind_t *f, WIN32_FIND_DATAW *out) {
    struct dirent *e;
    while ((e = readdir(f->d))) {
        char full[1400];
        struct stat st;
        /* "*.*" matches every name on Windows, also those without a dot */
        if (strcmp(f->pattern, "*.*") && strcmp(f->pattern, "*") && fnmatch(f->pattern, e->d_name, FNM_CASEFOLD) != 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", f->dir, e->d_name);
        memset(out, 0, sizeof(*out));
        if (stat(full, &st) == 0) {
            out->dwFileAttributes = attrs_of(&st, full);
            unix_to_filetime(st.st_ctim.tv_sec, st.st_ctim.tv_nsec, &out->ftCreationTime);
            unix_to_filetime(st.st_atim.tv_sec, st.st_atim.tv_nsec, &out->ftLastAccessTime);
            unix_to_filetime(st.st_mtim.tv_sec, st.st_mtim.tv_nsec, &out->ftLastWriteTime);
            out->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
            out->nFileSizeLow = (DWORD)st.st_size;
        } else {
            out->dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        }
        utf8_to_w(e->d_name, -1, out->cFileName, MAX_PATH);
        return 1;
    }
    return 0;
}

HANDLE WINAPI FindFirstFileExW(const WCHAR *spec, int level, WIN32_FIND_DATAW *data, int op, void *filter, DWORD flags) {
    char wp[1024], up[1024], *slash;
    wfind_t *f;
    HANDLE h;
    (void)level, (void)op, (void)filter, (void)flags;
    w_to_utf8(spec, -1, wp, sizeof(wp));
    f = calloc(1, sizeof(*f));
    /* the folder part is found case-blind; the last part is the pattern */
    {
        char *ws = strrchr(wp, '\\'), *fs = strrchr(wp, '/');
        char *sep = ws > fs ? ws : fs;
        char dirpart[1024];
        if (sep) {
            snprintf(f->pattern, sizeof(f->pattern), "%s", sep + 1);
            *sep = 0;
            snprintf(dirpart, sizeof(dirpart), "%s", wp[0] ? wp : "\\");
            if (strlen(wp) == 2 && wp[1] == ':') strcat(dirpart, "\\");
        } else {
            snprintf(f->pattern, sizeof(f->pattern), "%s", wp);
            snprintf(dirpart, sizeof(dirpart), ".");
        }
        if (path_find(dirpart, up, sizeof(up)) != 0) up[0] = 0;
    }
    slash = up;
    (void)slash;
    snprintf(f->dir, sizeof(f->dir), "%s", up[0] ? up : ".");
    f->d = opendir(f->dir);
    if (!f->d) {
        free(f);
        wset_last_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    if (!find_next(f, data)) {
        closedir(f->d);
        free(f);
        wset_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    h = h_new(H_FIND, f);
    return h ? h : INVALID_HANDLE_VALUE;
}

HANDLE WINAPI FindFirstFileW(const WCHAR *spec, WIN32_FIND_DATAW *data) { return FindFirstFileExW(spec, 0, data, 0, 0, 0); }

BOOL WINAPI FindNextFileW(HANDLE h, WIN32_FIND_DATAW *data) {
    whandle_t *w = h_get(h, H_FIND);
    if (!w) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!find_next(w->obj, data)) {
        wset_last_error(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    return TRUE;
}

static void find_a(const WIN32_FIND_DATAW *w, WIN32_FIND_DATAA *a) {
    memcpy(a, w, offsetof(WIN32_FIND_DATAA, cFileName));
    w_to_utf8(w->cFileName, -1, a->cFileName, MAX_PATH);
    a->cAlternateFileName[0] = 0;
}

HANDLE WINAPI FindFirstFileA(const char *spec, WIN32_FIND_DATAA *data) {
    WIN32_FIND_DATAW w;
    WCHAR *s = utf8_to_w_dup(spec);
    HANDLE h = FindFirstFileExW(s, 0, &w, 0, 0, 0);
    free(s);
    if (h != INVALID_HANDLE_VALUE) find_a(&w, data);
    return h;
}

BOOL WINAPI FindNextFileA(HANDLE h, WIN32_FIND_DATAA *data) {
    WIN32_FIND_DATAW w;
    if (!FindNextFileW(h, &w)) return FALSE;
    find_a(&w, data);
    return TRUE;
}

HANDLE WINAPI FindFirstFileExA(const char *spec, int level, WIN32_FIND_DATAA *data, int op, void *filter, DWORD flags) {
    WIN32_FIND_DATAW w;
    WCHAR *s = utf8_to_w_dup(spec);
    HANDLE h = FindFirstFileExW(s, level, &w, op, filter, flags);
    free(s);
    if (h != INVALID_HANDLE_VALUE) find_a(&w, data);
    return h;
}

BOOL WINAPI FindClose(HANDLE h) {
    whandle_t *w = h_get(h, H_FIND);
    if (!w) return FALSE;
    closedir(((wfind_t *)w->obj)->d);
    free(w->obj);
    h_close(h);
    return TRUE;
}

/* ---- folders --------------------------------------------------------------------------- */

DWORD WINAPI GetCurrentDirectoryW(DWORD n, WCHAR *buf) {
    char cwd[1024], wp[1024];
    int len;
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    path_to_win(cwd, wp, sizeof(wp));
    len = utf8_to_w(wp, -1, 0, 0);
    if ((DWORD)len > n || !buf) return (DWORD)len;
    utf8_to_w(wp, -1, buf, (int)n);
    return (DWORD)len - 1;
}

DWORD WINAPI GetCurrentDirectoryA(DWORD n, char *buf) {
    char cwd[1024], wp[1024];
    size_t len;
    if (!getcwd(cwd, sizeof(cwd))) return 0;
    path_to_win(cwd, wp, sizeof(wp));
    len = strlen(wp);
    if (len + 1 > n || !buf) return (DWORD)len + 1;
    memcpy(buf, wp, len + 1);
    return (DWORD)len;
}

BOOL WINAPI SetCurrentDirectoryW(const WCHAR *dir) {
    char wp[1024], up[1024];
    w_to_utf8(dir, -1, wp, sizeof(wp));
    if (path_find(wp, up, sizeof(up)) != 0 || chdir(up) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI SetCurrentDirectoryA(const char *dir) {
    WCHAR *w = utf8_to_w_dup(dir);
    BOOL r = SetCurrentDirectoryW(w);
    free(w);
    return r;
}

/* "x\..\y.txt" to "C:\dir\y.txt" */
static void full_path(const char *in, char *out, size_t cap) {
    char tmp[2048];
    char *parts[256];
    int np = 0;
    size_t o = 0;
    if (((in[0] | 32) >= 'a' && (in[0] | 32) <= 'z') && in[1] == ':') snprintf(tmp, sizeof(tmp), "%s", in);
    else if (in[0] == '\\' && in[1] == '\\') {
        snprintf(out, cap, "%s", in);
        return;
    } else {
        char cwd[1024], wcwd[1024];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        path_to_win(cwd, wcwd, sizeof(wcwd));
        if (in[0] == '\\' || in[0] == '/') snprintf(tmp, sizeof(tmp), "%c:%s", wcwd[0], in);
        else snprintf(tmp, sizeof(tmp), "%s\\%s", wcwd, in);
    }
    for (char *p = tmp; *p; p++)
        if (*p == '/') *p = '\\';
    {
        char *save = 0, *p = strtok_r(tmp + 2, "\\", &save);
        for (; p; p = strtok_r(0, "\\", &save)) {
            if (!strcmp(p, ".")) continue;
            if (!strcmp(p, "..")) {
                if (np) np--;
                continue;
            }
            if (np < 256) parts[np++] = p;
        }
    }
    if (cap < 4) return;
    out[o++] = (char)(tmp[0] >= 'a' && tmp[0] <= 'z' ? tmp[0] - 32 : tmp[0]);
    out[o++] = ':';
    for (int i = 0; i < np; i++) {
        size_t l = strlen(parts[i]);
        if (o + l + 2 >= cap) break;
        out[o++] = '\\';
        memcpy(out + o, parts[i], l);
        o += l;
    }
    if (np == 0) out[o++] = '\\';
    out[o] = 0;
}

DWORD WINAPI GetFullPathNameA(const char *in, DWORD n, char *buf, char **filepart) {
    char out[2048];
    size_t len;
    full_path(in, out, sizeof(out));
    len = strlen(out);
    if (len + 1 > n || !buf) return (DWORD)len + 1;
    memcpy(buf, out, len + 1);
    if (filepart) {
        char *s = strrchr(buf, '\\');
        *filepart = s && s[1] ? s + 1 : 0;
    }
    return (DWORD)len;
}

DWORD WINAPI GetFullPathNameW(const WCHAR *in, DWORD n, WCHAR *buf, WCHAR **filepart) {
    char a[2048], out[2048];
    int len;
    w_to_utf8(in, -1, a, sizeof(a));
    full_path(a, out, sizeof(out));
    len = utf8_to_w(out, -1, 0, 0);
    if ((DWORD)len > n || !buf) return (DWORD)len;
    utf8_to_w(out, -1, buf, (int)n);
    if (filepart) {
        WCHAR *s = 0;
        for (WCHAR *p = buf; *p; p++)
            if (*p == '\\') s = p;
        *filepart = s && s[1] ? s + 1 : 0;
    }
    return (DWORD)len - 1;
}

DWORD WINAPI GetLongPathNameW(const WCHAR *in, WCHAR *out, DWORD n) {
    size_t l = w_len(in);
    if (l + 1 > n || !out) return (DWORD)l + 1;
    memmove(out, in, (l + 1) * 2);
    return (DWORD)l;
}

DWORD WINAPI GetShortPathNameW(const WCHAR *in, WCHAR *out, DWORD n) { return GetLongPathNameW(in, out, n); }

static DWORD put_w(const char *s, WCHAR *buf, DWORD n) {
    int len = utf8_to_w(s, -1, 0, 0);
    if ((DWORD)len > n || !buf) return (DWORD)len;
    utf8_to_w(s, -1, buf, (int)n);
    return (DWORD)len - 1;
}

static DWORD put_a(const char *s, char *buf, DWORD n) {
    size_t len = strlen(s);
    if (len + 1 > n || !buf) return (DWORD)len + 1;
    memcpy(buf, s, len + 1);
    return (DWORD)len;
}

DWORD WINAPI GetTempPathW(DWORD n, WCHAR *buf) { return put_w("C:\\Windows\\Temp\\", buf, n); }
DWORD WINAPI GetTempPathA(DWORD n, char *buf) { return put_a("C:\\Windows\\Temp\\", buf, n); }
DWORD WINAPI GetTempPath2W(DWORD n, WCHAR *buf) { return GetTempPathW(n, buf); }

UINT WINAPI GetTempFileNameW(const WCHAR *dir, const WCHAR *prefix, UINT unique, WCHAR *out) {
    char d[1024], p[16], name[1100];
    static unsigned counter;
    w_to_utf8(dir, -1, d, sizeof(d));
    w_to_utf8(prefix ? prefix : (const WCHAR *)u"tmp", -1, p, 4);
    if (!unique) unique = (UINT)(getpid() * 31 + (int)++counter) & 0xFFFF;
    snprintf(name, sizeof(name), "%s%s%s%04X.tmp", d, d[0] && d[strlen(d) - 1] != '\\' ? "\\" : "", p, unique);
    utf8_to_w(name, -1, out, MAX_PATH);
    {
        HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
        if (h != INVALID_HANDLE_VALUE) {
            whandle_t *w = h_get(h, H_FILE);
            if (w) close(((wfileobj_t *)w->obj)->fd), free(w->obj);
            h_close(h);
        }
    }
    return unique;
}

UINT WINAPI GetSystemDirectoryW(WCHAR *buf, UINT n) { return put_w("C:\\Windows\\System32", buf, n); }
UINT WINAPI GetSystemDirectoryA(char *buf, UINT n) { return put_a("C:\\Windows\\System32", buf, n); }
UINT WINAPI GetWindowsDirectoryW(WCHAR *buf, UINT n) { return put_w("C:\\Windows", buf, n); }
UINT WINAPI GetWindowsDirectoryA(char *buf, UINT n) { return put_a("C:\\Windows", buf, n); }
UINT WINAPI GetSystemWindowsDirectoryW(WCHAR *buf, UINT n) { return put_w("C:\\Windows", buf, n); }
UINT WINAPI GetSystemWow64DirectoryW(WCHAR *buf, UINT n) {
    (void)buf, (void)n;
    wset_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    return 0;
}

UINT WINAPI GetDriveTypeW(const WCHAR *root) {
    (void)root;
    return 3;     /* DRIVE_FIXED */
}
UINT WINAPI GetDriveTypeA(const char *root) {
    (void)root;
    return 3;
}
DWORD WINAPI GetLogicalDrives(void) { return (1u << 2) | (1u << 25); }   /* C: and Z: */

DWORD WINAPI GetLogicalDriveStringsW(DWORD n, WCHAR *buf) {
    static const WCHAR s[] = u"C:\\\0Z:\\\0";
    if (n < 9) return 9;
    memcpy(buf, s, sizeof(s));
    return 8;
}

BOOL WINAPI GetDiskFreeSpaceExW(const WCHAR *dir, uint64_t *avail, uint64_t *total, uint64_t *freeb) {
    char wp[1024] = "C:\\", up[1024];
    struct statvfs v;
    if (dir) w_to_utf8(dir, -1, wp, sizeof(wp));
    if (path_find(wp, up, sizeof(up)) != 0 || statvfs(up, &v) != 0) {
        if (avail) *avail = 1ull << 34;
        if (total) *total = 1ull << 36;
        if (freeb) *freeb = 1ull << 34;
        return TRUE;
    }
    if (avail) *avail = (uint64_t)v.f_bavail * v.f_frsize;
    if (total) *total = (uint64_t)v.f_blocks * v.f_frsize;
    if (freeb) *freeb = (uint64_t)v.f_bfree * v.f_frsize;
    return TRUE;
}

BOOL WINAPI GetDiskFreeSpaceW(const WCHAR *dir, DWORD *spc, DWORD *bps, DWORD *freec, DWORD *totalc) {
    uint64_t a, t, f;
    GetDiskFreeSpaceExW(dir, &a, &t, &f);
    if (spc) *spc = 8;
    if (bps) *bps = 512;
    if (freec) *freec = (DWORD)(f / 4096 > 0xFFFFFFFF ? 0xFFFFFFFF : f / 4096);
    if (totalc) *totalc = (DWORD)(t / 4096 > 0xFFFFFFFF ? 0xFFFFFFFF : t / 4096);
    return TRUE;
}

BOOL WINAPI GetVolumeInformationW(const WCHAR *root, WCHAR *name, DWORD nn, DWORD *serial, DWORD *maxlen, DWORD *flags,
                                  WCHAR *fs, DWORD nfs) {
    (void)root;
    if (name && nn) put_w("ICDA", name, nn);
    if (serial) *serial = 0x1CDA2026;
    if (maxlen) *maxlen = 255;
    if (flags) *flags = 0x3;     /* case-preserved, case-sensitive names */
    if (fs && nfs) put_w("NTFS", fs, nfs);
    return TRUE;
}

BOOL WINAPI DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD nin, void *out, DWORD nout, DWORD *ret, OVERLAPPED *ov) {
    (void)h, (void)code, (void)in, (void)nin, (void)out, (void)nout, (void)ov;
    if (ret) *ret = 0;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI PeekNamedPipe(HANDLE h, void *buf, DWORD n, DWORD *read_, DWORD *avail, DWORD *left) {
    (void)h, (void)buf, (void)n;
    if (read_) *read_ = 0;
    if (avail) *avail = 0;
    if (left) *left = 0;
    return TRUE;
}

BOOL WINAPI LockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) {
    (void)h, (void)a, (void)b, (void)c, (void)d;
    return TRUE;
}
BOOL WINAPI UnlockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) {
    (void)h, (void)a, (void)b, (void)c, (void)d;
    return TRUE;
}
BOOL WINAPI LockFileEx(HANDLE h, DWORD f, DWORD r, DWORD lo, DWORD hi, OVERLAPPED *ov) {
    (void)h, (void)f, (void)r, (void)lo, (void)hi, (void)ov;
    return TRUE;
}
BOOL WINAPI UnlockFileEx(HANDLE h, DWORD r, DWORD lo, DWORD hi, OVERLAPPED *ov) {
    (void)h, (void)r, (void)lo, (void)hi, (void)ov;
    return TRUE;
}

/* ---- file mappings (sections) ---------------------------------------------------------------
 *
 * A view is an mmap of a descriptor.  A section of memory alone (no file)
 * is a memfd the section's size, so every view of it is the same memory, as
 * Windows has it (Chrome maps one region twice: to write, and read-only).
 * Handles to a section, duplicated or opened by name, share it; it goes
 * with the last, while its views stay.  The views are kept so VirtualQuery
 * can say how large one is and UnmapViewOfFile can unmap it. */
typedef struct wmapping {
    int              fd;
    DWORD            prot;
    uint64_t         size;
    int              refs;
    WCHAR            name[128];
    struct wmapping *next;      /* the named ones */
} wmapping_t;

typedef struct {
    uint8_t *base;
    size_t   size;
    int      write, exec;
} wview_t;

static pthread_mutex_t maplock = PTHREAD_MUTEX_INITIALIZER;
static wmapping_t *named_maps;
static wview_t *views;
static int nviews, cviews;

static void mapping_unref(wmapping_t *m) {
    pthread_mutex_lock(&maplock);
    if (--m->refs > 0) {
        pthread_mutex_unlock(&maplock);
        return;
    }
    for (wmapping_t **pp = &named_maps; *pp; pp = &(*pp)->next)
        if (*pp == m) {
            *pp = m->next;
            break;
        }
    pthread_mutex_unlock(&maplock);
    if (m->fd >= 0) close(m->fd);
    free(m);
}

static void mapping_ref(wmapping_t *m) {
    pthread_mutex_lock(&maplock);
    m->refs++;
    pthread_mutex_unlock(&maplock);
}

/* memory for a section without a file: a memfd, else an unlinked file in /tmp */
static int section_fd(uint64_t size) {
    int fd = memfd_create("icda-section", MFD_CLOEXEC);
    if (fd < 0) {
        char path[] = "/tmp/icda-section-XXXXXX";
        fd = mkstemp(path);
        if (fd >= 0) unlink(path);
    }
    if (fd >= 0 && ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

HANDLE WINAPI CreateFileMappingW(HANDLE file, void *sa, DWORD prot, DWORD hi, DWORD lo, const WCHAR *name) {
    wmapping_t *m;
    uint64_t size = (uint64_t)hi << 32 | lo;
    (void)sa;
    if (name && *name) {
        pthread_mutex_lock(&maplock);
        for (m = named_maps; m; m = m->next)
            if (!w_icmp(m->name, name)) break;
        if (m) {
            m->refs++;
            pthread_mutex_unlock(&maplock);
            wset_last_error(ERROR_ALREADY_EXISTS);
            return h_new(H_MAPPING, m);
        }
        pthread_mutex_unlock(&maplock);
    }
    m = calloc(1, sizeof(*m));
    m->refs = 1;
    m->prot = prot;
    m->size = size;
    if (file == INVALID_HANDLE_VALUE || !file) {
        if (!size || (m->fd = section_fd(size)) < 0) {
            free(m);
            wset_last_error(size ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
            return 0;
        }
    } else {
        int fd = file_fd(file);
        struct stat st;
        m->fd = fd >= 0 ? dup(fd) : -1;
        /* a writable mapping larger than its file makes the file that large */
        if (m->fd >= 0 && size && (prot & 0xCC) && fstat(m->fd, &st) == 0 && (uint64_t)st.st_size < size)
            if (ftruncate(m->fd, (off_t)size) != 0) wlog("CreateFileMapping: the file could not grow");
    }
    if (name && *name) {
        size_t n = w_len(name) < 127 ? w_len(name) : 127;
        memcpy(m->name, name, n * sizeof(WCHAR));
        pthread_mutex_lock(&maplock);
        m->next = named_maps;
        named_maps = m;
        pthread_mutex_unlock(&maplock);
    }
    wset_last_error(0);
    return h_new(H_MAPPING, m);
}

HANDLE WINAPI CreateFileMappingA(HANDLE file, void *sa, DWORD prot, DWORD hi, DWORD lo, const char *name) {
    WCHAR w[128];
    if (name) utf8_to_w(name, -1, w, 128);
    return CreateFileMappingW(file, sa, prot, hi, lo, name ? w : 0);
}

HANDLE WINAPI OpenFileMappingW(DWORD access, BOOL inherit, const WCHAR *name) {
    wmapping_t *m;
    (void)access, (void)inherit;
    pthread_mutex_lock(&maplock);
    for (m = named_maps; m && name; m = m->next)
        if (!w_icmp(m->name, name)) break;
    if (m) m->refs++;
    pthread_mutex_unlock(&maplock);
    if (!m) {
        wset_last_error(ERROR_FILE_NOT_FOUND);
        return 0;
    }
    return h_new(H_MAPPING, m);
}

HANDLE WINAPI OpenFileMappingA(DWORD access, BOOL inherit, const char *name) {
    WCHAR w[128];
    if (!name) return OpenFileMappingW(access, inherit, 0);
    utf8_to_w(name, -1, w, 128);
    return OpenFileMappingW(access, inherit, w);
}

void *WINAPI MapViewOfFile(HANDLE h, DWORD access, DWORD hi, DWORD lo, SIZE_T n) {
    whandle_t *w = h_get(h, H_MAPPING);
    wmapping_t *m;
    int prot = PROT_READ;
    void *p;
    uint64_t off = (uint64_t)hi << 32 | lo;
    if (!w) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    m = w->obj;
    if (access & 2) prot |= PROT_WRITE;   /* FILE_MAP_WRITE */
    if (access & 0x20) prot |= PROT_EXEC; /* FILE_MAP_EXECUTE */
    if (!n) {
        struct stat st;
        uint64_t total = m->size ? m->size : (fstat(m->fd, &st) == 0 ? (uint64_t)st.st_size : 0);
        n = total > off ? (SIZE_T)(total - off) : 0;
    }
    if (!n || m->fd < 0) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    p = mmap(0, n, prot, (access & 1) ? MAP_PRIVATE : MAP_SHARED, m->fd, (off_t)off);    /* FILE_MAP_COPY */
    if (p == MAP_FAILED) {
        set_errno_error();
        return 0;
    }
    pthread_mutex_lock(&maplock);
    if (nviews == cviews) {
        int nc = cviews ? cviews * 2 : 64;
        wview_t *nv = realloc(views, (size_t)nc * sizeof(*nv));
        if (nv) views = nv, cviews = nc;
    }
    if (nviews < cviews) {
        views[nviews].base = p;
        views[nviews].size = (n + 4095) & ~(size_t)4095;
        views[nviews].write = (prot & PROT_WRITE) != 0;
        views[nviews].exec = (prot & PROT_EXEC) != 0;
        nviews++;
    }
    pthread_mutex_unlock(&maplock);
    return p;
}

void *WINAPI MapViewOfFileEx(HANDLE h, DWORD access, DWORD hi, DWORD lo, SIZE_T n, void *at) {
    (void)at;
    return MapViewOfFile(h, access, hi, lo, n);
}

BOOL WINAPI UnmapViewOfFile(const void *p) {
    pthread_mutex_lock(&maplock);
    for (int i = 0; i < nviews; i++) {
        if (views[i].base != p) continue;
        munmap(views[i].base, views[i].size);
        views[i] = views[--nviews];
        pthread_mutex_unlock(&maplock);
        return TRUE;
    }
    pthread_mutex_unlock(&maplock);
    wset_last_error(487);      /* ERROR_INVALID_ADDRESS */
    return FALSE;
}

BOOL WINAPI FlushViewOfFile(const void *p, SIZE_T n) {
    (void)p, (void)n;
    return TRUE;
}


BOOL WINAPI GetVolumePathNameW(const WCHAR *path, WCHAR *out, DWORD n) {
    char a[1100], full[2048];
    WCHAR root[4] = { 'C', ':', '\\', 0 };
    w_to_utf8(path, -1, a, sizeof(a));
    full_path(a, full, sizeof(full));
    root[0] = (WCHAR)full[0];
    if (n < 4) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(out, root, sizeof(root));
    return TRUE;
}

BOOL WINAPI GetVolumeNameForVolumeMountPointW(const WCHAR *mp, WCHAR *out, DWORD n) {
    (void)mp;
    if (put_w("\\\\?\\Volume{1cda0000-0000-0000-0000-000000000001}\\", out, n) >= n) {
        wset_last_error(206);     /* ERROR_FILENAME_EXCED_RANGE */
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI GetFinalPathNameByHandleW(HANDLE h, WCHAR *out, DWORD n, DWORD flags) {
    whandle_t *w = h_get(h, H_FILE);
    char wp[1100], full[1200];
    (void)flags;
    if (!w || !((wfileobj_t *)w->obj)->path[0]) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    path_to_win(((wfileobj_t *)w->obj)->path, wp, sizeof(wp));
    snprintf(full, sizeof(full), "\\\\?\\%s", wp);
    return put_w(full, out, n);
}

BOOL WINAPI SetProcessWorkingSetSize(HANDLE h, SIZE_T lo, SIZE_T hi) {
    (void)h, (void)lo, (void)hi;
    return TRUE;
}
BOOL WINAPI SetProcessWorkingSetSizeEx(HANDLE h, SIZE_T lo, SIZE_T hi, DWORD f) {
    (void)h, (void)lo, (void)hi, (void)f;
    return TRUE;
}

/* SetFileInformationByHandle: renaming, deleting, sizing a file by its handle */
BOOL WINAPI SetFileInformationByHandle(HANDLE h, int cls, void *info, DWORD size) {
    whandle_t *w = h_get(h, H_FILE);
    wfileobj_t *f;
    (void)size;
    if (!w) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    f = w->obj;
    switch (cls) {
    case 0:                          /* FileBasicInfo: times and attributes */
        return TRUE;
    case 3: {                        /* FileRenameInfo */
        uint8_t *ri = info;
        DWORD len = *(DWORD *)(ri + 16);
        WCHAR *name = (WCHAR *)(ri + 20);
        char a[1100], up[1100];
        WCHAR *tmp = calloc(len / 2 + 1, sizeof(WCHAR));
        struct stat st;
        memcpy(tmp, name, len);
        w_to_utf8(tmp, -1, a, sizeof(a));
        free(tmp);
        if (path_find(a, up, sizeof(up)) != 0) {
            wset_last_error(ERROR_PATH_NOT_FOUND);
            return FALSE;
        }
        if (!ri[0] && stat(up, &st) == 0) {      /* ReplaceIfExists */
            wset_last_error(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
        if (rename(f->path, up) != 0) {
            set_errno_error();
            return FALSE;
        }
        snprintf(f->path, sizeof(f->path), "%s", up);
        return TRUE;
    }
    case 4:                          /* FileDispositionInfo: deleted when the handle closes (and can be taken back) */
        f->delete_on_close = *(BOOLEAN *)info != 0;
        return TRUE;
    case 21: {                       /* FileDispositionInfoEx */
        DWORD fl = *(DWORD *)info;
        if ((fl & 1) && (fl & 2) && f->path[0]) {      /* DELETE with POSIX_SEMANTICS: now */
            if (f->is_dir ? rmdir(f->path) : unlink(f->path)) {
                set_errno_error();
                return FALSE;
            }
            return TRUE;
        }
        f->delete_on_close = (fl & 1) != 0;
        return TRUE;
    }
    case 5:                          /* FileAllocationInfo */
        return TRUE;
    case 6:                          /* FileEndOfFileInfo */
        return ftruncate(f->fd, (off_t)*(int64_t *)info) == 0;
    default:
        wset_last_error(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
}

BOOL WINAPI CreateHardLinkW(const WCHAR *to, const WCHAR *from, void *sa) {
    char a[1100], b[1100], ua[1100], ub[1100];
    (void)sa;
    w_to_utf8(to, -1, a, sizeof(a));
    w_to_utf8(from, -1, b, sizeof(b));
    if (path_find(a, ua, sizeof(ua)) != 0 || path_find(b, ub, sizeof(ub)) != 0 || link(ub, ua) != 0) {
        set_errno_error();
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI ReplaceFileW(const WCHAR *replaced, const WCHAR *replacement, const WCHAR *backup, DWORD f, void *e, void *r) {
    (void)f, (void)e, (void)r;
    if (backup) CopyFileW(replaced, backup, FALSE);
    return MoveFileExW(replacement, replaced, 1);
}

BOOL WINAPI CopyFileExW(const WCHAR *from, const WCHAR *to, void *progress, void *data, BOOL *cancel, DWORD flags) {
    (void)progress, (void)data, (void)cancel;
    return CopyFileW(from, to, (flags & 1) != 0);       /* COPY_FILE_FAIL_IF_EXISTS */
}

BOOL WINAPI MoveFileWithProgressW(const WCHAR *from, const WCHAR *to, void *p, void *d, DWORD flags) {
    (void)p, (void)d;
    return MoveFileExW(from, to, flags);
}


HANDLE WINAPI CreateMailslotW(const WCHAR *name, DWORD max, DWORD to, void *sa) {
    (void)name, (void)max, (void)to, (void)sa;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
}
HANDLE WINAPI FindFirstChangeNotificationW(const WCHAR *p, BOOL sub, DWORD f) {
    (void)p, (void)sub, (void)f;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI GetProcessMitigationPolicy(HANDLE h, int policy, void *buf, SIZE_T n) {
    (void)h, (void)policy;
    if (buf) memset(buf, 0, n);
    return TRUE;
}
BOOL WINAPI SetProcessMitigationPolicy(int policy, void *buf, SIZE_T n) {
    (void)policy, (void)buf, (void)n;
    return TRUE;
}
BOOL WINAPI SetThreadInformation(HANDLE h, int cls, void *info, DWORD n) {
    (void)h, (void)cls, (void)info, (void)n;
    return TRUE;
}
BOOL WINAPI SetProcessInformation(HANDLE h, int cls, void *info, DWORD n) {
    (void)h, (void)cls, (void)info, (void)n;
    return TRUE;
}
/* "en-US\0\0": the languages the user wants */
static BOOL ui_languages(DWORD flags, ULONG *num, WCHAR *buf, ULONG *n) {
    static const WCHAR langs[] = u"en-US\0";
    (void)flags;
    if (num) *num = 1;
    if (!buf || *n < 7) {
        *n = 7;
        if (buf) {
            wset_last_error(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        return TRUE;
    }
    memcpy(buf, langs, sizeof(langs));
    *n = 7;
    return TRUE;
}
BOOL WINAPI GetThreadPreferredUILanguages(DWORD f, ULONG *num, WCHAR *buf, ULONG *n) { return ui_languages(f, num, buf, n); }
BOOL WINAPI GetUserPreferredUILanguages(DWORD f, ULONG *num, WCHAR *buf, ULONG *n) { return ui_languages(f, num, buf, n); }
BOOL WINAPI GetSystemPreferredUILanguages(DWORD f, ULONG *num, WCHAR *buf, ULONG *n) { return ui_languages(f, num, buf, n); }
BOOL WINAPI GetProcessPreferredUILanguages(DWORD f, ULONG *num, WCHAR *buf, ULONG *n) { return ui_languages(f, num, buf, n); }
BOOL WINAPI SetProcessPreferredUILanguages(DWORD f, const WCHAR *l, ULONG *num) {
    (void)f, (void)l;
    if (num) *num = 1;
    return TRUE;
}

/* ---- closing and duplicating handles ------------------------------------------------------ */

BOOL WINAPI CloseHandle(HANDLE h) {
    whandle_t *w = h_get(h, 0);
    if (!w) {
        if (h == (HANDLE)(intptr_t)-1 || h == (HANDLE)(intptr_t)-2) return TRUE;
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    switch (w->kind) {
    case H_FILE: {
        wfileobj_t *f = w->obj;
        if (f->delete_on_close && f->path[0]) {
            if (f->is_dir) rmdir(f->path);
            else unlink(f->path);
        }
        if (f->fd > 2) close(f->fd);
        if (f->listen_fd >= 0) {
            close(f->listen_fd);
            if (f->pipe_name[0]) unlink(f->pipe_name);
        }
        free(f);
        break;
    }
    case H_MAPPING:
        mapping_unref(w->obj);
        break;
    case H_CONSOLE:
        return TRUE;    /* the standard handles stay */
    case H_FIND:
        return FindClose(h);
    case H_THREAD:
        break;          /* the thread object stays: it may still run */
    case H_EVENT: case H_MUTEX: case H_SEMAPHORE:
        named_ref(w->obj, -1);
        break;
    default:
        break;
    }
    h_close(h);
    return TRUE;
}

BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE *dst, DWORD access, BOOL inherit, DWORD opts) {
    whandle_t *w;
    (void)sp, (void)tp, (void)access, (void)inherit;
    if (src == (HANDLE)(intptr_t)-2 || src == (HANDLE)(intptr_t)-1) {
        /* a real handle for the current thread */
        if (src == (HANDLE)(intptr_t)-2) {
            *dst = h_new(H_THREAD, thread_current());
            return *dst != 0;
        }
        *dst = src;
        return TRUE;
    }
    w = h_get(src, 0);
    if (!w) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (w->kind == H_FILE) {
        wfileobj_t *f = w->obj;
        *dst = file_handle(dup(f->fd), f->path, f->is_dir);
    } else if (w->kind == H_MAPPING) {
        mapping_ref(w->obj);
        *dst = h_new(H_MAPPING, w->obj);
    } else {
        *dst = h_new(w->kind, w->obj);
        if (*dst && (w->kind == H_EVENT || w->kind == H_MUTEX || w->kind == H_SEMAPHORE)) named_ref(w->obj, 1);
    }
    if (opts & 1) CloseHandle(src);      /* DUPLICATE_CLOSE_SOURCE */
    return *dst != 0;
}

BOOL WINAPI GetHandleInformation(HANDLE h, DWORD *flags) {
    (void)h;
    if (flags) *flags = 0;
    return TRUE;
}
BOOL WINAPI SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) {
    (void)h, (void)mask, (void)flags;
    return TRUE;
}

/* ---- consoles --------------------------------------------------------------------------- */

static HANDLE std_handles[3];

HANDLE h_std(int which) {
    if (which < 0 || which > 2) return INVALID_HANDLE_VALUE;
    if (!std_handles[which]) std_handles[which] = h_new(H_CONSOLE, (void *)(intptr_t)which);
    return std_handles[which];
}

HANDLE WINAPI GetStdHandle(DWORD which) {
    switch (which) {
    case (DWORD)-10: return h_std(0);
    case (DWORD)-11: return h_std(1);
    case (DWORD)-12: return h_std(2);
    default: wset_last_error(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE;
    }
}

BOOL WINAPI SetStdHandle(DWORD which, HANDLE h) {
    int i = which == (DWORD)-10 ? 0 : which == (DWORD)-11 ? 1 : which == (DWORD)-12 ? 2 : -1;
    if (i < 0) return FALSE;
    std_handles[i] = h;
    return TRUE;
}

BOOL WINAPI WriteConsoleW(HANDLE h, const WCHAR *s, DWORD n, DWORD *put, void *res) {
    char *buf = malloc((size_t)n * 3 + 4);
    int len, fd = file_fd(h);
    (void)res;
    if (!buf) return FALSE;
    len = w_to_utf8(s, (int)n, buf, (int)n * 3 + 4) - 1;
    if (fd >= 0 && len > 0) (void)!write(fd, buf, (size_t)len);
    free(buf);
    if (put) *put = n;
    return TRUE;
}

BOOL WINAPI WriteConsoleA(HANDLE h, const char *s, DWORD n, DWORD *put, void *res) {
    int fd = file_fd(h);
    (void)res;
    if (fd >= 0) (void)!write(fd, s, n);
    if (put) *put = n;
    return TRUE;
}

BOOL WINAPI ReadConsoleW(HANDLE h, WCHAR *buf, DWORD n, DWORD *got, void *ctl) {
    char tmp[1024];
    int fd = file_fd(h);
    ssize_t r;
    (void)ctl;
    if (fd < 0) return FALSE;
    r = read(fd, tmp, n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1);
    if (r < 0) return FALSE;
    tmp[r] = 0;
    *got = (DWORD)utf8_to_w(tmp, (int)r, buf, (int)n) - 1;
    return TRUE;
}

BOOL WINAPI ReadConsoleA(HANDLE h, char *buf, DWORD n, DWORD *got, void *ctl) {
    int fd = file_fd(h);
    ssize_t r;
    (void)ctl;
    if (fd < 0) return FALSE;
    r = read(fd, buf, n);
    if (r < 0) return FALSE;
    *got = (DWORD)r;
    return TRUE;
}

BOOL WINAPI GetConsoleMode(HANDLE h, DWORD *mode) {
    int fd = file_fd(h);
    whandle_t *w = h_get(h, H_CONSOLE);
    if (!w || fd < 0 || !isatty(fd)) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    *mode = 0x7;
    return TRUE;
}

BOOL WINAPI SetConsoleMode(HANDLE h, DWORD mode) {
    (void)h, (void)mode;
    return TRUE;
}
BOOL WINAPI AllocConsole(void) { return TRUE; }
BOOL WINAPI FreeConsole(void) { return TRUE; }
BOOL WINAPI AttachConsole(DWORD pid) {
    (void)pid;
    return TRUE;
}
HWND WINAPI GetConsoleWindow(void) { return 0; }
BOOL WINAPI SetConsoleTitleW(const WCHAR *t) {
    (void)t;
    return TRUE;
}
BOOL WINAPI SetConsoleTitleA(const char *t) {
    (void)t;
    return TRUE;
}
DWORD WINAPI GetConsoleTitleW(WCHAR *buf, DWORD n) {
    if (n) buf[0] = 0;
    return 0;
}
BOOL WINAPI SetConsoleCtrlHandler(void *fn, BOOL add) {
    (void)fn, (void)add;
    return TRUE;
}
UINT WINAPI GetConsoleCP(void) { return 65001; }
UINT WINAPI GetConsoleOutputCP(void) { return 65001; }
BOOL WINAPI SetConsoleCP(UINT cp) {
    (void)cp;
    return TRUE;
}
BOOL WINAPI SetConsoleOutputCP(UINT cp) {
    (void)cp;
    return TRUE;
}
BOOL WINAPI SetConsoleTextAttribute(HANDLE h, WORD a) {
    (void)h, (void)a;
    return TRUE;
}
BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, void *info) {
    (void)h;
    memset(info, 0, 22);
    ((SHORT *)info)[0] = 80, ((SHORT *)info)[1] = 25;
    ((SHORT *)info)[7] = 79, ((SHORT *)info)[8] = 24;
    ((SHORT *)info)[9] = 80, ((SHORT *)info)[10] = 25;
    return TRUE;
}
BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, DWORD *n) {
    (void)h;
    *n = 0;
    return TRUE;
}
BOOL WINAPI ReadConsoleInputA(HANDLE h, void *rec, DWORD n, DWORD *got) {
    (void)h, (void)rec, (void)n;
    *got = 0;
    return TRUE;
}
BOOL WINAPI ReadConsoleInputW(HANDLE h, void *rec, DWORD n, DWORD *got) { return ReadConsoleInputA(h, rec, n, got); }
BOOL WINAPI PeekConsoleInputW(HANDLE h, void *rec, DWORD n, DWORD *got) { return ReadConsoleInputA(h, rec, n, got); }
BOOL WINAPI FlushConsoleInputBuffer(HANDLE h) {
    (void)h;
    return TRUE;
}

void WINAPI OutputDebugStringA(const char *s) {
    if (wtrace && s) wlog("debug: %s", s);
}

void WINAPI OutputDebugStringW(const WCHAR *s) {
    if (wtrace && s) {
        char *a = w_to_utf8_dup(s);
        wlog("debug: %s", a);
        free(a);
    }
}

/* ---- memory ----------------------------------------------------------------------------- */

#define MEM_COMMIT   0x1000
#define MEM_RESERVE  0x2000
#define MEM_DECOMMIT 0x4000
#define MEM_RELEASE  0x8000
#define MEM_FREE     0x10000
#define MEM_PRIVATE  0x20000
#define MEM_IMAGE    0x1000000
#define MEM_RESET    0x80000
#define MEM_TOP_DOWN 0x100000

static int prot_of(DWORD p) {
    switch (p & 0xFF) {
    case 0x01: return PROT_NONE;                          /* PAGE_NOACCESS */
    case 0x02: return PROT_READ;                          /* PAGE_READONLY */
    case 0x04: case 0x08: return PROT_READ | PROT_WRITE;  /* READWRITE, WRITECOPY */
    case 0x10: return PROT_EXEC;                          /* EXECUTE */
    case 0x20: return PROT_READ | PROT_EXEC;              /* EXECUTE_READ */
    case 0x40: case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}

/* what VirtualAlloc made, so VirtualQuery and MEM_RELEASE know sizes */
typedef struct vregion {
    struct vregion *next;
    uint8_t        *base;
    size_t          size;
    DWORD           prot;
    uint8_t        *committed;    /* a page map: 1 committed */
} vregion_t;

static vregion_t *vregions;
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;

static vregion_t *vfind(const void *p) {
    for (vregion_t *r = vregions; r; r = r->next)
        if ((const uint8_t *)p >= r->base && (const uint8_t *)p < r->base + r->size) return r;
    return 0;
}

void *WINAPI VirtualAlloc(void *addr, SIZE_T size, DWORD type, DWORD prot) {
    uint8_t *p;
    vregion_t *r;
    size_t pages;
    if (!size) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&vlock);
    if (addr && (r = vfind(addr)) != 0) {
        /* committing (or resetting) part of a reservation */
        uint8_t *start = (uint8_t *)((uintptr_t)addr & ~(uintptr_t)4095);
        uint8_t *end = (uint8_t *)(((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095);
        if (end > r->base + r->size) end = r->base + r->size;
        if (type & MEM_COMMIT) {
            mprotect(start, (size_t)(end - start), prot_of(prot));
            for (uint8_t *q = start; q < end; q += 4096) r->committed[(q - r->base) >> 12] = 1;
        } else if (type & MEM_RESET) {
            madvise(start, (size_t)(end - start), MADV_DONTNEED);
        }
        pthread_mutex_unlock(&vlock);
        return start;
    }
    size = (size + 0xFFFF) & ~(SIZE_T)0xFFFF;       /* the 64 KB granularity */
    if (addr) {
        p = mmap((void *)((uintptr_t)addr & ~(uintptr_t)0xFFFF), size, (type & MEM_COMMIT) ? prot_of(prot) : PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE_COMPAT, -1, 0);
        if (p != MAP_FAILED && p != (uint8_t *)((uintptr_t)addr & ~(uintptr_t)0xFFFF)) {
            munmap(p, size);
            p = MAP_FAILED;
        }
    } else {
        /* 64 KB aligned, as Windows gives */
        uint8_t *raw = mmap(0, size + 0x10000, (type & MEM_COMMIT) ? prot_of(prot) : PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (raw == MAP_FAILED) p = MAP_FAILED;
        else {
            uint8_t *al = (uint8_t *)(((uintptr_t)raw + 0xFFFF) & ~(uintptr_t)0xFFFF);
            if (al > raw) munmap(raw, (size_t)(al - raw));
            if (al + size < raw + size + 0x10000) munmap(al + size, (size_t)((raw + size + 0x10000) - (al + size)));
            p = al;
        }
    }
    if (p == MAP_FAILED) {
        pthread_mutex_unlock(&vlock);
        wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    r = calloc(1, sizeof(*r));
    pages = size >> 12;
    r->base = p;
    r->size = size;
    r->prot = prot;
    r->committed = calloc(pages, 1);
    if (type & MEM_COMMIT) memset(r->committed, 1, pages);
    r->next = vregions;
    vregions = r;
    pthread_mutex_unlock(&vlock);
    return p;
}

BOOL WINAPI VirtualFree(void *addr, SIZE_T size, DWORD type) {
    vregion_t *r, **pp;
    pthread_mutex_lock(&vlock);
    r = vfind(addr);
    if (!r) {
        pthread_mutex_unlock(&vlock);
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (type & MEM_RELEASE) {
        munmap(r->base, r->size);
        for (pp = &vregions; *pp; pp = &(*pp)->next)
            if (*pp == r) {
                *pp = r->next;
                break;
            }
        free(r->committed);
        free(r);
    } else if (type & MEM_DECOMMIT) {
        uint8_t *start = (uint8_t *)((uintptr_t)addr & ~(uintptr_t)4095);
        uint8_t *end = size ? (uint8_t *)(((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095) : r->base + r->size;
        if (end > r->base + r->size) end = r->base + r->size;
        mmap(start, (size_t)(end - start), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        for (uint8_t *q = start; q < end; q += 4096) r->committed[(q - r->base) >> 12] = 0;
    }
    pthread_mutex_unlock(&vlock);
    return TRUE;
}

BOOL WINAPI VirtualProtect(void *addr, SIZE_T size, DWORD prot, DWORD *old) {
    uint8_t *start = (uint8_t *)((uintptr_t)addr & ~(uintptr_t)4095);
    uint8_t *end = (uint8_t *)(((uintptr_t)addr + size + 4095) & ~(uintptr_t)4095);
    vregion_t *r;
    pthread_mutex_lock(&vlock);
    r = vfind(addr);
    if (old) *old = r ? r->prot : 0x04;
    if (r) r->prot = prot;
    pthread_mutex_unlock(&vlock);
    if (mprotect(start, (size_t)(end - start), prot_of(prot)) != 0) {
        /* pages not mapped by us (an image's): still say yes for read/write */
        if (old) *old = 0x04;
    }
    return TRUE;
}

typedef struct {
    void   *BaseAddress, *AllocationBase;
    DWORD   AllocationProtect;
    WORD    PartitionId;
    SIZE_T  RegionSize;
    DWORD   State, Protect, Type;
} MEMORY_BASIC_INFORMATION;

/* the view an address is in, for VirtualQuery: 1 if it is in one */
static int view_query(const void *addr, MEMORY_BASIC_INFORMATION *mbi) {
    const uint8_t *a = addr;
    int found = 0;
    pthread_mutex_lock(&maplock);
    for (int i = 0; i < nviews; i++) {
        if (a < views[i].base || a >= views[i].base + views[i].size) continue;
        {
            uint8_t *page = (uint8_t *)((uintptr_t)a & ~(uintptr_t)4095);
            DWORD pr = views[i].exec ? (views[i].write ? 0x40 : 0x20) : (views[i].write ? 0x04 : 0x02);
            mbi->BaseAddress = page;
            mbi->AllocationBase = views[i].base;
            mbi->AllocationProtect = pr;
            mbi->RegionSize = (size_t)(views[i].base + views[i].size - page);
            mbi->State = MEM_COMMIT;
            mbi->Protect = pr;
            mbi->Type = 0x40000;      /* MEM_MAPPED */
            found = 1;
        }
        break;
    }
    pthread_mutex_unlock(&maplock);
    return found;
}

SIZE_T WINAPI VirtualQuery(const void *addr, MEMORY_BASIC_INFORMATION *mbi, SIZE_T n) {
    vregion_t *r;
    if (n < sizeof(*mbi)) return 0;
    memset(mbi, 0, sizeof(*mbi));
    pthread_mutex_lock(&vlock);
    r = vfind(addr);
    if (r) {
        size_t page = (size_t)(((const uint8_t *)addr - r->base) >> 12), last = page;
        uint8_t c = r->committed[page];
        while ((last + 1) < (r->size >> 12) && r->committed[last + 1] == c) last++;
        mbi->BaseAddress = r->base + (page << 12);
        mbi->AllocationBase = r->base;
        mbi->AllocationProtect = r->prot;
        mbi->RegionSize = (last - page + 1) << 12;
        mbi->State = c ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = c ? r->prot : 0;
        mbi->Type = MEM_PRIVATE;
        pthread_mutex_unlock(&vlock);
        return sizeof(*mbi);
    }
    pthread_mutex_unlock(&vlock);
    if (view_query(addr, mbi)) return sizeof(*mbi);
    /* a module's image? */
    for (wmodule_t *m = ld_modules(); m; m = m->next) {
        if ((const uint8_t *)addr >= m->base && (const uint8_t *)addr < m->base + m->size) {
            mbi->BaseAddress = (void *)((uintptr_t)addr & ~(uintptr_t)4095);
            mbi->AllocationBase = m->base;
            mbi->AllocationProtect = 0x80;
            mbi->RegionSize = 4096;
            mbi->State = MEM_COMMIT;
            mbi->Protect = 0x20;
            mbi->Type = MEM_IMAGE;
            return sizeof(*mbi);
        }
    }
    /* anything else that is mapped: say committed read/write, one page */
    {
        unsigned char vec;
        void *pg = (void *)((uintptr_t)addr & ~(uintptr_t)4095);
        if (mincore(pg, 4096, &vec) == 0 || errno != ENOMEM) {
            mbi->BaseAddress = pg;
            mbi->AllocationBase = pg;
            mbi->AllocationProtect = 0x04;
            mbi->RegionSize = 4096;
            mbi->State = MEM_COMMIT;
            mbi->Protect = 0x04;
            mbi->Type = MEM_PRIVATE;
        } else {
            mbi->BaseAddress = pg;
            mbi->RegionSize = 4096;
            mbi->State = MEM_FREE;
            mbi->Protect = 0x01;
        }
    }
    return sizeof(*mbi);
}

BOOL WINAPI VirtualLock(void *p, SIZE_T n) {
    (void)p, (void)n;
    return TRUE;
}
BOOL WINAPI VirtualUnlock(void *p, SIZE_T n) {
    (void)p, (void)n;
    return TRUE;
}
BOOL WINAPI FlushInstructionCache(HANDLE p, const void *a, SIZE_T n) {
    (void)p, (void)a, (void)n;
    return TRUE;
}

/* heaps: all are malloc's */
#define HEAP_ZERO_MEMORY 0x8
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10
static HANDLE process_heap;

HANDLE WINAPI GetProcessHeap(void) {
    if (!process_heap) process_heap = h_new(H_HEAP, 0);
    return process_heap;
}

HANDLE WINAPI HeapCreate(DWORD opts, SIZE_T init, SIZE_T max) {
    (void)opts, (void)init, (void)max;
    return h_new(H_HEAP, 0);
}

BOOL WINAPI HeapDestroy(HANDLE h) {
    h_close(h);
    return TRUE;
}

void *WINAPI HeapAlloc(HANDLE h, DWORD flags, SIZE_T n) {
    void *p;
    (void)h;
    p = (flags & HEAP_ZERO_MEMORY) ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
    if (!p) wset_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

void *WINAPI HeapReAlloc(HANDLE h, DWORD flags, void *p, SIZE_T n) {
    size_t old = p ? malloc_usable_size(p) : 0;
    void *q;
    (void)h;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        if (n <= old) return p;
        return 0;
    }
    q = realloc(p, n ? n : 1);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old) memset((uint8_t *)q + old, 0, n - old);
    return q;
}

BOOL WINAPI HeapFree(HANDLE h, DWORD flags, void *p) {
    (void)h, (void)flags;
    free(p);
    return TRUE;
}

SIZE_T WINAPI HeapSize(HANDLE h, DWORD flags, const void *p) {
    (void)h, (void)flags;
    return p ? malloc_usable_size((void *)p) : (SIZE_T)-1;
}

BOOL WINAPI HeapValidate(HANDLE h, DWORD flags, const void *p) {
    (void)h, (void)flags, (void)p;
    return TRUE;
}
BOOL WINAPI HeapSetInformation(HANDLE h, int cls, void *info, SIZE_T n) {
    (void)h, (void)cls, (void)info, (void)n;
    return TRUE;
}
SIZE_T WINAPI HeapCompact(HANDLE h, DWORD flags) {
    (void)h, (void)flags;
    return 1 << 20;
}
BOOL WINAPI HeapLock(HANDLE h) {
    (void)h;
    return TRUE;
}
BOOL WINAPI HeapUnlock(HANDLE h) {
    (void)h;
    return TRUE;
}
DWORD WINAPI GetProcessHeaps(DWORD n, HANDLE *hs) {
    if (n) hs[0] = GetProcessHeap();
    return 1;
}

/* Global and Local memory: GMEM_MOVEABLE blocks are plain blocks too, so a
 * handle is the pointer */
HGLOBAL WINAPI GlobalAlloc(UINT flags, SIZE_T n) { return (flags & 0x40) ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
void *WINAPI GlobalLock(HGLOBAL h) { return h; }
BOOL WINAPI GlobalUnlock(HGLOBAL h) {
    (void)h;
    wset_last_error(0);
    return FALSE;
}
SIZE_T WINAPI GlobalSize(HGLOBAL h) { return h ? malloc_usable_size(h) : 0; }
HGLOBAL WINAPI GlobalFree(HGLOBAL h) {
    free(h);
    return 0;
}
HGLOBAL WINAPI GlobalReAlloc(HGLOBAL h, SIZE_T n, UINT flags) {
    (void)flags;
    return realloc(h, n ? n : 1);
}
HGLOBAL WINAPI GlobalHandle(const void *p) { return (HGLOBAL)p; }
UINT WINAPI GlobalFlags(HGLOBAL h) {
    (void)h;
    return 0;
}
HLOCAL WINAPI LocalAlloc(UINT flags, SIZE_T n) { return GlobalAlloc(flags, n); }
HLOCAL WINAPI LocalFree(HLOCAL h) {
    free(h);
    return 0;
}
HLOCAL WINAPI LocalReAlloc(HLOCAL h, SIZE_T n, UINT flags) { return GlobalReAlloc(h, n, flags); }
void *WINAPI LocalLock(HLOCAL h) { return h; }
BOOL WINAPI LocalUnlock(HLOCAL h) {
    (void)h;
    return FALSE;
}
SIZE_T WINAPI LocalSize(HLOCAL h) { return GlobalSize(h); }

typedef struct {
    DWORD    dwLength, dwMemoryLoad;
    uint64_t ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual,
        ullAvailExtendedVirtual;
} MEMORYSTATUSEX;

static void mem_info(uint64_t *total, uint64_t *avail) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    *total = 2ull << 30;
    *avail = 1ull << 30;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long v;
        if (sscanf(line, "MemTotal: %llu kB", &v) == 1) *total = v * 1024;
        else if (sscanf(line, "MemAvailable: %llu kB", &v) == 1) *avail = v * 1024;
    }
    fclose(f);
}

BOOL WINAPI GlobalMemoryStatusEx(MEMORYSTATUSEX *m) {
    uint64_t total, avail;
    mem_info(&total, &avail);
    m->dwMemoryLoad = (DWORD)(total ? 100 - avail * 100 / total : 50);
    m->ullTotalPhys = total;
    m->ullAvailPhys = avail;
    m->ullTotalPageFile = total * 2;
    m->ullAvailPageFile = avail * 2;
    m->ullTotalVirtual = 0x7FFFFFFE0000ull;
    m->ullAvailVirtual = 0x7FF000000000ull;
    m->ullAvailExtendedVirtual = 0;
    return TRUE;
}

typedef struct {
    DWORD  dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile, dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS;

void WINAPI GlobalMemoryStatus(MEMORYSTATUS *m) {
    MEMORYSTATUSEX x;
    GlobalMemoryStatusEx(&x);
    m->dwMemoryLoad = x.dwMemoryLoad;
    m->dwTotalPhys = x.ullTotalPhys;
    m->dwAvailPhys = x.ullAvailPhys;
    m->dwTotalPageFile = x.ullTotalPageFile;
    m->dwAvailPageFile = x.ullAvailPageFile;
    m->dwTotalVirtual = x.ullTotalVirtual;
    m->dwAvailVirtual = x.ullAvailVirtual;
}

BOOL WINAPI IsBadReadPtr(const void *p, UINT_PTR n) {
    (void)n;
    return p == 0;
}
BOOL WINAPI IsBadWritePtr(void *p, UINT_PTR n) {
    (void)n;
    return p == 0;
}
BOOL WINAPI IsBadCodePtr(void *p) { return p == 0; }
BOOL WINAPI IsBadStringPtrA(const char *p, UINT_PTR n) {
    (void)n;
    return p == 0;
}

/* ---- time --------------------------------------------------------------------------------- */

DWORD WINAPI GetTickCount(void) { return (DWORD)(mono_ns() / 1000000ULL); }
ULONGLONG WINAPI GetTickCount64(void) { return mono_ns() / 1000000ULL; }

BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER *c) {
    c->QuadPart = (int64_t)(mono_ns() / 100);     /* 10 MHz, as on Windows 10 */
    return TRUE;
}

BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER *f) {
    f->QuadPart = 10000000;
    return TRUE;
}

void WINAPI GetSystemTimeAsFileTime(FILETIME *ft) {
    uint64_t v = now_100ns();
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

void WINAPI GetSystemTimePreciseAsFileTime(FILETIME *ft) { GetSystemTimeAsFileTime(ft); }

static void tm_to_st(const struct tm *t, int ms, SYSTEMTIME *st) {
    st->wYear = (WORD)(t->tm_year + 1900);
    st->wMonth = (WORD)(t->tm_mon + 1);
    st->wDayOfWeek = (WORD)t->tm_wday;
    st->wDay = (WORD)t->tm_mday;
    st->wHour = (WORD)t->tm_hour;
    st->wMinute = (WORD)t->tm_min;
    st->wSecond = (WORD)t->tm_sec;
    st->wMilliseconds = (WORD)ms;
}

void WINAPI GetSystemTime(SYSTEMTIME *st) {
    struct timespec ts;
    struct tm t;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &t);
    tm_to_st(&t, (int)(ts.tv_nsec / 1000000), st);
}

void WINAPI GetLocalTime(SYSTEMTIME *st) {
    struct timespec ts;
    struct tm t;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &t);
    tm_to_st(&t, (int)(ts.tv_nsec / 1000000), st);
}

BOOL WINAPI FileTimeToSystemTime(const FILETIME *ft, SYSTEMTIME *st) {
    uint64_t v = (uint64_t)ft->dwHighDateTime << 32 | ft->dwLowDateTime;
    time_t s = (time_t)((int64_t)(v / 10000000ULL) - 11644473600LL);
    struct tm t;
    gmtime_r(&s, &t);
    tm_to_st(&t, (int)((v % 10000000ULL) / 10000), st);
    return TRUE;
}

BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME *st, FILETIME *ft) {
    struct tm t;
    time_t s;
    memset(&t, 0, sizeof(t));
    t.tm_year = st->wYear - 1900;
    t.tm_mon = st->wMonth - 1;
    t.tm_mday = st->wDay;
    t.tm_hour = st->wHour;
    t.tm_min = st->wMinute;
    t.tm_sec = st->wSecond;
    s = timegm(&t);
    unix_to_filetime(s, (long)st->wMilliseconds * 1000000L, ft);
    return TRUE;
}

BOOL WINAPI DosDateTimeToFileTime(WORD date, WORD time, FILETIME *ft) {
    SYSTEMTIME st;
    memset(&st, 0, sizeof(st));
    st.wYear = (WORD)(1980 + (date >> 9));
    st.wMonth = (WORD)((date >> 5) & 15);
    st.wDay = (WORD)(date & 31);
    st.wHour = (WORD)(time >> 11);
    st.wMinute = (WORD)((time >> 5) & 63);
    st.wSecond = (WORD)((time & 31) * 2);
    return SystemTimeToFileTime(&st, ft);
}

BOOL WINAPI FileTimeToDosDateTime(const FILETIME *ft, WORD *date, WORD *time) {
    SYSTEMTIME st;
    FileTimeToSystemTime(ft, &st);
    *date = (WORD)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    *time = (WORD)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    return TRUE;
}

BOOL WINAPI FileTimeToLocalFileTime(const FILETIME *in, FILETIME *out) {
    time_t now = time(0);
    struct tm t;
    int64_t off, v;
    localtime_r(&now, &t);
    off = (int64_t)t.tm_gmtoff * 10000000LL;
    v = (int64_t)((uint64_t)in->dwHighDateTime << 32 | in->dwLowDateTime) + off;
    out->dwLowDateTime = (DWORD)v;
    out->dwHighDateTime = (DWORD)((uint64_t)v >> 32);
    return TRUE;
}

BOOL WINAPI LocalFileTimeToFileTime(const FILETIME *in, FILETIME *out) {
    time_t now = time(0);
    struct tm t;
    int64_t off, v;
    localtime_r(&now, &t);
    off = (int64_t)t.tm_gmtoff * 10000000LL;
    v = (int64_t)((uint64_t)in->dwHighDateTime << 32 | in->dwLowDateTime) - off;
    out->dwLowDateTime = (DWORD)v;
    out->dwHighDateTime = (DWORD)((uint64_t)v >> 32);
    return TRUE;
}

BOOL WINAPI SystemTimeToTzSpecificLocalTime(void *tz, const SYSTEMTIME *u, SYSTEMTIME *l) {
    FILETIME a, b;
    (void)tz;
    SystemTimeToFileTime(u, &a);
    FileTimeToLocalFileTime(&a, &b);
    return FileTimeToSystemTime(&b, l);
}

LONG WINAPI CompareFileTime(const FILETIME *a, const FILETIME *b) {
    uint64_t x = (uint64_t)a->dwHighDateTime << 32 | a->dwLowDateTime, y = (uint64_t)b->dwHighDateTime << 32 | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}

DWORD WINAPI GetTimeZoneInformation(void *tzi) {
    time_t now = time(0);
    struct tm t;
    localtime_r(&now, &t);
    memset(tzi, 0, 172);
    *(LONG *)tzi = (LONG)(-t.tm_gmtoff / 60);
    return 0;   /* TIME_ZONE_ID_UNKNOWN */
}

DWORD WINAPI GetDynamicTimeZoneInformation(void *tzi) {
    memset(tzi, 0, 432);
    return GetTimeZoneInformation(tzi);
}

void WINAPI Sleep(DWORD ms) {
    struct timespec ts;
    if (ms == 0) {
        sched_yield();
        return;
    }
    if (ms == INFINITE) {
        for (;;) pause();
    }
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

DWORD WINAPI SleepEx(DWORD ms, BOOL alertable) {
    (void)alertable;
    Sleep(ms);
    return 0;
}

BOOL WINAPI SwitchToThread(void) {
    sched_yield();
    return TRUE;
}

/* waitable timers: an event set when the time comes, by a thread of its own */
typedef struct {
    HANDLE   event;
    int64_t  due_100ns;
    LONG     period;
} wtimer_t;

HANDLE WINAPI CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name);
BOOL WINAPI ResetEvent(HANDLE h);

HANDLE WINAPI CreateWaitableTimerExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)sa, (void)access;
    return event_new(!(flags & 2) /* not a synchronization timer: manual */, FALSE, name, NS_TIMER);
}

HANDLE WINAPI CreateWaitableTimerW(void *sa, BOOL manual, const WCHAR *name) {
    (void)sa;
    return event_new(manual, FALSE, name, NS_TIMER);
}

HANDLE WINAPI OpenWaitableTimerW(DWORD access, BOOL inherit, const WCHAR *name) {
    (void)access, (void)inherit;
    return h_open_named(H_EVENT, NS_TIMER, name);
}

HANDLE WINAPI OpenWaitableTimerA(DWORD access, BOOL inherit, const char *name) {
    WCHAR *w = name ? utf8_to_w_dup(name) : 0;
    HANDLE h = OpenWaitableTimerW(access, inherit, w);
    free(w);
    return h;
}

typedef struct {
    HANDLE ev;
    int64_t ns;
} timer_arg_t;

static void *timer_thread(void *a) {
    timer_arg_t *t = a;
    struct timespec ts;
    ts.tv_sec = t->ns / 1000000000LL;
    ts.tv_nsec = (long)(t->ns % 1000000000LL);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    SetEvent(t->ev);
    free(t);
    return 0;
}

BOOL WINAPI SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, BOOL resume) {
    timer_arg_t *t = malloc(sizeof(*t));
    pthread_t pt;
    int64_t d = due->QuadPart;
    (void)period, (void)fn, (void)arg, (void)resume;
    ResetEvent(h);
    if (d < 0) t->ns = -d * 100;
    else t->ns = (int64_t)(d - (int64_t)now_100ns()) * 100;
    if (t->ns < 0) t->ns = 0;
    t->ev = h;
    if (t->ns == 0) {
        free(t);
        return SetEvent(h);
    }
    if (pthread_create(&pt, 0, timer_thread, t) != 0) {
        free(t);
        return FALSE;
    }
    pthread_detach(pt);
    return TRUE;
}

BOOL WINAPI SetWaitableTimerEx(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, void *arg, void *wake, ULONG delay) {
    (void)wake, (void)delay;
    return SetWaitableTimer(h, due, period, fn, arg, FALSE);
}

BOOL WINAPI CancelWaitableTimer(HANDLE h) { return ResetEvent(h); }

/* ---- the system ---------------------------------------------------------------------------- */

typedef struct {
    WORD      wProcessorArchitecture, wReserved;
    DWORD     dwPageSize;
    void     *lpMinimumApplicationAddress, *lpMaximumApplicationAddress;
    DWORD_PTR dwActiveProcessorMask;
    DWORD     dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity;
    WORD      wProcessorLevel, wProcessorRevision;
} SYSTEM_INFO;

static int ncpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

void WINAPI GetSystemInfo(SYSTEM_INFO *si) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    memset(si, 0, sizeof(*si));
    si->wProcessorArchitecture = 9;   /* AMD64 */
    si->dwPageSize = 4096;
    si->lpMinimumApplicationAddress = (void *)0x10000;
    si->lpMaximumApplicationAddress = (void *)0x7FFFFFFEFFFFull;
    si->dwNumberOfProcessors = (DWORD)ncpus();
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~0ull : (1ull << si->dwNumberOfProcessors) - 1;
    si->dwProcessorType = 8664;
    si->dwAllocationGranularity = 0x10000;
    __get_cpuid(1, &a, &b, &c, &d);
    si->wProcessorLevel = (WORD)(((a >> 8) & 0xF) + ((a >> 20) & 0xFF));
    si->wProcessorRevision = (WORD)(((a >> 4) & 0xF) << 8 | (a & 0xF));
    *(DWORD *)((uint8_t *)peb_get() + 0xB8) = si->dwNumberOfProcessors;
}

void WINAPI GetNativeSystemInfo(SYSTEM_INFO *si) { GetSystemInfo(si); }

BOOL WINAPI IsProcessorFeaturePresent(DWORD f) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    __get_cpuid(1, &a, &b, &c, &d);
    switch (f) {
    case 0: case 2: case 1: return FALSE;           /* old FPU errata */
    case 3: return (d >> 23) & 1;                   /* MMX */
    case 6: return (d >> 25) & 1;                   /* SSE */
    case 7: return FALSE;                           /* 3DNow */
    case 8: return TRUE;                            /* RDTSC */
    case 9: return TRUE;                            /* PAE */
    case 10: return (d >> 26) & 1;                  /* SSE2 */
    case 12: return TRUE;                           /* NX */
    case 13: return (c >> 0) & 1;                   /* SSE3 */
    case 14: return (c >> 13) & 1;                  /* CMPXCHG16B */
    case 17: return TRUE;                           /* XSAVE? said yes only with OSXSAVE */
    case 20: return TRUE;                           /* SLAT */
    case 23: return TRUE;                           /* FASTFAIL */
    case 36: return (c >> 9) & 1;                   /* SSSE3 */
    case 37: return (c >> 19) & 1;                  /* SSE4.1 */
    case 38: return (c >> 20) & 1;                  /* SSE4.2 */
    case 39: {                                      /* AVX */
        return (c >> 28) & 1 && (c >> 27) & 1;
    }
    case 40: {                                      /* AVX2 */
        unsigned a7 = 0, b7 = 0, c7 = 0, d7 = 0;
        __get_cpuid_count(7, 0, &a7, &b7, &c7, &d7);
        return (b7 >> 5) & 1 && (c >> 27) & 1;
    }
    default: return FALSE;
    }
}

typedef struct {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    WCHAR szCSDVersion[128];
    WORD  wServicePackMajor, wServicePackMinor, wSuiteMask;
    BYTE  wProductType, wReserved;
} OSVERSIONINFOEXW;

typedef struct {
    DWORD dwOSVersionInfoSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    char  szCSDVersion[128];
    WORD  wServicePackMajor, wServicePackMinor, wSuiteMask;
    BYTE  wProductType, wReserved;
} OSVERSIONINFOEXA;

BOOL WINAPI GetVersionExW(OSVERSIONINFOEXW *v) {
    DWORD size = v->dwOSVersionInfoSize;
    memset((uint8_t *)v + 4, 0, size - 4);
    v->dwMajorVersion = 10;
    v->dwMinorVersion = 0;
    v->dwBuildNumber = 19045;
    v->dwPlatformId = 2;
    if (size >= sizeof(OSVERSIONINFOEXW)) v->wProductType = 1;    /* VER_NT_WORKSTATION */
    return TRUE;
}

BOOL WINAPI GetVersionExA(OSVERSIONINFOEXA *v) {
    DWORD size = v->dwOSVersionInfoSize;
    memset((uint8_t *)v + 4, 0, size - 4);
    v->dwMajorVersion = 10;
    v->dwMinorVersion = 0;
    v->dwBuildNumber = 19045;
    v->dwPlatformId = 2;
    if (size >= sizeof(OSVERSIONINFOEXA)) v->wProductType = 1;
    return TRUE;
}

DWORD WINAPI GetVersion(void) { return (19045u << 16) | 10; }

LONG WINAPI RtlGetVersion(OSVERSIONINFOEXW *v) {
    GetVersionExW(v);
    return 0;
}

ULONGLONG WINAPI VerSetConditionMask(ULONGLONG mask, DWORD type, BYTE cond) {
    for (int i = 0; i < 8; i++)
        if (type & (1u << i)) mask |= (ULONGLONG)(cond & 7) << (i * 3);
    return mask;
}

/* is this Windows at least (or exactly...) what is asked: as Windows 10 22H2 */
BOOL WINAPI VerifyVersionInfoW(OSVERSIONINFOEXW *v, DWORD type, ULONGLONG mask) {
    static const DWORD have[4] = { 10, 0, 19045, 0 };
    DWORD want[4];
    int fields[4] = { 1, 0, 2, 5 };     /* major, minor, build, sp major bits */
    want[0] = v->dwMajorVersion, want[1] = v->dwMinorVersion, want[2] = v->dwBuildNumber, want[3] = v->wServicePackMajor;
    /* major/minor/sp are compared as one version when given together */
    {
        int64_t h = 0, w = 0;
        int cond = 0;
        if (type & 2) h = have[0], w = want[0], cond = (int)(mask >> 3) & 7;    /* VER_MAJORVERSION */
        if (type & 1) {
            h = h * 1000 + have[1], w = w * 1000 + want[1];
            if (!cond) cond = (int)(mask >> 0) & 7;
        }
        if (type & 0x20) {
            h = h * 1000 + have[3], w = w * 1000 + want[3];
            if (!cond) cond = (int)(mask >> 15) & 7;
        }
        if (cond) {
            int ok = cond == 1 ? h == w : cond == 2 ? h > w : cond == 3 ? h >= w : cond == 4 ? h < w : h <= w;
            if (!ok) {
                wset_last_error(1150);   /* ERROR_OLD_WIN_VERSION */
                return FALSE;
            }
        }
        if (type & 4) {               /* VER_BUILDNUMBER */
            int c = (int)(mask >> 6) & 7;
            int64_t hb = have[2], wb = want[2];
            int ok = c == 1 ? hb == wb : c == 2 ? hb > wb : c == 3 ? hb >= wb : c == 4 ? hb < wb : c == 5 ? hb <= wb : 1;
            if (!ok) {
                wset_last_error(1150);
                return FALSE;
            }
        }
    }
    (void)fields;
    return TRUE;
}

BOOL WINAPI VerifyVersionInfoA(void *v, DWORD type, ULONGLONG mask) {
    OSVERSIONINFOEXW w;
    OSVERSIONINFOEXA *a = v;
    memset(&w, 0, sizeof(w));
    w.dwMajorVersion = a->dwMajorVersion;
    w.dwMinorVersion = a->dwMinorVersion;
    w.dwBuildNumber = a->dwBuildNumber;
    w.wServicePackMajor = a->wServicePackMajor;
    return VerifyVersionInfoW(&w, type, mask);
}

BOOL WINAPI GetProductInfo(DWORD a, DWORD b, DWORD c, DWORD d, DWORD *type) {
    (void)a, (void)b, (void)c, (void)d;
    *type = 0x30;     /* PRODUCT_PROFESSIONAL */
    return TRUE;
}

BOOL WINAPI IsWow64Process(HANDLE h, BOOL *r) {
    (void)h;
    *r = FALSE;
    return TRUE;
}

/* a 64-bit process on a 64-bit PC: no emulation (IMAGE_FILE_MACHINE_UNKNOWN), AMD64 */
BOOL WINAPI IsWow64Process2(HANDLE h, USHORT *process, USHORT *native) {
    (void)h;
    if (process) *process = 0;
    if (native) *native = 0x8664;
    return TRUE;
}

/* the file APIs' A forms take UTF-8 here, which a program asking takes for ANSI */
BOOL WINAPI AreFileApisANSI(void) { return TRUE; }
void WINAPI SetFileApisToANSI(void) {}
void WINAPI SetFileApisToOEM(void) {}

BOOL WINAPI IsDebuggerPresent(void) { return FALSE; }
BOOL WINAPI CheckRemoteDebuggerPresent(HANDLE h, BOOL *r) {
    (void)h;
    *r = FALSE;
    return TRUE;
}

static UINT error_mode;
UINT WINAPI SetErrorMode(UINT m) {
    UINT o = error_mode;
    error_mode = m;
    return o;
}
UINT WINAPI GetErrorMode(void) { return error_mode; }
BOOL WINAPI SetThreadErrorMode(DWORD m, DWORD *old) {
    if (old) *old = error_mode;
    error_mode = m;
    return TRUE;
}
DWORD WINAPI SetThreadExecutionState(DWORD s) {
    (void)s;
    return 0x80000000;
}
BOOL WINAPI SetThreadPriority(HANDLE h, int p) {
    (void)h, (void)p;
    return TRUE;
}
int WINAPI GetThreadPriority(HANDLE h) {
    (void)h;
    return 0;
}
BOOL WINAPI SetThreadPriorityBoost(HANDLE h, BOOL d) {
    (void)h, (void)d;
    return TRUE;
}
BOOL WINAPI SetPriorityClass(HANDLE h, DWORD c) {
    (void)h, (void)c;
    return TRUE;
}
DWORD WINAPI GetPriorityClass(HANDLE h) {
    (void)h;
    return 0x20;   /* NORMAL_PRIORITY_CLASS */
}
DWORD_PTR WINAPI SetThreadAffinityMask(HANDLE h, DWORD_PTR m) {
    (void)h, (void)m;
    return 1;
}
BOOL WINAPI SetProcessAffinityMask(HANDLE h, DWORD_PTR m) {
    (void)h, (void)m;
    return TRUE;
}
BOOL WINAPI GetProcessAffinityMask(HANDLE h, DWORD_PTR *p, DWORD_PTR *s) {
    int n = ncpus();
    (void)h;
    *p = *s = n >= 64 ? ~0ull : (1ull << n) - 1;
    return TRUE;
}
DWORD WINAPI SetThreadIdealProcessor(HANDLE h, DWORD p) {
    (void)h, (void)p;
    return 0;
}
HRESULT WINAPI SetThreadDescription(HANDLE h, const WCHAR *d) {
    (void)h, (void)d;
    return 0;
}
HRESULT WINAPI GetThreadDescription(HANDLE h, WCHAR **d) {
    (void)h;
    *d = LocalAlloc(0x40, 2);
    return 0;
}
BOOL WINAPI SetThreadStackGuarantee(ULONG *n) {
    if (n) *n = 0;
    return TRUE;
}
DWORD WINAPI GetCurrentProcessorNumber(void) {
    int c = sched_getcpu();
    return c < 0 ? 0 : (DWORD)c;
}

void WINAPI GetCurrentThreadStackLimits(ULONG_PTR *lo, ULONG_PTR *hi) {
    uint8_t *teb = teb_current();
    *hi = *(ULONG_PTR *)(teb + 0x08);
    *lo = *(ULONG_PTR *)(teb + 0x10);
}

static void times_ft(const struct timespec *t, FILETIME *ft) {
    uint64_t v = (uint64_t)t->tv_sec * 10000000ULL + (uint64_t)t->tv_nsec / 100;
    ft->dwLowDateTime = (DWORD)v;
    ft->dwHighDateTime = (DWORD)(v >> 32);
}

BOOL WINAPI GetProcessTimes(HANDLE h, FILETIME *c, FILETIME *e, FILETIME *k, FILETIME *u) {
    struct timespec t;
    (void)h;
    GetSystemTimeAsFileTime(c);
    memset(e, 0, sizeof(*e));
    memset(k, 0, sizeof(*k));
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    times_ft(&t, u);
    return TRUE;
}

BOOL WINAPI GetThreadTimes(HANDLE h, FILETIME *c, FILETIME *e, FILETIME *k, FILETIME *u) {
    struct timespec t;
    (void)h;
    GetSystemTimeAsFileTime(c);
    memset(e, 0, sizeof(*e));
    memset(k, 0, sizeof(*k));
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    times_ft(&t, u);
    return TRUE;
}

BOOL WINAPI GetSystemTimes(FILETIME *idle, FILETIME *k, FILETIME *u) {
    if (idle) memset(idle, 0, sizeof(*idle));
    if (k) memset(k, 0, sizeof(*k));
    if (u) memset(u, 0, sizeof(*u));
    return TRUE;
}

typedef struct {
    BYTE  ACLineStatus, BatteryFlag, BatteryLifePercent, SystemStatusFlag;
    DWORD BatteryLifeTime, BatteryFullLifeTime;
} SYSTEM_POWER_STATUS;

BOOL WINAPI GetSystemPowerStatus(SYSTEM_POWER_STATUS *s) {
    s->ACLineStatus = 1;
    s->BatteryFlag = 128;          /* no battery */
    s->BatteryLifePercent = 255;
    s->SystemStatusFlag = 0;
    s->BatteryLifeTime = 0xFFFFFFFF;
    s->BatteryFullLifeTime = 0xFFFFFFFF;
    return TRUE;
}

BOOL WINAPI GetLogicalProcessorInformation(void *buf, DWORD *len) {
    (void)buf;
    *len = 0;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI GetLogicalProcessorInformationEx(int rel, void *buf, DWORD *len) {
    (void)rel, (void)buf;
    *len = 0;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}

DWORD WINAPI GetActiveProcessorCount(WORD group) {
    (void)group;
    return (DWORD)ncpus();
}

/* one processor group, as on any machine with at most 64 processors */
WORD WINAPI GetActiveProcessorGroupCount(void) { return 1; }
WORD WINAPI GetMaximumProcessorGroupCount(void) { return 1; }

DWORD WINAPI GetMaximumProcessorCount(WORD group) {
    (void)group;
    return (DWORD)ncpus();
}

BOOL WINAPI GetComputerNameW(WCHAR *buf, DWORD *n) {
    struct utsname u;
    char name[64];
    int len;
    if (uname(&u) != 0) snprintf(u.nodename, sizeof(u.nodename), "ICDA");
    snprintf(name, sizeof(name), "%s", u.nodename);
    for (char *p = name; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    len = utf8_to_w(name, -1, 0, 0);
    if ((DWORD)len > *n) {
        *n = (DWORD)len;
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    utf8_to_w(name, -1, buf, (int)*n);
    *n = (DWORD)len - 1;
    return TRUE;
}

BOOL WINAPI GetComputerNameA(char *buf, DWORD *n) {
    WCHAR w[64];
    DWORD wn = 64;
    if (!GetComputerNameW(w, &wn)) return FALSE;
    if (wn + 1 > *n) {
        *n = wn + 1;
        return FALSE;
    }
    w_to_utf8(w, -1, buf, (int)*n);
    *n = wn;
    return TRUE;
}

BOOL WINAPI GetComputerNameExW(int type, WCHAR *buf, DWORD *n) {
    (void)type;
    return GetComputerNameW(buf, n);
}

void WINAPI Beep(DWORD f, DWORD d) { (void)f, (void)d; }

/* ---- the environment ------------------------------------------------------------------------- */

DWORD WINAPI GetEnvironmentVariableA(const char *name, char *buf, DWORD n) {
    const char *v = getenv(name);
    size_t len;
    if (!v) {
        wset_last_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    len = strlen(v);
    if (len + 1 > n || !buf) return (DWORD)len + 1;
    memcpy(buf, v, len + 1);
    return (DWORD)len;
}

DWORD WINAPI GetEnvironmentVariableW(const WCHAR *name, WCHAR *buf, DWORD n) {
    char a[512];
    const char *v;
    int len;
    w_to_utf8(name, -1, a, sizeof(a));
    v = getenv(a);
    if (!v) {
        wset_last_error(ERROR_ENVVAR_NOT_FOUND);
        return 0;
    }
    len = utf8_to_w(v, -1, 0, 0);
    if ((DWORD)len > n || !buf) return (DWORD)len;
    utf8_to_w(v, -1, buf, (int)n);
    return (DWORD)len - 1;
}

BOOL WINAPI SetEnvironmentVariableA(const char *name, const char *v) {
    if (v) return setenv(name, v, 1) == 0;
    unsetenv(name);
    return TRUE;
}

BOOL WINAPI SetEnvironmentVariableW(const WCHAR *name, const WCHAR *v) {
    char *a = w_to_utf8_dup(name), *b = v ? w_to_utf8_dup(v) : 0;
    BOOL r = SetEnvironmentVariableA(a, b);
    free(a), free(b);
    return r;
}

WCHAR *WINAPI GetEnvironmentStringsW(void) {
    size_t total = 1, o = 0;
    WCHAR *out;
    for (char **e = environ; *e; e++) total += (size_t)utf8_to_w(*e, -1, 0, 0);
    out = malloc(total * sizeof(WCHAR));
    for (char **e = environ; *e; e++) o += (size_t)utf8_to_w(*e, -1, out + o, (int)(total - o));
    out[o] = 0;
    return out;
}

char *WINAPI GetEnvironmentStringsA(void) {
    size_t total = 1, o = 0;
    char *out;
    for (char **e = environ; *e; e++) total += strlen(*e) + 1;
    out = malloc(total);
    for (char **e = environ; *e; e++) {
        size_t l = strlen(*e) + 1;
        memcpy(out + o, *e, l);
        o += l;
    }
    out[o] = 0;
    return out;
}

BOOL WINAPI FreeEnvironmentStringsW(WCHAR *p) {
    free(p);
    return TRUE;
}
BOOL WINAPI FreeEnvironmentStringsA(char *p) {
    free(p);
    return TRUE;
}

DWORD WINAPI ExpandEnvironmentStringsW(const WCHAR *src, WCHAR *dst, DWORD n) {
    char *in = w_to_utf8_dup(src), out[4096];
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < sizeof(out);) {
        const char *e;
        if (*p == '%' && (e = strchr(p + 1, '%')) != 0) {
            char name[256];
            const char *v;
            size_t l = (size_t)(e - p - 1);
            if (l < sizeof(name)) {
                memcpy(name, p + 1, l);
                name[l] = 0;
                v = getenv(name);
                if (v) {
                    size_t vl = strlen(v);
                    if (o + vl < sizeof(out)) memcpy(out + o, v, vl), o += vl;
                    p = e + 1;
                    continue;
                }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    free(in);
    {
        int len = utf8_to_w(out, -1, 0, 0);
        if ((DWORD)len <= n && dst) utf8_to_w(out, -1, dst, (int)n);
        return (DWORD)len;
    }
}

DWORD WINAPI ExpandEnvironmentStringsA(const char *src, char *dst, DWORD n) {
    WCHAR *w = utf8_to_w_dup(src), out[4096];
    DWORD r = ExpandEnvironmentStringsW(w, out, 4096);
    free(w);
    if (r <= 4096) {
        char a[8192];
        int l = w_to_utf8(out, -1, a, sizeof(a));
        if ((DWORD)l <= n && dst) memcpy(dst, a, (size_t)l);
        return (DWORD)l;
    }
    return r;
}

char *WINAPI GetCommandLineA(void) { return g_cmdline_a; }
WCHAR *WINAPI GetCommandLineW(void) { return g_cmdline_w; }

typedef struct {
    DWORD  cb;
    void  *lpReserved, *lpDesktop, *lpTitle;
    DWORD  dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD   wShowWindow, cbReserved2;
    BYTE  *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;

void WINAPI GetStartupInfoW(STARTUPINFOW *si) {
    memset(si, 0, sizeof(*si));
    si->cb = sizeof(*si);
    si->dwFlags = 0x100;    /* STARTF_USESTDHANDLES */
    si->wShowWindow = 1;
    si->hStdInput = h_std(0);
    si->hStdOutput = h_std(1);
    si->hStdError = h_std(2);
}

void WINAPI GetStartupInfoA(STARTUPINFOW *si) { GetStartupInfoW(si); }

/* ---- text conversion --------------------------------------------------------------------- */

#define CP_ACP   0
#define CP_OEMCP 1
#define CP_UTF8  65001

/* Latin-1 for the Western code pages; everything else is UTF-8 */
static int is_latin1(UINT cp) { return cp == 1252 || cp == 28591 || cp == 850 || cp == 437; }

int WINAPI MultiByteToWideChar(UINT cp, DWORD flags, const char *s, int n, WCHAR *out, int cap) {
    int need;
    (void)flags;
    if (!s) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n < 0) n = (int)strlen(s) + 1;
    if (is_latin1(cp) || cp == 20127) {
        if (!cap) return n;
        if (cap < n) {
            wset_last_error(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (int i = 0; i < n; i++) out[i] = (uint8_t)s[i];
        return n;
    }
    /* utf8_to_w counts a NUL of its own: the count asked has its own NUL, if any */
    need = utf8_to_w(s, n, 0, 0) - 1;
    if (!cap) return need;
    if (cap < need) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    {
        WCHAR *tmp = malloc(((size_t)need + 1) * sizeof(WCHAR));
        utf8_to_w(s, n, tmp, need + 1);
        memcpy(out, tmp, (size_t)need * sizeof(WCHAR));
        free(tmp);
    }
    return need;
}

int WINAPI WideCharToMultiByte(UINT cp, DWORD flags, const WCHAR *s, int n, char *out, int cap, const char *defc, BOOL *used) {
    int need;
    (void)flags;
    if (used) *used = FALSE;
    if (!s) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (n < 0) n = (int)w_len(s) + 1;
    if (is_latin1(cp) || cp == 20127) {
        if (!cap) return n;
        if (cap < n) {
            wset_last_error(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        for (int i = 0; i < n; i++) {
            if (s[i] > (cp == 20127 ? 0x7F : 0xFF)) {
                out[i] = defc ? *defc : '?';
                if (used) *used = TRUE;
            } else {
                out[i] = (char)s[i];
            }
        }
        return n;
    }
    need = w_to_utf8(s, n, 0, 0) - 1;
    if (!cap) return need;
    if (cap < need) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    {
        char *tmp = malloc((size_t)need + 1);
        w_to_utf8(s, n, tmp, need + 1);
        memcpy(out, tmp, (size_t)need);
        free(tmp);
    }
    return need;
}

UINT WINAPI GetACP(void) { return CP_UTF8; }
UINT WINAPI GetOEMCP(void) { return CP_UTF8; }

typedef struct {
    UINT MaxCharSize;
    BYTE DefaultChar[2], LeadByte[12];
} CPINFO;

BOOL WINAPI GetCPInfo(UINT cp, CPINFO *i) {
    memset(i, 0, sizeof(*i));
    i->MaxCharSize = is_latin1(cp) ? 1 : 4;
    i->DefaultChar[0] = '?';
    return TRUE;
}

/* CPINFOEX: the CPINFO (18 bytes), UnicodeDefaultChar at 18, CodePage at 20
 * and its name at 24: 260 WCHARs in the W struct (544 bytes), 260 bytes in
 * the A one (284) */
BOOL WINAPI IsValidCodePage(UINT cp);
static BOOL cp_info_ex(UINT cp, uint8_t *b, int wide) {
    static const struct { UINT cp; const char *name; } names[] = {
        { CP_UTF8, "65001 (UTF-8)" }, { 1252, "1252  (ANSI - Latin I)" }, { 28591, "28591 (ISO 8859-1 Latin I)" },
        { 850, "850   (OEM - Multilingual Latin I)" }, { 437, "437   (OEM - United States)" }, { 20127, "20127 (US-ASCII)" },
    };
    UINT real = cp == 0 || cp == 1 ? GetACP() : cp;     /* CP_ACP, CP_OEMCP */
    const char *name = 0;
    if (!b || !IsValidCodePage(cp)) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(b, 0, wide ? 544 : 284);
    GetCPInfo(real, (CPINFO *)b);
    *(WCHAR *)(b + 18) = real == CP_UTF8 ? 0xFFFD : '?';
    *(UINT *)(b + 20) = real;
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++)
        if (names[k].cp == real) name = names[k].name;
    if (!name) name = "";
    if (wide) utf8_to_w(name, -1, (WCHAR *)(b + 24), 260);
    else snprintf((char *)b + 24, 260, "%s", name);
    return TRUE;
}

BOOL WINAPI GetCPInfoExW(UINT cp, DWORD f, void *i) {
    (void)f;
    return cp_info_ex(cp, i, 1);
}

BOOL WINAPI GetCPInfoExA(UINT cp, DWORD f, void *i) {
    (void)f;
    return cp_info_ex(cp, i, 0);
}

BOOL WINAPI IsValidCodePage(UINT cp) { return cp == CP_UTF8 || is_latin1(cp) || cp == 20127 || cp == 0 || cp == 1; }
BOOL WINAPI IsDBCSLeadByte(BYTE b) {
    (void)b;
    return FALSE;
}
BOOL WINAPI IsDBCSLeadByteEx(UINT cp, BYTE b) {
    (void)cp, (void)b;
    return FALSE;
}

static WCHAR wupper(WCHAR c) {
    if (c >= 'a' && c <= 'z') return (WCHAR)(c - 32);
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (WCHAR)(c - 32);
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return (WCHAR)(c - 32);   /* Greek */
    if (c >= 0x430 && c <= 0x44F) return (WCHAR)(c - 32);                  /* Cyrillic */
    return c;
}

static WCHAR wlower(WCHAR c) {
    if (c >= 'A' && c <= 'Z') return (WCHAR)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (WCHAR)(c + 32);
    if (c >= 0x391 && c <= 0x3A9) return (WCHAR)(c + 32);
    if (c >= 0x410 && c <= 0x42F) return (WCHAR)(c + 32);
    return c;
}

WCHAR win_towupper(WCHAR c) { return wupper(c); }
WCHAR win_towlower(WCHAR c) { return wlower(c); }

static int cmp_w(const WCHAR *a, int na, const WCHAR *b, int nb, int nocase) {
    int i = 0;
    if (na < 0) na = (int)w_len(a);
    if (nb < 0) nb = (int)w_len(b);
    for (; i < na && i < nb; i++) {
        WCHAR x = nocase ? wlower(a[i]) : a[i], y = nocase ? wlower(b[i]) : b[i];
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}

#define NORM_IGNORECASE 1

int WINAPI CompareStringW(DWORD lcid, DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb) {
    (void)lcid;
    return cmp_w(a, na, b, nb, (flags & NORM_IGNORECASE) || (flags & 0x10000000));
}

int WINAPI CompareStringEx(const WCHAR *loc, DWORD flags, const WCHAR *a, int na, const WCHAR *b, int nb, void *v, void *r,
                           LPARAM l) {
    (void)loc, (void)v, (void)r, (void)l;
    return CompareStringW(0, flags, a, na, b, nb);
}

int WINAPI CompareStringOrdinal(const WCHAR *a, int na, const WCHAR *b, int nb, BOOL nocase) { return cmp_w(a, na, b, nb, nocase); }

int WINAPI CompareStringA(DWORD lcid, DWORD flags, const char *a, int na, const char *b, int nb) {
    WCHAR *wa, *wb;
    int r;
    char *ca, *cb;
    (void)lcid;
    if (na < 0) na = (int)strlen(a);
    if (nb < 0) nb = (int)strlen(b);
    ca = strndup(a, (size_t)na);
    cb = strndup(b, (size_t)nb);
    wa = utf8_to_w_dup(ca);
    wb = utf8_to_w_dup(cb);
    r = cmp_w(wa, -1, wb, -1, (flags & NORM_IGNORECASE) != 0);
    free(ca), free(cb), free(wa), free(wb);
    return r;
}

#define LCMAP_LOWERCASE 0x100
#define LCMAP_UPPERCASE 0x200

int WINAPI LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *s, int n, WCHAR *out, int cap) {
    (void)lcid;
    if (n < 0) n = (int)w_len(s) + 1;
    if (!cap) return n;
    if (cap < n) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int i = 0; i < n; i++)
        out[i] = (flags & LCMAP_UPPERCASE) ? wupper(s[i]) : (flags & LCMAP_LOWERCASE) ? wlower(s[i]) : s[i];
    return n;
}

int WINAPI LCMapStringEx(const WCHAR *loc, DWORD flags, const WCHAR *s, int n, WCHAR *out, int cap, void *v, void *r, LPARAM l) {
    (void)loc, (void)v, (void)r, (void)l;
    return LCMapStringW(0, flags, s, n, out, cap);
}

int WINAPI LCMapStringA(DWORD lcid, DWORD flags, const char *s, int n, char *out, int cap) {
    (void)lcid;
    if (n < 0) n = (int)strlen(s) + 1;
    if (!cap) return n;
    if (cap < n) return 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if ((flags & LCMAP_UPPERCASE) && c >= 'a' && c <= 'z') c -= 32;
        if ((flags & LCMAP_LOWERCASE) && c >= 'A' && c <= 'Z') c += 32;
        out[i] = c;
    }
    return n;
}

BOOL WINAPI GetStringTypeW(DWORD type, const WCHAR *s, int n, WORD *out) {
    if (n < 0) n = (int)w_len(s) + 1;
    for (int i = 0; i < n; i++) {
        WCHAR c = s[i];
        WORD t = 0;
        if (type == 1) {      /* CT_CTYPE1 */
            if (c >= 'A' && c <= 'Z') t |= 0x1 | 0x100;
            if (c >= 'a' && c <= 'z') t |= 0x2 | 0x100;
            if (c >= '0' && c <= '9') t |= 0x4;
            if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x8;
            if (c == ' ' || c == '\t') t |= 0x40;
            if ((c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126)) t |= 0x10;
            if (c < 32 || c == 127) t |= 0x20;
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= 0x80;
            if (c >= 0xC0 && c != 0xD7 && c != 0xF7) t |= 0x100 | (wlower(c) == c ? 0x2 : 0x1);
            if (c) t |= 0x200;   /* C1_DEFINED */
        }
        out[i] = t;
    }
    return TRUE;
}

BOOL WINAPI GetStringTypeExW(DWORD lcid, DWORD type, const WCHAR *s, int n, WORD *out) {
    (void)lcid;
    return GetStringTypeW(type, s, n, out);
}

BOOL WINAPI GetStringTypeA(DWORD lcid, DWORD type, const char *s, int n, WORD *out) {
    WCHAR tmp[1024];
    (void)lcid;
    if (n < 0) n = (int)strlen(s) + 1;
    if (n > 1024) n = 1024;
    for (int i = 0; i < n; i++) tmp[i] = (uint8_t)s[i];
    return GetStringTypeW(type, tmp, n, out);
}

int WINAPI lstrlenA(const char *s) { return s ? (int)strlen(s) : 0; }
int WINAPI lstrlenW(const WCHAR *s) { return s ? (int)w_len(s) : 0; }
char *WINAPI lstrcpyA(char *d, const char *s) { return strcpy(d, s); }
WCHAR *WINAPI lstrcpyW(WCHAR *d, const WCHAR *s) {
    memmove(d, s, (w_len(s) + 1) * 2);
    return d;
}
char *WINAPI lstrcpynA(char *d, const char *s, int n) {
    if (n <= 0) return d;
    strncpy(d, s, (size_t)n - 1);
    d[n - 1] = 0;
    return d;
}
WCHAR *WINAPI lstrcpynW(WCHAR *d, const WCHAR *s, int n) {
    int i = 0;
    if (n <= 0) return d;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return d;
}
char *WINAPI lstrcatA(char *d, const char *s) { return strcat(d, s); }
WCHAR *WINAPI lstrcatW(WCHAR *d, const WCHAR *s) {
    lstrcpyW(d + w_len(d), s);
    return d;
}
int WINAPI lstrcmpA(const char *a, const char *b) {
    int r = strcmp(a, b);
    return r < 0 ? -1 : r > 0;
}
int WINAPI lstrcmpW(const WCHAR *a, const WCHAR *b) { return cmp_w(a, -1, b, -1, 0) - 2; }
int WINAPI lstrcmpiA(const char *a, const char *b) {
    int r = a_icmp(a, b);
    return r < 0 ? -1 : r > 0;
}
int WINAPI lstrcmpiW(const WCHAR *a, const WCHAR *b) { return cmp_w(a, -1, b, -1, 1) - 2; }

/* locales: English (United States) */
#define LOCALE_USER_DEFAULT 0x400

DWORD WINAPI GetUserDefaultLCID(void) { return 0x409; }
DWORD WINAPI GetSystemDefaultLCID(void) { return 0x409; }
WORD WINAPI GetUserDefaultLangID(void) { return 0x409; }
WORD WINAPI GetSystemDefaultLangID(void) { return 0x409; }
WORD WINAPI GetUserDefaultUILanguage(void) { return 0x409; }
WORD WINAPI GetSystemDefaultUILanguage(void) { return 0x409; }
DWORD WINAPI GetThreadLocale(void) { return 0x409; }
BOOL WINAPI SetThreadLocale(DWORD l) {
    (void)l;
    return TRUE;
}
WORD WINAPI SetThreadUILanguage(WORD l) { return l ? l : 0x409; }
WORD WINAPI GetThreadUILanguage(void) { return 0x409; }
BOOL WINAPI IsValidLocale(DWORD l, DWORD f) {
    (void)l, (void)f;
    return TRUE;
}
BOOL WINAPI IsValidLocaleName(const WCHAR *n) {
    (void)n;
    return TRUE;
}

int WINAPI GetUserDefaultLocaleName(WCHAR *buf, int n) { return (int)put_w("en-US", buf, (DWORD)n) + 1; }
int WINAPI GetSystemDefaultLocaleName(WCHAR *buf, int n) { return GetUserDefaultLocaleName(buf, n); }
int WINAPI LCIDToLocaleName(DWORD lcid, WCHAR *buf, int n, DWORD f) {
    (void)lcid, (void)f;
    return GetUserDefaultLocaleName(buf, n);
}
DWORD WINAPI LocaleNameToLCID(const WCHAR *n, DWORD f) {
    (void)n, (void)f;
    return 0x409;
}

static const char *locale_value(DWORD type) {
    switch (type & 0xFFFF) {
    case 0x1: return "0409";          /* LOCALE_ILANGUAGE */
    case 0x2: return "English (United States)";
    case 0x3: return "ENU";
    case 0x4: return "English";
    case 0x5: return "1";             /* ICOUNTRY */
    case 0x6: return "United States";
    case 0x7: return "USA";
    case 0xB: return "437";           /* IDEFAULTCODEPAGE */
    case 0x1004: return "65001";      /* IDEFAULTANSICODEPAGE */
    case 0xC: return ",";             /* SLIST */
    case 0xE: return ".";             /* SDECIMAL */
    case 0xF: return ",";             /* STHOUSAND */
    case 0x10: return "3;0";          /* SGROUPING */
    case 0x11: return "2";            /* IDIGITS */
    case 0x12: return "1";            /* ILZERO */
    case 0x14: return "$";
    case 0x1D: return "/";            /* SDATE */
    case 0x1E: return ":";            /* STIME */
    case 0x1F: return "M/d/yyyy";     /* SSHORTDATE */
    case 0x20: return "dddd, MMMM d, yyyy";
    case 0x1003: return "h:mm:ss tt"; /* STIMEFORMAT */
    case 0x59: return "en";           /* SISO639LANGNAME */
    case 0x5A: return "US";           /* SISO3166CTRYNAME */
    case 0x5C: return "en-US";        /* SNAME */
    case 0x1001: return "en";
    default: return "";
    }
}

int WINAPI GetLocaleInfoA(DWORD lcid, DWORD type, char *buf, int n) {
    const char *v = locale_value(type);
    (void)lcid;
    if (type & 0x20000000) {          /* LOCALE_RETURN_NUMBER */
        if (n < 2) return 2;
        *(DWORD *)buf = (DWORD)atoi(v);
        return 2;
    }
    return (int)put_a(v, buf, (DWORD)n) + (n && buf ? 1 : 0);
}

int WINAPI GetLocaleInfoW(DWORD lcid, DWORD type, WCHAR *buf, int n) {
    const char *v = locale_value(type);
    (void)lcid;
    if (type & 0x20000000) {
        if (n < 2) return 2;
        *(DWORD *)buf = (DWORD)atoi(v);
        return 2;
    }
    if (!n || !buf) return utf8_to_w(v, -1, 0, 0);
    return (int)put_w(v, buf, (DWORD)n) + 1;
}

int WINAPI GetLocaleInfoEx(const WCHAR *name, DWORD type, WCHAR *buf, int n) {
    (void)name;
    return GetLocaleInfoW(0x409, type, buf, n);
}

typedef BOOL (WCALLBACK *LOCALE_ENUMPROCW)(WCHAR *);
BOOL WINAPI EnumSystemLocalesW(LOCALE_ENUMPROCW fn, DWORD flags) {
    WCHAR s[] = u"00000409";
    (void)flags;
    fn(s);
    return TRUE;
}

typedef BOOL (WCALLBACK *LOCALE_ENUMPROCEX)(WCHAR *, DWORD, LPARAM);
BOOL WINAPI EnumSystemLocalesEx(LOCALE_ENUMPROCEX fn, DWORD flags, LPARAM param, void *reserved) {
    WCHAR s[] = u"en-US";
    (void)flags, (void)reserved;
    fn(s, 1 /* LOCALE_WINDOWS */, param);
    return TRUE;
}

int WINAPI GetDateFormatW(DWORD lcid, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n);
int WINAPI GetTimeFormatW(DWORD lcid, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n);
int WINAPI GetDateFormatEx(const WCHAR *locale, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n, const WCHAR *cal) {
    (void)locale, (void)cal;
    return GetDateFormatW(0x409, f, st, fmt, buf, n);
}
int WINAPI GetTimeFormatEx(const WCHAR *locale, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n) {
    (void)locale;
    return GetTimeFormatW(0x409, f, st, fmt, buf, n);
}

/* ---- dates and times as text: Windows' pictures, in US English --------------------------
 * Date: d dd (day), ddd dddd (its name), M MM (month), MMM MMMM (its name),
 * y yy yyyy (year), g gg (era).  Time: h hh (12-hour), H HH, m mm, s ss,
 * t tt (A / AM).  'text' is copied; '' is a quote.  Anything else is copied. */

static const char *const day_names[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const month_names[12] = { "January", "February", "March", "April", "May", "June", "July",
                                             "August", "September", "October", "November", "December" };

static int days_in(int y, int m) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return d[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
}

/* the day of the week (Sakamoto's method): Windows works it out, not reading wDayOfWeek */
static int weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

typedef struct {
    char  *out;
    size_t len, cap, lit;       /* lit: where the text after the last element began */
    int    any;                 /* an element has been written */
    int    drop_lit;            /* the first element was left out: so is the text after it */
} pic_t;

/* what does not fit is counted, not written; cap is never 0 */
static void pic_put(pic_t *p, const char *s, size_t n) {
    if (p->len + 1 < p->cap) {
        size_t room = p->cap - 1 - p->len;
        memcpy(p->out + p->len, s, n < room ? n : room);
    }
    p->len += n;
}

/* TIME_NO... flags leave an element out, with the separator before it */
static void pic_skip(pic_t *p) {
    if (p->any) p->len = p->lit;
    else p->drop_lit = 1;
}

static void pic_elem(pic_t *p, const char *s) {
    if (p->drop_lit) p->len = p->lit, p->drop_lit = 0;
    pic_put(p, s, strlen(s));
    p->lit = p->len;
    p->any = 1;
}

static int picture(const char *fmt, const SYSTEMTIME *st, int date, DWORD f, char *out, size_t cap) {
    pic_t p = { out, 0, cap, 0, 0, 0 };
    char v[32];
    while (*fmt) {
        char c = *fmt;
        int n = 1;
        if (c == '\'') {
            for (fmt++; *fmt; fmt++) {
                if (*fmt == '\'') {
                    if (fmt[1] != '\'') break;
                    fmt++;
                }
                pic_put(&p, fmt, 1);
            }
            if (*fmt) fmt++;     /* the closing quote */
            continue;
        }
        while (fmt[n] == c) n++;
        if (date && c == 'd') {
            if (n >= 3) snprintf(v, sizeof(v), n == 3 ? "%.3s" : "%s", day_names[weekday(st->wYear, st->wMonth, st->wDay)]);
            else snprintf(v, sizeof(v), n == 2 ? "%02u" : "%u", st->wDay);
            pic_elem(&p, v);
        } else if (date && c == 'M') {
            if (n >= 3) snprintf(v, sizeof(v), n == 3 ? "%.3s" : "%s", month_names[st->wMonth - 1]);
            else snprintf(v, sizeof(v), n == 2 ? "%02u" : "%u", st->wMonth);
            pic_elem(&p, v);
        } else if (date && c == 'y') {
            if (n >= 3) snprintf(v, sizeof(v), "%04u", st->wYear);
            else snprintf(v, sizeof(v), n == 2 ? "%02u" : "%u", st->wYear % 100);
            pic_elem(&p, v);
        } else if (date && c == 'g') {
            pic_elem(&p, "A.D.");
        } else if (!date && (c == 'h' || c == 'H')) {
            unsigned h = st->wHour;
            if (c == 'h' && !(f & 8)) h = h % 12 ? h % 12 : 12;      /* TIME_FORCE24HOURFORMAT */
            snprintf(v, sizeof(v), n >= 2 ? "%02u" : "%u", h);
            pic_elem(&p, v);
        } else if (!date && (c == 'm' || c == 's')) {
            if ((f & 1) || (c == 's' && (f & 2))) pic_skip(&p);      /* TIME_NOMINUTESORSECONDS, TIME_NOSECONDS */
            else {
                snprintf(v, sizeof(v), n >= 2 ? "%02u" : "%u", c == 'm' ? st->wMinute : st->wSecond);
                pic_elem(&p, v);
            }
        } else if (!date && c == 't') {
            if (f & (4 | 8)) pic_skip(&p);                          /* TIME_NOTIMEMARKER, or 24 hours */
            else pic_elem(&p, st->wHour < 12 ? (n >= 2 ? "AM" : "A") : (n >= 2 ? "PM" : "P"));
        } else {
            pic_put(&p, fmt, (size_t)n);
        }
        fmt += n;
    }
    if (p.drop_lit) p.len = p.lit;
    if (cap) out[p.len < cap ? p.len : cap - 1] = 0;
    return (int)p.len;
}

/* the counted rule: n 0 asks for the size, with the NUL; too small fails */
static int put_counted(const char *a, WCHAR *buf, int n) {
    int need = utf8_to_w(a, -1, 0, 0);
    if (!n) return need;
    if (n < 0 || !buf) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (need > n) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    utf8_to_w(a, -1, buf, n);
    return need;
}

static int date_time(int date, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n) {
    char pic[256], a[1024];     /* no picture of 255 bytes writes more than 4 times as many */
    SYSTEMTIME now;
    if (!st) GetLocalTime(&now), st = &now;
    if (date ? (st->wMonth < 1 || st->wMonth > 12 || st->wDay < 1 || st->wDay > days_in(st->wYear, st->wMonth) ||
                st->wYear < 1601 || st->wYear > 30827)
             : (st->wHour > 23 || st->wMinute > 59 || st->wSecond > 59 || st->wMilliseconds > 999)) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (fmt) {
        if (date && (f & (1 | 2 | 8))) {      /* a picture and DATE_SHORTDATE, _LONGDATE or _YEARMONTH */
            wset_last_error(1004);            /* ERROR_INVALID_FLAGS */
            return 0;
        }
        w_to_utf8(fmt, -1, pic, sizeof(pic));
    } else if (date)
        snprintf(pic, sizeof(pic), "%s", (f & 2) ? "dddd, MMMM d, yyyy" : (f & 8) ? "MMMM yyyy" : "M/d/yyyy");
    else
        snprintf(pic, sizeof(pic), "%s", "h:mm:ss tt");
    picture(pic, st, date, date ? 0 : f, a, sizeof(a));
    return put_counted(a, buf, n);
}

int WINAPI GetDateFormatW(DWORD lcid, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n) {
    (void)lcid;
    return date_time(1, f, st, fmt, buf, n);
}

int WINAPI GetTimeFormatW(DWORD lcid, DWORD f, const SYSTEMTIME *st, const WCHAR *fmt, WCHAR *buf, int n) {
    (void)lcid;
    return date_time(0, f, st, fmt, buf, n);
}

/* FormatMessage: system messages are short English ones, and a program's
 * own messages get %1.. inserts */
static const char *sys_message(DWORD id) {
    switch (id) {
    case 0: return "The operation completed successfully.";
    case 2: return "The system cannot find the file specified.";
    case 3: return "The system cannot find the path specified.";
    case 5: return "Access is denied.";
    case 6: return "The handle is invalid.";
    case 8: return "Not enough memory resources are available to process this command.";
    case 87: return "The parameter is incorrect.";
    case 120: return "This function is not supported on this system.";
    case 126: return "The specified module could not be found.";
    case 127: return "The specified procedure could not be found.";
    case 183: return "Cannot create a file when that file already exists.";
    default: return 0;
    }
}

#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x100
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x200
#define FORMAT_MESSAGE_FROM_STRING     0x400
#define FORMAT_MESSAGE_FROM_SYSTEM     0x1000

static int format_message(DWORD flags, const void *src, DWORD id, char *out, size_t cap, ms_va_list *args, int wide) {
    char tmp[2048];
    const char *msg = 0;
    size_t o = 0;
    if (flags & FORMAT_MESSAGE_FROM_STRING) {
        if (wide) {
            w_to_utf8(src, -1, tmp, sizeof(tmp));
            msg = tmp;
        } else {
            msg = src;
        }
    } else {
        msg = sys_message(id);
        if (!msg) {
            snprintf(tmp, sizeof(tmp), "Error %u.", id);
            msg = tmp;
        }
    }
    for (const char *p = msg; *p && o + 1 < cap; p++) {
        if (*p == '%' && !(flags & FORMAT_MESSAGE_IGNORE_INSERTS) && p[1] >= '1' && p[1] <= '9' && args) {
            /* inserts: as strings (the common case) */
            int idx = 0;
            const void *v;
            p++;
            while (*p >= '0' && *p <= '9') idx = idx * 10 + (*p++ - '0');
            if (*p == '!') {
                while (*++p && *p != '!') {
                }
            } else {
                p--;
            }
            v = ((void **)*args)[idx - 1];
            if (wide) {
                char a[1024];
                w_to_utf8(v, -1, a, sizeof(a));
                o += (size_t)snprintf(out + o, cap - o, "%s", a);
            } else {
                o += (size_t)snprintf(out + o, cap - o, "%s", (const char *)v);
            }
            if (o >= cap) o = cap - 1;
            continue;
        }
        if (*p == '%' && p[1] == 'n') {
            out[o++] = '\r';
            if (o + 1 < cap) out[o++] = '\n';
            p++;
            continue;
        }
        if (*p == '%' && p[1] && strchr("%.!", p[1])) {
            out[o++] = p[1];
            p++;
            continue;
        }
        out[o++] = *p;
    }
    if (!(flags & FORMAT_MESSAGE_FROM_STRING) && o + 2 < cap) out[o++] = '\r', out[o++] = '\n';
    out[o] = 0;
    return (int)o;
}

DWORD WINAPI FormatMessageA(DWORD flags, const void *src, DWORD id, DWORD lang, char *buf, DWORD n, ms_va_list *args) {
    char out[4096];
    int len;
    (void)lang;
    len = format_message(flags, src, id, out, sizeof(out), args, 0);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        char *p = LocalAlloc(0, (SIZE_T)len + 1);
        memcpy(p, out, (size_t)len + 1);
        *(char **)buf = p;
        return (DWORD)len;
    }
    if ((DWORD)len + 1 > n) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(buf, out, (size_t)len + 1);
    return (DWORD)len;
}

DWORD WINAPI FormatMessageW(DWORD flags, const void *src, DWORD id, DWORD lang, WCHAR *buf, DWORD n, ms_va_list *args) {
    char out[4096];
    int len;
    (void)lang;
    format_message(flags, src, id, out, sizeof(out), args, 1);
    len = utf8_to_w(out, -1, 0, 0);
    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        WCHAR *p = LocalAlloc(0, (SIZE_T)len * 2);
        utf8_to_w(out, -1, p, len);
        *(WCHAR **)buf = p;
        return (DWORD)len - 1;
    }
    if ((DWORD)len > n) {
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    utf8_to_w(out, -1, buf, (int)n);
    return (DWORD)len - 1;
}

int WINAPI MulDiv(int a, int b, int c) {
    int64_t r, half;
    if (!c) return -1;
    r = (int64_t)a * b;
    /* rounded half away from zero, as Windows does */
    half = (c < 0 ? -(int64_t)c : (int64_t)c) / 2;
    r = r >= 0 ? r + half : r - half;
    r /= c;
    if (r > INT32_MAX || r < INT32_MIN) return -1;
    return (int)r;
}

/* ---- locks -------------------------------------------------------------------------------- */

static long futex(void *addr, int op, int val, const struct timespec *ts) {
    return syscall(SYS_futex, addr, op, val, ts, 0, 0);
}

/* a small futex mutex: 0 free, 1 locked, 2 locked with waiters */
static void fmutex_lock(volatile int *w) {
    int c = __sync_val_compare_and_swap(w, 0, 1);
    if (c == 0) return;
    for (int spin = 0; spin < 100; spin++) {
        __builtin_ia32_pause();
        if (*w == 0 && __sync_bool_compare_and_swap(w, 0, 1)) return;
    }
    if (c != 2) c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex((void *)w, FUTEX_WAIT_PRIVATE, 2, 0);
        c = __atomic_exchange_n(w, 2, __ATOMIC_ACQUIRE);
    }
}

static int fmutex_trylock(volatile int *w) { return __sync_bool_compare_and_swap(w, 0, 1); }

static void fmutex_unlock(volatile int *w) {
    if (__atomic_exchange_n(w, 0, __ATOMIC_RELEASE) == 2) futex((void *)w, FUTEX_WAKE_PRIVATE, 1, 0);
}

/* CRITICAL_SECTION (40 bytes): DebugInfo, LockCount (our futex word),
 * RecursionCount, OwningThread, LockSemaphore, SpinCount */
typedef struct {
    void     *DebugInfo;
    LONG      LockCount;
    LONG      RecursionCount;
    HANDLE    OwningThread;
    HANDLE    LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION;

void WINAPI InitializeCriticalSection(CRITICAL_SECTION *cs) {
    memset(cs, 0, sizeof(*cs));
    cs->DebugInfo = (void *)(intptr_t)-1;
}

BOOL WINAPI InitializeCriticalSectionAndSpinCount(CRITICAL_SECTION *cs, DWORD spin) {
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
    return TRUE;
}

BOOL WINAPI InitializeCriticalSectionEx(CRITICAL_SECTION *cs, DWORD spin, DWORD flags) {
    (void)flags;
    return InitializeCriticalSectionAndSpinCount(cs, spin);
}

void WINAPI DeleteCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }

void WINAPI EnterCriticalSection(CRITICAL_SECTION *cs) {
    DWORD me = thread_id();
    if ((DWORD)(uintptr_t)cs->OwningThread == me) {
        cs->RecursionCount++;
        return;
    }
    fmutex_lock((volatile int *)&cs->LockCount);
    cs->OwningThread = (HANDLE)(uintptr_t)me;
    cs->RecursionCount = 1;
}

BOOL WINAPI TryEnterCriticalSection(CRITICAL_SECTION *cs) {
    DWORD me = thread_id();
    if ((DWORD)(uintptr_t)cs->OwningThread == me) {
        cs->RecursionCount++;
        return TRUE;
    }
    if (!fmutex_trylock((volatile int *)&cs->LockCount)) return FALSE;
    cs->OwningThread = (HANDLE)(uintptr_t)me;
    cs->RecursionCount = 1;
    return TRUE;
}

void WINAPI LeaveCriticalSection(CRITICAL_SECTION *cs) {
    if (--cs->RecursionCount > 0) return;
    cs->RecursionCount = 0;
    cs->OwningThread = 0;
    fmutex_unlock((volatile int *)&cs->LockCount);
}

DWORD WINAPI SetCriticalSectionSpinCount(CRITICAL_SECTION *cs, DWORD spin) {
    DWORD o = (DWORD)cs->SpinCount;
    cs->SpinCount = spin;
    return o;
}

/* SRW locks (one pointer): the low int is a futex mutex; shared holders
 * take it like writers (correct, if less parallel) */
void WINAPI InitializeSRWLock(void **l) { *l = 0; }
void WINAPI AcquireSRWLockExclusive(void **l) { fmutex_lock((volatile int *)l); }
void WINAPI ReleaseSRWLockExclusive(void **l) { fmutex_unlock((volatile int *)l); }
void WINAPI AcquireSRWLockShared(void **l) { fmutex_lock((volatile int *)l); }
void WINAPI ReleaseSRWLockShared(void **l) { fmutex_unlock((volatile int *)l); }
BOOLEAN WINAPI TryAcquireSRWLockExclusive(void **l) { return (BOOLEAN)fmutex_trylock((volatile int *)l); }
BOOLEAN WINAPI TryAcquireSRWLockShared(void **l) { return (BOOLEAN)fmutex_trylock((volatile int *)l); }

/* condition variables: a sequence number to wait on */
void WINAPI InitializeConditionVariable(void **cv) { *cv = 0; }

static BOOL cv_wait(void **cv, DWORD ms, void (*unlock)(void *), void (*lock)(void *), void *arg) {
    volatile int *seq = (volatile int *)cv;
    int v = *seq;
    struct timespec ts, *tp = 0;
    long r;
    if (ms != INFINITE) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        tp = &ts;
    }
    unlock(arg);
    r = futex((void *)seq, FUTEX_WAIT_PRIVATE, v, tp);
    lock(arg);
    if (r != 0 && errno == ETIMEDOUT) {
        wset_last_error(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

static void cs_unlock(void *a) { LeaveCriticalSection(a); }
static void cs_lock(void *a) { EnterCriticalSection(a); }
static void srw_unlock(void *a) { fmutex_unlock(a); }
static void srw_lock(void *a) { fmutex_lock(a); }

BOOL WINAPI SleepConditionVariableCS(void **cv, CRITICAL_SECTION *cs, DWORD ms) {
    /* the whole recursion is given up while waiting, as Windows does */
    LONG rec = cs->RecursionCount;
    BOOL r;
    cs->RecursionCount = 1;
    r = cv_wait(cv, ms, cs_unlock, cs_lock, cs);
    cs->RecursionCount = rec;
    return r;
}

BOOL WINAPI SleepConditionVariableSRW(void **cv, void **l, DWORD ms, ULONG flags) {
    (void)flags;
    return cv_wait(cv, ms, srw_unlock, srw_lock, (void *)l);
}

void WINAPI WakeConditionVariable(void **cv) {
    __atomic_add_fetch((int *)cv, 1, __ATOMIC_SEQ_CST);
    futex(cv, FUTEX_WAKE_PRIVATE, 1, 0);
}

void WINAPI WakeAllConditionVariable(void **cv) {
    __atomic_add_fetch((int *)cv, 1, __ATOMIC_SEQ_CST);
    futex(cv, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, 0);
}

/* WaitOnAddress (synchronization API) on futexes, for 4-byte values; others
 * by polling */
BOOL WINAPI WaitOnAddress(volatile void *addr, void *cmp, SIZE_T size, DWORD ms) {
    struct timespec ts, *tp = 0;
    if (ms != INFINITE) {
        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        tp = &ts;
    }
    if (size == 4) {
        long r = futex((void *)addr, FUTEX_WAIT_PRIVATE, *(int *)cmp, tp);
        if (r != 0 && errno == ETIMEDOUT) {
            wset_last_error(ERROR_TIMEOUT);
            return FALSE;
        }
        return TRUE;
    }
    if (memcmp((const void *)addr, cmp, size) == 0) Sleep(1);
    return TRUE;
}

void WINAPI WakeByAddressSingle(void *addr) { futex(addr, FUTEX_WAKE_PRIVATE, 1, 0); }
void WINAPI WakeByAddressAll(void *addr) { futex(addr, FUTEX_WAKE_PRIVATE, 0x7FFFFFFF, 0); }

/* one-time initialisation */
typedef BOOL (WCALLBACK *INIT_ONCE_FN)(void *, void *, void **);
static int once_lock;

BOOL WINAPI InitOnceExecuteOnce(void **once, INIT_ONCE_FN fn, void *param, void **ctx) {
    if (__atomic_load_n((uintptr_t *)once, __ATOMIC_ACQUIRE) & 2) {
        if (ctx) *ctx = (void *)((uintptr_t)*once & ~(uintptr_t)3);
        return TRUE;
    }
    fmutex_lock(&once_lock);
    if (!((uintptr_t)*once & 2)) {
        void *c = 0;
        BOOL ok = fn(once, param, &c);
        if (!ok) {
            fmutex_unlock(&once_lock);
            return FALSE;
        }
        __atomic_store_n((uintptr_t *)once, ((uintptr_t)c & ~(uintptr_t)3) | 2, __ATOMIC_RELEASE);
    }
    fmutex_unlock(&once_lock);
    if (ctx) *ctx = (void *)((uintptr_t)*once & ~(uintptr_t)3);
    return TRUE;
}

void WINAPI InitOnceInitialize(void **once) { *once = 0; }

BOOL WINAPI InitOnceBeginInitialize(void **once, DWORD flags, BOOL *pending, void **ctx) {
    (void)flags;
    if ((uintptr_t)*once & 2) {
        *pending = FALSE;
        if (ctx) *ctx = (void *)((uintptr_t)*once & ~(uintptr_t)3);
        return TRUE;
    }
    fmutex_lock(&once_lock);
    if ((uintptr_t)*once & 2) {
        fmutex_unlock(&once_lock);
        *pending = FALSE;
        if (ctx) *ctx = (void *)((uintptr_t)*once & ~(uintptr_t)3);
        return TRUE;
    }
    *pending = TRUE;      /* the lock stays held until InitOnceComplete */
    return TRUE;
}

BOOL WINAPI InitOnceComplete(void **once, DWORD flags, void *ctx) {
    if (!(flags & 4)) __atomic_store_n((uintptr_t *)once, ((uintptr_t)ctx & ~(uintptr_t)3) | 2, __ATOMIC_RELEASE);  /* not INIT_ONCE_INIT_FAILED */
    fmutex_unlock(&once_lock);
    return TRUE;
}

/* interlocked singly-linked lists (16-byte header: we use the first pointer) */
static int slist_lock;

void WINAPI InitializeSListHead(void **h) { h[0] = h[1] = 0; }

void *WINAPI InterlockedPushEntrySList(void **h, void **e) {
    void *old;
    fmutex_lock(&slist_lock);
    old = h[0];
    e[0] = old;
    h[0] = e;
    h[1] = (void *)((uintptr_t)h[1] + 1);
    fmutex_unlock(&slist_lock);
    return old;
}

void *WINAPI InterlockedPopEntrySList(void **h) {
    void **e;
    fmutex_lock(&slist_lock);
    e = h[0];
    if (e) {
        h[0] = e[0];
        h[1] = (void *)((uintptr_t)h[1] - 1);
    }
    fmutex_unlock(&slist_lock);
    return e;
}

void *WINAPI InterlockedFlushSList(void **h) {
    void *e;
    fmutex_lock(&slist_lock);
    e = h[0];
    h[0] = 0;
    h[1] = 0;
    fmutex_unlock(&slist_lock);
    return e;
}

USHORT WINAPI QueryDepthSList(void **h) { return (USHORT)(uintptr_t)h[1]; }

/* ---- modules -------------------------------------------------------------------------------- */

HMODULE WINAPI GetModuleHandleA(const char *name) {
    wmodule_t *m;
    if (!name) return ld_main()->base;
    m = ld_by_name(name);
    if (!m) {
        wset_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    return m->base;
}

HMODULE WINAPI GetModuleHandleW(const WCHAR *name) {
    char a[512];
    if (!name) return GetModuleHandleA(0);
    w_to_utf8(name, -1, a, sizeof(a));
    return GetModuleHandleA(a);
}

wmodule_t *module_at(const void *p) {
    for (wmodule_t *m = ld_modules(); m; m = m->next)
        if ((const uint8_t *)p >= m->base && (const uint8_t *)p < m->base + m->size) return m;
    return 0;
}

#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4

BOOL WINAPI GetModuleHandleExW(DWORD flags, const WCHAR *name, HMODULE *out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        wmodule_t *m = module_at(name);
        *out = m ? m->base : 0;
    } else {
        *out = GetModuleHandleW(name);
    }
    if (!*out) wset_last_error(ERROR_MOD_NOT_FOUND);
    return *out != 0;
}

BOOL WINAPI GetModuleHandleExA(DWORD flags, const char *name, HMODULE *out) {
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) return GetModuleHandleExW(flags, (const WCHAR *)name, out);
    *out = GetModuleHandleA(name);
    return *out != 0;
}

static void module_win_path(wmodule_t *m, char *out, size_t cap) {
    if (m == ld_main()) snprintf(out, cap, "%s", g_exe_win);
    else if (m->builtin || !m->path[0]) snprintf(out, cap, "C:\\Windows\\System32\\%s", m->name);
    else path_to_win(m->path, out, cap);
}

DWORD WINAPI GetModuleFileNameA(HMODULE h, char *buf, DWORD n) {
    wmodule_t *m = ld_by_handle(h);
    char p[1024];
    size_t len;
    if (!m) {
        wset_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    module_win_path(m, p, sizeof(p));
    len = strlen(p);
    if (!n) return 0;
    if (len + 1 > n) {
        memcpy(buf, p, n - 1);
        buf[n - 1] = 0;
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    memcpy(buf, p, len + 1);
    wset_last_error(0);
    return (DWORD)len;
}

DWORD WINAPI GetModuleFileNameW(HMODULE h, WCHAR *buf, DWORD n) {
    wmodule_t *m = ld_by_handle(h);
    char p[1024];
    int len;
    if (!m) {
        wset_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    module_win_path(m, p, sizeof(p));
    len = utf8_to_w(p, -1, 0, 0);
    if (!n) return 0;
    if ((DWORD)len > n) {
        utf8_to_w(p, -1, buf, (int)n);
        wset_last_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    utf8_to_w(p, -1, buf, (int)n);
    wset_last_error(0);
    return (DWORD)len - 1;
}

DWORD WINAPI K32GetModuleFileNameExW(HANDLE p, HMODULE h, WCHAR *buf, DWORD n) {
    (void)p;
    return GetModuleFileNameW(h, buf, n);
}

DWORD WINAPI K32GetModuleBaseNameW(HANDLE p, HMODULE h, WCHAR *buf, DWORD n) {
    wmodule_t *m = ld_by_handle(h);
    (void)p;
    if (!m) return 0;
    return put_w(m == ld_main() ? strrchr(g_exe_win, '\\') + 1 : m->name, buf, n);
}

BOOL WINAPI QueryFullProcessImageNameW(HANDLE p, DWORD f, WCHAR *buf, DWORD *n) {
    DWORD r;
    (void)p, (void)f;
    r = GetModuleFileNameW(0, buf, *n);
    if (!r || r >= *n) return FALSE;
    *n = r;
    return TRUE;
}

BOOL WINAPI QueryFullProcessImageNameA(HANDLE p, DWORD f, char *buf, DWORD *n) {
    DWORD r;
    (void)p, (void)f;
    r = GetModuleFileNameA(0, buf, *n);
    if (!r || r >= *n) return FALSE;
    *n = r;
    return TRUE;
}

/* explicit loads of system DLLs that ICDA lacks fail, so programs take
 * their fallbacks (imports of them still get stubs) */
static HMODULE load_library(const char *name) {
    int err = 0;
    wmodule_t *m;
    const wdll_t *d = builtin_find(name);
    if (d == &dll_misc || (!d && 0)) {
        wset_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    m = ld_load(name, 0, &err);
    if (!m || m->builtin == &dll_misc) {
        if (wtrace) wlog("LoadLibrary(%s): not here", name);
        wset_last_error(err ? (DWORD)err : ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (ld_init_all() != 0) {
        wset_last_error(1114);   /* ERROR_DLL_INIT_FAILED */
        return 0;
    }
    if (wtrace) wlog("LoadLibrary(%s) = %p", name, (void *)m->base);
    return m->base;
}

HMODULE WINAPI LoadLibraryA(const char *name) { return load_library(name); }

HMODULE WINAPI LoadLibraryW(const WCHAR *name) {
    char a[1024];
    w_to_utf8(name, -1, a, sizeof(a));
    return load_library(a);
}

HMODULE WINAPI LoadLibraryExA(const char *name, HANDLE f, DWORD flags) {
    (void)f;
    if (flags & 0x22) {       /* LOAD_LIBRARY_AS_DATAFILE(_EXCLUSIVE), _AS_IMAGE_RESOURCE */
        wmodule_t *m = ld_by_name(name);
        if (m) return m->base;
    }
    return load_library(name);
}

HMODULE WINAPI LoadLibraryExW(const WCHAR *name, HANDLE f, DWORD flags) {
    char a[1024];
    w_to_utf8(name, -1, a, sizeof(a));
    return LoadLibraryExA(a, f, flags);
}

HMODULE WINAPI LoadPackagedLibrary(const WCHAR *name, DWORD r) {
    (void)name, (void)r;
    wset_last_error(ERROR_MOD_NOT_FOUND);
    return 0;
}

BOOL WINAPI FreeLibrary(HMODULE h) {
    wmodule_t *m = ld_by_handle(h);
    if (m && m->refs > 1) m->refs--;
    return TRUE;
}

void WINAPI ExitThread(DWORD code);
void WINAPI FreeLibraryAndExitThread(HMODULE h, DWORD code) {
    FreeLibrary(h);
    ExitThread(code);
}

FARPROC WINAPI GetProcAddress(HMODULE h, const char *name) {
    wmodule_t *m = ld_by_handle(h);
    void *p;
    if (!m) {
        wset_last_error(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if ((uintptr_t)name < 0x10000) p = ld_proc(m, 0, (int)(uintptr_t)name);
    else p = ld_proc(m, name, 0);
    if (!p) {
        void *ld_delay_stub(wmodule_t *m, const char *name, int ordinal);
        void *ld_export_stub(wmodule_t *m, const char *name, int ordinal);
        int by_number = (uintptr_t)name < 0x10000;
        p = by_number ? ld_delay_stub(m, 0, (int)(uintptr_t)name) : ld_delay_stub(m, name, 0);
        /* a function Windows' DLL has: a stub that fails as it would */
        if (!p) p = by_number ? ld_export_stub(m, 0, (int)(uintptr_t)name) : ld_export_stub(m, name, 0);
        if (p) return p;
    }
    if (!p) {
        if (wtrace >= 2) {
            if ((uintptr_t)name < 0x10000) wlog("GetProcAddress(%s, #%d): none", m->name, (int)(uintptr_t)name);
            else wlog("GetProcAddress(%s, %s): none", m->name, name);
        }
        wset_last_error(ERROR_PROC_NOT_FOUND);
    }
    return p;
}

BOOL WINAPI DisableThreadLibraryCalls(HMODULE h) {
    (void)h;
    return TRUE;
}

BOOL WINAPI SetDllDirectoryW(const WCHAR *d) {
    (void)d;
    return TRUE;
}
BOOL WINAPI SetDllDirectoryA(const char *d) {
    (void)d;
    return TRUE;
}
void *WINAPI AddDllDirectory(const WCHAR *d) {
    (void)d;
    return (void *)1;
}
BOOL WINAPI RemoveDllDirectory(void *c) {
    (void)c;
    return TRUE;
}
BOOL WINAPI SetDefaultDllDirectories(DWORD f) {
    (void)f;
    return TRUE;
}

/* resources: a resource handle is the data's address */
typedef struct {
    void *data;
    DWORD size;
} wres_t;

HRSRC WINAPI FindResourceExW(HMODULE h, const WCHAR *type, const WCHAR *name, WORD lang) {
    wmodule_t *m = ld_by_handle(h);
    DWORD size = 0;
    void *p;
    wres_t *r;
    (void)lang;
    p = ld_resource(m, type, name, &size);
    if (!p) {
        wset_last_error(1813);   /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        return 0;
    }
    r = malloc(sizeof(*r));
    r->data = p;
    r->size = size;
    return r;
}

HRSRC WINAPI FindResourceW(HMODULE h, const WCHAR *name, const WCHAR *type) { return FindResourceExW(h, type, name, 0); }

static const WCHAR *res_name_a(const char *s, WCHAR *buf) {
    if ((uintptr_t)s < 0x10000) return (const WCHAR *)s;
    utf8_to_w(s, -1, buf, 256);
    return buf;
}

HRSRC WINAPI FindResourceA(HMODULE h, const char *name, const char *type) {
    WCHAR a[256], b[256];
    return FindResourceExW(h, res_name_a(type, b), res_name_a(name, a), 0);
}

HRSRC WINAPI FindResourceExA(HMODULE h, const char *type, const char *name, WORD lang) {
    WCHAR a[256], b[256];
    return FindResourceExW(h, res_name_a(type, b), res_name_a(name, a), lang);
}

HGLOBAL WINAPI LoadResource(HMODULE h, HRSRC r) {
    (void)h;
    return r ? ((wres_t *)r)->data : 0;
}
void *WINAPI LockResource(HGLOBAL h) { return h; }
DWORD WINAPI SizeofResource(HMODULE h, HRSRC r) {
    (void)h;
    return r ? ((wres_t *)r)->size : 0;
}
BOOL WINAPI FreeResource(HGLOBAL h) {
    (void)h;
    return FALSE;
}
typedef BOOL (WCALLBACK *ENUMRESNAMEPROCW)(HMODULE, const WCHAR *, WCHAR *, LONG_PTR);
typedef BOOL (WCALLBACK *ENUMRESNAMEPROCA)(HMODULE, const char *, char *, LONG_PTR);
typedef BOOL (WCALLBACK *ENUMRESTYPEPROCW)(HMODULE, WCHAR *, LONG_PTR);
typedef BOOL (WCALLBACK *ENUMRESTYPEPROCA)(HMODULE, char *, LONG_PTR);

typedef struct {
    HMODULE     h;
    const void *type;
    void       *fn;
    LONG_PTR    param;
    int         ansi, types;
} res_enum_t;

static int res_enum_cb(void *ctx, const WCHAR *name) {
    res_enum_t *e = ctx;
    if (e->ansi) {
        char a[256];
        const char *n = (uintptr_t)name < 0x10000 ? (const char *)name : (w_to_utf8(name, -1, a, sizeof(a)), a);
        if (e->types) return ((ENUMRESTYPEPROCA)e->fn)(e->h, (char *)n, e->param);
        return ((ENUMRESNAMEPROCA)e->fn)(e->h, e->type, (char *)n, e->param);
    }
    if (e->types) return ((ENUMRESTYPEPROCW)e->fn)(e->h, (WCHAR *)name, e->param);
    return ((ENUMRESNAMEPROCW)e->fn)(e->h, e->type, (WCHAR *)name, e->param);
}

BOOL WINAPI EnumResourceNamesW(HMODULE h, const WCHAR *type, void *fn, LONG_PTR p) {
    res_enum_t e = { h ? h : ld_main()->base, type, fn, p, 0, 0 };
    if (ld_enum_resources(ld_by_handle(h), type, res_enum_cb, &e) <= 0) {
        wset_last_error(1813);     /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI EnumResourceNamesA(HMODULE h, const char *type, void *fn, LONG_PTR p) {
    WCHAR wt[256];
    const WCHAR *t = res_name_a(type, wt);
    res_enum_t e = { h ? h : ld_main()->base, type, fn, p, 1, 0 };
    if (ld_enum_resources(ld_by_handle(h), t, res_enum_cb, &e) <= 0) {
        wset_last_error(1813);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI EnumResourceNamesExW(HMODULE h, const WCHAR *type, void *fn, LONG_PTR p, DWORD f, WORD lang) {
    (void)f, (void)lang;
    return EnumResourceNamesW(h, type, fn, p);
}

BOOL WINAPI EnumResourceTypesW(HMODULE h, void *fn, LONG_PTR p) {
    res_enum_t e = { h ? h : ld_main()->base, 0, fn, p, 0, 1 };
    return ld_enum_resources(ld_by_handle(h), 0, res_enum_cb, &e) > 0;
}

BOOL WINAPI EnumResourceTypesA(HMODULE h, void *fn, LONG_PTR p) {
    res_enum_t e = { h ? h : ld_main()->base, 0, fn, p, 1, 1 };
    return ld_enum_resources(ld_by_handle(h), 0, res_enum_cb, &e) > 0;
}

BOOL WINAPI EnumResourceNamesExA(HMODULE h, const char *type, void *fn, LONG_PTR p, DWORD f, WORD lang) {
    (void)f, (void)lang;
    return EnumResourceNamesA(h, type, fn, p);
}

/* ---- processes ------------------------------------------------------------------------------- */

/* a child of another Windows program says its whole exit code */
static void self_exit_report(UINT code) {
    const char *fd = getenv("ICDA_WIN_EXITFD");
    if (fd) {
        int f = atoi(fd);
        (void)!write(f, &code, sizeof(code));
        close(f);
        unsetenv("ICDA_WIN_EXITFD");
    }
}

void WINAPI ExitProcess(UINT code) {
    if (wtrace) wlog("ExitProcess(%u)", code);
    fflush(0);
    self_exit_report(code);
    exit((int)code);
}

BOOL WINAPI TerminateProcess(HANDLE h, UINT code) {
    whandle_t *w;
    if (h == (HANDLE)(intptr_t)-1 || h == 0) {
        fflush(0);
        self_exit_report(code);
        _exit((int)code);
    }
    w = h_get(h, H_PROCESS);
    if (w) {
        kill(((wproc_t *)w->obj)->pid, SIGKILL);
        return TRUE;
    }
    wset_last_error(ERROR_ACCESS_DENIED);
    return FALSE;
}

static void self_exit_report(UINT code);

BOOL WINAPI GetExitCodeProcess(HANDLE h, DWORD *code) {
    whandle_t *w = h_get(h, H_PROCESS);
    if (h == (HANDLE)(intptr_t)-1) {
        *code = 259;
        return TRUE;
    }
    if (!w) {
        wset_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    *code = ((wproc_t *)w->obj)->done ? ((wproc_t *)w->obj)->code : 259;   /* STILL_ACTIVE */
    return TRUE;
}

HANDLE WINAPI OpenProcess(DWORD a, BOOL i, DWORD pid) {
    (void)a, (void)i;
    if (pid == (DWORD)getpid()) return (HANDLE)(intptr_t)-1;
    wset_last_error(ERROR_ACCESS_DENIED);
    return 0;
}

DWORD WINAPI GetProcessId(HANDLE h) {
    whandle_t *w = h_get(h, H_PROCESS);
    return w ? (DWORD)((wproc_t *)w->obj)->pid : (DWORD)getpid();
}

DWORD WINAPI GetProcessVersion(DWORD pid) {
    (void)pid;
    return (10u << 16);
}

/* ---- starting programs ---------------------------------------------------------------------
 * A Windows program starts another as ICDA does it: icda-win runs it in a
 * process of its own.  The child gets the exact command line (Windows
 * passes one string), keeps the current folder it is given (Explorer's
 * "its own folder" is for programs the user starts), and writes its
 * 32-bit exit code back through a pipe, since Unix exit codes are 8 bits. */

static char self_path[1024];

static const char *icda_win_path(void) {
    if (!self_path[0]) {
        ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
        if (n > 0) self_path[n] = 0;
        else snprintf(self_path, sizeof(self_path), "/usr/bin/icda-win");
    }
    return self_path;
}

static void *reaper(void *arg) {
    wproc_t *p = arg;
    int st = 0;
    DWORD code = 0;
    ssize_t n;
    while (waitpid(p->pid, &st, 0) < 0 && errno == EINTR) {
    }
    /* the code the child wrote, else what Unix says */
    n = read(p->exit_fd, &code, sizeof(code));
    if (n != (ssize_t)sizeof(code)) code = WIFEXITED(st) ? (DWORD)WEXITSTATUS(st) : 0xC0000005u;
    close(p->exit_fd);
    sync_lock();
    p->code = code;
    p->done = 1;
    sync_broadcast();
    sync_unlock();
    if (wtrace) wlog("process %d ended: %u", p->pid, code);
    return 0;
}

/* the program a command line starts: its first word (quoted or not) */
static void first_word(const char *cmd, char *out, size_t cap) {
    size_t o = 0;
    const char *p = cmd;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && o + 1 < cap) out[o++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && o + 1 < cap) out[o++] = *p++;
    }
    out[o] = 0;
}

/* finds the program: as given, with .exe, then in the program's folder,
 * the current folder, System32 and PATH */
static int find_program(const char *name, char *out, size_t cap) {
    char tries[4][1100], with_exe[1100], dir[1024];
    int nt = 0;
    struct stat st;
    snprintf(with_exe, sizeof(with_exe), "%s%s", name, strrchr(name, '.') > strrchr(name, '\\') ? "" : ".exe");
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) {
        if (path_find(name, out, cap) == 0 && stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 0;
        if (path_find(with_exe, out, cap) == 0 && stat(out, &st) == 0 && S_ISREG(st.st_mode)) return 0;
        return -1;
    }
    snprintf(dir, sizeof(dir), "%s", g_exe_unix);
    if (strrchr(dir, '/')) *strrchr(dir, '/') = 0;
    snprintf(tries[nt++], sizeof(tries[0]), "%s/%s", dir, with_exe);
    snprintf(tries[nt++], sizeof(tries[0]), "./%s", with_exe);
    snprintf(tries[nt++], sizeof(tries[0]), "%s/Windows/System32/%s", path_prefix(), with_exe);
    for (int i = 0; i < nt; i++) {
        if (stat(tries[i], &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, cap, "%s", tries[i]);
            return 0;
        }
    }
    return -1;
}

typedef struct {
    HANDLE hProcess, hThread;
    DWORD  dwProcessId, dwThreadId;
} PROCESS_INFORMATION;

extern char **environ;

static BOOL create_process(const char *app, const char *cmd, BOOL inherit, DWORD flags, void *env, const char *dir,
                           STARTUPINFOW *si, PROCESS_INFORMATION *pi) {
    char exe[1100], word[1100], *argv[3], **envp;
    char udir[1100];
    int pipefd[2];
    pid_t pid;
    posix_spawn_file_actions_t fa;
    wproc_t *p;
    int nenv = 0, k = 0;
    (void)inherit;
    if (app && *app) snprintf(word, sizeof(word), "%s", app);
    else first_word(cmd ? cmd : "", word, sizeof(word));
    /* crash reporters that send reports to their maker are not started:
     * ICDA keeps a program's crashes to itself */
    if (cmd && strstr(cmd, "--type=crashpad-handler") && !getenv("ICDA_WIN_CRASHPAD")) {
        if (wtrace) wlog("CreateProcess: a crash reporter, not started");
        wset_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (!word[0] || find_program(word, exe, sizeof(exe)) != 0) {
        if (wtrace) wlog("CreateProcess(%s): not found", word);
        wset_last_error(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    {
        /* only Windows programs: a PE file */
        int fd = open(exe, O_RDONLY | O_CLOEXEC);
        char mz[2] = { 0, 0 };
        if (fd < 0 || read(fd, mz, 2) != 2 || mz[0] != 'M' || mz[1] != 'Z') {
            if (fd >= 0) close(fd);
            wlog("CreateProcess(%s): not a Windows program", word);
            wset_last_error(193);     /* ERROR_BAD_EXE_FORMAT */
            return FALSE;
        }
        close(fd);
    }
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        set_errno_error();
        return FALSE;
    }
    /* the environment: the given block (UTF-16 or ANSI), or ours */
    {
        char **src = environ;
        int unicode = (flags & 0x400) != 0;      /* CREATE_UNICODE_ENVIRONMENT */
        char **list = 0;
        if (env) {
            int count = 0;
            if (unicode)
                for (const WCHAR *e = env; *e; e += w_len(e) + 1) count++;
            else
                for (const char *e = env; *e; e += strlen(e) + 1) count++;
            list = calloc((size_t)count + 1, sizeof(char *));
            count = 0;
            if (unicode)
                for (const WCHAR *e = env; *e; e += w_len(e) + 1) list[count++] = w_to_utf8_dup(e);
            else
                for (const char *e = env; *e; e += strlen(e) + 1) list[count++] = strdup(e);
            src = list;
        }
        while (src[nenv]) nenv++;
        envp = calloc((size_t)nenv + 6, sizeof(char *));
        for (int i = 0; i < nenv; i++)
            if (strncmp(src[i], "ICDA_WIN_CMDLINE=", 17) && strncmp(src[i], "ICDA_WIN_EXITFD=", 16) &&
                strncmp(src[i], "ICDA_WIN_KEEP_CWD=", 18))
                envp[k++] = src[i];
    }
    {
        static char fdvar[32];
        char *cl = malloc(strlen(cmd ? cmd : word) + 32);
        sprintf(cl, "ICDA_WIN_CMDLINE=%s", cmd && *cmd ? cmd : word);
        envp[k++] = cl;
        snprintf(fdvar, sizeof(fdvar), "ICDA_WIN_EXITFD=3");
        envp[k++] = strdup(fdvar);
        envp[k++] = "ICDA_WIN_KEEP_CWD=1";
        envp[k] = 0;
    }
    posix_spawn_file_actions_init(&fa);
    /* the exit code pipe is the child's descriptor 3 */
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], 3);
    if (si && (si->dwFlags & 0x100)) {           /* STARTF_USESTDHANDLES */
        int fds[3] = { file_fd(si->hStdInput), file_fd(si->hStdOutput), file_fd(si->hStdError) };
        for (int i = 0; i < 3; i++)
            if (fds[i] >= 0 && fds[i] != i) posix_spawn_file_actions_adddup2(&fa, fds[i], i);
    }
    if (dir && *dir && path_find(dir, udir, sizeof(udir)) == 0) posix_spawn_file_actions_addchdir_np(&fa, udir);
    argv[0] = (char *)icda_win_path();
    argv[1] = exe;
    argv[2] = 0;
    if (posix_spawn(&pid, argv[0], &fa, 0, argv, envp) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        close(pipefd[0]);
        close(pipefd[1]);
        set_errno_error();
        return FALSE;
    }
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (wtrace) wlog("CreateProcess: %s -> pid %d (%s)", exe, pid, cmd ? cmd : "");
    p = calloc(1, sizeof(*p));
    p->pid = pid;
    p->exit_fd = pipefd[0];
    {
        pthread_t th;
        pthread_create(&th, 0, reaper, p);
        pthread_detach(th);
    }
    pi->hProcess = h_new(H_PROCESS, p);
    pi->hThread = h_new(H_PROCESS, p);           /* its thread ends with it */
    pi->dwProcessId = (DWORD)pid;
    pi->dwThreadId = (DWORD)pid;
    return TRUE;
}

BOOL WINAPI CreateProcessW(const WCHAR *app, WCHAR *cmd, void *psa, void *tsa, BOOL inherit, DWORD flags, void *env,
                           const WCHAR *dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi) {
    char *a = app ? w_to_utf8_dup(app) : 0, *c = cmd ? w_to_utf8_dup(cmd) : 0, *d = dir ? w_to_utf8_dup(dir) : 0;
    BOOL r;
    (void)psa, (void)tsa;
    r = create_process(a, c, inherit, flags, env, d, si, pi);
    free(a), free(c), free(d);
    return r;
}

BOOL WINAPI CreateProcessA(const char *app, char *cmd, void *psa, void *tsa, BOOL inherit, DWORD flags, void *env,
                           const char *dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi) {
    (void)psa, (void)tsa;
    return create_process(app, cmd, inherit, flags & ~(DWORD)0x400, env, dir, si, pi);
}

BOOL WINAPI CreateProcessAsUserW(HANDLE tok, const WCHAR *app, WCHAR *cmd, void *psa, void *tsa, BOOL inherit, DWORD flags,
                                 void *env, const WCHAR *dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi) {
    (void)tok;
    return CreateProcessW(app, cmd, psa, tsa, inherit, flags, env, dir, si, pi);
}

BOOL WINAPI CreateProcessAsUserA(HANDLE tok, const char *app, char *cmd, void *psa, void *tsa, BOOL inherit, DWORD flags,
                                 void *env, const char *dir, STARTUPINFOW *si, PROCESS_INFORMATION *pi) {
    (void)tok;
    return CreateProcessA(app, cmd, psa, tsa, inherit, flags, env, dir, si, pi);
}

/* Windows's WinExec: a program and its arguments in one string */
UINT WINAPI WinExec(const char *cmd, UINT show) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    (void)show;
    memset(&si, 0, sizeof(si));
    if (!create_process(0, cmd, FALSE, 0, 0, 0, &si, &pi)) return 2;
    return 33;
}

BOOL WINAPI CreatePipe(HANDLE *r, HANDLE *w, void *sa, DWORD size) {
    int fds[2];
    (void)sa, (void)size;
    if (pipe(fds) != 0) return FALSE;
    *r = file_handle(fds[0], 0, 0);
    *w = file_handle(fds[1], 0, 0);
    return TRUE;
}

HANDLE WINAPI CreateToolhelp32Snapshot(DWORD flags, DWORD pid) {
    (void)flags, (void)pid;
    wset_last_error(ERROR_NOT_SUPPORTED);
    return INVALID_HANDLE_VALUE;
}

BOOL WINAPI Process32FirstW(HANDLE h, void *e) {
    (void)h, (void)e;
    wset_last_error(ERROR_NO_MORE_FILES);
    return FALSE;
}
BOOL WINAPI Process32NextW(HANDLE h, void *e) {
    (void)h, (void)e;
    wset_last_error(ERROR_NO_MORE_FILES);
    return FALSE;
}
BOOL WINAPI Module32FirstW(HANDLE h, void *e) {
    (void)h, (void)e;
    return FALSE;
}
BOOL WINAPI Module32NextW(HANDLE h, void *e) {
    (void)h, (void)e;
    return FALSE;
}
BOOL WINAPI Thread32First(HANDLE h, void *e) {
    (void)h, (void)e;
    return FALSE;
}
BOOL WINAPI Thread32Next(HANDLE h, void *e) {
    (void)h, (void)e;
    return FALSE;
}

BOOL WINAPI K32EnumProcessModules(HANDLE p, HMODULE *mods, DWORD cb, DWORD *needed) {
    DWORD n = 0;
    (void)p;
    for (wmodule_t *m = ld_modules(); m; m = m->next) {
        if ((n + 1) * sizeof(HMODULE) <= cb) mods[n] = m->base;
        n++;
    }
    *needed = n * (DWORD)sizeof(HMODULE);
    return TRUE;
}

typedef struct {
    void *lpBaseOfDll;
    DWORD SizeOfImage;
    void *EntryPoint;
} MODULEINFO;

BOOL WINAPI K32GetModuleInformation(HANDLE p, HMODULE h, MODULEINFO *mi, DWORD cb) {
    wmodule_t *m = ld_by_handle(h);
    (void)p, (void)cb;
    if (!m) return FALSE;
    mi->lpBaseOfDll = m->base;
    mi->SizeOfImage = (DWORD)m->size;
    mi->EntryPoint = m->entry;
    return TRUE;
}

typedef struct {
    DWORD cb, PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
        QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS;

BOOL WINAPI K32GetProcessMemoryInfo(HANDLE p, PROCESS_MEMORY_COUNTERS *c, DWORD cb) {
    (void)p;
    memset(c, 0, cb);
    c->cb = cb;
    c->WorkingSetSize = c->PeakWorkingSetSize = 64u << 20;
    c->PagefileUsage = c->PeakPagefileUsage = 64u << 20;
    return TRUE;
}

/* ---- errors and exceptions -------------------------------------------------------------------- */

DWORD WINAPI GetLastError(void) { return wget_last_error(); }
void WINAPI SetLastError(DWORD e) { wset_last_error(e); }
void WINAPI RestoreLastError(DWORD e) { wset_last_error(e); }

/* exceptions: except.c */
void *WINAPI SetUnhandledExceptionFilter(void *fn);
LONG WINAPI UnhandledExceptionFilter(void *info);
void *WINAPI AddVectoredExceptionHandler(ULONG first, void *fn);
ULONG WINAPI RemoveVectoredExceptionHandler(void *h);
void *WINAPI AddVectoredContinueHandler(ULONG first, void *fn);
ULONG WINAPI RemoveVectoredContinueHandler(void *h);
void WINAPI RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args);
void WINAPI RtlRaiseException(void *rec);
void WINAPI RtlUnwindEx(void *frame, void *target, void *rec, void *ret, void *ctx, void *hist);
void WINAPI RtlUnwind(void *frame, void *target, void *rec, void *ret);
void WINAPI RtlRestoreContext(void *ctx, void *rec);
DWORD WINAPI __C_specific_handler(void *rec, uint64_t frame, void *ctx, void *dc);

void WINAPI RaiseFailFastException(void *rec, void *ctx, DWORD flags) {
    (void)rec, (void)ctx, (void)flags;
    wfatal("Windows program", "The program stopped itself (fail-fast).");
}

void WINAPI FatalAppExitA(UINT a, const char *msg) {
    (void)a;
    wfatal("Windows program", "%s", msg ? msg : "The program stopped.");
}

void WINAPI FatalAppExitW(UINT a, const WCHAR *msg) {
    char m[512] = "The program stopped.";
    (void)a;
    if (msg) w_to_utf8(msg, -1, m, sizeof(m));
    wfatal("Windows program", "%s", m);
}

void WINAPI DebugBreak(void) { wlog("DebugBreak"); }

/* EncodePointer: a pointer kept so it is not usable as one (XOR with this
 * process's cookie, then rotated, as Windows does); DecodePointer undoes it */
static uint64_t pointer_cookie(void) {
    static uint64_t cookie;
    if (!cookie) {
        uint64_t c = (uint64_t)now_100ns() ^ ((uint64_t)getpid() << 32) ^ 0x9E3779B97F4A7C15ull;
        __atomic_compare_exchange_n(&cookie, &(uint64_t){ 0 }, c | 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return cookie;
}
void *WINAPI EncodePointer(void *p) {
    uint64_t c = pointer_cookie(), v = (uint64_t)(uintptr_t)p ^ c;
    unsigned r = (unsigned)(c & 63);
    return (void *)(uintptr_t)(r ? (v >> r | v << (64 - r)) : v);
}
void *WINAPI DecodePointer(void *p) {
    uint64_t c = pointer_cookie(), v = (uint64_t)(uintptr_t)p;
    unsigned r = (unsigned)(c & 63);
    return (void *)(uintptr_t)((r ? (v << r | v >> (64 - r)) : v) ^ c);
}
void *WINAPI EncodeSystemPointer(void *p) { return EncodePointer(p); }

/* every processor sees this thread's writes */
void WINAPI FlushProcessWriteBuffers(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

/* CopyFile2(from, to, COPYFILE2_EXTENDED_PARAMETERS *): CopyFileW, an HRESULT */
HRESULT WINAPI CopyFile2(const WCHAR *from, const WCHAR *to, const void *params) {
    DWORD flags = params ? ((const DWORD *)params)[1] : 0;          /* dwSize, dwCopyFlags ... */
    if (CopyFileW(from, to, (flags & 1) != 0)) return 0;             /* COPY_FILE_FAIL_IF_EXISTS */
    return (HRESULT)(0x80070000u | (wget_last_error() & 0xFFFF));
}

/* a symbolic link; the target as given (a relative one stays relative) */
BOOLEAN WINAPI CreateSymbolicLinkW(const WCHAR *link, const WCHAR *target, DWORD flags) {
    char wl[1024], wt[1024], ul[1024], ut[1024];
    (void)flags;
    if (!link || !target || w_to_utf8(link, -1, wl, sizeof(wl)) <= 0 || w_to_utf8(target, -1, wt, sizeof(wt)) <= 0 ||
        path_to_unix(wl, ul, sizeof(ul)) != 0) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if ((wt[0] && wt[1] == ':') || wt[0] == '\\' || wt[0] == '/') {
        if (path_to_unix(wt, ut, sizeof(ut)) != 0) {
            wset_last_error(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    } else {
        size_t i = 0;
        for (; wt[i] && i + 1 < sizeof(ut); i++) ut[i] = wt[i] == '\\' ? '/' : wt[i];
        ut[i] = 0;
    }
    if (symlink(ut, ul) != 0) {
        wset_last_error(errno_to_win(errno));
        return FALSE;
    }
    return TRUE;
}
BOOLEAN WINAPI CreateSymbolicLinkA(const char *link, const char *target, DWORD flags) {
    WCHAR l[1024], t[1024];
    if (!link || !target) {
        wset_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    utf8_to_w(link, -1, l, 1024);
    utf8_to_w(target, -1, t, 1024);
    return CreateSymbolicLinkW(l, t, flags);
}
void *WINAPI DecodeSystemPointer(void *p) { return DecodePointer(p); }

void *WINAPI RtlPcToFileHeader(void *pc, void **base) {
    wmodule_t *m = module_at(pc);
    *base = m ? m->base : 0;
    return *base;
}

ULONG WINAPI RtlNtStatusToDosError(LONG status) {
    if (status == 0) return 0;
    if (status == (LONG)0xC0000034) return ERROR_FILE_NOT_FOUND;
    if (status == (LONG)0xC0000022) return ERROR_ACCESS_DENIED;
    return ERROR_INVALID_FUNCTION;
}

LONG WINAPI NtQuerySystemInformation(int cls, void *buf, ULONG n, ULONG *ret) {
    (void)cls, (void)buf, (void)n;
    if (ret) *ret = 0;
    return (LONG)0xC0000003;    /* STATUS_INVALID_INFO_CLASS */
}

LONG WINAPI NtQueryInformationProcess(HANDLE h, int cls, void *buf, ULONG n, ULONG *ret) {
    (void)h;
    if (cls == 0 && n >= 48) {     /* ProcessBasicInformation */
        memset(buf, 0, 48);
        *(void **)((uint8_t *)buf + 8) = peb_get();
        *(ULONG_PTR *)((uint8_t *)buf + 32) = (ULONG_PTR)getpid();
        if (ret) *ret = 48;
        return 0;
    }
    if (ret) *ret = 0;
    return (LONG)0xC0000003;
}

LONG WINAPI NtSetInformationThread(HANDLE h, int cls, void *buf, ULONG n) {
    (void)h, (void)cls, (void)buf, (void)n;
    return 0;
}

LONG WINAPI NtDelayExecution(BOOLEAN alertable, const LARGE_INTEGER *t) {
    int64_t v = t->QuadPart;
    struct timespec ts;
    (void)alertable;
    if (v < 0) v = -v;
    else v = v - (int64_t)now_100ns();
    if (v < 0) v = 0;
    ts.tv_sec = v / 10000000;
    ts.tv_nsec = (long)(v % 10000000) * 100;
    nanosleep(&ts, 0);
    return 0;
}

LONG WINAPI NtQueryTimerResolution(ULONG *mx, ULONG *mn, ULONG *cur) {
    *mx = 156250, *mn = 5000, *cur = 5000;
    return 0;
}

LONG WINAPI NtSetTimerResolution(ULONG want, BOOLEAN set, ULONG *cur) {
    (void)set;
    *cur = want;
    return 0;
}

void *WINAPI RtlAllocateHeap(HANDLE h, ULONG flags, SIZE_T n) { return HeapAlloc(h, flags, n); }
BOOLEAN WINAPI RtlFreeHeap(HANDLE h, ULONG flags, void *p) { return (BOOLEAN)HeapFree(h, flags, p); }
void WINAPI RtlZeroMemory(void *p, SIZE_T n) { memset(p, 0, n); }
void WINAPI RtlMoveMemory(void *d, const void *s, SIZE_T n) { memmove(d, s, n); }
void WINAPI RtlFillMemory(void *d, SIZE_T n, BYTE v) { memset(d, v, n); }
SIZE_T WINAPI RtlCompareMemory(const void *a, const void *b, SIZE_T n) {
    SIZE_T i = 0;
    while (i < n && ((const uint8_t *)a)[i] == ((const uint8_t *)b)[i]) i++;
    return i;
}

/* ---- the export table --------------------------------------------------------------------- */

#define X(f) WEXP(f)
/* thread.c */
HANDLE WINAPI CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name);
HANDLE WINAPI CreateEventExW(void *sa, const WCHAR *name, DWORD flags, DWORD access);
BOOL WINAPI PulseEvent(HANDLE h);
HANDLE WINAPI CreateMutexW(void *sa, BOOL owned, const WCHAR *name);
HANDLE WINAPI CreateMutexA(void *sa, BOOL owned, const char *name);
BOOL WINAPI ReleaseMutex(HANDLE h);
HANDLE WINAPI CreateSemaphoreW(void *sa, LONG initial, LONG max, const WCHAR *name);
HANDLE WINAPI CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name);
HANDLE WINAPI CreateSemaphoreExW(void *sa, LONG initial, LONG max, const WCHAR *name, DWORD flags, DWORD access);
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LONG *prev);
DWORD WINAPI WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms);
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable);
DWORD WINAPI WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable);
DWORD WINAPI SignalObjectAndWait(HANDLE sig, HANDLE wait, DWORD ms, BOOL alertable);
HANDLE WINAPI CreateThread(void *sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, void *param, DWORD flags, DWORD *tid);
DWORD WINAPI ResumeThread(HANDLE h);
DWORD WINAPI SuspendThread(HANDLE h);
void WINAPI ExitThread(DWORD code);
BOOL WINAPI GetExitCodeThread(HANDLE h, DWORD *code);
BOOL WINAPI TerminateThread(HANDLE h, DWORD code);
DWORD WINAPI GetThreadId(HANDLE h);
DWORD WINAPI GetCurrentThreadId(void);
DWORD WINAPI GetCurrentProcessId(void);
HANDLE WINAPI GetCurrentThread(void);
HANDLE WINAPI GetCurrentProcess(void);
DWORD WINAPI TlsAlloc(void);
BOOL WINAPI TlsFree(DWORD i);
void *WINAPI TlsGetValue(DWORD i);
BOOL WINAPI TlsSetValue(DWORD i, void *v);
DWORD WINAPI FlsAlloc(void *cb);
BOOL WINAPI FlsFree(DWORD i);
void *WINAPI FlsGetValue(DWORD i);
BOOL WINAPI FlsSetValue(DWORD i, void *v);

HANDLE WINAPI OpenEventW(DWORD a, BOOL i, const WCHAR *n);
HANDLE WINAPI OpenMutexW(DWORD a, BOOL i, const WCHAR *n);
HANDLE WINAPI OpenSemaphoreW(DWORD a, BOOL i, const WCHAR *n);
HANDLE WINAPI OpenSemaphoreA(DWORD a, BOOL i, const char *n);
HANDLE WINAPI CreateMutexExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)access;
    return CreateMutexW(sa, (flags & 1) != 0, name);
}
HANDLE WINAPI CreateEventExA(void *sa, const char *name, DWORD flags, DWORD access) {
    WCHAR *w = name ? utf8_to_w_dup(name) : 0;
    HANDLE h = CreateEventExW(sa, w, flags, access);
    free(w);
    return h;
}

static const wexport_t k32[] = {
    X(CreateFileW), X(CreateFileA), X(CreateFile2), X(ReadFile), X(WriteFile), X(GetOverlappedResult), X(CancelIo),
    X(CancelIoEx), X(FlushFileBuffers), X(GetFileSizeEx), X(GetFileSize), X(SetFilePointerEx), X(SetFilePointer),
    X(SetEndOfFile), X(GetFileType), X(GetFileTime), X(SetFileTime), X(GetFileAttributesW), X(GetFileAttributesA),
    X(GetFileAttributesExW), X(GetFileAttributesExA), X(SetFileAttributesW), X(GetFileInformationByHandle),
    X(GetFileInformationByHandleEx), X(DeleteFileW), X(DeleteFileA), X(MoveFileExW), X(MoveFileW), X(MoveFileA),
    X(CopyFileW), X(CreateDirectoryW), X(CreateDirectoryA), X(RemoveDirectoryW), X(RemoveDirectoryA),
    X(FindFirstFileExW), X(FindFirstFileW), X(FindNextFileW), X(FindFirstFileA), X(FindNextFileA), X(FindFirstFileExA), X(FindClose),
    X(GetCurrentDirectoryW), X(GetCurrentDirectoryA), X(SetCurrentDirectoryW), X(SetCurrentDirectoryA),
    X(GetFullPathNameA), X(GetFullPathNameW), X(GetLongPathNameW), X(GetShortPathNameW), X(GetTempPathW),
    X(GetTempPathA), X(GetTempPath2W), X(GetTempFileNameW), X(GetSystemDirectoryW), X(GetSystemDirectoryA),
    X(GetWindowsDirectoryW), X(GetWindowsDirectoryA), X(GetSystemWindowsDirectoryW), X(GetSystemWow64DirectoryW),
    X(GetDriveTypeW), X(GetDriveTypeA), X(GetLogicalDrives), X(GetLogicalDriveStringsW), X(GetDiskFreeSpaceExW),
    X(GetDiskFreeSpaceW), X(GetVolumeInformationW), X(DeviceIoControl), X(PeekNamedPipe), X(LockFile), X(UnlockFile),
    X(LockFileEx), X(UnlockFileEx), X(CreateFileMappingW), X(CreateFileMappingA), X(MapViewOfFile), X(OpenFileMappingW), X(OpenFileMappingA),
    X(MapViewOfFileEx), X(UnmapViewOfFile), X(FlushViewOfFile), X(CloseHandle), X(DuplicateHandle),
    X(GetHandleInformation), X(SetHandleInformation), X(GetStdHandle), X(SetStdHandle), X(WriteConsoleW),
    X(WriteConsoleA), X(ReadConsoleW), X(ReadConsoleA), X(GetConsoleMode), X(SetConsoleMode), X(AllocConsole),
    X(FreeConsole), X(AttachConsole), X(GetConsoleWindow), X(SetConsoleTitleW), X(SetConsoleTitleA),
    X(GetConsoleTitleW), X(SetConsoleCtrlHandler), X(GetConsoleCP), X(GetConsoleOutputCP), X(SetConsoleCP),
    X(SetConsoleOutputCP), X(SetConsoleTextAttribute), X(GetConsoleScreenBufferInfo),
    X(GetNumberOfConsoleInputEvents), X(ReadConsoleInputA), X(ReadConsoleInputW), X(PeekConsoleInputW),
    X(FlushConsoleInputBuffer), X(OutputDebugStringA), X(OutputDebugStringW), X(VirtualAlloc), X(VirtualFree),
    X(VirtualProtect), X(VirtualQuery), X(VirtualLock), X(VirtualUnlock), X(FlushInstructionCache), X(GetProcessHeap),
    X(HeapCreate), X(HeapDestroy), X(HeapAlloc), X(HeapReAlloc), X(HeapFree), X(HeapSize), X(HeapValidate),
    X(HeapSetInformation), X(HeapCompact), X(HeapLock), X(HeapUnlock), X(GetProcessHeaps), X(GlobalAlloc),
    X(GlobalLock), X(GlobalUnlock), X(GlobalSize), X(GlobalFree), X(GlobalReAlloc), X(GlobalHandle), X(GlobalFlags),
    X(LocalAlloc), X(LocalFree), X(LocalReAlloc), X(LocalLock), X(LocalUnlock), X(LocalSize),
    X(GlobalMemoryStatusEx), X(GlobalMemoryStatus), X(IsBadReadPtr), X(IsBadWritePtr), X(IsBadCodePtr),
    X(IsBadStringPtrA), X(GetTickCount), X(GetTickCount64), X(QueryPerformanceCounter), X(QueryPerformanceFrequency),
    X(GetSystemTimeAsFileTime), X(GetSystemTimePreciseAsFileTime), X(GetSystemTime), X(GetLocalTime),
    X(FileTimeToSystemTime), X(SystemTimeToFileTime), X(FileTimeToLocalFileTime), X(LocalFileTimeToFileTime),
    X(SystemTimeToTzSpecificLocalTime), X(CompareFileTime), X(GetTimeZoneInformation),
    X(GetDynamicTimeZoneInformation), X(Sleep), X(SleepEx), X(SwitchToThread), X(CreateWaitableTimerExW),
    X(CreateWaitableTimerW), X(SetWaitableTimer), X(SetWaitableTimerEx), X(CancelWaitableTimer), X(GetSystemInfo),
    X(GetNativeSystemInfo), X(IsProcessorFeaturePresent), X(GetVersionExW), X(GetVersionExA), X(GetVersion),
    X(VerSetConditionMask), X(VerifyVersionInfoW), X(VerifyVersionInfoA), X(GetProductInfo), X(IsWow64Process),
    X(IsDebuggerPresent), X(CheckRemoteDebuggerPresent), X(SetErrorMode), X(GetErrorMode), X(SetThreadErrorMode),
    X(SetThreadExecutionState), X(SetThreadPriority), X(GetThreadPriority), X(SetThreadPriorityBoost),
    X(SetPriorityClass), X(GetPriorityClass), X(SetThreadAffinityMask), X(SetProcessAffinityMask),
    X(GetProcessAffinityMask), X(SetThreadIdealProcessor), X(SetThreadDescription), X(GetThreadDescription),
    X(SetThreadStackGuarantee), X(GetCurrentProcessorNumber), X(GetCurrentThreadStackLimits), X(GetProcessTimes),
    X(GetThreadTimes), X(GetSystemTimes), X(GetSystemPowerStatus), X(GetLogicalProcessorInformation),
    X(GetLogicalProcessorInformationEx), X(GetActiveProcessorCount), X(GetMaximumProcessorCount),
    X(GetActiveProcessorGroupCount), X(GetMaximumProcessorGroupCount),
    X(GetComputerNameW), X(GetComputerNameA), X(GetComputerNameExW), X(Beep), X(GetEnvironmentVariableA),
    X(GetEnvironmentVariableW), X(SetEnvironmentVariableA), X(SetEnvironmentVariableW), X(GetEnvironmentStringsW),
    X(GetEnvironmentStringsA), X(FreeEnvironmentStringsW), X(FreeEnvironmentStringsA), X(ExpandEnvironmentStringsW),
    X(ExpandEnvironmentStringsA), X(GetCommandLineA), X(GetCommandLineW), X(GetStartupInfoW), X(GetStartupInfoA),
    WEXPN("GetEnvironmentStrings", GetEnvironmentStringsA), X(MultiByteToWideChar), X(WideCharToMultiByte),
    X(GetACP), X(GetOEMCP), X(GetCPInfo), X(GetCPInfoExW), X(GetCPInfoExA), X(IsValidCodePage), X(IsDBCSLeadByte),
    X(IsDBCSLeadByteEx), X(CompareStringW), X(CompareStringEx), X(CompareStringOrdinal), X(CompareStringA),
    X(LCMapStringW), X(LCMapStringEx), X(LCMapStringA), X(GetStringTypeW), X(GetStringTypeExW), X(GetStringTypeA),
    X(lstrlenA), X(lstrlenW), X(lstrcpyA), X(lstrcpyW), X(lstrcpynA), X(lstrcpynW), X(lstrcatA), X(lstrcatW),
    X(lstrcmpA), X(lstrcmpW), X(lstrcmpiA), X(lstrcmpiW), WEXPN("lstrlen", lstrlenA), WEXPN("lstrcpy", lstrcpyA),
    WEXPN("lstrcmpi", lstrcmpiA), X(GetUserDefaultLCID), X(GetSystemDefaultLCID), X(GetUserDefaultLangID),
    X(GetSystemDefaultLangID), X(GetUserDefaultUILanguage), X(GetSystemDefaultUILanguage), X(GetThreadLocale),
    X(SetThreadLocale), X(SetThreadUILanguage), X(GetThreadUILanguage), X(IsValidLocale), X(IsValidLocaleName),
    X(GetUserDefaultLocaleName), X(GetSystemDefaultLocaleName), X(LCIDToLocaleName), X(LocaleNameToLCID),
    X(GetLocaleInfoA), X(GetLocaleInfoW), X(GetLocaleInfoEx), X(EnumSystemLocalesW), X(GetDateFormatW),
    X(GetTimeFormatW), X(FormatMessageA), X(FormatMessageW), X(MulDiv), X(InitializeCriticalSection),
    X(InitializeCriticalSectionAndSpinCount), X(InitializeCriticalSectionEx), X(DeleteCriticalSection),
    X(EnterCriticalSection), X(TryEnterCriticalSection), X(LeaveCriticalSection), X(SetCriticalSectionSpinCount),
    X(InitializeSRWLock), X(AcquireSRWLockExclusive), X(ReleaseSRWLockExclusive), X(AcquireSRWLockShared),
    X(ReleaseSRWLockShared), X(TryAcquireSRWLockExclusive), X(TryAcquireSRWLockShared),
    X(InitializeConditionVariable), X(SleepConditionVariableCS), X(SleepConditionVariableSRW),
    X(WakeConditionVariable), X(WakeAllConditionVariable), X(WaitOnAddress), X(WakeByAddressSingle),
    X(WakeByAddressAll), X(InitOnceExecuteOnce), X(InitOnceInitialize), X(InitOnceBeginInitialize),
    X(InitOnceComplete), X(InitializeSListHead), X(InterlockedPushEntrySList), X(InterlockedPopEntrySList),
    X(InterlockedFlushSList), X(QueryDepthSList), X(GetModuleHandleA), X(GetModuleHandleW), X(GetModuleHandleExW),
    X(GetModuleHandleExA), X(GetModuleFileNameA), X(GetModuleFileNameW), X(K32GetModuleFileNameExW),
    X(K32GetModuleBaseNameW), X(QueryFullProcessImageNameW), X(QueryFullProcessImageNameA), X(LoadLibraryA),
    X(LoadLibraryW), X(LoadLibraryExA), X(LoadLibraryExW), X(LoadPackagedLibrary), X(FreeLibrary),
    X(FreeLibraryAndExitThread), X(GetProcAddress), X(DisableThreadLibraryCalls), X(SetDllDirectoryW),
    X(SetDllDirectoryA), X(AddDllDirectory), X(RemoveDllDirectory), X(SetDefaultDllDirectories), X(FindResourceExW),
    X(FindResourceW), X(FindResourceA), X(FindResourceExA), X(LoadResource), X(LockResource), X(SizeofResource),
    X(FreeResource), X(EnumResourceNamesW), X(EnumResourceNamesA), X(EnumResourceNamesExW), X(EnumResourceNamesExA), X(EnumResourceTypesW), X(EnumResourceTypesA),
    X(ExitProcess), X(TerminateProcess), X(CreateProcessAsUserW), X(WinExec), X(GetVolumePathNameW),
    X(GetVolumeNameForVolumeMountPointW), X(GetFinalPathNameByHandleW), X(SetProcessWorkingSetSize),
    X(SetProcessWorkingSetSizeEx), X(SetFileInformationByHandle), X(DosDateTimeToFileTime),
    X(FileTimeToDosDateTime), X(CreateHardLinkW), X(ReplaceFileW), X(CopyFileExW), X(MoveFileWithProgressW),
    X(CreateNamedPipeW), X(CreateNamedPipeA), X(CreateMailslotW), X(FindFirstChangeNotificationW), X(WaitNamedPipeW),
    X(ConnectNamedPipe), X(DisconnectNamedPipe), X(SetNamedPipeHandleState), X(GetNamedPipeInfo),
    X(GetNamedPipeClientProcessId), X(ImpersonateNamedPipeClient), X(TransactNamedPipe), X(CallNamedPipeW),
    X(GetProcessMitigationPolicy), X(SetProcessMitigationPolicy), X(SetThreadInformation), X(SetProcessInformation),
    X(GetThreadPreferredUILanguages), X(GetUserPreferredUILanguages), X(GetSystemPreferredUILanguages),
    X(GetProcessPreferredUILanguages), X(SetProcessPreferredUILanguages),
    X(GetExitCodeProcess), X(OpenProcess), X(GetProcessId), X(GetProcessVersion), X(CreateProcessW),
    X(CreateProcessA), X(CreatePipe), X(CreateToolhelp32Snapshot), X(Process32FirstW), X(Process32NextW),
    X(Module32FirstW), X(Module32NextW), X(Thread32First), X(Thread32Next), X(K32EnumProcessModules),
    X(K32GetModuleInformation), X(K32GetProcessMemoryInfo), WEXPN("GetProcessMemoryInfo", K32GetProcessMemoryInfo),
    WEXPN("EnumProcessModules", K32EnumProcessModules), X(GetLastError), X(SetLastError), X(RestoreLastError),
    X(SetUnhandledExceptionFilter), X(UnhandledExceptionFilter), X(AddVectoredExceptionHandler),
    X(RemoveVectoredExceptionHandler), X(AddVectoredContinueHandler), X(RemoveVectoredContinueHandler),
    X(RaiseException), X(RaiseFailFastException), X(FatalAppExitA), X(FatalAppExitW), X(DebugBreak),
    X(RtlPcToFileHeader), X(RtlUnwindEx), X(RtlUnwind), X(EncodePointer), X(DecodePointer), X(EncodeSystemPointer),
    X(DecodeSystemPointer), X(FlushProcessWriteBuffers), X(CopyFile2), X(CreateSymbolicLinkW), X(CreateSymbolicLinkA),
    X(CreateEventW), X(CreateEventA), X(CreateEventExW), X(CreateEventExA), X(SetEvent),
    X(ResetEvent), X(PulseEvent), X(OpenEventW), X(CreateMutexW), X(CreateMutexA), X(CreateMutexExW), X(OpenMutexW), X(OpenSemaphoreW),
    X(OpenSemaphoreA), X(OpenWaitableTimerW), X(OpenWaitableTimerA),
    X(ReleaseMutex), X(CreateSemaphoreW), X(CreateSemaphoreA), X(CreateSemaphoreExW), X(ReleaseSemaphore),
    X(WaitForMultipleObjects), X(WaitForSingleObject), X(WaitForSingleObjectEx), X(WaitForMultipleObjectsEx),
    X(SignalObjectAndWait), X(CreateThread), X(ResumeThread), X(SuspendThread), X(ExitThread), X(GetExitCodeThread),
    X(TerminateThread), X(GetThreadId), X(GetCurrentThreadId), X(GetCurrentProcessId), X(GetCurrentThread),
    X(GetCurrentProcess), X(TlsAlloc), X(TlsFree), X(TlsGetValue), X(TlsSetValue), X(FlsAlloc), X(FlsFree),
    X(FlsGetValue), WEXPN("FlsGetValue2", FlsGetValue), X(FlsSetValue),
    X(CreateIoCompletionPort), X(PostQueuedCompletionStatus), X(GetQueuedCompletionStatus),
    X(GetQueuedCompletionStatusEx), X(SetFileCompletionNotificationModes), X(RegisterWaitForSingleObject),
    X(UnregisterWaitEx), X(UnregisterWait), X(IsWow64Process2), X(AreFileApisANSI),
    X(SetFileApisToANSI), X(SetFileApisToOEM), X(EnumSystemLocalesEx), X(GetDateFormatEx), X(GetTimeFormatEx), WEXPN("RtlZeroMemory", RtlZeroMemory), WEXPN("RtlMoveMemory", RtlMoveMemory),
    WEXPN("RtlFillMemory", RtlFillMemory),
};

extern const wexport_t unwind_exports[], tpool_exports[];
extern const int unwind_nexports, tpool_nexports;
const wdll_t dll_kernel32 = { "kernel32.dll", k32, sizeof(k32) / sizeof(k32[0]), unwind_exports, &unwind_nexports,
                              tpool_exports, &tpool_nexports };

static const wexport_t ntd[] = {
    X(RtlGetVersion), X(RtlPcToFileHeader), X(RtlUnwindEx), X(RtlUnwind), X(RtlRaiseException), X(RtlRestoreContext),
    WEXPN("RtlEncodePointer", EncodePointer), WEXPN("RtlDecodePointer", DecodePointer),
    WEXPN("RtlEncodeSystemPointer", EncodeSystemPointer), WEXPN("RtlDecodeSystemPointer", DecodeSystemPointer),
    X(__C_specific_handler), WEXPN("RtlAddVectoredExceptionHandler", AddVectoredExceptionHandler),
    WEXPN("RtlRemoveVectoredExceptionHandler", RemoveVectoredExceptionHandler),
    WEXPN("RtlAddVectoredContinueHandler", AddVectoredContinueHandler),
    WEXPN("RtlRemoveVectoredContinueHandler", RemoveVectoredContinueHandler),
    X(RtlNtStatusToDosError), X(NtQuerySystemInformation), X(NtQueryInformationProcess), X(NtSetInformationThread),
    X(NtDelayExecution), X(NtQueryTimerResolution), X(NtSetTimerResolution), X(RtlAllocateHeap), X(RtlFreeHeap),
    X(RtlZeroMemory), X(RtlMoveMemory), X(RtlFillMemory), X(RtlCompareMemory),
    WEXPN("ZwQuerySystemInformation", NtQuerySystemInformation),
    WEXPN("ZwQueryInformationProcess", NtQueryInformationProcess), WEXPN("ZwDelayExecution", NtDelayExecution),
    WEXPN("RtlInitializeSListHead", InitializeSListHead), WEXPN("RtlInterlockedPushEntrySList", InterlockedPushEntrySList),
    WEXPN("RtlInterlockedPopEntrySList", InterlockedPopEntrySList), WEXPN("RtlEnterCriticalSection", EnterCriticalSection),
    WEXPN("RtlLeaveCriticalSection", LeaveCriticalSection), WEXPN("RtlInitializeCriticalSection", InitializeCriticalSection),
    WEXPN("RtlDeleteCriticalSection", DeleteCriticalSection),
};

extern const wexport_t reg_nt_exports[];
extern const int reg_nt_nexports;
const wdll_t dll_ntdll = { "ntdll.dll", ntd, sizeof(ntd) / sizeof(ntd[0]), reg_nt_exports, &reg_nt_nexports,
                           unwind_exports, &unwind_nexports };
