#include "syscall.h"
#include "../drivers/serial/serial.h"

#include "../drivers/console/console.h"
#include "../drivers/display/framebuffer.h"
#include "../drivers/display/gpu.h"
#include "../drivers/display/vga.h"
#include "../power/power.h"
#include "../drivers/input/input.h"
#include "../drivers/input/mouse.h"
#include "../drivers/audio/speaker.h"
#include "../drivers/audio/hda.h"
#include "../drivers/audio/playback.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "../fs/diskfmt.h"
#include "../fs/fat32.h"
#include "../fs/exfat.h"
#include "../fs/install.h"
#include "../fs/ntfs.h"
#include "../fs/vfs.h"
#include "../fs/persistfs.h"
#include "../ipc/shm.h"
#include "../ipc/msgq.h"
#include "../proc/sched.h"
#include "../proc/user.h"
#include "../net/net.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../fs/fd.h"
#include "uaccess.h"

/* Set once SYS_MAP_FRAMEBUFFER has been claimed, along with the pid of
 * the process that claimed it.  The claim is released when that process
 * exits so a respawned window manager (e.g. after a VT switch) can map
 * the framebuffer again. */
static int fb_claimed = 0;
static uint64_t fb_claim_pid = 0;

static void fb_release_if_owner_gone(void) {
    if (!fb_claimed) {
        return;
    }
    if (fb_claim_pid == 0) {
        return;
    }
    process_t *owner = sched_find_process(fb_claim_pid);
    if (!owner || owner->state == PROCESS_EXITED || owner->state == PROCESS_REAPED) {
        fb_claimed = 0;
        fb_claim_pid = 0;
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
    uint64_t len;

    if (!text) {
        return (uint64_t)-1;
    }
    /* P0 gate: probe the NUL-terminated string before the console
     * layer scans it (unbounded read otherwise). */
    len = strnlen_user(text, UACCESS_MAX_STR);
    if (len == (uint64_t)-1) {
        return (uint64_t)-U_EFAULT;
    }
    console_write(text, CONSOLE_STYLE_INFO);
    return len;
}

static uint64_t sys_get_pid(void) {
    process_t *proc = sched_current_process();
    return proc ? proc->pid : 0;
}

/* P0 gate helpers: validated path (512B cap) and buffer range. */
static uint64_t list_dir_entries(vfs_node_t *dir, char *buf, uint64_t cap,
                                 uint64_t skip, uint64_t *emitted_out);

static int gate_path_ok(const char *path) {
    if (!path) {
        return 0;
    }
    return strnlen_user(path, 511) != (uint64_t)-1;
}

static uint64_t sys_vfs_read(const char *path, char *buf, uint64_t cap) {
    process_t *proc = sched_current_process();
    uint64_t size = 0;
    const char *data;

    if (!proc || !path || !buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
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
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (size != 0 && !user_range_prepare_cur(buf, size)) {
        return (uint64_t)-U_EFAULT;
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

static uint64_t sys_vfs_read_at(const char *path, uint64_t offset, char *buf, uint64_t cap) {
    process_t *proc = sched_current_process();
    uint64_t size = 0;
    const char *data;
    uint64_t chunk;

    if (!proc || !path || !buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
    }

    data = vfs_read(proc->cwd ? proc->cwd : vfs_root(), path, &size);
    if (!data || offset >= size) {
        return 0;
    }

    chunk = size - offset;
    if (chunk > cap) {
        chunk = cap;
    }

    copy_bytes(buf, data + offset, chunk);
    return chunk;
}

static uint64_t sys_input_read(void) {
    int c = input_read_char();
    if (c < 0) {
        return (uint64_t)-1;
    }
    return (uint64_t)(uint8_t)c;
}

static uint64_t sys_input_read_timeout(uint64_t ticks) {
    int c = input_read_char();
    if (c >= 0) {
        return (uint64_t)(uint8_t)c;
    }

    sched_wait_input_timeout(ticks);
    c = input_read_char();
    if (c < 0) {
        return (uint64_t)-1;
    }
    return (uint64_t)(uint8_t)c;
}

static uint64_t sys_input_readline(char *buf, uint64_t cap) {
    uint64_t len = 0;

    if (!buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
    }

    buf[0] = '\0';

    for (;;) {
        int c;
        char out[2];

        while ((c = input_read_char()) < 0) {
            sched_sleep(1);
        }

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
            }
            continue;
        }

        if (c == '\t') {
            c = ' ';
        }
        if (c < 32 || c > 126) {
            continue;
        }
        if (len + 1 >= cap) {
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
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
    }
    if (vfs_getcwd(proc->cwd ? proc->cwd : vfs_root(), buf, (size_t)cap) != 0) {
        return (uint64_t)-1;
    }
    return str_len(buf);
}

static uint64_t sys_chdir(const char *path) {
    process_t *proc = sched_current_process();
    vfs_node_t *next;

    if (!proc || !path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
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

    if (!proc || !buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
    }
    if (path && !gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }

    if (!path || !*path || str_eq(path, ".")) {
        dir = proc->cwd ? proc->cwd : vfs_root();
    } else {
        dir = vfs_resolve(proc->cwd ? proc->cwd : vfs_root(), path);
    }
    if (!dir || vfs_node_type(dir) != VFS_NODE_DIR) {
        return (uint64_t)-1;
    }

    return list_dir_entries(dir, buf, cap, 0, NULL);
}

/* Shared directory formatter: writes one-per-line child entries of `dir`
 * into `buf`, skipping the first `skip` children (fd offset support).
 * Returns bytes written; optionally reports emitted entry count. */
static uint64_t list_dir_entries(vfs_node_t *dir, char *buf, uint64_t cap,
                                 uint64_t skip, uint64_t *emitted_out) {
    uint64_t count;
    uint64_t out = 0;
    uint64_t emitted = 0;

    buf[0] = '\0';
    count = vfs_child_count(dir);
    for (uint64_t i = skip; i < count; i++) {
        vfs_node_t *child = vfs_child_at(dir, i);
        if (!child) {
            break;
        }
        out = append_dir_entry(buf, out, cap, vfs_node_name(child),
                               vfs_node_type(child) == VFS_NODE_DIR);
        emitted++;
        if (out + 1 >= cap) {
            break;
        }
    }
    if (emitted_out) {
        *emitted_out = emitted;
    }
    return out;
}

static uint64_t sys_exec(const char *path) {
    if (!path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
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

static uint64_t sys_exec_args(const char *path, const char *args) {
    if (!path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
        return (uint64_t)-1;
    }
    if (args && strnlen_user(args, 2048) == (uint64_t)-1) {
        return (uint64_t)-U_EFAULT;
    }
    if (user_run_path_args(path, args) != 0) {
        return (uint64_t)-1;
    }
    return user_last_exit_code();
}

static uint64_t sys_mkdir(const char *path) {
    process_t *proc = sched_current_process();

    if (!proc || !path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
        return (uint64_t)-1;
    }

    return vfs_mkdir(proc->cwd ? proc->cwd : vfs_root(), path) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_create(const char *path) {
    process_t *proc = sched_current_process();

    if (!proc || !path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
        return (uint64_t)-1;
    }

    return vfs_create(proc->cwd ? proc->cwd : vfs_root(), path) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_stat(const char *path, vfs_stat_t *out) {
    process_t *proc = sched_current_process();

    if (!proc || !path || !out) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(out, sizeof(*out))) {
        return (uint64_t)-U_EFAULT;
    }

    return vfs_stat(proc->cwd ? proc->cwd : vfs_root(), path, out) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_list_procs(char *buf, uint64_t cap) {
    process_t *proc;
    uint64_t out = 0;

    if (!buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
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

    if (!path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
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
    if (!user_range_prepare_cur_w(out, sizeof(*out))) {
        return (uint64_t)-U_EFAULT;
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

static uint64_t sys_proc_stats(uint64_t pid, syscall_proc_stats_t *out) {
    process_t *proc;

    if (!out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(out, sizeof(*out))) {
        return (uint64_t)-U_EFAULT;
    }
    proc = sched_find_process(pid);
    if (!proc) {
        return (uint64_t)-1;
    }

    out->cpu_ticks = proc->cpu_ticks;
    /* Count the pages this process actually has mapped in its own
     * address space (kernel and framebuffer pages are shared/global
     * and excluded by the accounting in vmm_map_page). */
    out->mem_bytes = proc->addr_space ? proc->addr_space->mapped_pages * PAGE_SIZE_4K : 0;
    {
        uint64_t i = 0;
        while (proc->name[i] && i < sizeof(out->name) - 1) {
            out->name[i] = proc->name[i];
            i++;
        }
        out->name[i] = 0;
    }
    return 0;
}

static uint64_t sys_gpu_query(syscall_gpu_info_t *out) {
    gpu_device_t *dev;

    if (!out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(out, sizeof(*out))) {
        return (uint64_t)-U_EFAULT;
    }
    dev = gpu_primary();
    if (!dev) {
        return (uint64_t)-1;
    }

    {
        uint64_t i = 0;
        while (dev->name[i] && i < sizeof(out->name) - 1) {
            out->name[i] = dev->name[i];
            i++;
        }
        out->name[i] = 0;
    }
    out->width = (int32_t)dev->modes[dev->current_mode].width;
    out->height = (int32_t)dev->modes[dev->current_mode].height;
    out->pitch = dev->modes[dev->current_mode].pitch;
    out->bpp = dev->modes[dev->current_mode].bpp;
    out->mode_count = dev->mode_count;
    out->hw_cursor = dev->hw_cursor ? 1U : 0U;
    out->present_supported = dev->present_supported ? 1U : 0U;
    return 0;
}

static uint64_t sys_gpu_present(void) {
    gpu_device_t *dev = gpu_primary();
    if (!dev || !dev->present) {
        return (uint64_t)-1;
    }
    return dev->present(dev) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_cursor(int x, int y, const uint32_t *image, int w, int h) {
    gpu_device_t *dev = gpu_primary();
    uint64_t pixels;
    if (!dev || !dev->set_cursor) {
        return (uint64_t)-1;
    }
    if (w < 0 || h < 0) {
        return (uint64_t)-U_EINVAL;
    }
    pixels = (uint64_t)w * (uint64_t)h;
    /* Overflow-guarded image range probe (w*h*4 bytes). */
    if (pixels > UACCESS_MAX_LEN / 4) {
        return (uint64_t)-U_EINVAL;
    }
    if (pixels != 0 && !image) {
        return (uint64_t)-U_EINVAL;
    }
    if (image && !user_range_prepare_cur(image, pixels * 4)) {
        return (uint64_t)-U_EFAULT;
    }
    return dev->set_cursor(dev, x, y, image, w, h) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_power(uint64_t action) {
    /* 0 = shutdown, 1 = reboot.  Neither returns. */
    if (action == 1) {
        power_reboot();
    } else {
        power_shutdown();
    }
    return 0;
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

static uint64_t sys_spawn_args(const char *path, const char *args) {
    uint64_t pid = 0;

    if (!path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
        return (uint64_t)-1;
    }
    if (args && strnlen_user(args, 2048) == (uint64_t)-1) {
        return (uint64_t)-U_EFAULT;
    }
    if (user_spawn_path_args(path, args, &pid) != 0) {
        return (uint64_t)-1;
    }
    return pid;
}

static uint64_t sys_mount(uint64_t partition_index, const char *path) {
    const partition_info_t *part;

    if (!path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
        return (uint64_t)-1;
    }
    part = partition_get((uint32_t)partition_index);
    if (!part) {
        return (uint64_t)-1;
    }

    switch (part->fs_hint) {
        case PARTITION_FS_FAT32:
            return fat32_mount_partition((uint32_t)partition_index, path) == 0 ? 0 : (uint64_t)-1;
        case PARTITION_FS_EXFAT:
            return exfat_mount_partition((uint32_t)partition_index, path) == 0 ? 0 : (uint64_t)-1;
        case PARTITION_FS_NTFS:
            return ntfs_mount_partition((uint32_t)partition_index, path) == 0 ? 0 : (uint64_t)-1;
        default:
            return (uint64_t)-1;
    }
}

static uint64_t sys_format_device(uint64_t device_index, uint64_t fs_type) {
    int rc = diskfmt_format_device((uint32_t)device_index, (diskfmt_fs_t)fs_type);
    return rc == 0 ? 0 : (uint64_t)(int64_t)rc;
}

static uint64_t sys_format_partition(uint64_t partition_index, uint64_t fs_type) {
    int rc = diskfmt_format_partition((uint32_t)partition_index, (diskfmt_fs_t)fs_type);
    return rc == 0 ? 0 : (uint64_t)(int64_t)rc;
}

static uint64_t sys_set_partition_role(uint64_t partition_index, uint64_t role) {
    int rc = diskfmt_set_partition_role((uint32_t)partition_index, (partition_role_t)role);
    return rc == 0 ? 0 : (uint64_t)(int64_t)rc;
}

static uint64_t sys_install_system(uint64_t *files_out, uint64_t *bytes_out) {
    uint64_t files = 0;
    uint64_t bytes = 0;

    if (files_out && !user_range_prepare_cur_w(files_out, sizeof(*files_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (bytes_out && !user_range_prepare_cur_w(bytes_out, sizeof(*bytes_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (system_install_run(&files, &bytes) != 0) {
        return (uint64_t)-1;
    }
    if (files_out) {
        *files_out = files;
    }
    if (bytes_out) {
        *bytes_out = bytes;
    }
    return 0;
}

static uint64_t sys_install_device(uint64_t device_index, uint64_t *files_out, uint64_t *bytes_out) {
    uint64_t files = 0;
    uint64_t bytes = 0;
    int rc;

    if (files_out && !user_range_prepare_cur_w(files_out, sizeof(*files_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (bytes_out && !user_range_prepare_cur_w(bytes_out, sizeof(*bytes_out))) {
        return (uint64_t)-U_EFAULT;
    }
    rc = system_install_device((uint32_t)device_index, &files, &bytes);
    if (rc != 0) {
        return (uint64_t)(int64_t)rc;
    }
    if (files_out) {
        *files_out = files;
    }
    if (bytes_out) {
        *bytes_out = bytes;
    }
    return 0;
}

static uint64_t sys_install_partitions(const syscall_install_plan_t *plan, uint64_t *files_out, uint64_t *bytes_out) {
    uint64_t files = 0;
    uint64_t bytes = 0;
    int rc;

    if (!plan) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur(plan, sizeof(*plan))) {
        return (uint64_t)-U_EFAULT;
    }
    if (files_out && !user_range_prepare_cur_w(files_out, sizeof(*files_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (bytes_out && !user_range_prepare_cur_w(bytes_out, sizeof(*bytes_out))) {
        return (uint64_t)-U_EFAULT;
    }
    rc = system_install_partitions((uint32_t)plan->efi_partition, (uint32_t)plan->root_partition, (int32_t)plan->swap_partition, &files, &bytes);
    if (rc != 0) {
        return (uint64_t)(int64_t)rc;
    }
    if (files_out) {
        *files_out = files;
    }
    if (bytes_out) {
        *bytes_out = bytes;
    }
    return 0;
}

static uint64_t sys_console_set_cursor(uint64_t x, uint64_t y) {
    console_set_cursor((int)x, (int)y);
    return 0;
}

static uint64_t sys_console_size(uint64_t *cols_out, uint64_t *rows_out) {
    uint64_t cols = VGA_WIDTH;
    uint64_t rows = VGA_HEIGHT;

    if (!cols_out || !rows_out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(cols_out, sizeof(*cols_out)) ||
        !user_range_prepare_cur_w(rows_out, sizeof(*rows_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (fb_available()) {
        int fb_cols = fb_columns();
        int fb_rows_count = fb_rows();
        if (fb_cols > 0) cols = (uint64_t)fb_cols;
        if (fb_rows_count > 0) rows = (uint64_t)fb_rows_count;
    }
    *cols_out = cols;
    *rows_out = rows;
    return 0;
}

static uint64_t sys_console_get_cursor(uint64_t *x_out, uint64_t *y_out) {
    int x = 0;
    int y = 0;
    if (!x_out || !y_out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(x_out, sizeof(*x_out)) ||
        !user_range_prepare_cur_w(y_out, sizeof(*y_out))) {
        return (uint64_t)-U_EFAULT;
    }
    console_get_cursor(&x, &y);
    *x_out = x < 0 ? 0 : (uint64_t)x;
    *y_out = y < 0 ? 0 : (uint64_t)y;
    return 0;
}

static uint64_t sys_runtime_device(void) {
    int device = persistfs_active_device();
    return device >= 0 ? (uint64_t)device : (uint64_t)-1;
}

static uint64_t sys_storage_info(char *buf, uint64_t cap) {
    uint64_t out = 0;

    if (!buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
    }

    buf[0] = '\0';
    out = append_text(buf, out, cap, "devices:\n");
    for (uint32_t i = 0; i < block_count(); i++) {
        block_device_t *dev = block_get(i);
        if (!dev) continue;
        out = append_text(buf, out, cap, "  ");
        out = append_uint(buf, out, cap, i);
        out = append_text(buf, out, cap, ": ");
        out = append_text(buf, out, cap, dev->name ? dev->name : "disk");
        out = append_text(buf, out, cap, " table=");
        out = append_text(buf, out, cap, partition_kind_name(partition_device_kind(i)));
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
        out = append_text(buf, out, cap, " role=");
        out = append_text(buf, out, cap, partition_role_name(part->role));
        out = append_text(buf, out, cap, " start=");
        out = append_uint(buf, out, cap, part->start_lba);
        out = append_text(buf, out, cap, " sectors=");
        out = append_uint(buf, out, cap, part->sector_count);
        out = append_text(buf, out, cap, "\n");
    }

    out = append_text(buf, out, cap, "mounts:\n");
    if (fat32_mount_count() == 0 && exfat_mount_count() == 0 && ntfs_mount_count() == 0) {
        out = append_text(buf, out, cap, "  (none)\n");
    } else {
        for (uint32_t i = 0; i < fat32_mount_count(); i++) {
            out = append_text(buf, out, cap, "  /volumes/fat32-");
            out = append_uint(buf, out, cap, i);
            out = append_text(buf, out, cap, "\n");
        }
        for (uint32_t i = 0; i < exfat_mount_count(); i++) {
            out = append_text(buf, out, cap, "  /volumes/exfat-");
            out = append_uint(buf, out, cap, i);
            out = append_text(buf, out, cap, " (ro)\n");
        }
        for (uint32_t i = 0; i < ntfs_mount_count(); i++) {
            out = append_text(buf, out, cap, "  /volumes/ntfs-");
            out = append_uint(buf, out, cap, i);
            out = append_text(buf, out, cap, " (ro)\n");
        }
    }
    return out;
}

static uint64_t sys_sound_play(uint64_t frequency_hz, uint64_t ticks) {
    speaker_play_for((uint32_t)frequency_hz, ticks);
    return 0;
}

static uint64_t sys_audio_pcm_play(const uint8_t *buf, uint64_t size, uint64_t sample_rate) {
    (void)buf;
    (void)size;
    (void)sample_rate;
    return (uint64_t)-1;
}

static uint64_t sys_audio_play_file(const char *path) {
    process_t *proc = sched_current_process();
    if (!proc || !path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (!*path) {
        return (uint64_t)-1;
    }
    return audio_playback_play_wav(proc->cwd ? proc->cwd : vfs_root(), path) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_audio_stop(void) {
    audio_playback_stop();
    return 0;
}

static uint64_t sys_audio_status(syscall_audio_info_t *out) {
    audio_playback_status_t info;
    uint64_t i;

    if (!out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(out, sizeof(*out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (audio_playback_status(&info) != 0) {
        return (uint64_t)-1;
    }
    out->active = info.active;
    out->seconds_left = info.seconds_left;
    out->total_seconds = info.total_seconds;
    for (i = 0; i < sizeof(out->name); i++) {
        out->name[i] = info.name[i];
        if (info.name[i] == '\0') break;
    }
    if (i == sizeof(out->name)) {
        out->name[sizeof(out->name) - 1] = '\0';
    }
    return 0;
}

static uint64_t sys_audio_claim(uint64_t *token_out, uint64_t *sample_rate_out) {
    process_t *proc = sched_current_process();
    uint32_t rate = 0;
    uint64_t token = 0;

    if (!proc || !token_out || !sample_rate_out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(token_out, sizeof(*token_out)) ||
        !user_range_prepare_cur_w(sample_rate_out, sizeof(*sample_rate_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (audio_playback_claim(proc->pid, &token, &rate) != 0) {
        return (uint64_t)-1;
    }
    *token_out = token;
    *sample_rate_out = rate;
    return 0;
}

static uint64_t sys_audio_read_chunk(uint64_t token, uint8_t *buf, uint64_t cap) {
    if (!buf || cap == 0 || cap > 0xFFFFFFFFULL) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
    }
    return audio_playback_read_chunk(token, buf, (uint32_t)cap);
}

static uint64_t sys_audio_finish(uint64_t token) {
    audio_playback_finish(token);
    return 0;
}

/* P0 gate for the (host, path, out_path) string triple shared by the
 * HTTP/HTTPS fetch handlers. Paths into the network stack are the
 * classic remote-input vector, so bound them tightly. */
static int gate_fetch_args(const char *host, const char *path,
                           const char *out_path, uint64_t *bytes_out) {
    if (!host || !path || !out_path) {
        return 0;
    }
    if (strnlen_user(host, 255) == (uint64_t)-1 ||
        strnlen_user(path, 4095) == (uint64_t)-1 ||
        strnlen_user(out_path, 511) == (uint64_t)-1) {
        return 0;
    }
    if (bytes_out && !user_range_prepare_cur_w(bytes_out, sizeof(*bytes_out))) {
        return 0;
    }
    return 1;
}

static uint64_t sys_http_get_ipv4(uint64_t ipv4_addr, uint64_t port, const char *host, const char *path, const char *out_path, uint64_t *bytes_out) {
    uint64_t bytes = 0;
    if (!gate_fetch_args(host, path, out_path, bytes_out)) {
        return !host || !path || !out_path ? (uint64_t)-1
                                           : (uint64_t)-U_EFAULT;
    }
    if (net_http_get_ipv4((uint32_t)ipv4_addr, (uint16_t)port, host, path, out_path, &bytes) != 0) {
        return (uint64_t)(-(int64_t)net_last_error());
    }
    if (bytes_out) {
        *bytes_out = bytes;
    }
    return 0;
}

static uint64_t sys_https_get_ipv4(uint64_t ipv4_addr, uint64_t port, const char *host, const char *path, const char *out_path, uint64_t *bytes_out) {
    uint64_t bytes = 0;
    if (!gate_fetch_args(host, path, out_path, bytes_out)) {
        return !host || !path || !out_path ? (uint64_t)-1
                                           : (uint64_t)-U_EFAULT;
    }
    if (net_https_get_ipv4((uint32_t)ipv4_addr, (uint16_t)port, host, path, out_path, &bytes) != 0) {
        return (uint64_t)(-(int64_t)net_last_error());
    }
    if (bytes_out) {
        *bytes_out = bytes;
    }
    return 0;
}

static uint64_t sys_dns_resolve(const char *host, uint32_t *ipv4_out) {
    uint32_t ipv4 = 0;
    if (!host || !ipv4_out) {
        return (uint64_t)-1;
    }
    if (strnlen_user(host, 255) == (uint64_t)-1) {
        return (uint64_t)-U_EFAULT;
    }
    if (!user_range_prepare_cur_w(ipv4_out, sizeof(*ipv4_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (net_dns_resolve_ipv4(host, &ipv4) != 0) {
        return (uint64_t)(-(int64_t)net_last_error());
    }
    *ipv4_out = ipv4;
    return 0;
}

void syscall_init(void) {
}

static uint64_t linux_syscall_dispatch(struct registers *regs) {
    process_t *proc = sched_current_process();
    uint64_t nr = regs->rax;
    uint64_t a0 = regs->rdi, a1 = regs->rsi, a2 = regs->rdx;
    uint64_t a3 = regs->r10, a4 = regs->r8, a5 = regs->r9;

    if (!proc) {
        return (uint64_t)-1;
    }

    switch (nr) {
        case 0: { // read
            int fd = (int)a0;
            char *buf = (char *)(uintptr_t)a1;
            uint64_t count = a2;
            struct vfs_node *node;
            uint64_t off;
            int is_stdio = 0;
            uint64_t size;
            const char *data;
            uint64_t avail;
            uint64_t copy;
            if (!buf && count != 0) return (uint64_t)-U_EFAULT;
            if (count == 0) return 0;
            if (!user_range_prepare_cur_w(buf, count)) return (uint64_t)-U_EFAULT;
            if (fd_resolve(proc, fd, &node, &off, &is_stdio) != 0) {
                return (uint64_t)-U_EBADF;
            }
            if (is_stdio) {
                if (fd != 0) return (uint64_t)-U_EBADF;
                if (proc->linux_brk_pos == 0) proc->linux_brk_pos = 0x60000000;
                data = vfs_read(proc->cwd ? proc->cwd : vfs_root(), "/dev/stdin", &size);
                if (!data) return 0;
                copy = count < size ? count : size;
                if (copy_to_user(buf, data, copy) != 0) return (uint64_t)-U_EFAULT;
                return copy;
            }
            if (vfs_node_type(node) != VFS_NODE_FILE) {
                return (uint64_t)-U_EINVAL;
            }
            if ((fd_get_flags(proc, fd) & FD_O_ACCMODE) == FD_O_WRONLY) {
                return (uint64_t)-U_EBADF;
            }
            size = vfs_node_size(node);
            if (off >= size) return 0;
            data = vfs_node_data(node);
            if (!data) return 0;
            avail = size - off;
            copy = count < avail ? count : avail;
            if (copy_to_user(buf, data + off, copy) != 0) {
                return (uint64_t)-U_EFAULT;
            }
            fd_set_off(proc, fd, off + copy);
            return copy;
        }
        case 1: { // write
            int fd = (int)a0;
            const char *buf = (const char *)(uintptr_t)a1;
            uint64_t count = a2;
            struct vfs_node *node;
            uint64_t off;
            int is_stdio = 0;
            if (!buf && count != 0) return (uint64_t)-U_EFAULT;
            if (count == 0) return 0;
            /* Source buffer: read probe (a read-only source mapping is
             * legitimate here; chunks are re-probed per copy). */
            if (!user_range_prepare_cur(buf, count)) return (uint64_t)-U_EFAULT;
            if (fd_resolve(proc, fd, &node, &off, &is_stdio) != 0) {
                return (uint64_t)-U_EBADF;
            }
            if (is_stdio) {
                uint64_t done = 0;
                char kbuf[4096];
                if (fd == 0) return (uint64_t)-U_EBADF;
                /* NUL-safe chunked console output: console_write scans
                 * for NUL, so never hand it raw user memory. */
                while (done < count) {
                    uint64_t chunk = count - done;
                    if (chunk > sizeof(kbuf) - 1) chunk = sizeof(kbuf) - 1;
                    if (copy_from_user(kbuf, buf + done, chunk) != 0) {
                        return (uint64_t)-U_EFAULT;
                    }
                    kbuf[chunk] = '\0';
                    console_write(kbuf, CONSOLE_STYLE_INFO);
                    done += chunk;
                }
                return count;
            }
            if (vfs_node_type(node) != VFS_NODE_FILE) {
                return (uint64_t)-U_EINVAL;
            }
            if ((fd_get_flags(proc, fd) & FD_O_ACCMODE) == FD_O_RDONLY) {
                return (uint64_t)-U_EBADF;
            }
            if (vfs_node_readonly(node)) {
                return (uint64_t)-U_EACCES;
            }
            {
                uint64_t done = 0;
                char kbuf[4096];
                while (done < count) {
                    uint64_t chunk = count - done;
                    if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
                    if (copy_from_user(kbuf, buf + done, chunk) != 0) {
                        return (uint64_t)-U_EFAULT;
                    }
                    if (vfs_node_write_at(node, off + done, kbuf, chunk) != 0) {
                        return done ? done : (uint64_t)-1;
                    }
                    done += chunk;
                }
                fd_set_off(proc, fd, off + done);
                return done;
            }
        }
        case 2: { // open
            const char *pathname = (const char *)(uintptr_t)a0;
            uint64_t flags = a1;
            uint64_t plen;
            char kpath[512];
            int fd;
            if (!pathname) return (uint64_t)-U_EFAULT;
            plen = strnlen_user(pathname, sizeof(kpath) - 1);
            if (plen == (uint64_t)-1) return (uint64_t)-U_EFAULT;
            if (copy_from_user(kpath, pathname, plen + 1) != 0) {
                return (uint64_t)-U_EFAULT;
            }
            kpath[plen] = '\0';
            fd = fd_open_path(proc, proc->cwd ? proc->cwd : vfs_root(),
                              kpath, flags);
            if (fd < 0) return (uint64_t)(int64_t)fd;
            return (uint64_t)fd;
        }
        case 3: // close
            return fd_close(proc, (int)a0) == 0 ? 0 : (uint64_t)-U_EBADF;
        case 5: { // fstat
            int fd = (int)a0;
            vfs_stat_t *st = (vfs_stat_t *)(uintptr_t)a1;
            struct vfs_node *node;
            uint64_t off;
            int is_stdio = 0;
            vfs_stat_t ks;
            if (!st) return (uint64_t)-U_EFAULT;
            if (!user_range_prepare_cur_w(st, sizeof(*st))) {
                return (uint64_t)-U_EFAULT;
            }
            if (fd_resolve(proc, fd, &node, &off, &is_stdio) != 0) {
                return (uint64_t)-U_EBADF;
            }
            if (is_stdio) {
                ks.inode = 0;
                ks.size = 0;
                ks.created = 0;
                ks.modified = 0;
                ks.type = VFS_NODE_FILE;
                ks.readonly = 1;
            } else {
                ks.inode = vfs_node_inode(node);
                ks.size = vfs_node_size(node);
                ks.created = vfs_node_created(node);
                ks.modified = vfs_node_modified(node);
                ks.type = vfs_node_type(node);
                ks.readonly = vfs_node_readonly(node);
            }
            if (copy_to_user(st, &ks, sizeof(ks)) != 0) {
                return (uint64_t)-U_EFAULT;
            }
            return 0;
        }
        case 9: { // mmap
            uint64_t addr = a0;
            uint64_t length = a1;
            uint64_t prot = a2;
            uint64_t flags = a3;
            uint64_t fd = a4;
            (void)flags;
            (void)fd;
            (void)a5;
            if (length == 0) return (uint64_t)-1;
            uint64_t pages = (length + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
            if (addr == 0) {
                if (proc->linux_mmap_next == 0) proc->linux_mmap_next = 0x70000000;
                addr = proc->linux_mmap_next;
                proc->linux_mmap_next += pages * PAGE_SIZE_4K;
            }
            for (uint64_t i = 0; i < pages; i++) {
                uint64_t phys = pmm_alloc();
                if (!phys) return (uint64_t)-1;
                uint64_t vmm_flags = VMM_FLAGS_USER_RW;
                if (!(prot & 2)) vmm_flags = VMM_FLAGS_USER_RO;
                if (vmm_map_page(proc->addr_space, addr + i * PAGE_SIZE_4K, phys, vmm_flags) != 0) {
                    pmm_free(phys);
                    return (uint64_t)-1;
                }
                char *dst = (char *)PHYS_TO_VIRT(phys);
                for (int j = 0; j < (int)PAGE_SIZE_4K; j++) dst[j] = 0;
            }
            return addr;
        }
        case 10: { // mprotect — real implementation with W^X
            uint64_t addr = a0;
            uint64_t length = a1;
            uint64_t prot = a2;
            uint64_t end;
            uint64_t page;
            uint64_t fb_virt = 0x500000000ULL;
            uint64_t fb_size = fb_phys_size();
            uint64_t shm_end = SHM_VIRT_BASE + (uint64_t)SHM_MAX_REGIONS * SHM_SLOT_SIZE;
            uint64_t newflags;
            /* Linux PROT_* bits. PROT_EXEC without PROT_READ is mapped
             * to read-only (x86 cannot express execute-only); PROT_NONE
             * is rejected — no caller needs it yet. W|X is always
             * rejected: this kernel is W^X. */
            if (length == 0 || (addr & 0xFFFULL)) {
                return (uint64_t)-U_EINVAL;
            }
            if (prot & ~7ULL) {
                return (uint64_t)-U_EINVAL;
            }
            if ((prot & 2) && (prot & 4)) {
                return (uint64_t)-U_EINVAL;
            }
            if (prot == 0) {
                return (uint64_t)-U_EINVAL;
            }
            end = addr + length;
            if (end < addr || end > USER_HALF_END) {
                return (uint64_t)-U_EINVAL;
            }
            if (addr < fb_virt + fb_size && end > fb_virt) {
                return (uint64_t)-U_EINVAL;
            }
            if (addr < shm_end && end > SHM_VIRT_BASE) {
                return (uint64_t)-U_EINVAL;
            }
            if (!proc || !proc->addr_space) {
                return (uint64_t)-U_EINVAL;
            }
            newflags = (prot & 2) ? VMM_FLAGS_USER_RW : VMM_FLAGS_USER_RO;
            for (page = addr; page < end; page += PAGE_SIZE_4K) {
                uint64_t phys = vmm_virt_to_phys(proc->addr_space, page);
                if (!phys) {
                    return (uint64_t)-U_ENOMEM;
                }
                /* Re-map the same frame with new permissions. */
                if (vmm_map_page(proc->addr_space, page, phys & ~0xFFFULL,
                                 newflags) != 0) {
                    return (uint64_t)-U_ENOMEM;
                }
            }
            return 0;
        }
        case 11: { // munmap — real implementation (P0/B3)
            uint64_t addr = a0;
            uint64_t length = a1;
            uint64_t end;
            uint64_t page;
            /* Shared framebuffer window: device memory, not PMM-owned.
             * Never free it here; use SYS_MAP_FRAMEBUFFER/SYS_SHM_UNMAP. */
            uint64_t fb_virt = 0x500000000ULL;
            uint64_t fb_size = fb_phys_size();
            /* SHM window: ref-counted shared frames owned by shm.c.
             * Detaching must go through SYS_SHM_UNMAP/SYS_SHM_CLOSE. */
            uint64_t shm_end = SHM_VIRT_BASE + (uint64_t)SHM_MAX_REGIONS * SHM_SLOT_SIZE;
            if (length == 0 || (addr & 0xFFFULL)) {
                return (uint64_t)-U_EINVAL;
            }
            end = addr + length;
            if (end < addr || end > USER_HALF_END) {
                return (uint64_t)-U_EINVAL;
            }
            if (addr < fb_virt + fb_size && end > fb_virt) {
                return (uint64_t)-U_EINVAL;
            }
            if (addr < shm_end && end > SHM_VIRT_BASE) {
                return (uint64_t)-U_EINVAL;
            }
            if (!proc || !proc->addr_space) {
                return (uint64_t)-U_EINVAL;
            }
            for (page = addr; page < end; page += PAGE_SIZE_4K) {
                /* Present user pages here are always PMM-owned: text,
                 * stack, brk and mmap regions are privately allocated
                 * per process, and the shared FB/SHM windows are
                 * excluded above. Non-present pages are skipped. */
                if (vmm_virt_to_phys(proc->addr_space, page)) {
                    vmm_unmap_page(proc->addr_space, page, 1);
                }
            }
            return 0;
        }
        case 12: { // brk
            uint64_t new_brk = a0;
            if (proc->linux_brk_pos == 0) proc->linux_brk_pos = 0x60000000;
            if (new_brk == 0) return proc->linux_brk_pos;
            if (new_brk > proc->linux_brk_pos) {
                uint64_t old_end = (proc->linux_brk_pos + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
                uint64_t new_end = (new_brk + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
                for (uint64_t page = old_end; page < new_end; page += PAGE_SIZE_4K) {
                    uint64_t phys = pmm_alloc();
                    if (!phys) break;
                    if (vmm_map_page(proc->addr_space, page, phys, VMM_FLAGS_USER_RW) != 0) {
                        pmm_free(phys);
                        break;
                    }
                    char *dst = (char *)PHYS_TO_VIRT(phys);
                    for (int j = 0; j < (int)PAGE_SIZE_4K; j++) dst[j] = 0;
                }
            }
            proc->linux_brk_pos = new_brk;
            return new_brk;
        }
        case 60: // exit
        case 231: // exit_group
            user_request_exit_to_kernel(a0);
            return a0;
        case 78: { // getdents — fd-based (P0/B2)
            int fd = (int)a0;
            char *buf = (char *)(uintptr_t)a1;
            uint64_t count = a2;
            struct vfs_node *node;
            uint64_t entry;
            int is_stdio = 0;
            char dirbuf[4096];
            uint64_t len;
            if (!buf && count != 0) return (uint64_t)-U_EFAULT;
            if (count == 0) return 0;
            if (!user_range_prepare_cur_w(buf, count)) return (uint64_t)-U_EFAULT;
            if (fd_resolve(proc, fd, &node, &entry, &is_stdio) != 0 || is_stdio) {
                return (uint64_t)-U_EBADF;
            }
            if (vfs_node_type(node) != VFS_NODE_DIR) {
                return (uint64_t)-U_EINVAL;
            }
            if (count < 20) return 0;
            /* List the fd's own directory (not cwd), honoring the fd
             * offset so repeated calls page through entries. */
            len = list_dir_entries(node, dirbuf, sizeof(dirbuf), 0, NULL);
            {
                uint64_t written = 0;
                uint64_t pos = 0;
                uint64_t seen = 0;
                uint64_t emitted = 0;
                while (pos < len && written + 20 <= count) {
                    uint64_t name_start = pos;
                    uint8_t had_nl;
                    uint64_t name_len;
                    uint8_t *dirent;
                    while (pos < len && dirbuf[pos] != '\n') pos++;
                    had_nl = (pos < len && dirbuf[pos] == '\n');
                    if (seen < entry) {
                        seen++;
                        if (had_nl) pos++;
                        continue;
                    }
                    name_len = pos - name_start;
                    if (name_len > 255) name_len = 255;
                    dirent = (uint8_t *)(buf + written);
                    dirent[0] = 0;
                    dirent[1] = 0;
                    dirent[2] = 0;
                    dirent[3] = 0;
                    dirent[4] = 0;
                    dirent[5] = 0;
                    dirent[6] = 0;
                    dirent[7] = 0;
                    dirent[8] = 0;
                    dirent[9] = 0;
                    dirent[10] = 0;
                    dirent[11] = 0;
                    dirent[16] = (uint8_t)(name_len);
                    dirent[17] = (uint8_t)(name_len >> 8);
                    dirent[18] = 0; // DT_UNKNOWN
                    for (uint64_t i = 0; i < name_len && i < count - written - 19; i++) {
                        dirent[19 + i] = (uint8_t)dirbuf[name_start + i];
                    }
                    written += 19 + name_len;
                    emitted++;
                    if (had_nl) pos++;
                }
                fd_set_off(proc, fd, entry + emitted);
                return written;
            }
        }
        case 158: // arch_prctl
            return 0;
        default:
            return (uint64_t)-1;
    }
}

uint64_t syscall_dispatch(struct registers *regs) {
    process_t *proc = sched_current_process();
    if (proc && proc->linux_personality) {
        return linux_syscall_dispatch(regs);
    }
    switch (regs->rax) {
        case SYS_CONSOLE_WRITE:
            return sys_console_write((const char *)(uintptr_t)regs->rdi);
        case SYS_GET_PID:
            return sys_get_pid();
        case SYS_VFS_READ:
            return sys_vfs_read((const char *)(uintptr_t)regs->rdi,
                                (char *)(uintptr_t)regs->rsi,
                                regs->rdx);
        case SYS_VFS_READ_AT:
            return sys_vfs_read_at((const char *)(uintptr_t)regs->rdi,
                                   regs->rsi,
                                   (char *)(uintptr_t)regs->rdx,
                                   regs->r10);
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
        case SYS_EXEC_ARGS:
            return sys_exec_args((const char *)(uintptr_t)regs->rdi,
                                 (const char *)(uintptr_t)regs->rsi);
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
        case SYS_SPAWN_ARGS:
            return sys_spawn_args((const char *)(uintptr_t)regs->rdi,
                                  (const char *)(uintptr_t)regs->rsi);
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
        case SYS_SOUND_PLAY:
            return sys_sound_play(regs->rdi, regs->rsi);
        case SYS_AUDIO_PCM_PLAY:
            return sys_audio_pcm_play((const uint8_t *)(uintptr_t)regs->rdi, regs->rsi, regs->rdx);
        case SYS_AUDIO_PLAY_FILE:
            return sys_audio_play_file((const char *)(uintptr_t)regs->rdi);
        case SYS_AUDIO_STOP:
            return sys_audio_stop();
        case SYS_AUDIO_STATUS:
            return sys_audio_status((syscall_audio_info_t *)(uintptr_t)regs->rdi);
        case SYS_AUDIO_CLAIM:
            return sys_audio_claim((uint64_t *)(uintptr_t)regs->rdi,
                                   (uint64_t *)(uintptr_t)regs->rsi);
        case SYS_AUDIO_READ_CHUNK:
            return sys_audio_read_chunk(regs->rdi,
                                        (uint8_t *)(uintptr_t)regs->rsi,
                                        regs->rdx);
        case SYS_AUDIO_FINISH:
            return sys_audio_finish(regs->rdi);
        case SYS_TICKS:
            return sched_ticks();
        case SYS_INPUT_READ_TIMEOUT:
            return sys_input_read_timeout(regs->rdi);
        case SYS_INSTALL_SYSTEM:
            return sys_install_system((uint64_t *)(uintptr_t)regs->rdi,
                                      (uint64_t *)(uintptr_t)regs->rsi);
        case SYS_MOUNT:
            return sys_mount(regs->rdi, (const char *)(uintptr_t)regs->rsi);
        case SYS_FORMAT_DEVICE:
            return sys_format_device(regs->rdi, regs->rsi);
        case SYS_FORMAT_PARTITION:
            return sys_format_partition(regs->rdi, regs->rsi);
        case SYS_CONSOLE_SIZE:
            return sys_console_size((uint64_t *)(uintptr_t)regs->rdi,
                                    (uint64_t *)(uintptr_t)regs->rsi);
        case SYS_CONSOLE_GETCURSOR:
            return sys_console_get_cursor((uint64_t *)(uintptr_t)regs->rdi,
                                          (uint64_t *)(uintptr_t)regs->rsi);
        case SYS_INSTALL_DEVICE:
            return sys_install_device(regs->rdi,
                                      (uint64_t *)(uintptr_t)regs->rsi,
                                      (uint64_t *)(uintptr_t)regs->rdx);
        case SYS_INSTALL_PARTITIONS:
            return sys_install_partitions((const syscall_install_plan_t *)(uintptr_t)regs->rdi,
                                          (uint64_t *)(uintptr_t)regs->rsi,
                                          (uint64_t *)(uintptr_t)regs->rdx);
        case SYS_SET_PARTITION_ROLE:
            return sys_set_partition_role(regs->rdi, regs->rsi);
        case SYS_RUNTIME_DEVICE:
            return sys_runtime_device();
        case SYS_HTTP_GET_IPV4:
            return sys_http_get_ipv4(regs->rdi,
                                     regs->rsi,
                                     (const char *)(uintptr_t)regs->rdx,
                                     (const char *)(uintptr_t)regs->r10,
                                     (const char *)(uintptr_t)regs->r8,
                                     (uint64_t *)(uintptr_t)regs->r9);
        case SYS_DNS_RESOLVE:
            return sys_dns_resolve((const char *)(uintptr_t)regs->rdi,
                                   (uint32_t *)(uintptr_t)regs->rsi);
        case SYS_HTTPS_GET_IPV4:
            return sys_https_get_ipv4(regs->rdi,
                                      regs->rsi,
                                      (const char *)(uintptr_t)regs->rdx,
                                      (const char *)(uintptr_t)regs->r10,
                                      (const char *)(uintptr_t)regs->r8,
                                      (uint64_t *)(uintptr_t)regs->r9);

        /* ---- IPC / GUI syscalls ---- */
        case SYS_SHM_CREATE:
            return shm_create(regs->rdi);
        case SYS_SHM_MAP:
            return shm_map(regs->rdi);
        case SYS_SHM_UNMAP:
            return (uint64_t)shm_unmap(regs->rdi);
        case SYS_SHM_CLOSE:
            return (uint64_t)shm_close(regs->rdi);
        case SYS_MSG_OPEN: {
            const char *name = (const char *)(uintptr_t)regs->rdi;
            if (!name) {
                return (uint64_t)-1;
            }
            if (strnlen_user(name, 63) == (uint64_t)-1) {
                return (uint64_t)-U_EFAULT;
            }
            return msgq_open(name);
        }
        case SYS_MSG_SEND: {
            const void *msg = (const void *)(uintptr_t)regs->rsi;
            if (!msg) {
                return (uint64_t)-1;
            }
            if (!user_range_prepare_cur(msg, 64)) {
                return (uint64_t)-U_EFAULT;
            }
            return (uint64_t)msgq_send(regs->rdi, msg);
        }
        case SYS_MSG_RECV: {
            void *out = (void *)(uintptr_t)regs->rsi;
            if (!out) {
                return (uint64_t)-1;
            }
            if (!user_range_prepare_cur_w(out, 64)) {
                return (uint64_t)-U_EFAULT;
            }
            return (uint64_t)msgq_recv(regs->rdi, out, (int)regs->rdx);
        }
        case SYS_MSG_POLL:
            return (uint64_t)msgq_poll(regs->rdi);
        case SYS_MAP_FRAMEBUFFER: {
            syscall_fb_info_t *info = (syscall_fb_info_t *)(uintptr_t)regs->rdi;
            /* If the previous claimant is gone (killed, crashed, or exited
             * via a VT switch), let the new process take over the screen. */
            fb_release_if_owner_gone();
            if (fb_claimed) return (uint64_t)-1;
            if (info && !user_range_prepare_cur_w(info, sizeof(*info))) {
                return (uint64_t)-U_EFAULT;
            }
            if (!fb_available()) return (uint64_t)-1;
            process_t *fproc = sched_current_process();
            if (!fproc || !fproc->addr_space) return (uint64_t)-1;
            uint64_t fb_phys = fb_phys_addr();
            uint64_t fb_size = fb_phys_size();
            if (!fb_phys || !fb_size) return (uint64_t)-1;
            uint64_t fb_virt = 0x500000000ULL;
            uint64_t page_offset = fb_phys & 0xFFFULL;
            uint64_t fb_phys_aligned = fb_phys & ~0xFFFULL;
            uint64_t pages = (fb_size + page_offset + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
            for (uint64_t pi = 0; pi < pages; pi++) {
                if (vmm_map_page(fproc->addr_space,
                                 fb_virt + pi * PAGE_SIZE_4K,
                                 fb_phys_aligned + pi * PAGE_SIZE_4K,
                                 VMM_FLAGS_USER_RW) != 0) {
                    return (uint64_t)-1;
                }
            }
            if (info) {
                info->virt_addr = fb_virt + page_offset;
                info->width     = fb_width;
                info->height    = fb_height;
                info->pitch     = (fb_height > 0) ? (uint32_t)(fb_size / (uint64_t)fb_height) : 0;
                /* Report the real pixel format.  The window manager blits
                 * into this mapping, so it must know whether it is 32bpp
                 * (typical on real GPUs) or 24bpp (QEMU/GRUB fallbacks). */
                info->bpp       = (uint32_t)fb_bpp_value();
            }
            /* Keep the PS/2 cursor position clamped to the real screen size */
            mouse_set_screen(fb_width, fb_height);
            fb_claimed = 1;
            fb_claim_pid = fproc->pid;
            return fb_virt + page_offset;
        }
        case SYS_INPUT_READ_MOUSE: {
            syscall_mouse_event_t *out = (syscall_mouse_event_t *)(uintptr_t)regs->rdi;
            if (!out) return (uint64_t)-1;
            if (!user_range_prepare_cur_w(out, sizeof(*out))) {
                return (uint64_t)-U_EFAULT;
            }
            mouse_event_t ev;
            if (mouse_read_event(&ev) != 0) return (uint64_t)-1;
            out->abs_x   = ev.abs_x;
            out->abs_y   = ev.abs_y;
            out->dx      = ev.dx;
            out->dy      = ev.dy;
            out->buttons = ev.buttons;
            return 0;
        }
        case SYS_GUI_AVAILABLE:
            /* 1 once the window manager has claimed the framebuffer, so
             * GUI-capable apps know the desktop is on screen. */
            fb_release_if_owner_gone();
            return fb_claimed ? 1 : 0;
        case SYS_GPU_QUERY:
            return sys_gpu_query((syscall_gpu_info_t *)(uintptr_t)regs->rdi);
        case SYS_GPU_PRESENT:
            return sys_gpu_present();
        case SYS_GPU_CURSOR:
            return sys_gpu_cursor((int)regs->rdi, (int)regs->rsi,
                                  (const uint32_t *)(uintptr_t)regs->rdx,
                                  (int)regs->r10, (int)regs->r8);
        case SYS_POWER:
            return sys_power(regs->rdi);
        case SYS_PROC_STATS:
            return sys_proc_stats(regs->rdi,
                                  (syscall_proc_stats_t *)(uintptr_t)regs->rsi);
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
