#include "syscall.h"

#include "../drivers/console/console.h"
#include "../drivers/input/input.h"
#include "../fs/vfs.h"
#include "../proc/sched.h"
#include "../proc/user.h"

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

static int str_eq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static uint64_t append_dir_entry(char *buf, uint64_t out, uint64_t cap, const char *name, int is_dir) {
    uint64_t i = 0;
    while (name[i] && out + 1 < cap) {
        buf[out++] = name[i++];
    }
    if (is_dir && out + 1 < cap) {
        buf[out++] = '/';
    }
    if (out + 1 < cap) {
        buf[out++] = '\n';
    }
    if (out < cap) {
        buf[out] = '\0';
    }
    return out;
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

static uint64_t sys_exit(uint64_t code) {
    user_request_exit_to_kernel(code);
    return code;
}

static uint64_t sys_input_read(void) {
    int c = input_read_char();
    return c < 0 ? (uint64_t)-1 : (uint64_t)(uint8_t)c;
}

static uint64_t sys_getcwd(char *buf, uint64_t cap) {
    process_t *proc = sched_current_process();
    if (!proc || !buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (vfs_getcwd(proc->cwd ? proc->cwd : vfs_root(), buf, (size_t)cap) != 0) {
        return (uint64_t)-1;
    }
    return str_len(buf);
}

static uint64_t sys_chdir(const char *path) {
    process_t *proc = sched_current_process();
    vfs_node_t *next;

    if (!proc || !path || !*path) {
        return (uint64_t)-1;
    }

    next = vfs_resolve(proc->cwd ? proc->cwd : vfs_root(), path);
    if (!next || vfs_node_type(next) != VFS_NODE_DIR) {
        return (uint64_t)-1;
    }

    proc->cwd = next;
    return 0;
}

static uint64_t sys_list_dir(const char *path, char *buf, uint64_t cap) {
    process_t *proc = sched_current_process();
    vfs_node_t *dir;
    uint64_t count;
    uint64_t out = 0;

    if (!proc || !buf || cap == 0) {
        return (uint64_t)-1;
    }

    if (!path || !*path || str_eq(path, ".")) {
        dir = proc->cwd ? proc->cwd : vfs_root();
    } else {
        dir = vfs_resolve(proc->cwd ? proc->cwd : vfs_root(), path);
    }
    if (!dir || vfs_node_type(dir) != VFS_NODE_DIR) {
        return (uint64_t)-1;
    }

    buf[0] = '\0';
    count = vfs_child_count(dir);
    for (uint64_t i = 0; i < count; i++) {
        vfs_node_t *child = vfs_child_at(dir, i);
        out = append_dir_entry(buf, out, cap, vfs_node_name(child), vfs_node_type(child) == VFS_NODE_DIR);
        if (out + 1 >= cap) {
            break;
        }
    }

    return out;
}

static uint64_t sys_exec(const char *path) {
    if (!path || !*path) {
        return (uint64_t)-1;
    }
    if (user_run_path(path) != 0) {
        return (uint64_t)-1;
    }
    return user_last_exit_code();
}

static uint64_t sys_console_clear(void) {
    console_clear();
    return 0;
}

static uint64_t sys_console_backspace(void) {
    console_backspace();
    return 0;
}

static uint64_t sys_mkdir(const char *path) {
    process_t *proc = sched_current_process();

    if (!proc || !path || !*path) {
        return (uint64_t)-1;
    }

    return vfs_mkdir(proc->cwd ? proc->cwd : vfs_root(), path) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_create(const char *path) {
    process_t *proc = sched_current_process();

    if (!proc || !path || !*path) {
        return (uint64_t)-1;
    }

    return vfs_create(proc->cwd ? proc->cwd : vfs_root(), path) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_stat(const char *path, vfs_stat_t *out) {
    process_t *proc = sched_current_process();

    if (!proc || !path || !*path || !out) {
        return (uint64_t)-1;
    }

    return vfs_stat(proc->cwd ? proc->cwd : vfs_root(), path, out) == 0 ? 0 : (uint64_t)-1;
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
        case SYS_EXIT:
            return sys_exit(regs->rdi);
        case SYS_INPUT_READ:
            return sys_input_read();
        case SYS_GETCWD:
            return sys_getcwd((char *)(uintptr_t)regs->rdi, regs->rsi);
        case SYS_CHDIR:
            return sys_chdir((const char *)(uintptr_t)regs->rdi);
        case SYS_LIST_DIR:
            return sys_list_dir((const char *)(uintptr_t)regs->rdi,
                                (char *)(uintptr_t)regs->rsi,
                                regs->rdx);
        case SYS_EXEC:
            return sys_exec((const char *)(uintptr_t)regs->rdi);
        case SYS_CONSOLE_CLEAR:
            return sys_console_clear();
        case SYS_CONSOLE_BACKSPACE:
            return sys_console_backspace();
        case SYS_MKDIR:
            return sys_mkdir((const char *)(uintptr_t)regs->rdi);
        case SYS_CREATE:
            return sys_create((const char *)(uintptr_t)regs->rdi);
        case SYS_STAT:
            return sys_stat((const char *)(uintptr_t)regs->rdi,
                            (vfs_stat_t *)(uintptr_t)regs->rsi);
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
