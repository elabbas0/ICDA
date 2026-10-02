#ifndef FD_H
#define FD_H














#include <stdint.h>

#include "../proc/process.h"

struct vfs_node;


#define FD_O_RDONLY  0
#define FD_O_WRONLY  1
#define FD_O_RDWR    2
#define FD_O_ACCMODE 3
#define FD_O_CREAT   0100
#define FD_O_TRUNC   01000
#define FD_O_APPEND  02000


void fd_table_ensure(process_t *proc);



int fd_open_path(process_t *proc, struct vfs_node *cwd, const char *kpath,
                 uint64_t flags);




int fd_resolve(process_t *proc, int fd, struct vfs_node **node_out,
               uint64_t *off_out, int *is_stdio);


int fd_set_off(process_t *proc, int fd, uint64_t off);


uint64_t fd_get_flags(process_t *proc, int fd);


int fd_close(process_t *proc, int fd);



void fd_proc_exit(process_t *proc);

#endif
