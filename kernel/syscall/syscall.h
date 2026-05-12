#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "../cpu/isr.h"

#define SYSCALL_VECTOR 0x80

typedef enum {
    SYS_CONSOLE_WRITE = 0,
    SYS_GET_PID       = 1,
    SYS_VFS_READ      = 2,
    SYS_VFS_WRITE     = 3,
    SYS_EXIT          = 4,
    SYS_INPUT_READ    = 5,
    SYS_GETCWD        = 6,
    SYS_CHDIR         = 7,
    SYS_LIST_DIR      = 8,
    SYS_EXEC          = 9,
    SYS_CONSOLE_CLEAR = 10,
    SYS_CONSOLE_BACKSPACE = 11,
    SYS_MKDIR         = 12,
    SYS_CREATE        = 13,
    SYS_STAT          = 14,
    SYS_LIST_PROCS    = 15,
    SYS_SPAWN         = 16,
    SYS_WAITPID       = 17,
    SYS_YIELD         = 18,
    SYS_SLEEP         = 19,
    SYS_PROC_INFO     = 20,
    SYS_KILL          = 21,
    SYS_SUSPEND       = 22,
    SYS_RESUME        = 23,
    SYS_INPUT_READLINE = 24,
    SYS_SYNC          = 25
} syscall_number_t;

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    uint64_t sid;
    uint64_t pgid;
    uint64_t kind;
    uint64_t state;
    uint64_t exit_code;
} syscall_proc_info_t;

void syscall_init(void);
uint64_t syscall_dispatch(struct registers *regs);

uint64_t syscall_kernel_write(const char *text);
uint64_t syscall_kernel_get_pid(void);

#endif
