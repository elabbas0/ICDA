#ifndef LX_H
#define LX_H

#include <stdint.h>
#include "../proc/process.h"

struct registers;

/* Linux personality: syscalls, open-file table, pipes, tty line discipline,
 * fork/execve/wait4 and signals for unmodified static x86-64 binaries. */
uint64_t lx_syscall(struct registers *regs);
void     lx_after_syscall(struct registers *regs);
void     lx_proc_exit(process_t *proc);
int      lx_signal(process_t *proc, int sig);
void     lx_mark_signaled(process_t *proc, int sig);
void     lx_init(void);
/* a path ICDA lacks, redirected into the Linux root if it is there (1) */
int      lx_overlay_path(char *path);

#endif
