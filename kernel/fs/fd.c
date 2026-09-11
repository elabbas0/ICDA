#include "fd.h"

#include "../syscall/syscall.h"
#include "vfs.h"

void fd_table_ensure(process_t *proc) {
    int i;

    if (!proc || proc->fds_inited) {
        return;
    }
    for (i = 0; i < FD_TABLE_SIZE; i++) {
        proc->fd_nodes[i] = 0;
        proc->fd_off[i] = 0;
        proc->fd_flags[i] = 0;
        proc->fd_used[i] = 0;
    }
    proc->fds_inited = 1;
}

int fd_open_path(process_t *proc, struct vfs_node *cwd, const char *kpath,
                 uint64_t flags) {
    struct vfs_node *node;
    int fd;
    int i;

    if (!proc || !kpath || !*kpath) {
        return -U_EINVAL;
    }
    fd_table_ensure(proc);
    node = vfs_resolve(cwd, kpath);
    if (!node) {
        if (!(flags & FD_O_CREAT)) {
            return -U_ENOENT;
        }
        if (vfs_create(cwd, kpath) != 0) {
            return -U_ENOENT;
        }
        node = vfs_resolve(cwd, kpath);
        if (!node) {
            return -U_ENOENT;
        }
    }
    for (i = 3; i < FD_TABLE_SIZE; i++) {
        if (!proc->fd_used[i]) {
            break;
        }
    }
    if (i == FD_TABLE_SIZE) {
        return -1;
    }
    fd = i;
    proc->fd_nodes[fd] = node;
    proc->fd_flags[fd] = flags;
    proc->fd_off[fd] = 0;
    if ((flags & FD_O_APPEND) && vfs_node_type(node) == VFS_NODE_FILE) {
        proc->fd_off[fd] = vfs_node_size(node);
    }
    if ((flags & FD_O_TRUNC) && vfs_node_type(node) == VFS_NODE_FILE &&
        !vfs_node_readonly(node)) {
        if (vfs_node_write_at(node, 0, "", 0) != 0) {
            return -1;
        }
    }
    proc->fd_used[fd] = 1;
    return fd;
}

int fd_resolve(process_t *proc, int fd, struct vfs_node **node_out,
               uint64_t *off_out, int *is_stdio) {
    if (!proc) {
        return -U_EBADF;
    }
    fd_table_ensure(proc);
    if (fd < 0 || fd >= FD_TABLE_SIZE) {
        return -U_EBADF;
    }
    if (fd <= 2) {
        if (is_stdio) {
            *is_stdio = 1;
        }
        return 0;
    }
    if (!proc->fd_used[fd] || !proc->fd_nodes[fd]) {
        return -U_EBADF;
    }
    if (node_out) {
        *node_out = proc->fd_nodes[fd];
    }
    if (off_out) {
        *off_out = proc->fd_off[fd];
    }
    if (is_stdio) {
        *is_stdio = 0;
    }
    return 0;
}

int fd_set_off(process_t *proc, int fd, uint64_t off) {
    if (!proc) {
        return -U_EBADF;
    }
    fd_table_ensure(proc);
    if (fd < 3 || fd >= FD_TABLE_SIZE || !proc->fd_used[fd]) {
        return -U_EBADF;
    }
    proc->fd_off[fd] = off;
    return 0;
}

uint64_t fd_get_flags(process_t *proc, int fd) {
    if (!proc || fd < 0 || fd >= FD_TABLE_SIZE || !proc->fd_used[fd]) {
        return 0;
    }
    return proc->fd_flags[fd];
}

int fd_close(process_t *proc, int fd) {
    if (!proc) {
        return -U_EBADF;
    }
    fd_table_ensure(proc);
    if (fd < 3 || fd >= FD_TABLE_SIZE || !proc->fd_used[fd]) {
        return -U_EBADF;
    }
    proc->fd_used[fd] = 0;
    proc->fd_nodes[fd] = 0;
    proc->fd_off[fd] = 0;
    proc->fd_flags[fd] = 0;
    return 0;
}

void fd_proc_exit(process_t *proc) {
    int i;

    if (!proc || !proc->fds_inited) {
        return;
    }
    for (i = 0; i < FD_TABLE_SIZE; i++) {
        proc->fd_used[i] = 0;
        proc->fd_nodes[i] = 0;
        proc->fd_off[i] = 0;
        proc->fd_flags[i] = 0;
    }
}
