#ifndef LX_INTERNAL_H
#define LX_INTERNAL_H

/* What the Linux personality's modules share: open file descriptions and
 * the calls lx.c offers the others (lx_ipc.c: sockets, eventfd, epoll,
 * timerfd, memfd). */
#include <stdint.h>
#include "../fs/vfs.h"

enum { LXF_TTY = 1, LXF_VFS, LXF_PIPE, LXF_NULL, LXF_ZERO, LXF_RANDOM, LXF_MEM, LXF_PROCDIR, LXF_OBJ };

typedef struct lx_pipe {
    char    *buf;
    uint32_t head;
    uint32_t len;
    int      readers;
    int      writers;
} lx_pipe_t;

struct lx_file;

/* A file kind implemented outside lx.c (LXF_OBJ).  read / write take user
 * buffers; results are byte counts or minus a Linux errno. */
typedef struct lx_fops {
    const char *name;                       /* for /proc/self/fd links: "socket", "anon_inode:[eventfd]" ... */
    int64_t (*read)(struct lx_file *f, char *ubuf, uint64_t count);
    int64_t (*write)(struct lx_file *f, const char *ubuf, uint64_t count);
    int     (*ready)(struct lx_file *f, int want_write);     /* poll bits: 1 in, 4 out, 8 err, 0x10 hup */
    void    (*release)(void *obj);                            /* the last reference went */
    uint64_t (*size)(void *obj);                              /* seekable objects (memfd), 0 if none */
    int64_t (*truncate)(void *obj, uint64_t len);
    uint32_t mode;                                            /* st_mode type bits */
    /* memory objects that mmap shares: the frame for page offset off,
     * allocated (zeroed) on first use; 0 if beyond the object */
    uint64_t (*frame)(void *obj, uint64_t off);
} lx_fops_t;

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
    const lx_fops_t *ops;                   /* LXF_OBJ */
    void       *obj;
} lx_file_t;

#define LX_O_NONBLOCK 04000
#define LX_O_CLOEXEC  02000000

/* lx.c offers */
lx_file_t *lxi_file_new(const lx_fops_t *ops, void *obj, uint64_t flags);   /* one reference */
void       lxi_file_ref(lx_file_t *f);
void       lxi_file_unref(lx_file_t *f);
lx_file_t *lxi_fd_file(int64_t fd);                          /* current process; 0 if not open */
int64_t    lxi_fd_install(lx_file_t *f, int cloexec);        /* takes the reference; fd or -errno */
int        lxi_file_ready(lx_file_t *f, int want_write);
int        lxi_interrupted(void);                            /* a signal is waiting */
uint64_t   lxi_now_ns(void);                                 /* monotonic */
uint64_t   lxi_realtime_ns(void);

/* lx_ipc.c offers: the syscalls (Linux numbers' arguments, results as
 * syscalls return them: value or minus errno) */
int64_t lxi_socket(int domain, int type, int proto);
int64_t lxi_socketpair(int domain, int type, int proto, int32_t *usv);
int64_t lxi_bind(int64_t fd, const void *uaddr, uint64_t len);
int64_t lxi_listen(int64_t fd, int backlog);
int64_t lxi_connect(int64_t fd, const void *uaddr, uint64_t len);
int64_t lxi_accept(int64_t fd, void *uaddr, uint32_t *ulen, int flags);
int64_t lxi_sendmsg(int64_t fd, const void *umsg, int flags);
int64_t lxi_recvmsg(int64_t fd, void *umsg, int flags);
int64_t lxi_sendto(int64_t fd, const void *ubuf, uint64_t len, int flags, const void *uaddr, uint64_t alen);
int64_t lxi_recvfrom(int64_t fd, void *ubuf, uint64_t len, int flags, void *uaddr, uint32_t *ualen);
int64_t lxi_shutdown(int64_t fd, int how);
int64_t lxi_getsockopt(int64_t fd, int level, int opt, void *uval, uint32_t *ulen);
int64_t lxi_setsockopt(int64_t fd, int level, int opt, const void *uval, uint64_t len);
int64_t lxi_getsockname(int64_t fd, void *uaddr, uint32_t *ulen, int peer);
int64_t lxi_eventfd(uint64_t initval, int flags);
int64_t lxi_epoll_create(int flags);
int64_t lxi_epoll_ctl(int64_t epfd, int op, int64_t fd, const void *uev);
int64_t lxi_epoll_wait(int64_t epfd, void *uevs, int maxevents, int64_t timeout_ms);
int64_t lxi_timerfd_create(int clockid, int flags);
int64_t lxi_timerfd_settime(int64_t fd, int flags, const void *unew, void *uold);
int64_t lxi_timerfd_gettime(int64_t fd, void *ucur);
int64_t lxi_memfd_create(const char *uname, unsigned flags);
lx_file_t *lxi_shared_anon(uint64_t len);     /* MAP_SHARED | MAP_ANONYMOUS: an unnamed memory file (one reference) */

#endif
