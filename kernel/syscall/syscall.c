#include "syscall.h"

#include "../drivers/console/console.h"
#include "../fs/vfs.h"
#include "../proc/sched.h"

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) {
        len++;
    }
    return len;
}

static void copy_bytes(char *dst, const char *src, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static uint64_t sys_console_write(const char *text) {
    if (!text) {
        return (uint64_t)-1;
    }
    console_write(text, CONSOLE_STYLE_INFO);
    return str_len(text);
}

static uint64_t sys_get_pid(void) {
    process_t *proc = sched_current_process();
    return proc ? proc->pid : 0;
}

static uint64_t sys_vfs_read(const char *path, char *buf, uint64_t cap) {
    process_t *proc = sched_current_process();
    uint64_t size = 0;
    const char *data;

    if (!proc || !path || !buf || cap == 0) {
        return (uint64_t)-1;
    }

    data = vfs_read(proc->cwd ? proc->cwd : vfs_root(), path, &size);
    if (!data) {
        return (uint64_t)-1;
    }

    if (size >= cap) {
        size = cap - 1;
    }

    copy_bytes(buf, data, size);
    buf[size] = '\0';
    return size;
}

static uint64_t sys_vfs_write(const char *path, const char *buf, uint64_t size) {
    process_t *proc = sched_current_process();

    if (!proc || !path || (!buf && size != 0)) {
        return (uint64_t)-1;
    }

    if (vfs_write(proc->cwd ? proc->cwd : vfs_root(), path, buf ? buf : "", size) != 0) {
        return (uint64_t)-1;
    }

    return size;
}

void syscall_init(void) {
}

uint64_t syscall_dispatch(struct registers *regs) {
    switch (regs->rax) {
        case SYS_CONSOLE_WRITE:
            return sys_console_write((const char *)(uintptr_t)regs->rdi);
        case SYS_GET_PID:
            return sys_get_pid();
        case SYS_VFS_READ:
            return sys_vfs_read((const char *)(uintptr_t)regs->rdi,
                                (char *)(uintptr_t)regs->rsi,
                                regs->rdx);
        case SYS_VFS_WRITE:
            return sys_vfs_write((const char *)(uintptr_t)regs->rdi,
                                 (const char *)(uintptr_t)regs->rsi,
                                 regs->rdx);
        default:
            return (uint64_t)-1;
    }
}

uint64_t syscall_kernel_write(const char *text) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((uint64_t)SYS_CONSOLE_WRITE), "D"(text)
        : "rcx", "r11", "memory"
    );
    return ret;
}

uint64_t syscall_kernel_get_pid(void) {
    uint64_t ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"((uint64_t)SYS_GET_PID)
        : "rcx", "r11", "memory"
    );
    return ret;
}
