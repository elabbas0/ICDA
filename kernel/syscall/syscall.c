#include "syscall.h"
#include "../drivers/serial/serial.h"

#include "../drivers/display/framebuffer.h"
#include "../power/power.h"
#include "../drivers/input/mouse.h"
#include "../drivers/audio/speaker.h"
#include "../drivers/audio/hda.h"
#include "../drivers/audio/playback.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "../fs/diskfmt.h"
#include "../fs/fatfs.h"
#include "../fs/volumes.h"
#include "../firmware/efi.h"
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
#include "../net/sock.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"
#include "../fs/fd.h"
#include "../cpu/gdt.h"
#include "../linux/lx.h"
#include "../dev/devops.h"
#include "uaccess.h"
#include "native_abi.h"
#include "../tty/pty.h"
#include "../fs/sysupdate.h"



#ifndef SERIAL_VERBOSE
#define SERIAL_VERBOSE 0
#endif




_Static_assert(SYS_CONSOLE_WRITE == 0, "native ABI v9: first number moved");
_Static_assert(SYS_VM_FREE == 75, "native ABI v9: v4 numbers moved");
_Static_assert(SYS_DISK_EDIT == 76, "native ABI v9: v5 numbers moved");
_Static_assert(SYS_VFS_TRUNCATE == 78, "native ABI v9: v6 numbers moved");
_Static_assert(SYS_VFS_RENAME == 79, "native ABI v9: v7 numbers moved");
_Static_assert(SYS_NET == 80, "native ABI v9: v8 numbers moved");
_Static_assert(SYS_AUDIO_MIX == 81, "native ABI v9: last number moved");
_Static_assert(ICDA_NATIVE_SYS_MAX == 82, "native ABI v9: count changed");




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

static int cur_pty(void) {
    process_t *p = sched_current_process();
    return (p && p->pty && pty_alive(p->pty)) ? p->pty : 0;
}

static void pty_puts(int pty, const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    pty_slave_write(pty, s, n);
}

static uint64_t sys_console_write(const char *text) {
    const dev_calls_t *dcon;
    uint64_t len;

    if (!text) {
        return (uint64_t)-1;
    }
    

    len = strnlen_user(text, UACCESS_MAX_STR);
    if (len == (uint64_t)-1) {
        return (uint64_t)-U_EFAULT;
    }
    if (cur_pty()) {
        pty_slave_write(cur_pty(), text, len);
        return len;
    }
    dcon = dev_console();
    if (!dcon) {
        return (uint64_t)-1;
    }
    return dcon->con_write(text);
}

static uint64_t sys_get_pid(void) {
    process_t *proc = sched_current_process();
    return proc ? proc->pid : 0;
}


#if SERIAL_VERBOSE
static void ident_log_u64(uint64_t v) {
    char buf[21];
    int i = 0;
    int a;
    int b;
    char t;

    if (v == 0) {
        serial_write("0");
        return;
    }
    while (v > 0 && i < 20) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    buf[i] = '\0';
    for (a = 0, b = i - 1; a < b; a++, b--) {
        t = buf[a];
        buf[a] = buf[b];
        buf[b] = t;
    }
    serial_write(buf);
}
#endif


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

    
    {
        const dev_calls_t *node = path[0] == '/' ? devops_lookup(path) : 0;
        if (node && node->node_read) {
            /* Small nodes (rtc) use the stack; larger reports (/dev/wifi)
             * get a heap snapshot of up to 32 KiB. */
            char small[64];
            uint64_t snapcap = cap < 32768 ? cap : 32768;
            char *snap = snapcap <= sizeof(small) ? small : (char *)kmalloc(snapcap);
            if (!snap) {
                return (uint64_t)-1;
            }
            size = node->node_read(snap, snapcap);
            if (size >= cap) {
                size = cap - 1;
            }
            copy_bytes(buf, snap, size);
            buf[size] = '\0';
            if (snap != small) {
                kfree(snap);
            }
            return size;
        }
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

    /* /dev/serial: test output for the host (the serial log) */
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v' && path[4] == '/' &&
        path[5] == 's' && path[6] == 'e' && path[7] == 'r' && path[8] == 'i' && path[9] == 'a' &&
        path[10] == 'l' && path[11] == 0) {
        for (uint64_t i = 0; i < size; i++) serial_write_char(buf[i]);
        return size;
    }

    /* /dev/sysupdate takes whole patch chunks ("put"), not just commands */
    {
        const char *s = "/dev/sysupdate";
        uint64_t i = 0;
        while (s[i] && path[i] == s[i]) i++;
        if (!s[i] && !path[i]) return sysupdate_write_user(buf, size);
    }

    /* Device nodes that take commands (/dev/wifi) */
    {
        const dev_calls_t *node = path[0] == '/' ? devops_lookup(path) : 0;
        if (node && node->node_write) {
            char cmd[256];
            uint64_t n = size < sizeof(cmd) ? size : sizeof(cmd);
            uint64_t rc;
            copy_bytes(cmd, buf, n);
            rc = node->node_write(cmd, n);
            zero_bytes(cmd, sizeof(cmd));
            return rc == (uint64_t)-1 ? (uint64_t)-1 : size;
        }
    }

    if (vfs_write(proc->cwd ? proc->cwd : vfs_root(), path, buf ? buf : "", size) != 0) {
        return (uint64_t)-1;
    }

    return size;
}

static uint64_t sys_vfs_write_at(const char *path, uint64_t off, const char *buf, uint64_t size) {
    process_t *proc = sched_current_process();
    vfs_node_t *node;

    if (!proc || !path || (!buf && size != 0)) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    if (size != 0 && !user_range_prepare_cur(buf, size)) {
        return (uint64_t)-U_EFAULT;
    }
    node = vfs_resolve(proc->cwd ? proc->cwd : vfs_root(), path);
    if (!node || vfs_node_write_at(node, off, buf, size) != 0) {
        return (uint64_t)-1;
    }
    return size;
}

static uint64_t sys_vfs_truncate(const char *path, uint64_t len) {
    process_t *proc = sched_current_process();
    vfs_node_t *node;

    if (!proc || !path) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(path)) {
        return (uint64_t)-U_EFAULT;
    }
    node = vfs_resolve(proc->cwd ? proc->cwd : vfs_root(), path);
    return node && vfs_node_truncate(node, len) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_vfs_rename(const char *from, const char *to) {
    process_t *proc = sched_current_process();

    if (!proc || !from || !to) {
        return (uint64_t)-1;
    }
    if (!gate_path_ok(from) || !gate_path_ok(to)) {
        return (uint64_t)-U_EFAULT;
    }
    return vfs_rename(proc->cwd ? proc->cwd : vfs_root(), from, to) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_exit(uint64_t code) {
    user_request_exit_to_kernel(code);
    return code;
}

static uint64_t sys_vfs_read_at(const char *path, uint64_t offset, char *buf, uint64_t cap) {
    process_t *proc = sched_current_process();
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

    {
        vfs_node_t *node = vfs_resolve(proc->cwd ? proc->cwd : vfs_root(), path);
        int64_t got;
        if (!node) {
            return 0;
        }
        got = vfs_node_read_at(node, offset, buf, cap);
        chunk = got < 0 ? 0 : (uint64_t)got;
    }
    return chunk;
}

static uint64_t sys_input_read(void) {
    const dev_calls_t *din = dev_input();
    int c;
    if (cur_pty()) {
        c = pty_slave_read_char(cur_pty());
        return c < 0 ? (uint64_t)-1 : (uint64_t)c;
    }
    if (!din) {
        return (uint64_t)-1;
    }
    c = din->in_read_char();
    if (c < 0) {
        return (uint64_t)-1;
    }
    return (uint64_t)(uint8_t)c;
}

static uint64_t sys_input_read_timeout(uint64_t ticks) {
    const dev_calls_t *din = dev_input();
    int c;
    if (cur_pty()) {
        int pty = cur_pty();
        c = pty_slave_read_char(pty);
        for (uint64_t t = 0; c < 0 && t < ticks && pty_alive(pty); t++) {
            sched_sleep(1);
            c = pty_slave_read_char(pty);
        }
        return c < 0 ? (uint64_t)-1 : (uint64_t)c;
    }
    if (!din) {
        return (uint64_t)-1;
    }
    c = din->in_read_char();
    if (c >= 0) {
        return (uint64_t)(uint8_t)c;
    }

    sched_wait_input_timeout(ticks);
    c = din->in_read_char();
    if (c < 0) {
        return (uint64_t)-1;
    }
    return (uint64_t)(uint8_t)c;
}

static uint64_t sys_input_readline(char *buf, uint64_t cap) {
    const dev_calls_t *din = dev_input();
    const dev_calls_t *dcon = dev_console();
    uint64_t len = 0;

    if (!buf || cap == 0) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(buf, cap)) {
        return (uint64_t)-U_EFAULT;
    }
    if (cur_pty()) {
        int pty = cur_pty();
        buf[0] = 0;
        for (;;) {
            int c = pty_slave_read_char(pty);
            char ch;
            if (c < 0) {
                if (!pty_alive(pty)) return (uint64_t)-1;
                sched_sleep(1);
                continue;
            }
            if (c == '\r' || c == '\n') {
                pty_puts(pty, "\r\n");
                buf[len] = 0;
                return len;
            }
            if (c == 8 || c == 127) {
                if (len > 0) {
                    buf[--len] = 0;
                    pty_puts(pty, "\b \b");
                }
                continue;
            }
            if (c < 32 || len + 1 >= cap) continue;
            ch = (char)c;
            buf[len++] = ch;
            buf[len] = 0;
            pty_slave_write(pty, &ch, 1);
        }
    }
    if (!din || !dcon || !dcon->con_write || !dcon->con_backspace) {
        return (uint64_t)-1;
    }

    buf[0] = '\0';

    for (;;) {
        int c;
        char out[2];

        while ((c = din->in_read_char()) < 0) {
            sched_sleep(1);
        }

        if (c == '\r' || c == '\n') {
            dcon->con_write("\n");
            buf[len] = '\0';
            return len;
        }

        if (c == '\b') {
            if (len > 0) {
                len--;
                buf[len] = '\0';
                dcon->con_backspace();
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
        dcon->con_write(out);
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
    const dev_calls_t *dcon = dev_console();
    if (cur_pty()) {
        pty_puts(cur_pty(), "\x1b[2J\x1b[H");
        return 0;
    }
    if (!dcon || !dcon->con_clear) {
        return (uint64_t)-1;
    }
    dcon->con_clear();
    return 0;
}

static uint64_t sys_console_backspace(void) {
    const dev_calls_t *dcon = dev_console();
    if (cur_pty()) {
        pty_puts(cur_pty(), "\b \b");
        return 0;
    }
    if (!dcon || !dcon->con_backspace) {
        return (uint64_t)-1;
    }
    dcon->con_backspace();
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


#define UVM_ANON_BASE 0x70000000ULL
#define UVM_ANON_END  0x500000000ULL

static uint64_t uvm_map_anon(process_t *proc, uint64_t addr, uint64_t length, int writable) {
    uint64_t pages, size;
    if (!proc || !proc->addr_space || length == 0) return (uint64_t)-1;
    pages = (length + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    size = pages * PAGE_SIZE_4K;
    if (addr == 0) {
        if (proc->linux_mmap_next == 0) proc->linux_mmap_next = UVM_ANON_BASE;
        addr = proc->linux_mmap_next;
        if (addr + size < addr || addr + size > UVM_ANON_END) return (uint64_t)-1;
        proc->linux_mmap_next += size;
    }
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc();
        char *dst;
        if (!phys || vmm_map_page(proc->addr_space, addr + i * PAGE_SIZE_4K, phys,
                                  writable ? VMM_FLAGS_USER_RW : VMM_FLAGS_USER_RO) != 0) {
            if (phys) pmm_free(phys);
            for (uint64_t k = 0; k < i; k++) vmm_unmap_page(proc->addr_space, addr + k * PAGE_SIZE_4K, 1);
            return (uint64_t)-1;
        }
        dst = (char *)PHYS_TO_VIRT(phys);
        for (int j = 0; j < (int)PAGE_SIZE_4K; j++) dst[j] = 0;
    }
    return addr;
}

static uint64_t sys_vm_alloc(uint64_t length) {
    process_t *proc = sched_current_process();
    if (length == 0 || length > UVM_ANON_END - UVM_ANON_BASE) return (uint64_t)-U_EINVAL;
    return uvm_map_anon(proc, 0, length, 1);
}

static uint64_t sys_vm_free(uint64_t addr, uint64_t length) {
    process_t *proc = sched_current_process();
    uint64_t end = addr + length;
    if (!proc || !proc->addr_space || length == 0 || (addr & 0xFFFULL)) return (uint64_t)-U_EINVAL;
    if (addr < UVM_ANON_BASE || end < addr || end > proc->linux_mmap_next) return (uint64_t)-U_EINVAL;
    for (uint64_t page = addr; page < end; page += PAGE_SIZE_4K) {
        if (vmm_virt_to_phys(proc->addr_space, page)) vmm_unmap_page(proc->addr_space, page, 1);
    }
    return 0;
}


typedef struct {
    uint32_t op;
    uint32_t device;
    uint32_t partition;
    uint32_t role;
    uint32_t fs;
    uint32_t reserved;
    uint64_t start_lba;
    uint64_t sectors;
    char     name[36];
} disk_edit_req_t;

static uint64_t sys_disk_edit(void *user_req) {
    disk_edit_req_t req;
    int rc = -1;
    if (!user_req || !user_range_prepare_cur_w(user_req, sizeof(req))) return (uint64_t)-U_EFAULT;
    copy_bytes((char *)&req, (const char *)user_req, sizeof(req));
    req.name[sizeof(req.name) - 1] = 0;
    switch (req.op) {
    case 1:
        rc = diskfmt_create_partition(req.device, req.start_lba, req.sectors, (partition_role_t)req.role,
                                      (diskfmt_fs_t)req.fs, req.name);
        break;
    case 2:
        rc = diskfmt_delete_partition(req.partition);
        break;
    case 3:
        rc = diskfmt_resize_partition(req.partition, req.sectors);
        break;
    case 4: {
        const partition_info_t *part = partition_get(req.partition);
        fatfs_t vol;
        uint32_t free_n = 0, highest = 0;
        if (!part || part->fs_hint != PARTITION_FS_FAT32 || fatfs_mount_part(&vol, part) != 0) {
            rc = -2;
            break;
        }
        fatfs_usage(&vol, &free_n, &highest);
        req.start_lba = (uint64_t)(vol.cluster_count - free_n) * vol.cluster_bytes;
        req.sectors = (uint64_t)vol.cluster_count * vol.cluster_bytes;
        rc = 0;
        break;
    }
    case 5:
        rc = efi_available() ? 1 : 0;
        break;
    case 6: {
        install_status_t st;
        uint64_t n = 0;
        install_status_get(&st);
        req.role = (uint32_t)st.active;
        req.partition = (uint32_t)st.finished;
        req.fs = (uint32_t)st.rc;
        req.start_lba = st.current;
        req.sectors = st.total;
        for (uint64_t i = 0; st.stage[i] && n + 1 < sizeof(req.name); i++) req.name[n++] = st.stage[i];
        req.name[n] = 0;
        rc = 0;
        break;
    }
    default:
        return (uint64_t)-U_EINVAL;
    }
    copy_bytes((char *)user_req, (const char *)&req, sizeof(req));
    return (uint64_t)(int64_t)rc;
}

static uint64_t sys_vfs_remove(const char *path) {
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
    return vfs_remove(proc->cwd ? proc->cwd : vfs_root(), path) == 0 ? 0 : (uint64_t)-1;
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
    const dev_calls_t *dfb = dev_fb();

    if (!out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(out, sizeof(*out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (!dfb || !dfb->gpu_query) {
        return (uint64_t)-1;
    }
    return dfb->gpu_query(out) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_gpu_present(uint64_t flags) {
    const dev_calls_t *dfb = dev_fb();
    if (!dfb || !dfb->gpu_present) {
        return (uint64_t)-1;
    }
    if (dfb->gpu_present() != 0) {
        return (uint64_t)-1;
    }
    


    if (flags & 1) {
        sched_yield();
    }
    return 0;
}

static uint64_t sys_gpu_cursor(int x, int y, const uint32_t *image, int w, int h) {
    const dev_calls_t *dfb = dev_fb();
    uint64_t pixels;
    if (!dfb || !dfb->gpu_set_cursor) {
        return (uint64_t)-1;
    }
    if (w < 0 || h < 0) {
        return (uint64_t)-U_EINVAL;
    }
    pixels = (uint64_t)w * (uint64_t)h;
    
    if (pixels > UACCESS_MAX_LEN / 4) {
        return (uint64_t)-U_EINVAL;
    }
    if (pixels != 0 && !image) {
        return (uint64_t)-U_EINVAL;
    }
    if (image && !user_range_prepare_cur(image, pixels * 4)) {
        return (uint64_t)-U_EFAULT;
    }
    return dfb->gpu_set_cursor(x, y, image, w, h) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_power(uint64_t action) {
    (void)vfs_flush(1);
    /* a downloaded patch is installed only here, with nothing running */
    sysupdate_apply();

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
    return vfs_flush(1) == 0 ? 0 : (uint64_t)-1;
}

static uint64_t sys_pty_open(void) {
    int id = pty_open(sched_current_process());
    return id < 0 ? (uint64_t)-1 : (uint64_t)id;
}

static uint64_t sys_pty_spawn(uint64_t id, const char *path, const char *args) {
    process_t *proc = sched_current_process();
    uint64_t pid = 0;
    int saved;
    int rc;
    if (!proc || !pty_owned_by((int)id, proc) || !path) return (uint64_t)-1;
    if (!gate_path_ok(path) || !*path) return (uint64_t)-U_EFAULT;
    if (args && strnlen_user(args, 2048) == (uint64_t)-1) return (uint64_t)-U_EFAULT;
    saved = proc->pty;
    proc->pty = (int)id;
    rc = args ? user_spawn_path_args(path, args, &pid) : user_spawn_path(path, &pid);
    proc->pty = saved;
    return rc != 0 ? (uint64_t)-1 : pid;
}

static uint64_t sys_pty_io(uint64_t id, uint64_t op, char *buf, uint64_t len) {
    process_t *proc = sched_current_process();
    char kbuf[1024];
    uint64_t done = 0;
    if (!proc || !pty_owned_by((int)id, proc)) return op == 3 ? 0 : (uint64_t)-1;
    if (op == 2) {
        pty_set_size((int)id, (uint32_t)(len & 0xFFFF), (uint32_t)((len >> 16) & 0xFFFF));
        return 0;
    }
    if (op == 3) return 1;
    if (op > 3 || (!buf && len)) return (uint64_t)-1;
    if (op == 0) {
        if (!user_range_prepare_cur_w(buf, len)) return (uint64_t)-U_EFAULT;
        while (done < len) {
            uint64_t want = len - done < sizeof(kbuf) ? len - done : sizeof(kbuf);
            uint64_t got = pty_master_read((int)id, kbuf, want);
            if (!got) break;
            if (copy_to_user(buf + done, kbuf, got) != 0) return (uint64_t)-U_EFAULT;
            done += got;
        }
        return done;
    }
    if (!user_range_prepare_cur(buf, len)) return (uint64_t)-U_EFAULT;
    while (done < len) {
        uint64_t chunk = len - done < sizeof(kbuf) ? len - done : sizeof(kbuf);
        uint64_t put;
        if (copy_from_user(kbuf, buf + done, chunk) != 0) return (uint64_t)-U_EFAULT;
        put = pty_master_write((int)id, kbuf, chunk);
        done += put;
        if (put < chunk) break;
    }
    return done;
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
    


#if SERIAL_VERBOSE
    {
        int pi = 0;
        serial_write("[ident] op=mount pid=");
        ident_log_u64(proc->pid);
        serial_write(" uid=");
        ident_log_u64(proc->ex_uid);
        serial_write(" part=");
        ident_log_u64(partition_index);
        serial_write(" path=");
        while (pi < 64 && path[pi]) {
            serial_write_char(path[pi]);
            pi++;
        }
        serial_write("\n");
    }
#endif
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
    if (rc != 0) {
        char msg[40] = "format_device rc=-";
        int v = -rc, n = 18;
        if (v >= 10) msg[n++] = (char)('0' + v / 10);
        msg[n++] = (char)('0' + v % 10);
        msg[n++] = '\n';
        msg[n] = 0;
        serial_write(msg);
    }
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
    install_status_finish(rc);
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
    install_status_finish(rc);
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
    const dev_calls_t *dcon = dev_console();
    if (cur_pty()) {
        char seq[32];
        uint64_t n = 0;
        char d[24];
        int k;
        seq[n++] = 0x1b;
        seq[n++] = '[';
        k = 0; y++; do { d[k++] = (char)('0' + y % 10); y /= 10; } while (y && k < 20);
        while (k) seq[n++] = d[--k];
        seq[n++] = ';';
        k = 0; x++; do { d[k++] = (char)('0' + x % 10); x /= 10; } while (x && k < 20);
        while (k) seq[n++] = d[--k];
        seq[n++] = 'H';
        pty_slave_write(cur_pty(), seq, n);
        return 0;
    }
    if (!dcon || !dcon->con_set_cursor) {
        return (uint64_t)-1;
    }
    dcon->con_set_cursor((int)x, (int)y);
    return 0;
}

static uint64_t sys_console_size(uint64_t *cols_out, uint64_t *rows_out) {
    const dev_calls_t *dcon = dev_console();

    if (!cols_out || !rows_out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(cols_out, sizeof(*cols_out)) ||
        !user_range_prepare_cur_w(rows_out, sizeof(*rows_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (cur_pty()) {
        uint32_t c, r;
        pty_get_size(cur_pty(), &c, &r);
        *cols_out = c;
        *rows_out = r;
        return 0;
    }
    if (!dcon || !dcon->con_columns || !dcon->con_rows) {
        return (uint64_t)-1;
    }
    *cols_out = (uint64_t)dcon->con_columns();
    *rows_out = (uint64_t)dcon->con_rows();
    return 0;
}

static uint64_t sys_console_get_cursor(uint64_t *x_out, uint64_t *y_out) {
    const dev_calls_t *dcon = dev_console();
    int x = 0;
    int y = 0;
    if (!x_out || !y_out) {
        return (uint64_t)-1;
    }
    if (!user_range_prepare_cur_w(x_out, sizeof(*x_out)) ||
        !user_range_prepare_cur_w(y_out, sizeof(*y_out))) {
        return (uint64_t)-U_EFAULT;
    }
    if (cur_pty()) {
        return (uint64_t)-1;
    }
    if (!dcon || !dcon->con_get_cursor) {
        return (uint64_t)-1;
    }
    dcon->con_get_cursor(&x, &y);
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

    out = append_text(buf, out, cap, "free:\n");
    for (uint32_t i = 0; i < block_count(); i++) {
        diskfmt_region_t regions[16];
        uint32_t n = diskfmt_free_regions(i, regions, 16);
        for (uint32_t r = 0; r < n; r++) {
            out = append_text(buf, out, cap, "  dev=");
            out = append_uint(buf, out, cap, i);
            out = append_text(buf, out, cap, " start=");
            out = append_uint(buf, out, cap, regions[r].start_lba);
            out = append_text(buf, out, cap, " sectors=");
            out = append_uint(buf, out, cap, regions[r].sectors);
            out = append_text(buf, out, cap, "\n");
        }
    }
    out = append_text(buf, out, cap, "firmware=");
    out = append_text(buf, out, cap, efi_available() ? "uefi" : "bios");
    out = append_text(buf, out, cap, "\n");

    out = append_text(buf, out, cap, "mounts:\n");
    if (fat32_mount_count() == 0 && exfat_mount_count() == 0 && ntfs_mount_count() == 0) {
        out = append_text(buf, out, cap, "  (none)\n");
    } else {
        for (uint32_t i = 0; i < fat32_mount_count(); i++) {
            out = append_text(buf, out, cap, "  /volumes/fat32-");
            out = append_uint(buf, out, cap, i);
            out = append_text(buf, out, cap, volumes_is_writable(i, VOLUME_FAT32) ? "\n" : " (ro)\n");
        }
        for (uint32_t i = 0; i < exfat_mount_count(); i++) {
            out = append_text(buf, out, cap, "  /volumes/exfat-");
            out = append_uint(buf, out, cap, i);
            out = append_text(buf, out, cap, volumes_is_writable(i, VOLUME_EXFAT) ? "\n" : " (ro)\n");
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
    

    if (ticks > 500U) {
        ticks = 500U;
    }
    if (frequency_hz > 0xFFFFFFFFU) {
        frequency_hz = 0xFFFFFFFFU;
    }
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
    

#if SERIAL_VERBOSE
    serial_write("[ident] op=audio-claim pid=");
    ident_log_u64(proc->pid);
    serial_write(" uid=");
    ident_log_u64(proc->ex_uid);
    serial_write(" tok=");
    ident_log_u64(proc->ex_token);
    serial_write("\n");
#endif
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

/* The mixer (drivers/audio/playback.c): effects and app PCM streams. */
static uint64_t sys_audio_mix(uint64_t op, uint64_t a, uint64_t b, uint64_t c) {
    switch (op) {
    case MIX_EFFECT: {
        const char *path = (const char *)(uintptr_t)a;
        if (!path || !gate_path_ok(path)) return (uint64_t)-U_EFAULT;
        return audio_effect_play(path, (uint32_t)b) == 0 ? 0 : (uint64_t)-1;
    }
    case MIX_STREAM_OPEN:
        return (uint64_t)(int64_t)audio_stream_open((uint32_t)a, (uint32_t)b);
    case MIX_STREAM_WRITE:
        if (!b || c == 0 || c > (16ULL << 20)) return (uint64_t)-1;
        if (!user_range_prepare_cur((const void *)(uintptr_t)b, c)) return (uint64_t)-U_EFAULT;
        return (uint64_t)audio_stream_write((int)a, (const int16_t *)(uintptr_t)b, c);
    case MIX_STREAM_POSITION:
        return (uint64_t)audio_stream_position((int)a);
    case MIX_STREAM_QUEUED:
        return (uint64_t)audio_stream_queued((int)a);
    case MIX_STREAM_CONTROL:
        return audio_stream_control((int)a, (int)b, (uint32_t)c) == 0 ? 0 : (uint64_t)-1;
    case MIX_STREAM_CLOSE:
        audio_stream_close((int)a);
        return 0;
    default:
        return (uint64_t)-1;
    }
}




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






static uint64_t syscall_dispatch_native(struct registers *regs);

uint64_t syscall_dispatch(struct registers *regs) {
    process_t *proc = sched_current_process();
    (void)vfs_flush(0);
    if (proc && proc->linux_personality) {
        /* the gateway to ICDA's own calls (windows, shared memory, input) for
         * Linux programs that draw on ICDA's desktop: native number + 0x1C000 */
        if (regs->rax >= 0x1C000 && regs->rax < 0x1C000 + 1024) {
            regs->rax -= 0x1C000;
            return syscall_dispatch_native(regs);
        }
        return lx_syscall(regs);
    }
    return syscall_dispatch_native(regs);
}

static uint64_t syscall_dispatch_native(struct registers *regs) {
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
            const dev_calls_t *dfb = dev_fb();
            

            if (info && !user_range_prepare_cur_w(info, sizeof(*info))) {
                return (uint64_t)-U_EFAULT;
            }
            if (!dfb || !dfb->fb_claim_map) {
                return (uint64_t)-1;
            }
            return dfb->fb_claim_map(info);
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
            out->dz      = ev.dz;
            return 0;
        }
        case SYS_GUI_AVAILABLE: {
            const dev_calls_t *dfb = dev_fb();
            

            if (!dfb || !dfb->fb_claimed) {
                return (uint64_t)-1;
            }
            return (uint64_t)dfb->fb_claimed();
        }
        case SYS_GPU_QUERY:
            return sys_gpu_query((syscall_gpu_info_t *)(uintptr_t)regs->rdi);
        case SYS_GPU_PRESENT:
            

            return sys_gpu_present(regs->rdi & 0xFF);
        case SYS_GPU_CURSOR:
            return sys_gpu_cursor((int)regs->rdi, (int)regs->rsi,
                                  (const uint32_t *)(uintptr_t)regs->rdx,
                                  (int)regs->r10, (int)regs->r8);
        case SYS_POWER:
            return sys_power(regs->rdi);
        case SYS_PTY_OPEN:
            return sys_pty_open();
        case SYS_PTY_SPAWN:
            return sys_pty_spawn(regs->rdi, (const char *)(uintptr_t)regs->rsi,
                                 (const char *)(uintptr_t)regs->rdx);
        case SYS_PTY_IO:
            return sys_pty_io(regs->rdi, regs->rsi, (char *)(uintptr_t)regs->rdx, regs->r10);
        case SYS_VFS_REMOVE:
            return sys_vfs_remove((const char *)(uintptr_t)regs->rdi);
        case SYS_VM_ALLOC:
            return sys_vm_alloc(regs->rdi);
        case SYS_VM_FREE:
            return sys_vm_free(regs->rdi, regs->rsi);
        case SYS_DISK_EDIT:
            return sys_disk_edit((void *)(uintptr_t)regs->rdi);
        case SYS_VFS_WRITE_AT:
            return sys_vfs_write_at((const char *)(uintptr_t)regs->rdi, regs->rsi,
                                    (const char *)(uintptr_t)regs->rdx, regs->r10);
        case SYS_VFS_TRUNCATE:
            return sys_vfs_truncate((const char *)(uintptr_t)regs->rdi, regs->rsi);
        case SYS_VFS_RENAME:
            return sys_vfs_rename((const char *)(uintptr_t)regs->rdi, (const char *)(uintptr_t)regs->rsi);
        case SYS_NET:
            return (uint64_t)sock_syscall(regs->rdi, regs->rsi, regs->rdx, regs->r10, regs->r8, regs->r9);
        case SYS_AUDIO_MIX:
            return sys_audio_mix(regs->rdi, regs->rsi, regs->rdx, regs->r10);
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
