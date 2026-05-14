#include "syscall.h"

#include "../drivers/console/console.h"
#include "../drivers/input/input.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "../fs/fat32.h"
#include "../fs/vfs.h"
#include "../fs/persistfs.h"
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

static uint64_t append_text(char *buf, uint64_t out, uint64_t cap, const char *text) {
    uint64_t i = 0;
    while (text && text[i] && out + 1 < cap) {
        buf[out++] = text[i++];
    }
    if (out < cap) {
        buf[out] = '\0';
    }
    return out;
}

static uint64_t append_uint(char *buf, uint64_t out, uint64_t cap, uint64_t value) {
    char tmp[32];
    uint64_t len = 0;

    if (value == 0) {
        if (out + 1 < cap) {
            buf[out++] = '0';
            buf[out] = '\0';
        }
        return out;
    }

    while (value && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (len && out + 1 < cap) {
        buf[out++] = tmp[--len];
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
    int c;

    while ((c = input_read_char()) < 0) {
        sched_yield();
    }
    return (uint64_t)(uint8_t)c;
}

static void sys_line_show_cursor(int *visible) {
    if (!*visible) {
        console_write("_", CONSOLE_STYLE_INFO);
        *visible = 1;
    }
}

static void sys_line_hide_cursor(int *visible) {
    if (*visible) {
        console_backspace();
        *visible = 0;
    }
}

static uint64_t sys_input_readline(char *buf, uint64_t cap) {
    uint64_t len = 0;
    int visible = 0;

    if (!buf || cap == 0) {
        return (uint64_t)-1;
    }

    buf[0] = '\0';
    sys_line_show_cursor(&visible);

    for (;;) {
        int c;
        char out[2];

        while ((c = input_read_char()) < 0) {
            sched_yield();
        }

        sys_line_hide_cursor(&visible);

        if (c == '\r' || c == '\n') {
            console_write("\n", CONSOLE_STYLE_INFO);
            buf[len] = '\0';
            return len;
        }

        if (c == '\b') {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                console_backspace();
                if (len == 0) {
                    sys_line_show_cursor(&visible);
                }
            } else {
                sys_line_show_cursor(&visible);
            }
            continue;
        }

        if (c == '\t') {
            c = ' ';
        }
        if (c < 32 || c > 126) {
            sys_line_show_cursor(&visible);
            continue;
        }
        if (len + 1 >= cap) {
            sys_line_show_cursor(&visible);
            continue;
        }

        buf[len++] = (char)c;
        buf[len] = '\0';
        out[0] = (char)c;
        out[1] = '\0';
        console_write(out, CONSOLE_STYLE_INFO);
    }
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

static uint64_t sys_list_procs(char *buf, uint64_t cap) {
    process_t *proc;
    uint64_t out = 0;

    if (!buf || cap == 0) {
        return (uint64_t)-1;
    }

    buf[0] = '\0';
    out = append_text(buf, out, cap, "pid ppid sid pgid kind state exit\n");
    for (proc = sched_first_process(); proc; proc = proc->next_all) {
        out = append_uint(buf, out, cap, proc->pid);
        out = append_text(buf, out, cap, " ");
        out = append_uint(buf, out, cap, proc->parent ? proc->parent->pid : 0);
        out = append_text(buf, out, cap, " ");
        out = append_uint(buf, out, cap, proc->session_id);
        out = append_text(buf, out, cap, " ");
        out = append_uint(buf, out, cap, proc->process_group_id);
        out = append_text(buf, out, cap, " ");
        out = append_text(buf, out, cap, proc->kind == PROCESS_USER ? "user" : "kernel");
        out = append_text(buf, out, cap, " ");
        out = append_text(buf, out, cap, sched_process_state_name(proc->state));
        out = append_text(buf, out, cap, " ");
        out = append_uint(buf, out, cap, proc->exit_code);
        out = append_text(buf, out, cap, "\n");
        if (out + 1 >= cap) {
            break;
        }
    }

    return out;
}

static uint64_t sys_spawn(const char *path) {
    uint64_t pid = 0;

    if (!path || !*path) {
        return (uint64_t)-1;
    }
    if (user_spawn_path(path, &pid) != 0) {
        return (uint64_t)-1;
    }
    return pid;
}

static uint64_t sys_waitpid(uint64_t pid) {
    uint64_t code = 0;

    if (user_wait_pid(pid, &code) != 0) {
        return (uint64_t)-1;
    }
    return code;
}

static uint64_t sys_yield(void) {
    sched_yield();
    return 0;
}

static uint64_t sys_sleep(uint64_t ticks) {
    sched_sleep(ticks);
    return 0;
}

static uint64_t sys_proc_info(uint64_t pid, syscall_proc_info_t *out) {
    process_t *proc;

    if (!out) {
        return (uint64_t)-1;
    }

    proc = sched_find_process(pid);
    if (!proc) {
        return (uint64_t)-1;
    }

    out->pid = proc->pid;
    out->ppid = proc->parent ? proc->parent->pid : 0;
    out->sid = proc->session_id;
    out->pgid = proc->process_group_id;
    out->kind = (uint64_t)proc->kind;
    out->state = (uint64_t)proc->state;
    out->exit_code = proc->exit_code;
    return 0;
}

static uint64_t sys_kill(uint64_t pid, uint64_t exit_code) {
    return sched_kill_process(pid, exit_code) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_suspend(uint64_t pid) {
    return sched_suspend_process(pid) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_resume(uint64_t pid) {
    return sched_resume_process(pid) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_sync(void) {
    return vfs_sync() == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_console_set_cursor(uint64_t x, uint64_t y) {
    console_set_cursor((int)x, (int)y);
    return 0;
}

static uint64_t sys_storage_info(char *buf, uint64_t cap) {
    uint64_t out = 0;

    if (!buf || cap == 0) {
        return (uint64_t)-1;
    }

    buf[0] = '\0';
    out = append_text(buf, out, cap, "devices:\n");
    for (uint32_t i = 0; i < block_count(); i++) {
        block_device_t *dev = block_get(i);
        if (!dev) continue;
        out = append_text(buf, out, cap, "  ");
        out = append_text(buf, out, cap, dev->name ? dev->name : "disk");
        out = append_text(buf, out, cap, " sectors=");
        out = append_uint(buf, out, cap, dev->sector_count);
        out = append_text(buf, out, cap, " sector_size=");
        out = append_uint(buf, out, cap, dev->sector_size);
        out = append_text(buf, out, cap, "\n");
    }

    out = append_text(buf, out, cap, "partitions:\n");
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        if (!part) continue;
        out = append_text(buf, out, cap, "  ");
        out = append_uint(buf, out, cap, i);
        out = append_text(buf, out, cap, ": ");
        out = append_text(buf, out, cap, part->name[0] ? part->name : "part");
        out = append_text(buf, out, cap, " dev=");
        out = append_text(buf, out, cap, (part->device && part->device->name) ? part->device->name : "disk");
        out = append_text(buf, out, cap, " fs=");
        out = append_text(buf, out, cap, partition_fs_name(part->fs_hint));
        out = append_text(buf, out, cap, " start=");
        out = append_uint(buf, out, cap, part->start_lba);
        out = append_text(buf, out, cap, " sectors=");
        out = append_uint(buf, out, cap, part->sector_count);
        out = append_text(buf, out, cap, "\n");
    }

    out = append_text(buf, out, cap, "mounts:\n");
    if (fat32_mount_count() == 0) {
        out = append_text(buf, out, cap, "  (none)\n");
    } else {
        for (uint32_t i = 0; i < fat32_mount_count(); i++) {
            out = append_text(buf, out, cap, "  /volumes/fat32-");
            out = append_uint(buf, out, cap, i);
            out = append_text(buf, out, cap, "\n");
        }
    }
    return out;
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
        case SYS_LIST_PROCS:
            return sys_list_procs((char *)(uintptr_t)regs->rdi, regs->rsi);
        case SYS_SPAWN:
            return sys_spawn((const char *)(uintptr_t)regs->rdi);
        case SYS_WAITPID:
            return sys_waitpid(regs->rdi);
        case SYS_YIELD:
            return sys_yield();
        case SYS_SLEEP:
            return sys_sleep(regs->rdi);
        case SYS_PROC_INFO:
            return sys_proc_info(regs->rdi, (syscall_proc_info_t *)(uintptr_t)regs->rsi);
        case SYS_KILL:
            return sys_kill(regs->rdi, regs->rsi);
        case SYS_SUSPEND:
            return sys_suspend(regs->rdi);
        case SYS_RESUME:
            return sys_resume(regs->rdi);
        case SYS_INPUT_READLINE:
            return sys_input_readline((char *)(uintptr_t)regs->rdi, regs->rsi);
        case SYS_SYNC:
            return sys_sync();
        case SYS_CONSOLE_SETCURSOR:
            return sys_console_set_cursor(regs->rdi, regs->rsi);
        case SYS_STORAGE_INFO:
            return sys_storage_info((char *)(uintptr_t)regs->rdi, regs->rsi);
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
