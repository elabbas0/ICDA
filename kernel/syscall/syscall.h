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
    SYS_STAT          = 14
} syscall_number_t;

void syscall_init(void);
uint64_t syscall_dispatch(struct registers *regs);

uint64_t syscall_kernel_write(const char *text);
uint64_t syscall_kernel_get_pid(void);

#endif
