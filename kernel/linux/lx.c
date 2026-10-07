#include "lx.h"
#include "../cpu/isr.h"
#include "../cpu/gdt.h"
#include "../cpu/smp.h"
#include "../dev/devops.h"
#include "../drivers/rtc/rtc.h"
#include "../drivers/serial/serial.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../memory/pf.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../proc/sched.h"
#include "../proc/user.h"
#include "../syscall/uaccess.h"
#include "../tty/pty.h"
#include "lx_vm.h"

/* errno values (x86-64 Linux) */
#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENOEXEC 8
#define EBADF   9
#define ECHILD  10
#define EAGAIN  11
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define EXDEV   18
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOTTY  25
#define ESPIPE  29
#define EROFS   30
#define EPIPE   32
#define ERANGE  34
#define ENAMETOOLONG 36
#define ENOSYS  38
#define ENOTEMPTY 39

#define ERR(e) ((uint64_t)-(int64_t)(e))

#define LX_FD_MAX    128
#define LX_PIPE_CAP  65536U
#define LX_NSIG      65
#define LX_PATH_MAX  512
#define LX_AT_FDCWD  (-100)

#define O_ACCMODE   3
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0100
#define O_EXCL      0200
#define O_TRUNC     01000
#define O_APPEND    02000
#define O_NONBLOCK  04000
#define O_DIRECTORY 0200000
#define O_CLOEXEC   02000000

#define SIGINT  2
#define SIGKILL 9
#define SIGPIPE 13
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG  23
#define SIGWINCH 28

#define SA_RESTORER  0x04000000ULL
#define SA_RESTART   0x10000000ULL
#define SA_NODEFER   0x40000000ULL
#define SA_RESETHAND 0x80000000ULL

enum { LXF_TTY = 1, LXF_VFS, LXF_PIPE, LXF_NULL, LXF_ZERO, LXF_RANDOM, LXF_MEM, LXF_PROCDIR };

typedef struct lx_pipe {
    char    *buf;
    uint32_t head;
    uint32_t len;
    int      readers;
    int      writers;
} lx_pipe_t;

typedef struct lx_file {
    int         refs;
    int         kind;
    int         write_end;
    uint64_t    flags;
    uint64_t    off;
    vfs_node_t *node;
    lx_pipe_t  *pipe;
    char       *mem;
    uint64_t    mem_len;
    uint64_t    proc_pid;
} lx_file_t;

typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} lx_sigaction_t;

struct lx_state {
    lx_file_t     *fd[LX_FD_MAX];
    uint8_t        cloexec[LX_FD_MAX];
    lx_sigaction_t sa[LX_NSIG];
    uint64_t       sigmask;
    uint64_t       pending;
    uint32_t       umask;
    int            term_sig;
    uint64_t       clear_tid;
    char           exe[128];
};

typedef struct {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t  line;
    uint8_t  cc[19];
} lx_termios_t;

typedef struct {
    int          init;
    lx_termios_t t;
    uint64_t     fg_pgrp;
    char         line[1024];
    uint32_t     len;
    uint32_t     ready;
    int          eof;
} lx_tty_t;

static lx_tty_t ttys[PTY_MAX + 1];
static uint64_t cur_nr;
static uint64_t rng_state = 0x2545F4914F6CDD1DULL;
static uint64_t boot_epoch;

static void kzero(void *p, uint64_t n) {
    uint8_t *d = (uint8_t *)p;
    while (n--) *d++ = 0;
}

static void kcopy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static uint64_t kstrlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static void kstrcpy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!cap) return;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int kstreq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void log_hex(const char *what, uint64_t v) {
    char buf[20];
    static const char hex[] = "0123456789abcdef";
    int n = 0;
    serial_write(what);
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int i = 15; i >= 0; i--) {
        if (n == 2 && i > 0 && ((v >> (i * 4)) & 0xF) == 0) continue;
        buf[n++] = hex[(v >> (i * 4)) & 0xF];
    }
    buf[n++] = '\n';
    buf[n] = 0;
    serial_write(buf);
}

static uint64_t rng_next(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    rng_state ^= ((uint64_t)hi << 32) | lo;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* ---- state and open files ---------------------------------------------- */

static lx_file_t *file_new(int kind) {
    lx_file_t *f = (lx_file_t *)kmalloc(sizeof(lx_file_t));
    if (!f) return 0;
    kzero(f, sizeof(*f));
    f->kind = kind;
    f->refs = 1;
    return f;
}

static void file_get(lx_file_t *f) {
    f->refs++;
    if (f->kind == LXF_PIPE) {
        if (f->write_end) f->pipe->writers++;
        else f->pipe->readers++;
    }
}

static void file_put(lx_file_t *f) {
    if (!f) return;
    if (f->kind == LXF_PIPE) {
        if (f->write_end) f->pipe->writers--;
        else f->pipe->readers--;
        sched_wake_input_waiters();
    }
    if (--f->refs > 0) return;
    if (f->mem) kfree(f->mem);
    if (f->kind == LXF_PIPE && f->pipe->readers <= 0 && f->pipe->writers <= 0) {
        kfree(f->pipe->buf);
        kfree(f->pipe);
    }
    kfree(f);
}

static struct lx_state *lx_get(process_t *proc) {
    struct lx_state *s;
    lx_file_t *tty;
    if (!proc) return 0;
    if (proc->lx) return proc->lx;
    s = (struct lx_state *)kmalloc(sizeof(struct lx_state));
    if (!s) return 0;
    kzero(s, sizeof(*s));
    s->umask = 022;
    tty = file_new(LXF_TTY);
    if (tty) {
        tty->flags = O_RDWR;
        tty->refs = 3;
        s->fd[0] = s->fd[1] = s->fd[2] = tty;
    }
    kstrcpy(s->exe, "/bin/", sizeof(s->exe));
    kstrcpy(s->exe + 5, proc->name, sizeof(s->exe) - 5);
    proc->lx = s;
    return s;
}

static int fd_alloc(struct lx_state *s, int min, lx_file_t *f, int cloexec) {
    for (int i = min < 0 ? 0 : min; i < LX_FD_MAX; i++) {
        if (!s->fd[i]) {
            s->fd[i] = f;
            s->cloexec[i] = (uint8_t)(cloexec != 0);
            return i;
        }
    }
    return -EMFILE;
}

static lx_file_t *fd_get(struct lx_state *s, int64_t fd) {
    if (!s || fd < 0 || fd >= LX_FD_MAX) return 0;
    return s->fd[fd];
}

static int fd_close(struct lx_state *s, int64_t fd) {
    lx_file_t *f = fd_get(s, fd);
    if (!f) return -EBADF;
    s->fd[fd] = 0;
    s->cloexec[fd] = 0;
    file_put(f);
    return 0;
}

void lx_proc_exit(process_t *proc) {
    struct lx_state *s = proc ? proc->lx : 0;
    if (!s) return;
    for (int i = 0; i < LX_FD_MAX; i++) {
        if (s->fd[i]) fd_close(s, i);
    }
    if (proc->parent && proc->parent->lx && proc->parent->linux_personality) {
        proc->parent->lx->pending |= 1ULL << (SIGCHLD - 1);
    }
}

/* ---- signals: queueing ------------------------------------------------- */

static int signal_pending(struct lx_state *s) {
    return s && (s->pending & ~s->sigmask) != 0;
}

int lx_signal(process_t *proc, int sig) {
    struct lx_state *s;
    if (!proc || sig <= 0 || sig >= LX_NSIG) return -1;
    if (proc->state == PROCESS_EXITED || proc->state == PROCESS_REAPED) return -1;
    if (!proc->linux_personality) {
        return sched_kill_process(proc->pid, 128 + (uint64_t)sig);
    }
    s = lx_get(proc);
    if (!s) return -1;
    if (sig == SIGKILL) {
        s->term_sig = SIGKILL;
        return sched_kill_process(proc->pid, 128 + SIGKILL);
    }
    s->pending |= 1ULL << (sig - 1);
    if (proc->main_thread && proc->main_thread->state == THREAD_BLOCKED) {
        sched_wake_thread(proc->main_thread);
    }
    return 0;
}

/* ---- tty line discipline (on top of the raw pty) ------------------------ */

static int cur_pty(void) {
    process_t *p = sched_current_process();
    return p ? p->pty : 0;
}

static lx_tty_t *tty_get(int pty) {
    lx_tty_t *t;
    if (pty < 1 || pty > PTY_MAX) return 0;
    t = &ttys[pty];
    if (!t->init || !pty_alive(pty)) {
        kzero(t, sizeof(*t));
        t->init = 1;
        t->t.iflag = 0x100 | 0x400;               /* ICRNL IXON */
        t->t.oflag = 0x1 | 0x4;                   /* OPOST ONLCR */
        t->t.cflag = 0xBF;                        /* B38400 CS8 CREAD */
        t->t.lflag = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x200 | 0x800 | 0x8000;
        t->t.cc[0] = 3;
        t->t.cc[1] = 0x1C;
        t->t.cc[2] = 0x7F;
        t->t.cc[3] = 0x15;
        t->t.cc[4] = 4;
        t->t.cc[6] = 1;
        t->t.cc[8] = 0x11;
        t->t.cc[9] = 0x13;
        t->t.cc[10] = 0x1A;
        t->t.cc[14] = 0x17;
        t->t.cc[15] = 0x16;
    }
    return t;
}

static void tty_echo(int pty, const char *s, uint64_t n) {
    pty_slave_write(pty, s, n);
}

static uint64_t tty_write(const char *ubuf, uint64_t count) {
    char kbuf[1024];
    uint64_t done = 0;
    int pty = cur_pty();
    const dev_calls_t *dcon = dev_console();
    while (done < count) {
        uint64_t chunk = count - done;
        if (chunk > sizeof(kbuf) - 1) chunk = sizeof(kbuf) - 1;
        if (copy_from_user(kbuf, ubuf + done, chunk) != 0) return done ? done : ERR(EFAULT);
        if (pty) {
            pty_slave_write(pty, kbuf, chunk);
        } else if (dcon) {
            kbuf[chunk] = 0;
            dcon->con_write(kbuf);
        }
        done += chunk;
    }
    return count;
}

static int tty_cook(int pty, lx_tty_t *t);

static int tty_has_input(int pty) {
    lx_tty_t *t = tty_get(pty);
    if (!t) return 0;
    if (t->t.lflag & 0x2) return tty_cook(pty, t);
    return t->len > 0 || pty_slave_peek(pty);
}

/* Feeds raw pty input into the canonical line buffer; returns 1 once a
 * line (or EOF) is ready. */
static int tty_cook(int pty, lx_tty_t *t) {
    int c;
    while (!t->ready && !t->eof && (c = pty_slave_read_char(pty)) >= 0) {
        char ch = (char)c;
        if ((t->t.iflag & 0x100) && ch == '\r') ch = '\n';
        if (ch == (char)t->t.cc[2] || ch == 0x08) {
            if (t->len > 0) {
                t->len--;
                if (t->t.lflag & 0x8) tty_echo(pty, "\b \b", 3);
            }
            continue;
        }
        if (ch == (char)t->t.cc[3]) {
            while (t->len > 0 && (t->t.lflag & 0x8)) {
                tty_echo(pty, "\b \b", 3);
                t->len--;
            }
            t->len = 0;
            continue;
        }
        if (ch == (char)t->t.cc[4]) {
            if (t->len > 0) t->ready = 1;
            else t->eof = 1;
            break;
        }
        if (t->len < sizeof(t->line) - 1) t->line[t->len++] = ch;
        if (t->t.lflag & 0x8) tty_echo(pty, &ch, 1);
        if (ch == '\n') t->ready = 1;
    }
    return t->ready || t->eof;
}

static uint64_t tty_read(lx_file_t *f, char *ubuf, uint64_t count) {
    int pty = cur_pty();
    lx_tty_t *t = tty_get(pty);
    struct lx_state *s = lx_get(sched_current_process());
    if (!t) return 0;
    if (t->t.lflag & 0x2) {
        uint64_t n;
        while (!tty_cook(pty, t)) {
            if (!pty_alive(pty)) return 0;
            if (f->flags & O_NONBLOCK) return ERR(EAGAIN);
            if (signal_pending(s)) return ERR(EINTR);
            sched_sleep(1);
        }
        if (t->eof && t->len == 0) {
            t->eof = 0;
            return 0;
        }
        n = count < t->len ? count : t->len;
        if (copy_to_user(ubuf, t->line, n) != 0) return ERR(EFAULT);
        kcopy(t->line, t->line + n, t->len - n);
        t->len -= (uint32_t)n;
        if (t->len == 0) t->ready = 0;
        t->eof = 0;
        return n;
    }
    {
        uint64_t got = 0, start = sched_ticks();
        uint64_t vmin = t->t.cc[6], vtime = t->t.cc[5];
        char kbuf[256];
        while (got == 0) {
            int c;
            while (got < count && got < sizeof(kbuf) && t->len) {
                kbuf[got++] = t->line[0];
                kcopy(t->line, t->line + 1, --t->len);
            }
            while (got < count && got < sizeof(kbuf) && (c = pty_slave_read_char(pty)) >= 0) {
                char ch = (char)c;
                if ((t->t.iflag & 0x100) && ch == '\r') ch = '\n';
                kbuf[got++] = ch;
                if (t->t.lflag & 0x8) tty_echo(pty, &ch, 1);
            }
            if (got) break;
            if (!pty_alive(pty)) return 0;
            if (f->flags & O_NONBLOCK) return ERR(EAGAIN);
            if (vmin == 0 && (vtime == 0 || sched_ticks() - start >= vtime * 10)) return 0;
            if (signal_pending(s)) return ERR(EINTR);
            sched_sleep(1);
        }
        if (copy_to_user(ubuf, kbuf, got) != 0) return ERR(EFAULT);
        t->ready = 0;
        return got;
    }
}

static uint64_t tty_ioctl(lx_file_t *f, uint64_t req, uint64_t arg) {
    int pty = cur_pty();
    lx_tty_t *t = tty_get(pty);
    process_t *proc = sched_current_process();
    (void)f;
    if (!t) return ERR(ENOTTY);
    switch (req) {
        case 0x5401: /* TCGETS */
            return copy_to_user((void *)arg, &t->t, sizeof(t->t)) ? ERR(EFAULT) : 0;
        case 0x5402: case 0x5403: case 0x5404: /* TCSETS* */
            if (copy_from_user(&t->t, (const void *)arg, sizeof(t->t)) != 0) return ERR(EFAULT);
            if (!(t->t.lflag & 0x2)) t->ready = 0;
            return 0;
        case 0x5413: { /* TIOCGWINSZ */
            uint32_t cols, rows;
            uint16_t ws[4];
            pty_get_size(pty, &cols, &rows);
            ws[0] = (uint16_t)rows;
            ws[1] = (uint16_t)cols;
            ws[2] = ws[3] = 0;
            return copy_to_user((void *)arg, ws, sizeof(ws)) ? ERR(EFAULT) : 0;
        }
        case 0x5414: return 0;
        case 0x540F: { /* TIOCGPGRP */
            int32_t pg = (int32_t)(t->fg_pgrp ? t->fg_pgrp : (proc ? proc->process_group_id : 0));
            if (!pg && proc) pg = (int32_t)proc->pid;
            return copy_to_user((void *)arg, &pg, 4) ? ERR(EFAULT) : 0;
        }
        case 0x5410: { /* TIOCSPGRP */
            int32_t pg;
            if (copy_from_user(&pg, (const void *)arg, 4) != 0) return ERR(EFAULT);
            t->fg_pgrp = (uint64_t)pg;
            return 0;
        }
        case 0x5429: { /* TIOCGSID */
            int32_t sid = proc ? (int32_t)proc->session_id : 0;
            return copy_to_user((void *)arg, &sid, 4) ? ERR(EFAULT) : 0;
        }
        case 0x540E: case 0x5422: case 0x540B: return 0; /* TIOCSCTTY TIOCNOTTY TCFLSH */
        case 0x541B: { /* FIONREAD */
            int32_t n = (int32_t)t->len;
            return copy_to_user((void *)arg, &n, 4) ? ERR(EFAULT) : 0;
        }
        default:
            return ERR(EINVAL);
    }
}

/* ---- pipes -------------------------------------------------------------- */

static uint64_t pipe_read(lx_file_t *f, char *ubuf, uint64_t count) {
    lx_pipe_t *p = f->pipe;
    struct lx_state *s = lx_get(sched_current_process());
    char kbuf[1024];
    uint64_t got = 0;
    while (p->len == 0) {
        if (p->writers <= 0) return 0;
        if (f->flags & O_NONBLOCK) return ERR(EAGAIN);
        if (signal_pending(s)) return ERR(EINTR);
        sched_sleep(1);
    }
    while (got < count && p->len) {
        uint64_t n = 0;
        while (n < sizeof(kbuf) && got + n < count && p->len) {
            kbuf[n++] = p->buf[p->head];
            p->head = (p->head + 1) % LX_PIPE_CAP;
            p->len--;
        }
        if (copy_to_user(ubuf + got, kbuf, n) != 0) return got ? got : ERR(EFAULT);
        got += n;
    }
    return got;
}

static uint64_t pipe_write(lx_file_t *f, const char *ubuf, uint64_t count) {
    lx_pipe_t *p = f->pipe;
    struct lx_state *s = lx_get(sched_current_process());
    char kbuf[1024];
    uint64_t done = 0;
    while (done < count) {
        uint64_t chunk = count - done, room;
        if (p->readers <= 0) {
            lx_signal(sched_current_process(), SIGPIPE);
            return done ? done : ERR(EPIPE);
        }
        room = LX_PIPE_CAP - p->len;
        if (room == 0) {
            if (f->flags & O_NONBLOCK) return done ? done : ERR(EAGAIN);
            if (signal_pending(s)) return done ? done : ERR(EINTR);
            sched_sleep(1);
            continue;
        }
        if (chunk > room) chunk = room;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, ubuf + done, chunk) != 0) return done ? done : ERR(EFAULT);
        for (uint64_t i = 0; i < chunk; i++) {
            p->buf[(p->head + p->len) % LX_PIPE_CAP] = kbuf[i];
            p->len++;
        }
        done += chunk;
    }
    return done;
}

static uint64_t sys_pipe2(int32_t *ufds, uint64_t flags) {
    struct lx_state *s = lx_get(sched_current_process());
    lx_pipe_t *p = (lx_pipe_t *)kmalloc(sizeof(lx_pipe_t));
    lx_file_t *r, *w;
    int32_t fds[2];
    if (!s || !p) return ERR(ENOMEM);
    kzero(p, sizeof(*p));
    p->buf = (char *)kmalloc(LX_PIPE_CAP);
    r = file_new(LXF_PIPE);
    w = file_new(LXF_PIPE);
    if (!p->buf || !r || !w) return ERR(ENOMEM);
    p->readers = 1;
    p->writers = 1;
    r->pipe = w->pipe = p;
    w->write_end = 1;
    r->flags = flags & O_NONBLOCK;
    w->flags = O_WRONLY | (flags & O_NONBLOCK);
    fds[0] = fd_alloc(s, 0, r, (flags & O_CLOEXEC) != 0);
    if (fds[0] < 0) {
        file_put(r);
        file_put(w);
        return ERR(EMFILE);
    }
    fds[1] = fd_alloc(s, 0, w, (flags & O_CLOEXEC) != 0);
    if (fds[1] < 0) {
        fd_close(s, fds[0]);
        file_put(w);
        return ERR(EMFILE);
    }
    if (copy_to_user(ufds, fds, sizeof(fds)) != 0) return ERR(EFAULT);
    return 0;
}

/* ---- paths -------------------------------------------------------------- */

static vfs_node_t *cwd_node(void) {
    process_t *p = sched_current_process();
    return p && p->cwd ? p->cwd : vfs_root();
}

static int64_t copy_path(const char *upath, char *out) {
    uint64_t len;
    if (!upath) return -EFAULT;
    len = strnlen_user(upath, LX_PATH_MAX - 1);
    if (len == (uint64_t)-1) return -EFAULT;
    if (len >= LX_PATH_MAX - 1) return -ENAMETOOLONG;
    if (copy_from_user(out, upath, len + 1) != 0) return -EFAULT;
    out[len] = 0;
    return (int64_t)len;
}

/* Turns (dirfd, user path) into an absolute kernel path. */
/* Linux programs see ICDA's tree with a Linux root filling the gaps: a path
 * ICDA lacks (/lib/..., /usr/..., /etc/fonts/...) is looked up under /linux,
 * or under a "linux" folder at the top of a mounted volume.  ICDA's own
 * paths (/home, /volumes, /dev) keep priority. */
static const char *const overlay_roots[] = {
    "/linux", "/volumes/fat32-1/linux", "/volumes/fat32-2/linux", "/volumes/fat32-3/linux", "/volumes/fat32-4/linux",
    "/volumes/exfat-1/linux", "/volumes/exfat-2/linux", "/volumes/ntfs-1/linux", "/volumes/ntfs-2/linux",
};

static int has_prefix(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

int lx_overlay_path(char *path) {
    vfs_node_t *root = vfs_root();
    char alt[LX_PATH_MAX];
    if (path[0] != '/' || vfs_resolve(root, path)) return 0;
    if (has_prefix(path, "/proc") || has_prefix(path, "/dev") || has_prefix(path, "/volumes")) return 0;
    for (uint64_t i = 0; i < sizeof(overlay_roots) / sizeof(overlay_roots[0]); i++) {
        uint64_t n = kstrlen(overlay_roots[i]);
        if (n + kstrlen(path) + 1 > LX_PATH_MAX || !vfs_resolve(root, overlay_roots[i])) continue;
        kstrcpy(alt, overlay_roots[i], LX_PATH_MAX);
        kstrcpy(alt + n, path, LX_PATH_MAX - n);
        if (vfs_resolve(root, alt)) {
            kstrcpy(path, alt, LX_PATH_MAX);
            return 1;
        }
    }
    return 0;
}

static int64_t at_path(int64_t dirfd, const char *upath, char *out) {
    char rel[LX_PATH_MAX];
    int64_t rc = copy_path(upath, rel);
    vfs_node_t *base;
    uint64_t n;
    if (rc < 0) return rc;
    if (rel[0] == '/') {
        kstrcpy(out, rel, LX_PATH_MAX);
        (void)lx_overlay_path(out);
        return 0;
    }
    if ((int32_t)dirfd == LX_AT_FDCWD) {
        base = cwd_node();
    } else {
        lx_file_t *f = fd_get(lx_get(sched_current_process()), (int32_t)dirfd);
        if (!f) return -EBADF;
        if (f->kind != LXF_VFS || vfs_node_type(f->node) != VFS_NODE_DIR) return -ENOTDIR;
        base = f->node;
    }
    if (vfs_getcwd(base, out, LX_PATH_MAX) != 0) return -ENAMETOOLONG;
    if (!rel[0]) return 0;
    n = kstrlen(out);
    if (n + 1 + kstrlen(rel) + 1 > LX_PATH_MAX) return -ENAMETOOLONG;
    if (n > 1) out[n++] = '/';
    kstrcpy(out + n, rel, LX_PATH_MAX - n);
    (void)lx_overlay_path(out);
    return 0;
}

static int dev_kind(const char *path) {
    if (kstreq(path, "/dev/null")) return LXF_NULL;
    if (kstreq(path, "/dev/zero")) return LXF_ZERO;
    if (kstreq(path, "/dev/random") || kstreq(path, "/dev/urandom")) return LXF_RANDOM;
    if (kstreq(path, "/dev/tty") || kstreq(path, "/dev/console") || kstreq(path, "/dev/stdin") ||
        kstreq(path, "/dev/stdout") || kstreq(path, "/dev/stderr")) {
        return LXF_TTY;
    }
    return 0;
}

static int proc_parse(const char *path, uint64_t *pid, const char **leaf);
static int proc_valid(uint64_t pid, const char *leaf);

/* ---- stat --------------------------------------------------------------- */

/* Imported volume nodes carry inode 0, which glibc treats as a deleted
 * directory entry; derive a stable number from the node instead. */
static uint64_t node_ino(vfs_node_t *node) {
    uint64_t ino = vfs_node_inode(node);
    return ino ? ino : (((uint64_t)(uintptr_t)node >> 4) & 0xFFFFFFFFFFULL) | (1ULL << 40);
}


typedef struct {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t  size, blksize, blocks;
    uint64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t  reserved[3];
} lx_stat_t;

static uint64_t now_epoch(void) {
    if (!boot_epoch) {
        rtc_time_t t;
        static const uint16_t cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
        if (rtc_read(&t) == 0 && t.year >= 1970 && t.month >= 1 && t.month <= 12) {
            uint64_t y = t.year, days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
            days += cum[t.month - 1] + t.day - 1;
            if (t.month > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) days++;
            boot_epoch = days * 86400 + t.hour * 3600ULL + t.minute * 60ULL + t.second - sched_ticks() / 100;
        } else {
            boot_epoch = 1767225600ULL;
        }
    }
    return boot_epoch + sched_ticks() / 100;
}

static void fill_stat(lx_stat_t *st, int kind, vfs_node_t *node) {
    uint64_t now = now_epoch();
    kzero(st, sizeof(*st));
    st->dev = 1;
    st->nlink = 1;
    st->blksize = 4096;
    st->atime = st->mtime = st->ctime = now;
    if (kind == LXF_VFS && node) {
        st->ino = node_ino(node);
        if (vfs_node_type(node) == VFS_NODE_DIR) {
            st->mode = 040000 | (vfs_node_readonly(node) ? 0555 : 0755);
            st->nlink = 2;
            st->size = 4096;
        } else {
            st->mode = 0100000 | (vfs_node_readonly(node) ? 0555 : 0755);
            st->size = (int64_t)vfs_node_size(node);
        }
        st->blocks = (st->size + 511) / 512;
    } else if (kind == LXF_PROCDIR) {
        st->mode = 040555;
        st->nlink = 2;
    } else if (kind == LXF_MEM) {
        st->mode = 0100444;
    } else if (kind == LXF_PIPE) {
        st->mode = 010600;
        st->ino = 0x10000;
    } else {
        st->mode = kind == LXF_TTY ? 020620 : 020666;
        st->ino = 0x20000 + (uint64_t)kind;
        st->rdev = kind == LXF_TTY ? (136ULL << 8) : ((1ULL << 8) | 3);
    }
}

static uint64_t stat_path(const char *path, lx_stat_t *ust) {
    lx_stat_t st;
    int kind = dev_kind(path);
    vfs_node_t *node = 0;
    uint64_t ppid;
    const char *leaf;
    if (proc_parse(path, &ppid, &leaf)) {
        if (!proc_valid(ppid, leaf)) return ERR(ENOENT);
        fill_stat(&st, *leaf ? LXF_MEM : LXF_PROCDIR, 0);
        return copy_to_user(ust, &st, sizeof(st)) ? ERR(EFAULT) : 0;
    }
    if (!kind) {
        node = vfs_resolve(cwd_node(), path);
        if (!node) return ERR(ENOENT);
        kind = LXF_VFS;
    }
    fill_stat(&st, kind, node);
    return copy_to_user(ust, &st, sizeof(st)) ? ERR(EFAULT) : 0;
}

static uint64_t sys_fstat(int64_t fd, lx_stat_t *ust) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    lx_stat_t st;
    if (!f) return ERR(EBADF);
    fill_stat(&st, f->kind, f->node);
    return copy_to_user(ust, &st, sizeof(st)) ? ERR(EFAULT) : 0;
}

static uint64_t sys_newfstatat(int64_t dirfd, const char *upath, lx_stat_t *ust, uint64_t flags) {
    char path[LX_PATH_MAX];
    int64_t rc;
    char first;
    if (copy_from_user(&first, upath, 1) != 0) return ERR(EFAULT);
    if (first == 0 && (flags & 0x1000)) return sys_fstat(dirfd, ust); /* AT_EMPTY_PATH */
    rc = at_path(dirfd, upath, path);
    if (rc < 0) return ERR(-rc);
    return stat_path(path, ust);
}

static uint64_t sys_statfs(uint8_t *ubuf) {
    uint64_t sf[15];
    kzero(sf, sizeof(sf));
    sf[0] = 0xEF53;
    sf[1] = 4096;
    sf[2] = pmm_total_frames();
    sf[3] = pmm_free_frames();
    sf[4] = pmm_free_frames();
    sf[5] = 65536;
    sf[6] = 32768;
    sf[8] = 255;
    sf[9] = 4096;
    return copy_to_user(ubuf, sf, 120) ? ERR(EFAULT) : 0;
}

/* ---- /proc -------------------------------------------------------------- */

typedef struct {
    char    *buf;
    uint64_t len;
    uint64_t cap;
} sbuf_t;

static void sb_put(sbuf_t *b, const char *s) {
    while (*s && b->len + 1 < b->cap) b->buf[b->len++] = *s++;
    b->buf[b->len] = 0;
}

static void sb_num(sbuf_t *b, uint64_t v) {
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n && b->len + 1 < b->cap) b->buf[b->len++] = tmp[--n];
    b->buf[b->len] = 0;
}

static void sb_kv(sbuf_t *b, const char *key, uint64_t kb) {
    sb_put(b, key);
    sb_num(b, kb);
    sb_put(b, " kB\n");
}

static const char *const proc_files[] = { "meminfo", "uptime", "loadavg", "mounts", "cpuinfo", "version", "stat" };

/* Parses "/proc", "/proc/<pid|self>[/leaf]" or "/proc/<file>".  Returns 1 for
 * a /proc path, filling pid (0 = top level) and leaf ("" for a directory). */
static int proc_parse(const char *path, uint64_t *pid, const char **leaf) {
    const char *p;
    if (path[0] != '/' || path[1] != 'p' || path[2] != 'r' || path[3] != 'o' || path[4] != 'c') return 0;
    if (path[5] != 0 && path[5] != '/') return 0;
    *pid = 0;
    p = path + 5;
    while (*p == '/') p++;
    if (!*p) {
        *leaf = "";
        return 1;
    }
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' && (p[4] == 0 || p[4] == '/')) {
        *pid = sched_current_process()->pid;
        p += 4;
    } else if (*p >= '0' && *p <= '9') {
        uint64_t v = 0;
        while (*p >= '0' && *p <= '9') v = v * 10 + (uint64_t)(*p++ - '0');
        if (*p && *p != '/') return 0;
        *pid = v;
    } else {
        *leaf = p;
        return 1;
    }
    while (*p == '/') p++;
    *leaf = p;
    return 1;
}

static int proc_valid(uint64_t pid, const char *leaf) {
    if (pid) {
        process_t *q = sched_find_process(pid);
        if (!q || q->state == PROCESS_REAPED) return 0;
        return !*leaf || kstreq(leaf, "stat") || kstreq(leaf, "cmdline") || kstreq(leaf, "status") ||
               kstreq(leaf, "comm");
    }
    if (!*leaf) return 1;
    for (uint64_t i = 0; i < sizeof(proc_files) / sizeof(proc_files[0]); i++) {
        if (kstreq(leaf, proc_files[i])) return 1;
    }
    return 0;
}

static char proc_state_char(process_t *q) {
    if (q->state == PROCESS_EXITED) return 'Z';
    if (q->state == PROCESS_BLOCKED) return 'S';
    if (q->state == PROCESS_STOPPED) return 'T';
    return q == sched_current_process() ? 'R' : 'S';
}

static uint64_t proc_mem_kb(process_t *q) {
    return q->addr_space ? q->addr_space->mapped_pages * 4 : 0;
}

static void proc_render(sbuf_t *b, uint64_t pid, const char *leaf) {
    uint64_t total = pmm_total_frames() * 4, free = pmm_free_frames() * 4, t = sched_ticks();
    if (pid) {
        process_t *q = sched_find_process(pid);
        char st[2] = { proc_state_char(q), 0 };
        if (kstreq(leaf, "cmdline")) {
            sb_put(b, q->name);
            b->len++;
            return;
        }
        if (kstreq(leaf, "comm")) {
            sb_put(b, q->name);
            sb_put(b, "\n");
            return;
        }
        if (kstreq(leaf, "status")) {
            sb_put(b, "Name:\t");
            sb_put(b, q->name);
            sb_put(b, "\nState:\t");
            sb_put(b, st);
            sb_put(b, "\nPid:\t");
            sb_num(b, q->pid);
            sb_put(b, "\nPPid:\t");
            sb_num(b, q->parent ? q->parent->pid : 0);
            sb_put(b, "\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n");
            sb_kv(b, "VmRSS:\t", proc_mem_kb(q));
            return;
        }
        sb_num(b, q->pid);
        sb_put(b, " (");
        sb_put(b, q->name);
        sb_put(b, ") ");
        sb_put(b, st);
        sb_put(b, " ");
        sb_num(b, q->parent ? q->parent->pid : 0);
        sb_put(b, " ");
        sb_num(b, q->process_group_id ? q->process_group_id : q->pid);
        sb_put(b, " ");
        sb_num(b, q->session_id ? q->session_id : q->pid);
        sb_put(b, " 0 -1 0 0 0 0 0 ");
        sb_num(b, q->cpu_ticks);
        sb_put(b, " 0 0 0 20 0 1 0 0 ");
        sb_num(b, proc_mem_kb(q) * 1024);
        sb_put(b, " ");
        sb_num(b, proc_mem_kb(q) / 4);
        sb_put(b, " 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n");
        return;
    }
    if (kstreq(leaf, "meminfo")) {
        sb_kv(b, "MemTotal:       ", total);
        sb_kv(b, "MemFree:        ", free);
        sb_kv(b, "MemAvailable:   ", free);
        sb_kv(b, "Buffers:        ", 0);
        sb_kv(b, "Cached:         ", 0);
        sb_kv(b, "SwapCached:     ", 0);
        sb_kv(b, "SwapTotal:      ", 0);
        sb_kv(b, "SwapFree:       ", 0);
        sb_kv(b, "Shmem:          ", 0);
        sb_kv(b, "SReclaimable:   ", 0);
    } else if (kstreq(leaf, "uptime")) {
        sb_num(b, t / 100);
        sb_put(b, ".");
        if (t % 100 < 10) sb_put(b, "0");
        sb_num(b, t % 100);
        sb_put(b, " 0.00\n");
    } else if (kstreq(leaf, "loadavg")) {
        uint64_t n = 0, last = 0;
        for (process_t *q = sched_first_process(); q; q = q->next_all) {
            if (q->state != PROCESS_EXITED && q->state != PROCESS_REAPED) n++;
            if (q->pid > last) last = q->pid;
        }
        sb_put(b, "0.00 0.00 0.00 1/");
        sb_num(b, n);
        sb_put(b, " ");
        sb_num(b, last);
        sb_put(b, "\n");
    } else if (kstreq(leaf, "mounts")) {
        sb_put(b, "rootfs / rootfs rw 0 0\n");
    } else if (kstreq(leaf, "cpuinfo")) {
        sb_put(b, "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: x86-64 (ICDA)\nflags\t\t: fpu sse sse2\n\n");
    } else if (kstreq(leaf, "version")) {
        sb_put(b, "Linux version 6.1.0-icda (ICDA kernel) #1 SMP\n");
    } else if (kstreq(leaf, "stat")) {
        sb_put(b, "cpu  ");
        sb_num(b, t);
        sb_put(b, " 0 0 0 0 0 0 0 0 0\nbtime ");
        sb_num(b, now_epoch() - t / 100);
        sb_put(b, "\n");
    }
}

static lx_file_t *proc_open(uint64_t pid, const char *leaf) {
    lx_file_t *f;
    sbuf_t b;
    if (!*leaf) {
        f = file_new(LXF_PROCDIR);
        if (f) f->proc_pid = pid;
        return f;
    }
    f = file_new(LXF_MEM);
    if (!f) return 0;
    b.cap = 4096;
    b.len = 0;
    b.buf = (char *)kmalloc(b.cap);
    if (!b.buf) {
        kfree(f);
        return 0;
    }
    b.buf[0] = 0;
    proc_render(&b, pid, leaf);
    f->mem = b.buf;
    f->mem_len = b.len;
    return f;
}

/* Directory entry idx of a /proc directory; returns 0 past the end. */
static int proc_dirent(lx_file_t *f, uint64_t idx, const char **name, char *numbuf, int *is_dir) {
    uint64_t nfiles = sizeof(proc_files) / sizeof(proc_files[0]);
    *is_dir = 0;
    if (f->proc_pid) {
        static const char *const leaves[] = { "stat", "cmdline", "status", "comm" };
        if (idx >= 4) return 0;
        *name = leaves[idx];
        return 1;
    }
    if (idx < nfiles) {
        *name = proc_files[idx];
        return 1;
    }
    if (idx == nfiles) {
        *name = "self";
        *is_dir = 1;
        return 1;
    }
    idx -= nfiles + 1;
    for (process_t *q = sched_first_process(); q; q = q->next_all) {
        sbuf_t b;
        if (q->kind != PROCESS_USER || q->state == PROCESS_REAPED || q->state == PROCESS_EXITED) continue;
        if (idx--) continue;
        b.buf = numbuf;
        b.cap = 24;
        b.len = 0;
        sb_num(&b, q->pid);
        *name = numbuf;
        *is_dir = 1;
        return 1;
    }
    return 0;
}

/* ---- open/read/write ---------------------------------------------------- */

static uint64_t sys_openat(int64_t dirfd, const char *upath, uint64_t flags) {
    char path[LX_PATH_MAX];
    struct lx_state *s = lx_get(sched_current_process());
    vfs_node_t *node;
    lx_file_t *f;
    int64_t rc = at_path(dirfd, upath, path);
    int kind, fd;
    if (rc < 0) return ERR(-rc);
    if (!s) return ERR(ENOMEM);
    kind = dev_kind(path);
    if (kind) {
        f = file_new(kind);
        if (!f) return ERR(ENOMEM);
        f->flags = flags;
        fd = fd_alloc(s, 0, f, (flags & O_CLOEXEC) != 0);
        if (fd < 0) file_put(f);
        return (uint64_t)(int64_t)fd;
    }
    {
        uint64_t ppid;
        const char *leaf;
        if (proc_parse(path, &ppid, &leaf)) {
            if (!proc_valid(ppid, leaf)) return ERR(ENOENT);
            if ((flags & O_ACCMODE) != 0) return ERR(EACCES);
            f = proc_open(ppid, leaf);
            if (!f) return ERR(ENOMEM);
            fd = fd_alloc(s, 0, f, (flags & O_CLOEXEC) != 0);
            if (fd < 0) file_put(f);
            return (uint64_t)(int64_t)fd;
        }
    }
    node = vfs_resolve(cwd_node(), path);
    if (node && (flags & O_CREAT) && (flags & O_EXCL)) return ERR(EEXIST);
    if (!node) {
        if (!(flags & O_CREAT)) return ERR(ENOENT);
        if (vfs_create(cwd_node(), path) != 0) return ERR(EACCES);
        node = vfs_resolve(cwd_node(), path);
        if (!node) return ERR(ENOENT);
    }
    if (vfs_node_type(node) == VFS_NODE_DIR) {
        if ((flags & O_ACCMODE) != 0) return ERR(EISDIR);
    } else {
        if (flags & O_DIRECTORY) return ERR(ENOTDIR);
        if ((flags & O_ACCMODE) != 0 && vfs_node_readonly(node)) return ERR(EROFS);
        if ((flags & O_TRUNC) && (flags & O_ACCMODE) != 0 && vfs_node_size(node) != 0 &&
            vfs_node_truncate(node, 0) != 0) {
            return ERR(EIO);
        }
    }
    f = file_new(LXF_VFS);
    if (!f) return ERR(ENOMEM);
    f->node = node;
    f->flags = flags;
    fd = fd_alloc(s, 0, f, (flags & O_CLOEXEC) != 0);
    if (fd < 0) file_put(f);
    return (uint64_t)(int64_t)fd;
}

static uint64_t file_read(lx_file_t *f, char *ubuf, uint64_t count, uint64_t *offp) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return ERR(EBADF);
    if (count == 0) return 0;
    if (!user_range_prepare_cur_w(ubuf, count)) return ERR(EFAULT);
    switch (f->kind) {
        case LXF_TTY: return tty_read(f, ubuf, count);
        case LXF_PIPE: return f->write_end ? ERR(EBADF) : pipe_read(f, ubuf, count);
        case LXF_PROCDIR: return ERR(EISDIR);
        case LXF_MEM: {
            uint64_t n;
            if (*offp >= f->mem_len) return 0;
            n = f->mem_len - *offp;
            if (n > count) n = count;
            if (copy_to_user(ubuf, f->mem + *offp, n) != 0) return ERR(EFAULT);
            *offp += n;
            return n;
        }
        case LXF_NULL: return 0;
        case LXF_ZERO:
        case LXF_RANDOM: {
            uint8_t kbuf[256];
            uint64_t n = count < sizeof(kbuf) ? count : sizeof(kbuf);
            for (uint64_t i = 0; i < n; i++) kbuf[i] = f->kind == LXF_ZERO ? 0 : (uint8_t)rng_next();
            return copy_to_user(ubuf, kbuf, n) ? ERR(EFAULT) : n;
        }
        case LXF_VFS: {
            uint64_t size, done = 0, off = *offp;
            char *bounce;
            if (vfs_node_type(f->node) == VFS_NODE_DIR) return ERR(EISDIR);
            size = vfs_node_size(f->node);
            if (off >= size) return 0;
            if (count > size - off) count = size - off;
            bounce = (char *)kmalloc(count < 65536 ? count : 65536);
            if (!bounce) return ERR(ENOMEM);
            while (done < count) {
                uint64_t chunk = count - done;
                int64_t got;
                if (chunk > 65536) chunk = 65536;
                got = vfs_node_read_at(f->node, off + done, bounce, chunk);
                if (got <= 0) break;
                if (copy_to_user(ubuf + done, bounce, (uint64_t)got) != 0) {
                    kfree(bounce);
                    return done ? done : ERR(EFAULT);
                }
                done += (uint64_t)got;
                if ((uint64_t)got < chunk) break;
            }
            kfree(bounce);
            *offp = off + done;
            return done;
        }
    }
    return ERR(EBADF);
}

static uint64_t file_write(lx_file_t *f, const char *ubuf, uint64_t count, uint64_t *offp) {
    if ((f->flags & O_ACCMODE) == 0 && f->kind == LXF_VFS) return ERR(EBADF);
    if (count == 0) return 0;
    if (!user_range_prepare_cur(ubuf, count)) return ERR(EFAULT);
    switch (f->kind) {
        case LXF_TTY: return tty_write(ubuf, count);
        case LXF_PIPE: return f->write_end ? pipe_write(f, ubuf, count) : ERR(EBADF);
        case LXF_NULL:
        case LXF_ZERO:
        case LXF_RANDOM: return count;
        case LXF_VFS: {
            char kbuf[2048];
            uint64_t done = 0, off = (f->flags & O_APPEND) ? vfs_node_size(f->node) : *offp;
            if (vfs_node_type(f->node) == VFS_NODE_DIR) return ERR(EISDIR);
            if (vfs_node_readonly(f->node)) return ERR(EROFS);
            while (done < count) {
                uint64_t chunk = count - done;
                if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
                if (copy_from_user(kbuf, ubuf + done, chunk) != 0) return done ? done : ERR(EFAULT);
                if (vfs_node_write_at(f->node, off + done, kbuf, chunk) != 0) return done ? done : ERR(EIO);
                done += chunk;
            }
            *offp = off + done;
            return done;
        }
    }
    return ERR(EBADF);
}

static uint64_t sys_rw(int64_t fd, char *ubuf, uint64_t count, int write) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    if (!f) return ERR(EBADF);
    return write ? file_write(f, ubuf, count, &f->off) : file_read(f, ubuf, count, &f->off);
}

static uint64_t sys_prw(int64_t fd, char *ubuf, uint64_t count, uint64_t off, int write) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    uint64_t pos = off, saved, rc;
    if (!f) return ERR(EBADF);
    if (f->kind != LXF_VFS) return ERR(ESPIPE);
    saved = f->flags;
    f->flags &= ~(uint64_t)O_APPEND;
    rc = write ? file_write(f, ubuf, count, &pos) : file_read(f, ubuf, count, &pos);
    f->flags = saved;
    return rc;
}

static uint64_t sys_rwv(int64_t fd, const uint64_t *uiov, uint64_t cnt, int write) {
    uint64_t total = 0;
    if (cnt > 1024) return ERR(EINVAL);
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t iov[2], rc;
        if (copy_from_user(iov, uiov + i * 2, sizeof(iov)) != 0) return total ? total : ERR(EFAULT);
        if (!iov[1]) continue;
        rc = sys_rw(fd, (char *)iov[0], iov[1], write);
        if ((int64_t)rc < 0) return total ? total : rc;
        total += rc;
        if (rc < iov[1]) break;
    }
    return total;
}

static uint64_t sys_lseek(int64_t fd, int64_t off, uint64_t whence) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    int64_t base;
    if (!f) return ERR(EBADF);
    if (f->kind == LXF_PIPE || f->kind == LXF_TTY) return ERR(ESPIPE);
    if (f->kind != LXF_VFS && f->kind != LXF_MEM && f->kind != LXF_PROCDIR) return 0;
    if (whence == 0) base = 0;
    else if (whence == 1) base = (int64_t)f->off;
    else if (whence == 2) base = f->kind == LXF_VFS ? (int64_t)vfs_node_size(f->node) : (int64_t)f->mem_len;
    else return ERR(EINVAL);
    if (base + off < 0) return ERR(EINVAL);
    f->off = (uint64_t)(base + off);
    return f->off;
}

static uint64_t sys_getdents64(int64_t fd, uint8_t *ubuf, uint64_t count) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    uint8_t kbuf[288];
    uint64_t written = 0;
    if (!f) return ERR(EBADF);
    if (f->kind != LXF_PROCDIR && (f->kind != LXF_VFS || vfs_node_type(f->node) != VFS_NODE_DIR)) return ERR(ENOTDIR);
    for (;;) {
        uint64_t idx = f->off;
        const char *name;
        vfs_node_t *child = 0;
        uint64_t nlen, reclen;
        int pdir = 1;
        char numbuf[24];
        if (idx == 0) name = ".";
        else if (idx == 1) name = "..";
        else if (f->kind == LXF_PROCDIR) {
            if (!proc_dirent(f, idx - 2, &name, numbuf, &pdir)) break;
        } else {
            if (idx >= vfs_child_count(f->node) + 2) break;
            child = vfs_child_at(f->node, idx - 2);
            if (!child) break;
            name = vfs_node_name(child);
            pdir = vfs_node_type(child) == VFS_NODE_DIR;
        }
        nlen = kstrlen(name);
        if (nlen > 255) nlen = 255;
        reclen = (19 + nlen + 1 + 7) & ~7ULL;
        if (written + reclen > count) {
            if (written == 0) return ERR(EINVAL);
            break;
        }
        kzero(kbuf, reclen);
        *(uint64_t *)kbuf = child ? node_ino(child) : (idx + 1);
        *(int64_t *)(kbuf + 8) = (int64_t)(idx + 1);
        *(uint16_t *)(kbuf + 16) = (uint16_t)reclen;
        kbuf[18] = (uint8_t)(pdir ? 4 : 8);
        kcopy(kbuf + 19, name, nlen);
        if (copy_to_user(ubuf + written, kbuf, reclen) != 0) return written ? written : ERR(EFAULT);
        written += reclen;
        f->off = idx + 1;
    }
    return written;
}

static uint64_t sys_ftruncate(int64_t fd, uint64_t len) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    if (!f) return ERR(EBADF);
    if (f->kind != LXF_VFS || vfs_node_type(f->node) != VFS_NODE_FILE) return ERR(EINVAL);
    return vfs_node_truncate(f->node, len) == 0 ? 0 : ERR(EIO);
}

static uint64_t sys_truncate(const char *upath, uint64_t len) {
    char path[LX_PATH_MAX];
    vfs_node_t *node;
    int64_t rc = at_path(LX_AT_FDCWD, upath, path);
    if (rc < 0) return ERR(-rc);
    node = vfs_resolve(cwd_node(), path);
    if (!node) return ERR(ENOENT);
    if (vfs_node_type(node) != VFS_NODE_FILE) return ERR(EISDIR);
    return vfs_node_truncate(node, len) == 0 ? 0 : ERR(EIO);
}

static uint64_t sys_dup3(int64_t oldfd, int64_t newfd, uint64_t flags, int dup2) {
    struct lx_state *s = lx_get(sched_current_process());
    lx_file_t *f = fd_get(s, oldfd);
    if (!f) return ERR(EBADF);
    if (newfd < 0 || newfd >= LX_FD_MAX) return ERR(EBADF);
    if (oldfd == newfd) return dup2 ? (uint64_t)newfd : ERR(EINVAL);
    if (s->fd[newfd]) fd_close(s, newfd);
    file_get(f);
    s->fd[newfd] = f;
    s->cloexec[newfd] = (uint8_t)((flags & O_CLOEXEC) != 0);
    return (uint64_t)newfd;
}

static uint64_t sys_fcntl(int64_t fd, uint64_t cmd, uint64_t arg) {
    struct lx_state *s = lx_get(sched_current_process());
    lx_file_t *f = fd_get(s, fd);
    if (!f) return ERR(EBADF);
    switch (cmd) {
        case 0: case 1030: { /* F_DUPFD, F_DUPFD_CLOEXEC */
            int nfd;
            file_get(f);
            nfd = fd_alloc(s, (int)arg, f, cmd == 1030);
            if (nfd < 0) file_put(f);
            return (uint64_t)(int64_t)nfd;
        }
        case 1: return s->cloexec[fd] ? 1 : 0;
        case 2: s->cloexec[fd] = (uint8_t)(arg & 1); return 0;
        case 3: return f->flags;
        case 4:
            f->flags = (f->flags & ~(uint64_t)(O_APPEND | O_NONBLOCK)) | (arg & (O_APPEND | O_NONBLOCK));
            return 0;
        case 5: case 6: case 7: return 0;
        default: return ERR(EINVAL);
    }
}

static uint64_t sys_ioctl(int64_t fd, uint64_t req, uint64_t arg) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    if (!f) return ERR(EBADF);
    if (f->kind == LXF_TTY) return tty_ioctl(f, req, arg);
    if (req == 0x541B && f->kind == LXF_PIPE) {
        int32_t n = (int32_t)f->pipe->len;
        return copy_to_user((void *)arg, &n, 4) ? ERR(EFAULT) : 0;
    }
    if (req == 0x5421) return 0; /* FIONBIO */
    return ERR(ENOTTY);
}

/* ---- directories and names --------------------------------------------- */

static uint64_t sys_getcwd(char *ubuf, uint64_t size) {
    char path[LX_PATH_MAX];
    uint64_t n;
    if (vfs_getcwd(cwd_node(), path, sizeof(path)) != 0) return ERR(ENAMETOOLONG);
    n = kstrlen(path) + 1;
    if (n > size) return ERR(ERANGE);
    return copy_to_user(ubuf, path, n) ? ERR(EFAULT) : n;
}

static uint64_t set_cwd(vfs_node_t *node) {
    process_t *p = sched_current_process();
    if (!node) return ERR(ENOENT);
    if (vfs_node_type(node) != VFS_NODE_DIR) return ERR(ENOTDIR);
    p->cwd = node;
    return 0;
}

static uint64_t sys_chdir(const char *upath) {
    char path[LX_PATH_MAX];
    int64_t rc = at_path(LX_AT_FDCWD, upath, path);
    if (rc < 0) return ERR(-rc);
    return set_cwd(vfs_resolve(cwd_node(), path));
}

static uint64_t sys_fchdir(int64_t fd) {
    lx_file_t *f = fd_get(lx_get(sched_current_process()), fd);
    if (!f) return ERR(EBADF);
    if (f->kind != LXF_VFS) return ERR(ENOTDIR);
    return set_cwd(f->node);
}

static uint64_t sys_mkdirat(int64_t dirfd, const char *upath) {
    char path[LX_PATH_MAX];
    int64_t rc = at_path(dirfd, upath, path);
    if (rc < 0) return ERR(-rc);
    if (vfs_resolve(cwd_node(), path)) return ERR(EEXIST);
    return vfs_mkdir(cwd_node(), path) == 0 ? 0 : ERR(EACCES);
}

static uint64_t sys_unlinkat(int64_t dirfd, const char *upath, uint64_t flags) {
    char path[LX_PATH_MAX];
    vfs_node_t *node;
    int64_t rc = at_path(dirfd, upath, path);
    if (rc < 0) return ERR(-rc);
    node = vfs_resolve(cwd_node(), path);
    if (!node) return ERR(ENOENT);
    if (flags & 0x200) { /* AT_REMOVEDIR */
        if (vfs_node_type(node) != VFS_NODE_DIR) return ERR(ENOTDIR);
        if (vfs_child_count(node)) return ERR(ENOTEMPTY);
    } else if (vfs_node_type(node) == VFS_NODE_DIR) {
        return ERR(EISDIR);
    }
    if (vfs_node_readonly(node)) return ERR(EROFS);
    return vfs_remove(cwd_node(), path) == 0 ? 0 : ERR(EACCES);
}

static uint64_t sys_renameat(int64_t olddir, const char *uold, int64_t newdir, const char *unew) {
    char from[LX_PATH_MAX], to[LX_PATH_MAX];
    int64_t rc = at_path(olddir, uold, from);
    if (rc < 0) return ERR(-rc);
    rc = at_path(newdir, unew, to);
    if (rc < 0) return ERR(-rc);
    if (!vfs_resolve(cwd_node(), from)) return ERR(ENOENT);
    return vfs_rename(cwd_node(), from, to) == 0 ? 0 : ERR(EXDEV);
}

static uint64_t sys_faccessat(int64_t dirfd, const char *upath, uint64_t mode) {
    char path[LX_PATH_MAX];
    vfs_node_t *node;
    int64_t rc = at_path(dirfd, upath, path);
    if (rc < 0) return ERR(-rc);
    {
        uint64_t ppid;
        const char *leaf;
        if (proc_parse(path, &ppid, &leaf)) return proc_valid(ppid, leaf) ? 0 : ERR(ENOENT);
    }
    if (dev_kind(path)) return 0;
    node = vfs_resolve(cwd_node(), path);
    if (!node) return ERR(ENOENT);
    if ((mode & 2) && vfs_node_readonly(node)) return ERR(EROFS);
    return 0;
}

static uint64_t sys_readlinkat(int64_t dirfd, const char *upath, char *ubuf, uint64_t size) {
    char path[LX_PATH_MAX];
    struct lx_state *s = lx_get(sched_current_process());
    int64_t rc = at_path(dirfd, upath, path);
    uint64_t n;
    if (rc < 0) return ERR(-rc);
    if (!kstreq(path, "/proc/self/exe")) {
        return vfs_resolve(cwd_node(), path) ? ERR(EINVAL) : ERR(ENOENT);
    }
    n = kstrlen(s->exe);
    if (n > size) n = size;
    return copy_to_user(ubuf, s->exe, n) ? ERR(EFAULT) : n;
}

/* ---- memory ------------------------------------------------------------- */

#define LX_MMAP_BASE 0x70000000ULL
#define LX_MMAP_END  0x500000000ULL
#define LX_BRK_BASE  0x60000000ULL

static void unmap_range(process_t *p, uint64_t addr, uint64_t len) {
    for (uint64_t page = addr; page < addr + len; page += PAGE_SIZE_4K) {
        if (vmm_virt_to_phys(p->addr_space, page)) vmm_unmap_page(p->addr_space, page, 1);
    }
}

static int map_zero(process_t *p, uint64_t addr, uint64_t len, uint64_t flags) {
    for (uint64_t page = addr; page < addr + len; page += PAGE_SIZE_4K) {
        uint64_t phys;
        if (vmm_virt_to_phys(p->addr_space, page)) vmm_unmap_page(p->addr_space, page, 1);
        phys = pmm_alloc();
        if (!phys) return -1;
        kzero(PHYS_TO_VIRT(phys), PAGE_SIZE_4K);
        if (vmm_map_page(p->addr_space, page, phys, flags) != 0) {
            pmm_free(phys);
            return -1;
        }
    }
    return 0;
}

static uint64_t lx_res(int64_t r) {
    return r < 0 ? ERR(-r) : (uint64_t)r;
}

static uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
    process_t *p = sched_current_process();
    vfs_node_t *node = 0;
    if (!(flags & 0x20)) { /* not MAP_ANONYMOUS */
        lx_file_t *f = fd_get(lx_get(p), fd);
        if (!f) return ERR(EBADF);
        if (f->kind == LXF_ZERO) flags |= 0x20;      /* /dev/zero: anonymous memory */
        else if (f->kind != LXF_VFS) return ERR(19);
        else node = f->node;
    }
    return lx_res(lxvm_mmap(p, addr, len, (uint32_t)(prot & 7), (uint32_t)flags, node, off));
}

static uint64_t sys_munmap(uint64_t addr, uint64_t len) {
    return lx_res(lxvm_munmap(sched_current_process(), addr, len));
}

static uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    return lx_res(lxvm_mprotect(sched_current_process(), addr, len, (uint32_t)(prot & 7)));
}

static uint64_t sys_brk(uint64_t want) {
    process_t *p = sched_current_process();
    uint64_t old_end, new_end;
    if (!p->linux_brk_pos) p->linux_brk_pos = LX_BRK_BASE;
    if (want < LX_BRK_BASE || want >= LX_MMAP_BASE) return p->linux_brk_pos;
    old_end = (p->linux_brk_pos + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    new_end = (want + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    if (new_end > old_end) {
        if (map_zero(p, old_end, new_end - old_end, VMM_FLAGS_USER_RW) != 0) {
            unmap_range(p, old_end, new_end - old_end);
            return p->linux_brk_pos;
        }
    } else if (new_end < old_end) {
        unmap_range(p, new_end, old_end - new_end);
    }
    p->linux_brk_pos = want;
    return want;
}

/* ---- time and system info ---------------------------------------------- */

static uint64_t sys_clock_gettime(uint64_t clk, uint64_t *uts) {
    uint64_t ts[2], ticks = sched_ticks();
    if (clk == 0 || clk == 5 || clk == 8) {
        ts[0] = now_epoch();
        ts[1] = (ticks % 100) * 10000000ULL;
    } else {
        ts[0] = ticks / 100;
        ts[1] = (ticks % 100) * 10000000ULL;
    }
    return copy_to_user(uts, ts, sizeof(ts)) ? ERR(EFAULT) : 0;
}

static uint64_t sleep_ticks(uint64_t ticks) {
    struct lx_state *s = lx_get(sched_current_process());
    uint64_t end = sched_ticks() + ticks;
    while (sched_ticks() < end) {
        if (signal_pending(s)) return ERR(EINTR);
        sched_sleep(end - sched_ticks() > 10 ? 10 : end - sched_ticks());
    }
    return 0;
}

static uint64_t sys_nanosleep(const uint64_t *ureq) {
    uint64_t req[2];
    if (copy_from_user(req, ureq, sizeof(req)) != 0) return ERR(EFAULT);
    if (req[1] >= 1000000000ULL) return ERR(EINVAL);
    return sleep_ticks(req[0] * 100 + (req[1] + 9999999ULL) / 10000000ULL);
}

static uint64_t sys_clock_nanosleep(uint64_t clk, uint64_t flags, const uint64_t *ureq) {
    uint64_t req[2], now, target;
    if (!(flags & 1)) return sys_nanosleep(ureq);
    if (copy_from_user(req, ureq, sizeof(req)) != 0) return ERR(EFAULT);
    now = (clk == 0 ? now_epoch() * 100 : sched_ticks());
    target = req[0] * 100 + req[1] / 10000000ULL;
    return target > now ? sleep_ticks(target - now) : 0;
}

static uint64_t sys_uname(char *ubuf) {
    char u[390];
    static const char *const fields[6] = { "Linux", "icda", "6.1.0-icda", "#1 SMP ICDA", "x86_64", "(none)" };
    kzero(u, sizeof(u));
    for (int i = 0; i < 6; i++) kstrcpy(u + i * 65, fields[i], 65);
    return copy_to_user(ubuf, u, sizeof(u)) ? ERR(EFAULT) : 0;
}

static uint64_t sys_sysinfo(uint8_t *ubuf) {
    uint8_t b[112];
    uint64_t procs = 0;
    kzero(b, sizeof(b));
    for (process_t *q = sched_first_process(); q; q = q->next_all) {
        if (q->state != PROCESS_EXITED && q->state != PROCESS_REAPED) procs++;
    }
    *(uint64_t *)(b + 0) = sched_ticks() / 100;
    *(uint64_t *)(b + 32) = pmm_total_frames() * PAGE_SIZE_4K;
    *(uint64_t *)(b + 40) = pmm_free_frames() * PAGE_SIZE_4K;
    *(uint16_t *)(b + 80) = (uint16_t)procs;
    *(uint32_t *)(b + 104) = 1;
    return copy_to_user(ubuf, b, sizeof(b)) ? ERR(EFAULT) : 0;
}

static uint64_t sys_prlimit(uint64_t res, const uint64_t *unew, uint64_t *uold) {
    uint64_t lim[2] = { ~0ULL, ~0ULL };
    (void)unew;
    if (res == 3) lim[0] = lim[1] = 8ULL << 20;
    if (res == 7) lim[0] = lim[1] = LX_FD_MAX;
    if (uold && copy_to_user(uold, lim, sizeof(lim)) != 0) return ERR(EFAULT);
    return 0;
}

static uint64_t sys_getrandom(uint8_t *ubuf, uint64_t len) {
    uint8_t kbuf[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done < sizeof(kbuf) ? len - done : sizeof(kbuf);
        for (uint64_t i = 0; i < n; i++) kbuf[i] = (uint8_t)rng_next();
        if (copy_to_user(ubuf + done, kbuf, n) != 0) return done ? done : ERR(EFAULT);
        done += n;
    }
    return len;
}

static uint64_t zero_user(void *u, uint64_t n) {
    uint8_t z[256];
    kzero(z, sizeof(z));
    while (n) {
        uint64_t k = n < sizeof(z) ? n : sizeof(z);
        if (copy_to_user(u, z, k) != 0) return ERR(EFAULT);
        u = (uint8_t *)u + k;
        n -= k;
    }
    return 0;
}

/* ---- poll / select ------------------------------------------------------ */

static int file_ready(lx_file_t *f, int want_write) {
    if (!f) return 0x20;
    switch (f->kind) {
        case LXF_TTY:
            if (want_write) return 4;
            return (tty_has_input(cur_pty()) || !pty_alive(cur_pty())) ? 1 : 0;
        case LXF_PIPE:
            if (f->write_end) {
                if (f->pipe->readers <= 0) return 8;
                return (want_write && f->pipe->len < LX_PIPE_CAP) ? 4 : 0;
            }
            if (f->pipe->len) return 1;
            return f->pipe->writers <= 0 ? 0x11 : 0;
        default:
            return want_write ? 4 : 1;
    }
}

static uint64_t do_poll(uint8_t *ufds, uint64_t nfds, int64_t timeout_ticks) {
    struct lx_state *s = lx_get(sched_current_process());
    uint8_t kfds[8 * 64];
    uint64_t start = sched_ticks();
    if (nfds > 64) return ERR(EINVAL);
    if (copy_from_user(kfds, ufds, nfds * 8) != 0) return ERR(EFAULT);
    for (;;) {
        uint64_t ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            int32_t fd = *(int32_t *)(kfds + i * 8);
            uint16_t ev = *(uint16_t *)(kfds + i * 8 + 4), rev = 0;
            if (fd >= 0) {
                lx_file_t *f = fd_get(s, fd);
                int r = file_ready(f, (ev & 4) != 0);
                rev = (uint16_t)(r & (ev | 0x38));
            }
            *(uint16_t *)(kfds + i * 8 + 6) = rev;
            if (rev) ready++;
        }
        if (ready || timeout_ticks == 0 || (timeout_ticks > 0 && (int64_t)(sched_ticks() - start) >= timeout_ticks)) {
            if (copy_to_user(ufds, kfds, nfds * 8) != 0) return ERR(EFAULT);
            return ready;
        }
        if (signal_pending(s)) return ERR(EINTR);
        sched_sleep(1);
    }
}

static uint64_t do_select(uint64_t nfds, uint8_t *ur, uint8_t *uw, uint8_t *ue, int64_t timeout_ticks) {
    struct lx_state *s = lx_get(sched_current_process());
    uint8_t in_r[16], in_w[16], out_r[16], out_w[16];
    uint64_t start = sched_ticks(), bytes;
    if (nfds > LX_FD_MAX) nfds = LX_FD_MAX;
    bytes = (nfds + 7) / 8;
    kzero(in_r, sizeof(in_r));
    kzero(in_w, sizeof(in_w));
    if (ur && copy_from_user(in_r, ur, bytes) != 0) return ERR(EFAULT);
    if (uw && copy_from_user(in_w, uw, bytes) != 0) return ERR(EFAULT);
    for (;;) {
        uint64_t ready = 0;
        kzero(out_r, sizeof(out_r));
        kzero(out_w, sizeof(out_w));
        for (uint64_t fd = 0; fd < nfds; fd++) {
            uint8_t bit = (uint8_t)(1U << (fd & 7));
            if ((in_r[fd / 8] & bit) && (file_ready(fd_get(s, (int64_t)fd), 0) & 0x31)) {
                out_r[fd / 8] |= bit;
                ready++;
            }
            if ((in_w[fd / 8] & bit) && (file_ready(fd_get(s, (int64_t)fd), 1) & 0x2C)) {
                out_w[fd / 8] |= bit;
                ready++;
            }
        }
        if (ready || timeout_ticks == 0 || (timeout_ticks > 0 && (int64_t)(sched_ticks() - start) >= timeout_ticks)) {
            if (ur && copy_to_user(ur, out_r, bytes) != 0) return ERR(EFAULT);
            if (uw && copy_to_user(uw, out_w, bytes) != 0) return ERR(EFAULT);
            if (ue && zero_user(ue, bytes) != 0) return ERR(EFAULT);
            return ready;
        }
        if (signal_pending(s)) return ERR(EINTR);
        sched_sleep(1);
    }
}

static int64_t ts_ticks(const uint64_t *uts, int usec) {
    uint64_t ts[2];
    if (!uts) return -1;
    if (copy_from_user(ts, uts, sizeof(ts)) != 0) return 0;
    return (int64_t)(ts[0] * 100 + (usec ? (ts[1] + 9999) / 10000 : (ts[1] + 9999999) / 10000000));
}

/* ---- signals: actions, delivery and sigreturn --------------------------- */

static uint64_t sys_rt_sigaction(uint64_t sig, const lx_sigaction_t *uact, lx_sigaction_t *uold) {
    struct lx_state *s = lx_get(sched_current_process());
    if (sig < 1 || sig >= LX_NSIG) return ERR(EINVAL);
    if (uold && copy_to_user(uold, &s->sa[sig], sizeof(lx_sigaction_t)) != 0) return ERR(EFAULT);
    if (uact) {
        lx_sigaction_t a;
        if (sig == SIGKILL || sig == SIGSTOP) return ERR(EINVAL);
        if (copy_from_user(&a, uact, sizeof(a)) != 0) return ERR(EFAULT);
        s->sa[sig] = a;
        if (a.handler == 1) s->pending &= ~(1ULL << (sig - 1));
    }
    return 0;
}

static uint64_t sys_rt_sigprocmask(uint64_t how, const uint64_t *uset, uint64_t *uold) {
    struct lx_state *s = lx_get(sched_current_process());
    uint64_t set;
    if (uold && copy_to_user(uold, &s->sigmask, 8) != 0) return ERR(EFAULT);
    if (!uset) return 0;
    if (copy_from_user(&set, uset, 8) != 0) return ERR(EFAULT);
    if (how == 0) s->sigmask |= set;
    else if (how == 1) s->sigmask &= ~set;
    else if (how == 2) s->sigmask = set;
    else return ERR(EINVAL);
    s->sigmask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    return 0;
}

static int default_ignored(int sig) {
    return sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH ||
           sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
}

static uint8_t fx_area[512] __attribute__((aligned(16)));

static int build_sigframe(struct registers *regs, int sig, lx_sigaction_t *sa, uint64_t old_mask) {
    uint64_t sp = regs->rsp - 128, fx, info, uc;
    uint64_t g[32];
    uint64_t hdr[5];
    uint8_t si[128];
    sp = (sp - 512) & ~63ULL;
    fx = sp;
    sp -= 128;
    info = sp;
    sp -= 304;
    uc = sp;
    sp = (sp & ~15ULL) - 8;
    __asm__ volatile("fxsave64 %0" : "=m"(fx_area));
    if (copy_to_user((void *)fx, fx_area, 512) != 0) return -1;
    kzero(si, sizeof(si));
    *(int32_t *)si = sig;
    if (copy_to_user((void *)info, si, sizeof(si)) != 0) return -1;
    kzero(hdr, sizeof(hdr));
    if (copy_to_user((void *)uc, hdr, sizeof(hdr)) != 0) return -1;
    kzero(g, sizeof(g));
    g[0] = regs->r8; g[1] = regs->r9; g[2] = regs->r10; g[3] = regs->r11;
    g[4] = regs->r12; g[5] = regs->r13; g[6] = regs->r14; g[7] = regs->r15;
    g[8] = regs->rdi; g[9] = regs->rsi; g[10] = regs->rbp; g[11] = regs->rbx;
    g[12] = regs->rdx; g[13] = regs->rax; g[14] = regs->rcx; g[15] = regs->rsp;
    g[16] = regs->rip; g[17] = regs->rflags; g[18] = 0x33;
    g[23] = fx;
    if (copy_to_user((void *)(uc + 40), g, sizeof(g)) != 0) return -1;
    if (copy_to_user((void *)(uc + 296), &old_mask, 8) != 0) return -1;
    if (copy_to_user((void *)sp, &sa->restorer, 8) != 0) return -1;
    regs->rsp = sp;
    regs->rip = sa->handler;
    regs->rdi = (uint64_t)sig;
    regs->rsi = info;
    regs->rdx = uc;
    regs->rax = 0;
    regs->rflags &= ~0x400ULL;
    return 0;
}

static uint64_t sys_rt_sigreturn(struct registers *regs) {
    struct lx_state *s = lx_get(sched_current_process());
    uint64_t uc = regs->rsp, g[32], mask;
    if (copy_from_user(g, (const void *)(uc + 40), sizeof(g)) != 0 ||
        copy_from_user(&mask, (const void *)(uc + 296), 8) != 0 || g[16] >= 0x0000800000000000ULL) {
        s->term_sig = 11;
        user_request_exit_to_kernel(128 + 11);
        return 0;
    }
    if (g[23] && copy_from_user(fx_area, (const void *)g[23], 512) == 0) {
        __asm__ volatile("fxrstor64 %0" : : "m"(fx_area));
    }
    regs->r8 = g[0]; regs->r9 = g[1]; regs->r10 = g[2]; regs->r11 = g[3];
    regs->r12 = g[4]; regs->r13 = g[5]; regs->r14 = g[6]; regs->r15 = g[7];
    regs->rdi = g[8]; regs->rsi = g[9]; regs->rbp = g[10]; regs->rbx = g[11];
    regs->rdx = g[12]; regs->rcx = g[14]; regs->rsp = g[15]; regs->rip = g[16];
    regs->rflags = (g[17] & 0xCD5ULL) | 0x202ULL;
    s->sigmask = mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    return g[13];
}

void lx_after_syscall(struct registers *regs) {
    process_t *p = sched_current_process();
    thread_t *t = sched_current_thread();
    struct lx_state *s;
    if (!p || !t || !p->linux_personality || !p->lx || t->user_return_pending) return;
    s = p->lx;
    for (;;) {
        uint64_t ready = s->pending & ~s->sigmask;
        int sig = 0;
        lx_sigaction_t *sa;
        uint64_t old_mask;
        if (!ready) return;
        while (!(ready & (1ULL << sig))) sig++;
        sig++;
        s->pending &= ~(1ULL << (sig - 1));
        sa = &s->sa[sig];
        if (sa->handler == 1) continue;
        if (sa->handler == 0) {
            if (default_ignored(sig)) continue;
            s->term_sig = sig;
            user_request_exit_to_kernel(128 + (uint64_t)sig);
            return;
        }
        if ((int64_t)regs->rax == -EINTR && (sa->flags & SA_RESTART)) {
            regs->rax = cur_nr;
            regs->rip -= 2;
        }
        old_mask = s->sigmask;
        if (build_sigframe(regs, sig, sa, old_mask) != 0) {
            s->term_sig = 11;
            user_request_exit_to_kernel(128 + 11);
            return;
        }
        s->sigmask |= sa->mask;
        if (!(sa->flags & SA_NODEFER)) s->sigmask |= 1ULL << (sig - 1);
        if (sa->flags & SA_RESETHAND) sa->handler = 0;
        return;
    }
}

static uint64_t sys_kill(int64_t pid, int64_t sig) {
    process_t *self = sched_current_process();
    int sent = 0;
    if (sig < 0 || sig >= LX_NSIG) return ERR(EINVAL);
    if (pid > 0) {
        process_t *t = sched_find_process((uint64_t)pid);
        if (!t || t->state == PROCESS_EXITED || t->state == PROCESS_REAPED) return ERR(ESRCH);
        if (sig && lx_signal(t, (int)sig) != 0) return ERR(EPERM);
        return 0;
    }
    for (process_t *q = sched_first_process(); q; q = q->next_all) {
        uint64_t group = pid == 0 ? self->process_group_id : (uint64_t)(-pid);
        if (q->kind != PROCESS_USER || q->state == PROCESS_EXITED || q->state == PROCESS_REAPED) continue;
        if (pid == -1 ? (q == self || q->pid <= 1) : q->process_group_id != group) continue;
        if (!sig || lx_signal(q, (int)sig) == 0) sent++;
    }
    return sent ? 0 : ERR(ESRCH);
}

/* ---- fork / execve / wait ---------------------------------------------- */

extern void lx_resume_user(struct registers *frame);

static void lx_fork_child_start(void) {
    thread_t *t = sched_current_thread();
    process_t *p = sched_current_process();
    struct registers frame = *(struct registers *)t->lx_frame;
    kfree(t->lx_frame);
    t->lx_frame = 0;
    p->state = PROCESS_RUNNING;
    t->user_return_pending = 0;
    vmm_switch_address_space(p->addr_space);
    pf_set_current_as(p->addr_space);
    tss_set_rsp0(t->user_entry_stack_top ? t->user_entry_stack_top : t->kernel_stack_top);
    cpu_set_fs_base(t->fs_base);
    bkl_exit();
    lx_resume_user(&frame);
}

/* ---- threads: clone(CLONE_VM | CLONE_THREAD), futex, thread exit ---------
 * A thread is another thread_t in the same process: the address space, the
 * descriptors and the signal state are the process's.  Thread ids: the main
 * thread's is the pid (as in Linux), the others LX_TID_BASE + their kernel
 * thread id, which no pid reaches. */
#define ETIMEDOUT   110
#define LX_TID_BASE 0x100000

static int64_t lx_tid(const thread_t *t) {
    if (!t || !t->sibling) return t && t->owner ? (int64_t)t->owner->pid : 0;
    return LX_TID_BASE + (int64_t)t->tid;
}

/* tid -> thread of the current process (0 if none) */
static thread_t *lx_thread_of(process_t *p, int64_t tid) {
    thread_t *start = sched_current_thread(), *t = start;
    if (!start) return 0;
    do {
        if (t->owner == p && t->state != THREAD_ZOMBIE && lx_tid(t) == tid) return t;
        t = t->next;
    } while (t && t != start);
    return 0;
}

static uint64_t sys_clone_thread(struct registers *regs, uint64_t flags, uint64_t new_sp, uint64_t parent_tid,
                                 uint64_t child_tid, uint64_t tls) {
    process_t *p = sched_current_process();
    struct registers *frame;
    thread_t *t;
    int32_t tid;
    if (!(flags & 0x100) || !(flags & 0x800) || !new_sp) return ERR(EINVAL);  /* CLONE_VM, CLONE_SIGHAND */
    frame = (struct registers *)kmalloc(sizeof(struct registers));
    if (!frame) return ERR(ENOMEM);
    *frame = *regs;
    frame->rax = 0;
    frame->rsp = new_sp;
    t = proc_create_sibling_thread(p, frame->rip, frame->rsp, lx_fork_child_start);
    if (!t) {
        kfree(frame);
        return ERR(EAGAIN);
    }
    t->lx_frame = frame;
    t->fs_base = (flags & 0x80000) ? tls : cpu_fs_base();           /* CLONE_SETTLS */
    t->clear_tid = (flags & 0x200000) ? child_tid : 0;              /* CLONE_CHILD_CLEARTID */
    tid = (int32_t)lx_tid(t);
    if ((flags & 0x100000) && parent_tid) (void)copy_to_user((void *)parent_tid, &tid, 4);  /* CLONE_PARENT_SETTID */
    if ((flags & 0x1000000) && child_tid) (void)copy_to_user((void *)child_tid, &tid, 4);   /* CLONE_CHILD_SETTID */
    return (uint64_t)(int64_t)tid;
}

/* exit() of one thread while others go on; the last one ends the process */
static void lx_thread_exit(uint64_t code) {
    thread_t *t = sched_current_thread();
    process_t *p = sched_current_process();
    if (t->clear_tid) {
        int32_t zero = 0;
        (void)copy_to_user((void *)t->clear_tid, &zero, 4);
        (void)sched_futex_wake(t->clear_tid, 0x7FFFFFFF);
        t->clear_tid = 0;
    }
    if (p->nthreads <= 1) {
        user_request_exit_to_kernel(code);
        return;
    }
    p->nthreads--;
    t->state = THREAD_ZOMBIE;
    t->block_reason = THREAD_BLOCK_NONE;
    sched_yield();
    for (;;) __asm__ volatile("hlt");
}

/* timespec -> ticks from now (rounded up); abs: against the given clock */
static int64_t futex_ticks(uint64_t uts, int abs, int realtime, uint64_t *out) {
    uint64_t ts[2], now_ns, when_ns;
    if (copy_from_user(ts, (const void *)uts, sizeof(ts)) != 0) return ERR(EFAULT);
    if (ts[1] >= 1000000000ULL) return ERR(EINVAL);
    when_ns = ts[0] * 1000000000ULL + ts[1];
    if (abs) {
        uint64_t ticks = sched_ticks();
        now_ns = (realtime ? now_epoch() * 1000000000ULL : (ticks / 100) * 1000000000ULL) + (ticks % 100) * 10000000ULL;
        if (when_ns <= now_ns) return ERR(ETIMEDOUT);
        when_ns -= now_ns;
    }
    *out = (when_ns + 9999999ULL) / 10000000ULL;
    if (!*out) *out = 1;
    return 0;
}

static uint64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t utime, uint64_t uaddr2, uint64_t val3) {
    struct lx_state *s = lx_get(sched_current_process());
    int cmd = (int)(op & 0x7F), realtime = (op & 0x100) != 0;
    if (uaddr & 3) return ERR(EINVAL);
    switch (cmd) {
    case 0:     /* FUTEX_WAIT (relative timeout) */
    case 9: {   /* FUTEX_WAIT_BITSET (absolute timeout) */
        int32_t cur;
        uint64_t ticks = 0;
        if (cmd == 9 && !val3) return ERR(EINVAL);
        if (utime) {
            int64_t r = futex_ticks(utime, cmd == 9, realtime, &ticks);
            if (r < 0) return (uint64_t)r;
        }
        if (copy_from_user(&cur, (const void *)uaddr, 4) != 0) return ERR(EFAULT);
        if (cur != (int32_t)val) return ERR(EAGAIN);
        if (signal_pending(s)) return ERR(EINTR);
        if (sched_futex_wait(uaddr, ticks)) return 0;
        if (signal_pending(s)) return ERR(EINTR);
        return utime ? ERR(ETIMEDOUT) : 0;
    }
    case 1:     /* FUTEX_WAKE */
    case 10:    /* FUTEX_WAKE_BITSET */
        return (uint64_t)sched_futex_wake(uaddr, (int)(val > 0x7FFFFFFF ? 0x7FFFFFFF : val));
    case 3:     /* FUTEX_REQUEUE */
    case 4: {   /* FUTEX_CMP_REQUEUE */
        int n;
        if (cmd == 4) {
            int32_t cur;
            if (copy_from_user(&cur, (const void *)uaddr, 4) != 0) return ERR(EFAULT);
            if (cur != (int32_t)val3) return ERR(EAGAIN);
        }
        n = sched_futex_wake(uaddr, (int)(val > 0x7FFFFFFF ? 0x7FFFFFFF : val));
        return (uint64_t)(n + sched_futex_requeue(uaddr, uaddr2, (int)(utime > 0x7FFFFFFF ? 0x7FFFFFFF : utime)));
    }
    case 5: {   /* FUTEX_WAKE_OP: *uaddr2 op= arg, wake uaddr, and uaddr2 if the old value passes */
        int32_t old, arg = (int32_t)((val3 >> 12) & 0xFFF), cmparg = (int32_t)(val3 & 0xFFF), nv;
        int opc = (int)((val3 >> 28) & 7), cmp = (int)((val3 >> 24) & 15), n, ok;
        if (val3 & (1u << 31)) arg = 1 << (arg & 31);
        if (copy_from_user(&old, (const void *)uaddr2, 4) != 0) return ERR(EFAULT);
        nv = opc == 0 ? arg : opc == 1 ? old + arg : opc == 2 ? (old | arg) : opc == 3 ? (old & ~arg) : (old ^ arg);
        if (copy_to_user((void *)uaddr2, &nv, 4) != 0) return ERR(EFAULT);
        n = sched_futex_wake(uaddr, (int)val);
        ok = cmp == 0 ? old == cmparg : cmp == 1 ? old != cmparg : cmp == 2 ? old < cmparg :
             cmp == 3 ? old <= cmparg : cmp == 4 ? old > cmparg : old >= cmparg;
        if (ok) n += sched_futex_wake(uaddr2, (int)(utime > 0x7FFFFFFF ? 0x7FFFFFFF : utime));
        return (uint64_t)n;
    }
    default:
        return ERR(ENOSYS);  /* priority-inheritance futexes */
    }
}

static uint64_t sys_fork(struct registers *regs, uint64_t flags, uint64_t new_sp, uint64_t child_tid) {
    process_t *parent = sched_current_process();
    struct lx_state *ps = lx_get(parent), *cs;
    process_t *child;
    thread_t *thread;
    struct registers *frame;
    if ((flags & 0x100) && !(flags & 0x4000)) return ERR(ENOSYS); /* threads: CLONE_VM without CLONE_VFORK */
    child = proc_create_empty(PROCESS_USER);
    cs = (struct lx_state *)kmalloc(sizeof(struct lx_state));
    frame = (struct registers *)kmalloc(sizeof(struct registers));
    if (!child || !cs || !frame) return ERR(ENOMEM);
    child->addr_space = vmm_clone_user(parent->addr_space);
    if (!child->addr_space) return ERR(ENOMEM);
    kcopy(cs, ps, sizeof(*cs));
    cs->pending = 0;
    cs->term_sig = 0;
    cs->clear_tid = (flags & 0x00200000) ? child_tid : 0;
    for (int i = 0; i < LX_FD_MAX; i++) {
        if (cs->fd[i]) file_get(cs->fd[i]);
    }
    child->lx = cs;
    child->parent = parent;
    child->cwd = parent->cwd;
    child->linux_personality = 1;
    child->pty = parent->pty;
    child->linux_brk_pos = parent->linux_brk_pos;
    child->linux_mmap_next = parent->linux_mmap_next;
    if (lxvm_fork(parent, child) != 0) return ERR(ENOMEM);
    child->lx_phdr = parent->lx_phdr;
    child->lx_phnum = parent->lx_phnum;
    child->lx_entry = parent->lx_entry;
    child->ex_uid = parent->ex_uid;
    child->ex_token = parent->ex_token;
    child->session_id = parent->session_id;
    child->process_group_id = parent->process_group_id;
    kstrcpy(child->name, parent->name, sizeof(child->name));
    *frame = *regs;
    frame->rax = 0;
    if (new_sp) frame->rsp = new_sp;
    thread = proc_create_user_thread(child, frame->rip, frame->rsp, lx_fork_child_start);
    if (!thread) {
        kfree(frame);
        return ERR(ENOMEM);
    }
    thread->lx_frame = frame;
    thread->fs_base = cpu_fs_base();
    if ((flags & 0x01000000) && child_tid) { /* CLONE_CHILD_SETTID */
        uint64_t phys;
        (void)vmm_cow_break(child->addr_space, child_tid);
        phys = vmm_virt_to_phys(child->addr_space, child_tid);
        if (phys && (child_tid & 0xFFFULL) <= PAGE_SIZE_4K - 4) *(int32_t *)PHYS_TO_VIRT(phys) = (int32_t)child->pid;
    }
    return child->pid;
}

#define LX_ARG_BYTES (128 * 1024)
#define LX_ARGV_MAX  1024
#define LX_ENVP_MAX  256

static int64_t copy_strv(const uint64_t *uv, char **out, uint64_t max, char *buf, uint64_t *used) {
    uint64_t n = 0;
    if (!uv) {
        out[0] = 0;
        return 0;
    }
    for (;;) {
        uint64_t ptr, len;
        if (copy_from_user(&ptr, uv + n, 8) != 0) return -EFAULT;
        if (!ptr) break;
        if (n >= max) return -7;
        len = strnlen_user((const char *)ptr, LX_ARG_BYTES);
        if (len == (uint64_t)-1) return -EFAULT;
        if (*used + len + 1 > LX_ARG_BYTES) return -7;
        if (copy_from_user(buf + *used, (const void *)ptr, len + 1) != 0) return -EFAULT;
        buf[*used + len] = 0;
        out[n++] = buf + *used;
        *used += len + 1;
    }
    out[n] = 0;
    return (int64_t)n;
}

static uint64_t sys_execve(struct registers *regs, const char *upath, const uint64_t *uargv, const uint64_t *uenvp) {
    process_t *p = sched_current_process();
    thread_t *t = sched_current_thread();
    struct lx_state *s = lx_get(p);
    char *path = (char *)kmalloc(LX_PATH_MAX);
    char *buf = (char *)kmalloc(LX_ARG_BYTES + 1024);
    char **argv = (char **)kmalloc(sizeof(char *) * (LX_ARGV_MAX + 4));
    char **envp = (char **)kmalloc(sizeof(char *) * (LX_ENVP_MAX + 1));
    uint64_t used = 0, size = 0, rip = 0, rsp = 0;
    int64_t argc, envc, rc = 0;
    const char *image = 0;
    addr_space_t *old_as;
    int old_pers;
    void *old_vmas;
    if (!path || !buf || !argv || !envp) {
        rc = -ENOMEM;
        goto out;
    }
    rc = at_path(LX_AT_FDCWD, upath, path);
    if (rc < 0) goto out;
    argc = copy_strv(uargv, argv, LX_ARGV_MAX, buf, &used);
    if (argc < 0) {
        rc = argc;
        goto out;
    }
    envc = copy_strv(uenvp, envp, LX_ENVP_MAX, buf, &used);
    if (envc < 0) {
        rc = envc;
        goto out;
    }
    if (argc == 0) {
        argv[0] = path;
        argv[1] = 0;
        argc = 1;
    }
    for (int depth = 0;; depth++) {
        vfs_node_t *node = vfs_resolve(cwd_node(), path);
        if (!node) {
            rc = -ENOENT;
            goto out;
        }
        if (vfs_node_type(node) == VFS_NODE_DIR) {
            rc = -EACCES;
            goto out;
        }
        image = vfs_read(cwd_node(), path, &size);
        if (!image || size < 4) {
            rc = -ENOEXEC;
            goto out;
        }
        if (image[0] == '#' && image[1] == '!' && depth < 4) {
            char interp[128], arg[128];
            uint64_t i = 2, n = 0, m = 0, shift;
            while (i < size && (image[i] == ' ' || image[i] == '\t')) i++;
            while (i < size && image[i] != ' ' && image[i] != '\t' && image[i] != '\n' && n < sizeof(interp) - 1) {
                interp[n++] = image[i++];
            }
            interp[n] = 0;
            while (i < size && (image[i] == ' ' || image[i] == '\t')) i++;
            while (i < size && image[i] != '\n' && m < sizeof(arg) - 1) arg[m++] = image[i++];
            while (m > 0 && (arg[m - 1] == ' ' || arg[m - 1] == '\t' || arg[m - 1] == '\r')) m--;
            arg[m] = 0;
            if (!n) {
                rc = -ENOEXEC;
                goto out;
            }
            shift = m ? 2 : 1;
            if ((uint64_t)argc + shift > LX_ARGV_MAX || used + n + m + kstrlen(path) + 3 > LX_ARG_BYTES + 1024) {
                rc = -7;
                goto out;
            }
            for (int64_t k = argc; k >= 1; k--) argv[k + (int64_t)shift] = argv[k];
            kcopy(buf + used, path, kstrlen(path) + 1);
            argv[shift] = buf + used;
            used += kstrlen(path) + 1;
            kcopy(buf + used, interp, n + 1);
            argv[0] = buf + used;
            used += n + 1;
            if (m) {
                kcopy(buf + used, arg, m + 1);
                argv[1] = buf + used;
                used += m + 1;
            }
            argc += (int64_t)shift;
            kstrcpy(path, interp, LX_PATH_MAX);
            continue;
        }
        break;
    }
    old_as = p->addr_space;
    old_pers = p->linux_personality;
    old_vmas = p->lx_vmas;
    p->lx_vmas = 0;
    p->linux_personality = !(size > 7 && (uint8_t)image[7] == 0xFF);
    if (user_exec_image(p, image, size, (uint64_t)argc, argv, (uint64_t)envc, envp, path, &rip, &rsp) != 0) {
        if (p->addr_space && p->addr_space != old_as) vmm_destroy_address_space(p->addr_space);
        p->addr_space = old_as;
        p->linux_personality = old_pers;
        lxvm_free(p);
        p->lx_vmas = old_vmas;
        rc = -ENOEXEC;
        goto out;
    }
    sched_stop_threads(p, t);        /* the other threads end; this one becomes the process */
    p->main_thread = t;
    t->sibling = 0;
    t->clear_tid = 0;
    vmm_switch_address_space(p->addr_space);
    pf_set_current_as(p->addr_space);
    if (old_as) vmm_destroy_address_space(old_as);
    p->linux_brk_pos = 0;
    p->linux_mmap_next = 0;
    lxvm_free_list(old_vmas);
    {
        const char *base = path;
        for (const char *c = path; *c; c++) {
            if (*c == '/') base = c + 1;
        }
        kstrcpy(p->name, base, sizeof(p->name));
    }
    kstrcpy(s->exe, path, sizeof(s->exe));
    for (int i = 0; i < LX_FD_MAX; i++) {
        if (s->fd[i] && s->cloexec[i]) fd_close(s, i);
    }
    for (int i = 1; i < LX_NSIG; i++) {
        if (s->sa[i].handler > 1) s->sa[i].handler = 0;
        s->sa[i].flags = 0;
    }
    s->clear_tid = 0;
    t->fs_base = 0;
    cpu_set_fs_base(0);
    kzero(regs, sizeof(*regs) - 5 * 8);
    regs->rip = rip;
    regs->rsp = rsp;
    regs->rflags = 0x202;
    rc = 0;
out:
    if (path) kfree(path);
    if (buf) kfree(buf);
    if (argv) kfree(argv);
    if (envp) kfree(envp);
    return (uint64_t)rc;
}

static uint64_t sys_wait4(int64_t pid, int32_t *ustatus, uint64_t options) {
    process_t *self = sched_current_process();
    struct lx_state *s = lx_get(self);
    for (;;) {
        int found = 0;
        for (process_t *q = sched_first_process(); q; q = q->next_all) {
            if (q->parent != self || q->state == PROCESS_REAPED) continue;
            if (pid > 0 && q->pid != (uint64_t)pid) continue;
            if (pid == 0 && q->process_group_id != self->process_group_id) continue;
            if (pid < -1 && q->process_group_id != (uint64_t)(-pid)) continue;
            found = 1;
            if (q->state == PROCESS_EXITED) {
                int32_t status;
                uint64_t cpid = q->pid;
                if (q->lx && q->lx->term_sig) status = q->lx->term_sig & 0x7F;
                else status = (int32_t)((q->exit_code & 0xFF) << 8);
                if (ustatus && copy_to_user(ustatus, &status, 4) != 0) return ERR(EFAULT);
                q->state = PROCESS_REAPED;
                if (q->lx) {
                    kfree(q->lx);
                    q->lx = 0;
                }
                return cpid;
            }
        }
        if (!found) return ERR(ECHILD);
        if (options & 1) return 0;
        if (signal_pending(s)) return ERR(EINTR);
        sched_sleep(1);
    }
}

static uint64_t sys_setpgid(int64_t pid, int64_t pgid) {
    process_t *self = sched_current_process();
    process_t *t = pid ? sched_find_process((uint64_t)pid) : self;
    if (!t) return ERR(ESRCH);
    t->process_group_id = pgid ? (uint64_t)pgid : t->pid;
    return 0;
}

/* ---- dispatch ----------------------------------------------------------- */

uint64_t lx_syscall(struct registers *regs) {
    process_t *p = sched_current_process();
    uint64_t nr = regs->rax;
    uint64_t a0 = regs->rdi, a1 = regs->rsi, a2 = regs->rdx;
    uint64_t a3 = regs->r10, a4 = regs->r8, a5 = regs->r9;
    if (!p || !lx_get(p)) return ERR(ENOMEM);
    cur_nr = nr;
    switch (nr) {
        case 0: return sys_rw((int64_t)a0, (char *)a1, a2, 0);
        case 1: return sys_rw((int64_t)a0, (char *)a1, a2, 1);
        case 2: return sys_openat(LX_AT_FDCWD, (const char *)a0, a1);
        case 3: return fd_close(p->lx, (int64_t)a0) == 0 ? 0 : ERR(EBADF);
        case 4: case 6: return sys_newfstatat(LX_AT_FDCWD, (const char *)a0, (lx_stat_t *)a1, 0);
        case 5: return sys_fstat((int64_t)a0, (lx_stat_t *)a1);
        case 7: return do_poll((uint8_t *)a0, a1, (int64_t)(int32_t)a2 < 0 ? -1 : (int64_t)((a2 + 9) / 10));
        case 8: return sys_lseek((int64_t)a0, (int64_t)a1, a2);
        case 9: return sys_mmap(a0, a1, a2, a3, (int64_t)(int32_t)a4, a5);
        case 10: return sys_mprotect(a0, a1, a2);
        case 11: return sys_munmap(a0, a1);
        case 12: return sys_brk(a0);
        case 13: return sys_rt_sigaction(a0, (const lx_sigaction_t *)a1, (lx_sigaction_t *)a2);
        case 14: return sys_rt_sigprocmask(a0, (const uint64_t *)a1, (uint64_t *)a2);
        case 15: return sys_rt_sigreturn(regs);
        case 16: return sys_ioctl((int64_t)a0, a1, a2);
        case 17: return sys_prw((int64_t)a0, (char *)a1, a2, a3, 0);
        case 18: return sys_prw((int64_t)a0, (char *)a1, a2, a3, 1);
        case 19: return sys_rwv((int64_t)a0, (const uint64_t *)a1, a2, 0);
        case 20: return sys_rwv((int64_t)a0, (const uint64_t *)a1, a2, 1);
        case 21: return sys_faccessat(LX_AT_FDCWD, (const char *)a0, a1);
        case 22: return sys_pipe2((int32_t *)a0, 0);
        case 23: return do_select(a0, (uint8_t *)a1, (uint8_t *)a2, (uint8_t *)a3, ts_ticks((const uint64_t *)a4, 1));
        case 24: sched_yield(); return 0;
        case 25: return lx_res(lxvm_mremap(p, a0, a1, a2, (uint32_t)a3, a4));
        case 28: return lx_res(lxvm_madvise(p, a0, a1, (int)a2));
        case 32: return sys_fcntl((int64_t)a0, 0, 0);
        case 33: return sys_dup3((int64_t)a0, (int64_t)a1, 0, 1);
        case 34: while (!signal_pending(p->lx)) sched_sleep(1); return ERR(EINTR);
        case 35: return sys_nanosleep((const uint64_t *)a0);
        case 37: return 0;
        case 39: return p->pid;
        case 56:
            if (a0 & 0x10000) return sys_clone_thread(regs, a0, a1, a2, a3, a4);   /* CLONE_THREAD */
            return sys_fork(regs, a0, a1, a3);
        case 57: case 58: return sys_fork(regs, 0, 0, 0);
        case 59: return sys_execve(regs, (const char *)a0, (const uint64_t *)a1, (const uint64_t *)a2);
        case 60: lx_thread_exit(a0 & 0xFF); return 0;
        case 231:
            user_request_exit_to_kernel(a0 & 0xFF);
            return 0;
        case 61: return sys_wait4((int64_t)(int32_t)a0, (int32_t *)a1, a2);
        case 62: return sys_kill((int64_t)(int32_t)a0, (int64_t)a1);
        case 63: return sys_uname((char *)a0);
        case 72: return sys_fcntl((int64_t)a0, a1, a2);
        case 73: case 74: case 75: case 162: return 0;
        case 76: return sys_truncate((const char *)a0, a1);
        case 77: return sys_ftruncate((int64_t)a0, a1);
        case 79: return sys_getcwd((char *)a0, a1);
        case 80: return sys_chdir((const char *)a0);
        case 81: return sys_fchdir((int64_t)a0);
        case 82: return sys_renameat(LX_AT_FDCWD, (const char *)a0, LX_AT_FDCWD, (const char *)a1);
        case 83: return sys_mkdirat(LX_AT_FDCWD, (const char *)a0);
        case 84: return sys_unlinkat(LX_AT_FDCWD, (const char *)a0, 0x200);
        case 85: return sys_openat(LX_AT_FDCWD, (const char *)a0, O_CREAT | O_WRONLY | O_TRUNC);
        case 86: case 88: case 133: return ERR(EPERM);
        case 87: return sys_unlinkat(LX_AT_FDCWD, (const char *)a0, 0);
        case 89: return sys_readlinkat(LX_AT_FDCWD, (const char *)a0, (char *)a1, a2);
        case 90: case 91: case 92: case 93: case 94: return 0;
        case 95: { uint64_t old = p->lx->umask; p->lx->umask = (uint32_t)(a0 & 0777); return old; }
        case 96: {
            uint64_t tv[2] = { now_epoch(), (sched_ticks() % 100) * 10000 };
            if (a0 && copy_to_user((void *)a0, tv, sizeof(tv)) != 0) return ERR(EFAULT);
            return 0;
        }
        case 97: return sys_prlimit(a0, 0, (uint64_t *)a1);
        case 98: return zero_user((void *)a1, 144);
        case 99: return sys_sysinfo((uint8_t *)a0);
        case 100: {
            uint64_t tms[4] = { p->cpu_ticks, 0, 0, 0 };
            if (a0 && copy_to_user((void *)a0, tms, sizeof(tms)) != 0) return ERR(EFAULT);
            return sched_ticks();
        }
        case 102: case 104: case 107: case 108: return 0;
        case 105: case 106: case 113: case 114: case 117: case 119: return 0;
        case 109: return sys_setpgid((int64_t)a0, (int64_t)a1);
        case 110: return p->parent ? p->parent->pid : 1;
        case 111: return p->process_group_id ? p->process_group_id : p->pid;
        case 112: p->session_id = p->pid; p->process_group_id = p->pid; return p->pid;
        case 115: return 0;
        case 118: case 120: {
            uint32_t ids[3] = { 0, 0, 0 };
            if (copy_to_user((void *)a0, ids, 4) || copy_to_user((void *)a1, ids, 4) || copy_to_user((void *)a2, ids, 4)) {
                return ERR(EFAULT);
            }
            return 0;
        }
        case 121: {
            process_t *t = a0 ? sched_find_process(a0) : p;
            if (!t) return ERR(ESRCH);
            return t->process_group_id ? t->process_group_id : t->pid;
        }
        case 124: return p->session_id ? p->session_id : p->pid;
        case 127: return 0;
        case 130: while (!signal_pending(p->lx)) sched_sleep(1); return ERR(EINTR);
        case 131: return a1 ? zero_user((void *)a1, 24) : 0;
        case 137: case 138: return sys_statfs((uint8_t *)a1);
        case 157: return 0;
        case 158:
            if (a0 == 0x1002) {
                sched_current_thread()->fs_base = a1;
                cpu_set_fs_base(a1);
                return 0;
            }
            if (a0 == 0x1003) {
                uint64_t base = cpu_fs_base();
                return copy_to_user((void *)a1, &base, 8) ? ERR(EFAULT) : 0;
            }
            return ERR(EINVAL);
        case 160: return 0;
        case 186: return (uint64_t)lx_tid(sched_current_thread());
        case 200: return sys_kill(lx_thread_of(p, (int64_t)a0) ? (int64_t)p->pid : (int64_t)a0, (int64_t)a1);
        case 201: {
            uint64_t now = now_epoch();
            if (a0 && copy_to_user((void *)a0, &now, 8) != 0) return ERR(EFAULT);
            return now;
        }
        case 202: return sys_futex(a0, a1, a2, a3, a4, a5);
        case 217: return sys_getdents64((int64_t)a0, (uint8_t *)a1, a2);
        case 218: sched_current_thread()->clear_tid = a0; return (uint64_t)lx_tid(sched_current_thread());
        case 228: return sys_clock_gettime(a0, (uint64_t *)a1);
        case 229: {
            uint64_t res[2] = { 0, 10000000ULL };
            if (a1 && copy_to_user((void *)a1, res, sizeof(res)) != 0) return ERR(EFAULT);
            return 0;
        }
        case 230: return sys_clock_nanosleep(a0, a1, (const uint64_t *)a2);
        case 234: return sys_kill((int64_t)a0 == (int64_t)p->pid ? (int64_t)p->pid : (int64_t)a1, (int64_t)a2);
        case 257: return sys_openat((int64_t)(int32_t)a0, (const char *)a1, a2);
        case 258: return sys_mkdirat((int64_t)(int32_t)a0, (const char *)a1);
        case 260: case 268: case 280: return 0;
        case 262: return sys_newfstatat((int64_t)(int32_t)a0, (const char *)a1, (lx_stat_t *)a2, a3);
        case 263: return sys_unlinkat((int64_t)(int32_t)a0, (const char *)a1, a2);
        case 264: case 316:
            return sys_renameat((int64_t)(int32_t)a0, (const char *)a1, (int64_t)(int32_t)a2, (const char *)a3);
        case 267: return sys_readlinkat((int64_t)(int32_t)a0, (const char *)a1, (char *)a2, a3);
        case 269: case 439: return sys_faccessat((int64_t)(int32_t)a0, (const char *)a1, a2);
        case 270: return do_select(a0, (uint8_t *)a1, (uint8_t *)a2, (uint8_t *)a3, ts_ticks((const uint64_t *)a4, 0));
        case 271: return do_poll((uint8_t *)a0, a1, ts_ticks((const uint64_t *)a2, 0));
        case 273: return 0;
        case 292: return sys_dup3((int64_t)a0, (int64_t)a1, a2, 0);
        case 293: return sys_pipe2((int32_t *)a0, a1);
        case 302: return sys_prlimit(a1, (const uint64_t *)a2, (uint64_t *)a3);
        case 318: return sys_getrandom((uint8_t *)a0, a1);
        case 40: case 332: case 334: case 435: return ERR(ENOSYS);
        default:
            log_hex("lx: unimplemented syscall ", nr);
            return ERR(ENOSYS);
    }
}

/* ---- boot-time setup ---------------------------------------------------- */

static const char *const bb_applets[] = {
    "sh", "ash", "awk", "base64", "basename", "bc", "cal", "cat", "chmod", "clear", "cmp", "cp", "cut", "date",
    "dc", "dd", "df", "diff", "dirname", "du", "echo", "ed", "egrep", "env", "expand", "expr", "factor", "false",
    "fgrep", "find", "fold", "free", "grep", "groups", "gunzip", "gzip", "head", "hexdump", "hostname", "id",
    "kill", "killall", "less", "ln", "ls", "md5sum", "mkdir", "mktemp", "more", "mv", "nl", "nproc", "od",
    "paste", "patch", "pidof", "printf", "ps", "pwd", "readlink", "realpath", "reset", "rev", "rm", "rmdir",
    "sed", "seq", "sha1sum", "sha256sum", "sha512sum", "shuf", "sleep", "sort", "split", "stat", "strings",
    "stty", "sync", "tac", "tail", "tar", "tee", "test", "time", "timeout", "top", "touch", "tr", "true",
    "truncate", "tty", "uname", "uniq", "unzip", "uptime", "usleep", "vi", "watch", "wc", "which", "whoami",
    "xargs", "xxd", "yes", "zcat", "[", "[["
};

void lx_init(void) {
    static const char stub[] = "#!/bin/busybox\n";
    char path[64];
    vfs_node_t *root = vfs_root();
    if (!vfs_resolve(root, "/bin/busybox")) return;
    for (uint64_t i = 0; i < sizeof(bb_applets) / sizeof(bb_applets[0]); i++) {
        kstrcpy(path, "/bin/", sizeof(path));
        kstrcpy(path + 5, bb_applets[i], sizeof(path) - 5);
        if (!vfs_resolve(root, path)) vfs_seed_readonly(path, stub, sizeof(stub) - 1);
    }
    if (!vfs_resolve(root, "/etc/passwd")) {
        static const char pw[] = "root:x:0:0:root:/home:/bin/sh\n";
        vfs_seed_readonly("/etc/passwd", pw, sizeof(pw) - 1);
    }
    if (!vfs_resolve(root, "/etc/group")) {
        static const char gr[] = "root:x:0:\n";
        vfs_seed_readonly("/etc/group", gr, sizeof(gr) - 1);
    }
}

/* Records that proc died from sig, for wait4 status. */
void lx_mark_signaled(process_t *proc, int sig) {
    if (proc && proc->lx) proc->lx->term_sig = sig;
}
