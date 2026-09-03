#ifndef FD_H
#define FD_H

/*
 * Real file-descriptor table for the Linux syscall personality (P0/B2).
 *
 * Replaces the old global `static int next_fd` counter (which handed out
 * numbers backed by nothing while read/write ignored the fd entirely).
 *
 * Model: fds 0/1/2 are the console stdio trio (stdin reads /dev/stdin,
 * stdout/stderr write the console), always valid, never stored.
 * fds 3..FD_TABLE_SIZE-1 map to VFS nodes with an offset and open flags.
 * VFS nodes are never freed by the VFS itself, so entries only need
 * clearing on process exit (fd_proc_exit) — no refcounting.
 */

#include <stdint.h>

#include "../proc/process.h"

struct vfs_node;

/* open(2) flag subset we honor (Linux values). */
#define FD_O_RDONLY  0
#define FD_O_WRONLY  1
#define FD_O_RDWR    2
#define FD_O_ACCMODE 3
#define FD_O_CREAT   0100
#define FD_O_TRUNC   01000
#define FD_O_APPEND  02000

/* Lazily initializes the table (idempotent, safe on zeroed procs). */
void fd_table_ensure(process_t *proc);

/* Resolve `kpath` (already validated kernel-side string) under `cwd`
 * and allocate an fd >= 3. Returns the fd, or a negative -U_Exxx. */
int fd_open_path(process_t *proc, struct vfs_node *cwd, const char *kpath,
                 uint64_t flags);

/* Look up an fd. Stdio (0..2) returns 0 with *is_stdio = 1 and
 * *node_out untouched; a live entry returns 0 with node+offset;
 * anything else returns -U_EBADF. */
int fd_resolve(process_t *proc, int fd, struct vfs_node **node_out,
               uint64_t *off_out, int *is_stdio);

/* Update the offset of a live non-stdio fd. Returns 0 or -U_EBADF. */
int fd_set_off(process_t *proc, int fd, uint64_t off);

/* Stored open flags of a live non-stdio fd (FD_O_* above). */
uint64_t fd_get_flags(process_t *proc, int fd);

/* Close one fd. Stdio or unused fds return -U_EBADF. */
int fd_close(process_t *proc, int fd);

/* Drop every entry. Called on every process-exit path; VFS nodes
 * themselves are owned by the VFS and need no release. */
void fd_proc_exit(process_t *proc);

#endif
