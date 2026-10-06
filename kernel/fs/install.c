#include "install.h"

#include "boot_assets.h"
#include "diskfmt.h"
#include "fatfs.h"
#include "initramfs.h"
#include "persistfs.h"
#include "vfs.h"

#include "../drivers/console/console.h"
#include "../drivers/display/framebuffer.h"
#include "../drivers/display/vga.h"
#include "../drivers/storage/block.h"
#include "../drivers/storage/partition.h"
#include "../drivers/serial/serial.h"
#include "../memory/heap.h"
#include "../firmware/efi.h"

#define ICDA_ROOT_BUNDLE_NAME "ICDAROOT.BIN"
#define ICDA_ROOT_CFG_NAME    "ICDACFG.TXT"


static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) {
        len++;
    }
    return len;
}

static uint64_t append_text(char *buf, uint64_t out, uint64_t cap, const char *text);
static uint64_t append_uint(char *buf, uint64_t out, uint64_t cap, uint64_t value);
static void install_progress(const char *stage, const char *detail, uint64_t current, uint64_t total);
static void install_progress_done(void);

static void copy_bytes(char *dst, const char *src, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static uint64_t min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

static int install_console_cols(void) {
    if (fb_available()) {
        return fb_columns();
    }
    return VGA_WIDTH;
}

static int install_console_rows(void) {
    if (fb_available()) {
        return fb_rows();
    }
    return VGA_HEIGHT;
}

static void install_fill_line(char *dst, uint64_t cap, char fill) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    for (; i + 1 < cap; i++) {
        dst[i] = fill;
    }
    dst[i] = '\0';
}

static void install_write_line(int col, int row, int width, const char *text, console_style_t style) {
    char line[160];
    uint64_t len = str_len(text);
    uint64_t copy = min_u64((uint64_t)width, len);

    if (width <= 0) return;
    install_fill_line(line, sizeof(line), ' ');
    if ((uint64_t)width >= sizeof(line)) {
        width = (int)sizeof(line) - 1;
    }
    for (uint64_t i = 0; i < copy; i++) {
        line[i] = text[i];
    }
    line[width] = '\0';
    console_set_cursor(col, row);
    console_write(line, style);
}

static install_status_t install_status;

static void install_status_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void install_status_note(const char *stage, const char *detail, uint64_t current, uint64_t total) {
    install_status.active = 1;
    if (stage) install_status_copy(install_status.stage, stage, sizeof(install_status.stage));
    if (detail) install_status_copy(install_status.detail, detail, sizeof(install_status.detail));
    install_status.current = current;
    install_status.total = total;
}

void install_status_get(install_status_t *out) {
    *out = install_status;
}

void install_status_finish(int rc) {
    install_status.active = 0;
    install_status.finished++;
    install_status.rc = rc;
}

static void install_progress(const char *stage, const char *detail, uint64_t current, uint64_t total) {
    char line[160];
    int cols = install_console_cols();
    int rows = install_console_rows();
    int width = cols > 72 ? 72 : cols - 4;
    int left;
    int top;
    int bar_width;
    uint64_t filled;

    install_status_note(stage, detail, current, total);
    if (console_fb_muted()) return;
    if (width < 32 || rows < 10) {
        return;
    }

    left = (cols - width) / 2;
    top = (rows - 7) / 2;
    bar_width = width - 16;
    if (bar_width < 10) bar_width = 10;

    console_clear();

    install_fill_line(line, sizeof(line), '=');
    line[width] = '\0';
    install_write_line(left, top + 0, width, line, CONSOLE_STYLE_MUTED);

    install_fill_line(line, sizeof(line), ' ');
    line[0] = '['; line[1] = ' ';
    {
        uint64_t out = 2;
        out = append_text(line, out, sizeof(line), "ICDA Installer");
        if (stage && stage[0]) {
            out = append_text(line, out, sizeof(line), "  ");
            out = append_text(line, out, sizeof(line), stage);
        }
    }
    line[width - 2] = ' ';
    line[width - 1] = ']';
    line[width] = '\0';
    install_write_line(left, top + 1, width, line, CONSOLE_STYLE_INFO);

    install_fill_line(line, sizeof(line), ' ');
    if (detail && detail[0]) {
        append_text(line, 0, sizeof(line), detail);
    }
    line[width] = '\0';
    install_write_line(left, top + 2, width, line, CONSOLE_STYLE_ACCENT);

    install_fill_line(line, sizeof(line), ' ');
    line[0] = '[';
    line[bar_width + 1] = ']';
    line[bar_width + 2] = ' ';
    if (total == 0) {
        total = 1;
    }
    if (current > total) {
        current = total;
    }
    filled = (current * (uint64_t)bar_width) / total;
    for (int i = 0; i < bar_width; i++) {
        line[1 + i] = ((uint64_t)i < filled) ? '#' : '-';
    }
    {
        uint64_t out = (uint64_t)(bar_width + 3);
        out = append_uint(line, out, sizeof(line), (current * 100U) / total);
        out = append_text(line, out, sizeof(line), "%");
    }
    line[width] = '\0';
    install_write_line(left, top + 4, width, line, CONSOLE_STYLE_OK);

    install_fill_line(line, sizeof(line), ' ');
    append_text(line, 0, sizeof(line), "Installing system files and boot assets...");
    line[width] = '\0';
    install_write_line(left, top + 6, width, line, CONSOLE_STYLE_MUTED);
}

static void install_progress_done(void) {
    console_clear();
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

static int parse_uint64(const char *text, uint64_t *out) {
    uint64_t value = 0;
    uint64_t i = 0;
    if (!text || !*text || !out) return 0;
    while (text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (uint64_t)(text[i] - '0');
        i++;
    }
    if (i == 0) return 0;
    *out = value;
    return 1;
}

static int str_prefix(const char *text, const char *prefix) {
    uint64_t i = 0;
    while (prefix[i]) {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int ensure_layout(void) {
    (void)vfs_mkdir(vfs_root(), "/system");
    (void)vfs_mkdir(vfs_root(), "/system/install");
    (void)vfs_mkdir(vfs_root(), "/home");
    (void)vfs_mkdir(vfs_root(), "/apps");
    (void)vfs_mkdir(vfs_root(), "/bin");
    (void)vfs_mkdir(vfs_root(), "/etc");
    (void)vfs_mkdir(vfs_root(), "/usr");
    (void)vfs_mkdir(vfs_root(), "/usr/share");
    (void)vfs_mkdir(vfs_root(), "/usr/share/audio");
    (void)vfs_mkdir(vfs_root(), "/volumes");
    return 0;
}

/* System files for OTA patches (see sysupdate.h): \SYSTEM gets the live
 * system's programs, and the kernel goes next to ICDAROOT.BIN, which is
 * where GRUB looks first and where patches replace it. */
static int install_copy_tree(fatfs_t *vol, vfs_node_t *node, char *vpath, uint64_t cap, uint32_t *files) {
    uint64_t base = 0;
    while (vpath[base]) base++;
    if (vfs_node_type(node) == VFS_NODE_DIR) {
        uint64_t count = vfs_child_count(node);
        for (uint64_t i = 0; i < count; i++) {
            vfs_node_t *child = vfs_child_at(node, i);
            const char *name = vfs_node_name(child);
            uint64_t n = 0;
            if (!name) continue;
            while (name[n]) n++;
            if (base + 1 + n + 1 > cap) continue;
            vpath[base] = '/';
            for (uint64_t k = 0; k <= n; k++) vpath[base + 1 + k] = name[k];
            if (install_copy_tree(vol, child, vpath, cap, files) != 0) return -1;
            vpath[base] = 0;
        }
        return 0;
    }
    {
        char fpath[300];
        uint64_t o = 0;
        const char *pre = "/SYSTEM";
        while (*pre) fpath[o++] = *pre++;
        for (uint64_t k = 0; vpath[k] && o + 1 < sizeof(fpath); k++) fpath[o++] = vpath[k];
        fpath[o] = 0;
        for (uint64_t k = o; k > 0; k--) {
            if (fpath[k] == '/') {
                fpath[k] = 0;
                (void)fatfs_mkdir(vol, fpath);
                fpath[k] = '/';
                break;
            }
        }
        if (fatfs_write(vol, fpath, vfs_node_data(node), vfs_node_size(node)) != 0) return -1;
        (*files)++;
    }
    return 0;
}

static int install_system_files(const partition_info_t *root, uint32_t *files) {
    static const char *const dirs[] = { "/apps", "/bin", "/sbin", "/etc/ssl" };
    fatfs_t vol;
    char vpath[256];
    if (fatfs_mount_part(&vol, root) != 0) return -1;
    install_progress("Writing system files", "\\SYSTEM", 0, 1);
    for (uint64_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        vfs_node_t *node = vfs_resolve(vfs_root(), dirs[d]);
        uint64_t k = 0;
        if (!node) continue;
        while (dirs[d][k]) {
            vpath[k] = dirs[d][k];
            k++;
        }
        vpath[k] = 0;
        if (install_copy_tree(&vol, node, vpath, sizeof(vpath), files) != 0) return -1;
    }
    if (fatfs_mkdir(&vol, "/EFI/ICDA") != 0) return -1;
    if (fatfs_write(&vol, "/EFI/ICDA/KERNEL.BIN", boot_asset_kernel_bin_start, boot_asset_kernel_bin_size()) != 0)
        return -1;
    (*files)++;
    return fatfs_flush(&vol);
}

static int fat32_install_boot_partition(const partition_info_t *part) {
    fatfs_t vol;
    fatfs_entry_t existing;
    static const char startup_nsh[] = "\\EFI\\ICDA\\GRUBX64.EFI\r\n";

    if (boot_asset_efi_start == 0 || boot_asset_efi_size() == 0 ||
        boot_asset_kernel_bin_start == 0 || boot_asset_kernel_bin_size() == 0) {
        return -31;
    }
    install_progress("Preparing boot disk", "Opening EFI partition", 0, 5);
    if (fatfs_mount_part(&vol, part) != 0) return -32;
    install_progress("Preparing boot disk", "Creating \\EFI\\ICDA", 1, 5);
    if (fatfs_mkdir(&vol, "/EFI/ICDA") != 0) return -33;
    install_progress("Writing boot files", "GRUBX64.EFI", 2, 5);
    if (fatfs_write(&vol, "/EFI/ICDA/GRUBX64.EFI", boot_asset_efi_start, boot_asset_efi_size()) != 0) return -34;
    install_progress("Writing boot files", "KERNEL.BIN", 3, 5);
    if (fatfs_write(&vol, "/EFI/ICDA/KERNEL.BIN", boot_asset_kernel_bin_start, boot_asset_kernel_bin_size()) != 0) return -35;
    install_progress("Preparing boot disk", "Fallback loader", 4, 5);
    if (fatfs_lookup(&vol, "/EFI/BOOT/BOOTX64.EFI", &existing) != 0) {
        if (fatfs_mkdir(&vol, "/EFI/BOOT") != 0) return -36;
        if (fatfs_write(&vol, "/EFI/BOOT/BOOTX64.EFI", boot_asset_efi_start, boot_asset_efi_size()) != 0) return -37;
    }
    if (fatfs_lookup(&vol, "/STARTUP.NSH", &existing) != 0) {
        (void)fatfs_write(&vol, "/STARTUP.NSH", startup_nsh, sizeof(startup_nsh) - 1);
    }
    install_progress("Preparing boot disk", "Boot partition complete", 5, 5);
    return 0;
}

int system_install_write_root_bundle(const partition_info_t *part, const char *bundle, uint64_t size, int32_t swap_partition_index) {
    fatfs_t vol;
    char cfg[64];
    uint64_t out = 0;

    if (!part || part->fs_hint != PARTITION_FS_FAT32 || !bundle || size == 0) return -41;
    if (fatfs_mount_part(&vol, part) != 0) return -42;
    install_progress("Preparing root partition", "Writing ICDA bundle", 0, size);
    if (fatfs_write(&vol, "/" ICDA_ROOT_BUNDLE_NAME, bundle, size) != 0) return -43;

    cfg[0] = '\0';
    out = append_text(cfg, out, sizeof(cfg), "swap=");
    if (swap_partition_index >= 0) {
        out = append_uint(cfg, out, sizeof(cfg), (uint64_t)swap_partition_index);
    } else {
        out = append_text(cfg, out, sizeof(cfg), "-1");
    }
    out = append_text(cfg, out, sizeof(cfg), "\n");
    if (fatfs_write(&vol, "/" ICDA_ROOT_CFG_NAME, cfg, out) != 0) return -44;
    return 0;
}

int system_install_read_root_bundle(const partition_info_t *part, char **bundle_out, uint64_t *size_out, int32_t *swap_partition_index_out) {
    fatfs_t vol;
    char *bundle = 0;
    char *cfg = 0;
    uint64_t size = 0;
    uint64_t cfg_size = 0;

    if (!part || !bundle_out || !size_out || part->fs_hint != PARTITION_FS_FAT32) return -1;
    *bundle_out = 0;
    *size_out = 0;
    if (swap_partition_index_out) *swap_partition_index_out = -1;
    if (fatfs_mount_part(&vol, part) != 0) return -1;
    if (fatfs_read(&vol, "/" ICDA_ROOT_BUNDLE_NAME, &bundle, &size) != 0 || size == 0) {
        if (bundle) kfree(bundle);
        return -1;
    }
    *bundle_out = bundle;
    *size_out = size;

    if (swap_partition_index_out && fatfs_read(&vol, "/" ICDA_ROOT_CFG_NAME, &cfg, &cfg_size) == 0) {
        if (cfg_size >= 6 && cfg[0] == 's' && cfg[1] == 'w' && cfg[2] == 'a' && cfg[3] == 'p' && cfg[4] == '=') {
            int32_t value = -1;
            uint64_t parsed = 0;
            if (cfg[5] == '-' && cfg[6] == '1') {
                value = -1;
            } else if (parse_uint64(cfg + 5, &parsed)) {
                value = (int32_t)parsed;
            }
            *swap_partition_index_out = value;
        }
        kfree(cfg);
    }
    return 0;
}

static int system_install_core(uint64_t *files_installed, uint64_t *bytes_installed) {
    uint64_t count = initramfs_file_count();
    uint64_t installable_count = 0;
    uint64_t total_bytes = 0;
    uint64_t installed_files = 0;
    uint64_t manifest_cap = 512;
    uint64_t out = 0;
    char *manifest = 0;
    char status[256];

    if (ensure_layout() != 0) {
        return -21;
    }

    manifest = (char *)kmalloc((size_t)manifest_cap);
    if (!manifest) {
        return -22;
    }
    manifest[0] = '\0';
    out = append_text(manifest, out, manifest_cap, "installed files:\n");

    for (uint64_t i = 0; i < count; i++) {
        const initramfs_file_t *file = initramfs_file_at(i);
        if (file && file->path && !str_prefix(file->path, "/usr/share/audio/")) {
            installable_count++;
        }
    }
    if (installable_count == 0) {
        installable_count = 1;
    }

    for (uint64_t i = 0; i < count; i++) {
        const initramfs_file_t *file = initramfs_file_at(i);
        uint64_t needed;
        if (!file || !file->path) {
            kfree(manifest);
            return -23;
        }
        if (str_prefix(file->path, "/usr/share/audio/")) {
            continue;
        }

        if (vfs_import_node(file->path, VFS_NODE_FILE, 0, file->data ? file->data : "", file->size, 0, 0, 0) != 0) {
            kfree(manifest);
            return -24;
        }
        installed_files++;
        total_bytes += file->size;
        install_progress("Preparing system", file->path, installed_files, installable_count);

        needed = str_len(file->path) + 32;
        while (out + needed + 1 >= manifest_cap) {
            char *next;
            uint64_t next_cap = manifest_cap * 2;
            next = (char *)kmalloc((size_t)next_cap);
            if (!next) {
                kfree(manifest);
                return -25;
            }
            copy_bytes(next, manifest, out + 1);
            kfree(manifest);
            manifest = next;
            manifest_cap = next_cap;
        }

        out = append_text(manifest, out, manifest_cap, "  ");
        out = append_text(manifest, out, manifest_cap, file->path);
        out = append_text(manifest, out, manifest_cap, " bytes=");
        out = append_uint(manifest, out, manifest_cap, file->size);
        out = append_text(manifest, out, manifest_cap, "\n");
    }

    out = 0;
    status[0] = '\0';
    out = append_text(status, out, sizeof(status), "installed=yes\nfiles=");
    out = append_uint(status, out, sizeof(status), installed_files);
    out = append_text(status, out, sizeof(status), "\nbytes=");
    out = append_uint(status, out, sizeof(status), total_bytes);
    out = append_text(status, out, sizeof(status), "\nmode=core-system-writable-overlay\n");
    out = append_text(status, out, sizeof(status), "excluded=/usr/share/audio/*\n");

    if (vfs_write(vfs_root(), "/system/install/state.txt", status, str_len(status)) != 0) {
        kfree(manifest);
        return -26;
    }
    if (vfs_write(vfs_root(), "/system/install/manifest.txt", manifest, str_len(manifest)) != 0) {
        kfree(manifest);
        return -27;
    }
    kfree(manifest);

    if (vfs_flush(1) != 0) {
        return -28;
    }

    if (files_installed) {
        *files_installed = installed_files;
    }
    if (bytes_installed) {
        *bytes_installed = total_bytes;
    }
    return 0;
}

int system_install_present(void) {
    vfs_stat_t st;
    return vfs_stat(vfs_root(), "/system/install/state.txt", &st) == 0;
}

int system_install_run(uint64_t *files_installed, uint64_t *bytes_installed) {
    return system_install_core(files_installed, bytes_installed);
}

int system_install_partitions(uint32_t efi_partition_index, uint32_t root_partition_index, int32_t swap_partition_index,
                              uint64_t *files_installed, uint64_t *bytes_installed) {
    const partition_info_t *boot_part = partition_get(efi_partition_index);
    const partition_info_t *root_part = partition_get(root_partition_index);
    const partition_info_t *swap_part = swap_partition_index >= 0 ? partition_get((uint32_t)swap_partition_index) : 0;
    uint64_t files = 0;
    uint64_t bytes = 0;
    char *bundle = 0;
    uint64_t bundle_size = 0;
    uint64_t entry_count = 0;
    uint64_t total_bytes_written = 0;
    uint64_t total_files_written = 0;

    if (!boot_part || !root_part) {
        return -10;
    }
    if (boot_part->fs_hint != PARTITION_FS_FAT32 || root_part->fs_hint != PARTITION_FS_FAT32) {
        return -11;
    }
    if (efi_partition_index == root_partition_index ||
        (swap_partition_index >= 0 && ((uint32_t)swap_partition_index == efi_partition_index || (uint32_t)swap_partition_index == root_partition_index))) {
        return -12;
    }
    if (swap_partition_index >= 0 && !swap_part) {
        return -13;
    }
    if (boot_part->role != PARTITION_ROLE_EFI) {
        (void)diskfmt_set_partition_role(efi_partition_index, PARTITION_ROLE_EFI);
    }
    (void)diskfmt_set_partition_role(root_partition_index, PARTITION_ROLE_SYSTEM);
    if (swap_partition_index >= 0) {
        (void)diskfmt_set_partition_role((uint32_t)swap_partition_index, PARTITION_ROLE_SWAP);
    }
    install_progress("Preparing system", "Seeding installed files", 0, 1);
    if (system_install_core(&files, &bytes) != 0) {
        return -14;
    }
    total_files_written += files;
    total_bytes_written += bytes;
    install_progress("Preparing root partition", "Packing ICDA system bundle", 0, 1);
    if (persistfs_export_image(&bundle, &bundle_size, &entry_count) != 0) {
        return -15;
    }
    if (system_install_write_root_bundle(root_part, bundle, bundle_size, swap_partition_index) != 0) {
        kfree(bundle);
        return -16;
    }
    total_files_written += 2;
    total_bytes_written += bundle_size;
    total_bytes_written += 16;
    kfree(bundle);
    {
        uint32_t sys_files = 0;
        if (install_system_files(root_part, &sys_files) != 0) {
            return -18;
        }
        total_files_written += sys_files;
    }
    install_progress("Preparing boot disk", "Writing EFI boot files", 0, 1);
    if (fat32_install_boot_partition(boot_part) != 0) {
        return -17;
    }
    if (efi_available()) {
        install_progress("Registering boot entry", "UEFI boot manager", 0, 1);
        if (efi_register_boot_entry(boot_part, "/EFI/ICDA/GRUBX64.EFI", "ICDA") != 0) {
            install_progress("Registering boot entry", "Could not add a UEFI boot entry", 1, 1);
        }
    }
    total_files_written += 4;
    total_bytes_written += (uint64_t)boot_asset_efi_size();
    total_bytes_written += (uint64_t)boot_asset_grub_cfg_size();
    total_bytes_written += (uint64_t)boot_asset_kernel_bin_size();
    total_bytes_written += 23;
    install_progress("Finalizing install", "Syncing installed state", 0, 1);
    install_progress("Finalizing install", "Done", 1, 1);
    install_progress_done();
    if (files_installed) {
        *files_installed = total_files_written;
    }
    if (bytes_installed) {
        *bytes_installed = total_bytes_written;
    }
    return 0;
}

int system_install_device(uint32_t device_index, uint64_t *files_installed, uint64_t *bytes_installed) {
    block_device_t *dev = block_get(device_index);
    uint32_t efi_partition = UINT32_MAX;
    uint32_t root_partition = UINT32_MAX;
    int32_t swap_partition = -1;

    if (!dev) {
        return -10;
    }
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        if (!part || part->device != dev) continue;
        if (part->role == PARTITION_ROLE_EFI && efi_partition == UINT32_MAX) {
            efi_partition = i;
        } else if (part->role == PARTITION_ROLE_SYSTEM && root_partition == UINT32_MAX) {
            root_partition = i;
        } else if (part->role == PARTITION_ROLE_SWAP && swap_partition < 0) {
            swap_partition = (int32_t)i;
        }
    }
    if (efi_partition == UINT32_MAX || root_partition == UINT32_MAX) {
        return -11;
    }
    return system_install_partitions(efi_partition, root_partition, swap_partition, files_installed, bytes_installed);
}

/* Build-host support: when a disk's first sector carries the scratch marker,
 * a live boot packs the installed system exactly as the installer would and
 * writes ICDAROOT.BIN to that disk (header at LBA 0, image from LBA 1).  The
 * dual-boot script on the build host uses this so real disks are only ever
 * written by the host OS's own tools.  No real disk carries this marker. */
#define ICDA_SCRATCH_MAGIC "ICDA-BUNDLE-SCRATCH-V1"

static void scratch_log(const char *text, uint64_t value) {
    char line[96];
    uint64_t out = 0;
    line[0] = '\0';
    out = append_text(line, out, sizeof(line), "[mkbundle] ");
    out = append_text(line, out, sizeof(line), text);
    out = append_uint(line, out, sizeof(line), value);
    out = append_text(line, out, sizeof(line), "\n");
    serial_write(line);
}

int system_install_export_scratch(void) {
    static uint8_t sector[512];
    uint64_t magic_len = str_len(ICDA_SCRATCH_MAGIC);

    for (uint32_t i = 0; i < block_count(); i++) {
        block_device_t *dev = block_get(i);
        char *bundle = 0;
        uint64_t size = 0, entries = 0, files = 0, bytes = 0, out;
        int match = 1;

        if (!dev || !dev->read || !dev->write || dev->sector_size != 512) continue;
        if (dev->read(dev->context, 0, 1, sector) != 0) continue;
        for (uint64_t k = 0; k < magic_len; k++) {
            if (sector[k] != (uint8_t)ICDA_SCRATCH_MAGIC[k]) { match = 0; break; }
        }
        if (!match) continue;

        scratch_log("scratch disk found, device=", i);
        if (system_install_core(&files, &bytes) != 0) { scratch_log("install core failed ", 0); return -1; }
        if (persistfs_export_image(&bundle, &size, &entries) != 0) { scratch_log("export failed ", 0); return -1; }
        if (size / 512 + 1 > dev->sector_count) { kfree(bundle); scratch_log("scratch disk too small, need bytes=", size); return -1; }
        for (uint64_t done = 0; done < size / 512;) {
            uint32_t chunk = (uint32_t)((size / 512 - done) > 128 ? 128 : (size / 512 - done));
            if (dev->write(dev->context, 1 + done, chunk, bundle + done * 512) != 0) {
                kfree(bundle);
                scratch_log("write failed at sector ", done);
                return -1;
            }
            done += chunk;
        }
        kfree(bundle);
        for (uint32_t k = 0; k < 512; k++) sector[k] = 0;
        out = 0;
        out = append_text((char *)sector, out, 512, ICDA_SCRATCH_MAGIC);
        out = append_text((char *)sector, out, 512, " DONE bytes=");
        out = append_uint((char *)sector, out, 512, size);
        out = append_text((char *)sector, out, 512, "\n");
        if (dev->write(dev->context, 0, 1, sector) != 0) { scratch_log("header write failed ", 0); return -1; }
        scratch_log("done bytes=", size);
        scratch_log("entries=", entries);
        return 1;
    }
    return 0;
}
