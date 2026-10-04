#include "libicda.h"

#define WIN_W 880
#define WIN_H 580

#define INFO_CAP   8192
#define MAX_DEV    8
#define MAX_PART   32
#define MAX_FREE   32
#define MAX_SEG    48
#define STATUS_CAP 160

#define SIDEBAR_W  236
#define DEV_ROW_H  58
#define MAP_H      76
#define MB         (1024ULL * 1024ULL)
#define SECTORS_MB 2048ULL

#define FS_FAT32 1
#define FS_EXFAT 2
#define LAYOUT_MBR 5
#define LAYOUT_GPT 6
#define LAYOUT_ICDA 4

#define ICDA_MIN_MB   1024ULL
#define ESP_MB        260ULL

enum { DRIVE_HDD = 0, DRIVE_NVME, DRIVE_USB };

enum {
    M_NONE = 0, M_CREATE, M_RESIZE, M_RESIZE_NTFS, M_FORMAT, M_DELETE, M_ERASE_GPT, M_ERASE_MBR,
    M_INSTALL, M_PROGRESS, M_RESULT, M_INSTALL_MODE, M_INSTALL_MANUAL
};

enum {
    H_NONE = 0, H_INSTALL, H_DISKMENU, H_ADD, H_REMOVE, H_GEAR, H_REFRESH,
    H_M_CANCEL, H_M_OK, H_M_SLIDER, H_M_SEG, H_M_CARD_A, H_M_CARD_B, H_M_TOGGLE,
    H_M_EFI_ROW = 100, H_M_ROOT_ROW = 200
};

enum { MA_FORMAT = 1, MA_RESIZE, MA_TYPE_EFI, MA_TYPE_SYSTEM, MA_TYPE_SWAP, MA_TYPE_DATA, MA_ERASE_GPT, MA_ERASE_MBR };

typedef struct {
    uint64_t index;
    char     name[16];
    char     table[16];
    uint64_t sectors;
    uint64_t sector_size;
} disk_t;

typedef struct {
    uint64_t index;
    char     name[48];
    char     dev[16];
    char     fs[16];
    char     role[16];
    uint64_t start;
    uint64_t sectors;
    int      usage_known;
    uint64_t used;
    uint64_t total;
} part_t;

typedef struct {
    uint64_t dev;
    uint64_t start;
    uint64_t sectors;
} gap_t;

typedef struct {
    int       is_free;
    int       ref;
    uint64_t  start;
    uint64_t  sectors;
    ic_rect_t r;
} seg_t;

static struct {
    char    info[INFO_CAP];
    disk_t  disks[MAX_DEV];
    part_t  parts[MAX_PART];
    gap_t   gaps[MAX_FREE];
    seg_t   segs[MAX_SEG];
    int     disk_count, part_count, gap_count, seg_count;
    int     uefi;
    int     runtime_device;
    int     selected_disk;
    int     selected_seg;
    int     hover_disk;
    int     hover_seg;
    int     hover;
    int     modal;
    float   slider;
    int     dragging;
    int     seg_choice;
    int     card;
    int     mode_card;
    int     efi_list[MAX_PART], root_list[MAX_PART];
    int     efi_n, root_n;
    int     man_efi, man_root, man_format;
    ic_menu_model_t menu;
    int     menu_actions[IC_MENU_ITEMS_MAX];
    int     menu_open;
    int     menu_x, menu_y;
    char    status[STATUS_CAP];
    char    result_title[64];
    char    result_text[200];
    int     result_ok;
    uint32_t install_finished_base;
    uint64_t install_pid;
    uint32_t last_poll;
    uint32_t install_started;
    char    progress_stage[40];
    float   progress;
} dm;

static ic_app_t *dm_app;

static uint64_t s_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int s_eq(const char *a, const char *b) {
    return ic_streq(a ? a : "", b ? b : "");
}

static int s_prefix(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static void set_status(const char *a, const char *b) {
    dm.status[0] = 0;
    ic_strlcat(dm.status, a, STATUS_CAP);
    if (b) ic_strlcat(dm.status, b, STATUS_CAP);
}

static void size_text(uint64_t bytes, char *out, uint64_t cap) {
    static const char *const units[5] = { "B", "KB", "MB", "GB", "TB" };
    uint64_t whole = bytes, frac = 0;
    int unit = 0;
    char num[24];
    while (whole >= 1000 && unit < 4) {
        frac = whole % 1000;
        whole /= 1000;
        unit++;
    }
    out[0] = 0;
    ic_uint_to_str(whole, num, sizeof(num));
    ic_strlcat(out, num, cap);
    if (unit > 0 && whole < 100) {
        char d[4];
        ic_strlcat(out, ".", cap);
        ic_uint_to_str(frac / 100, d, sizeof(d));
        ic_strlcat(out, d, cap);
    }
    ic_strlcat(out, " ", cap);
    ic_strlcat(out, units[unit], cap);
}

static disk_t *cur_disk(void) {
    return dm.selected_disk >= 0 && dm.selected_disk < dm.disk_count ? &dm.disks[dm.selected_disk] : 0;
}

static int disk_kind(const disk_t *d) {
    if (!d) return DRIVE_HDD;
    if (s_prefix(d->name, "nvme")) return DRIVE_NVME;
    if (s_prefix(d->name, "usb")) return DRIVE_USB;
    return DRIVE_HDD;
}

static const char *disk_kind_name(const disk_t *d) {
    if (s_prefix(d->name, "nvme")) return "NVMe SSD";
    if (s_prefix(d->name, "usb")) return "USB Drive";
    if (s_prefix(d->name, "ata")) return "IDE Disk";
    return "SATA Disk";
}

static const char *table_name(const disk_t *d) {
    if (s_eq(d->table, "gpt")) return "GUID Partition Table";
    if (s_eq(d->table, "mbr")) return "Master Boot Record";
    return "No partition table";
}

static int disk_is_runtime(const disk_t *d) {
    return d && dm.runtime_device >= 0 && d->index == (uint64_t)dm.runtime_device;
}

static void disk_title(const disk_t *d, char *out, uint64_t cap) {
    out[0] = 0;
    size_text(d->sectors * d->sector_size, out, cap);
    ic_strlcat(out, " ", cap);
    ic_strlcat(out, disk_kind_name(d), cap);
}

static part_t *seg_part(const seg_t *s) {
    return s && !s->is_free && s->ref >= 0 && s->ref < dm.part_count ? &dm.parts[s->ref] : 0;
}

static seg_t *cur_seg(void) {
    return dm.selected_seg >= 0 && dm.selected_seg < dm.seg_count ? &dm.segs[dm.selected_seg] : 0;
}

static ic_volume_t part_volume(const part_t *p) {
    if (!p) return IC_VOL_FREE;
    if (s_eq(p->role, "efi")) return IC_VOL_EFI;
    if (s_eq(p->role, "system")) return IC_VOL_ICDA;
    if (s_eq(p->role, "swap")) return IC_VOL_SWAP;
    if (s_eq(p->role, "linux")) return IC_VOL_LINUX;
    if (s_eq(p->role, "msr") || s_eq(p->role, "recovery")) return IC_VOL_RESERVED;
    if (s_eq(p->role, "data")) {
        if (s_eq(p->fs, "fat32") || s_eq(p->fs, "exfat")) return IC_VOL_DATA;
        return IC_VOL_WINDOWS;
    }
    return IC_VOL_UNKNOWN;
}

static const char *part_type_name(const part_t *p) {
    if (s_eq(p->role, "efi")) return "EFI System";
    if (s_eq(p->role, "system")) return "ICDA System";
    if (s_eq(p->role, "swap")) return "Swap";
    if (s_eq(p->role, "msr")) return "Microsoft Reserved";
    if (s_eq(p->role, "recovery")) return "Windows Recovery";
    if (s_eq(p->role, "linux")) return "Linux Filesystem";
    if (s_eq(p->role, "data")) {
        if (s_eq(p->fs, "ntfs")) return "Windows (NTFS)";
        if (s_eq(p->fs, "fat32")) return "FAT32 Data";
        if (s_eq(p->fs, "exfat")) return "exFAT Data";
        return "Microsoft Basic Data";
    }
    return "Unknown";
}

static const char *fs_name(const part_t *p) {
    if (s_eq(p->fs, "fat32")) return "FAT32";
    if (s_eq(p->fs, "exfat")) return "exFAT";
    if (s_eq(p->fs, "ntfs")) return "NTFS";
    if (s_eq(p->role, "swap")) return "Swap space";
    return "Unknown";
}

static void copy_span(char *out, uint64_t cap, const char *s, uint64_t n) {
    uint64_t i = 0;
    while (i < n && i + 1 < cap) {
        out[i] = s[i];
        i++;
    }
    out[i] = 0;
}

static uint64_t span_u64(const char *s, uint64_t n) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint64_t)(s[i] - '0');
    return v;
}

static int key_is(const char *s, uint64_t n, const char *key) {
    return n == s_len(key) && s_prefix(s, key);
}

static void parse_fields(const char *line, uint64_t len, int section) {
    disk_t *d = 0;
    part_t *p = 0;
    gap_t *g = 0;
    uint64_t pos = 0;
    int field = 0;
    if (section == 0 && dm.disk_count < MAX_DEV) {
        d = &dm.disks[dm.disk_count++];
        ic_memzero(d, sizeof(*d));
        d->sector_size = 512;
    } else if (section == 1 && dm.part_count < MAX_PART) {
        p = &dm.parts[dm.part_count++];
        ic_memzero(p, sizeof(*p));
    } else if (section == 2 && dm.gap_count < MAX_FREE) {
        g = &dm.gaps[dm.gap_count++];
        ic_memzero(g, sizeof(*g));
        field = 2;
    } else {
        return;
    }
    while (pos < len) {
        const char *t, *v;
        uint64_t start, n, eq, vn;
        while (pos < len && line[pos] == ' ') pos++;
        start = pos;
        while (pos < len && line[pos] != ' ') pos++;
        n = pos - start;
        if (n == 0) break;
        t = line + start;
        if (field == 0) {
            if (d) d->index = span_u64(t, n);
            else p->index = span_u64(t, n);
            field = 1;
            continue;
        }
        for (eq = 0; eq < n && t[eq] != '='; eq++) {}
        if (field == 1 && eq == n) {
            char *name = d ? d->name : p->name;
            uint64_t cap = d ? sizeof(d->name) : sizeof(p->name);
            uint64_t at = s_len(name);
            if (at && t[n - 1] == ':' && n == 1) continue;
            if (at > 0 && at + 1 < cap) name[at++] = ' ';
            copy_span(name + at, cap - at, t, t[n - 1] == ':' ? n - 1 : n);
            continue;
        }
        field = 2;
        if (eq == n) continue;
        v = t + eq + 1;
        vn = n - eq - 1;
        if (d) {
            if (key_is(t, eq, "table")) copy_span(d->table, sizeof(d->table), v, vn);
            else if (key_is(t, eq, "sectors")) d->sectors = span_u64(v, vn);
            else if (key_is(t, eq, "sector_size")) d->sector_size = span_u64(v, vn);
        } else if (p) {
            if (key_is(t, eq, "dev")) copy_span(p->dev, sizeof(p->dev), v, vn);
            else if (key_is(t, eq, "fs")) copy_span(p->fs, sizeof(p->fs), v, vn);
            else if (key_is(t, eq, "role")) copy_span(p->role, sizeof(p->role), v, vn);
            else if (key_is(t, eq, "start")) p->start = span_u64(v, vn);
            else if (key_is(t, eq, "sectors")) p->sectors = span_u64(v, vn);
        } else {
            if (key_is(t, eq, "dev")) g->dev = span_u64(v, vn);
            else if (key_is(t, eq, "start")) g->start = span_u64(v, vn);
            else if (key_is(t, eq, "sectors")) g->sectors = span_u64(v, vn);
        }
    }
    if (d && d->sector_size == 0) d->sector_size = 512;
    if (d && d->name[0] && d->name[s_len(d->name) - 1] == ':') d->name[s_len(d->name) - 1] = 0;
    if (p && p->name[0] && p->name[s_len(p->name) - 1] == ':') p->name[s_len(p->name) - 1] = 0;
}

static void parse_info(void) {
    const char *p = dm.info;
    int section = -1;
    dm.disk_count = dm.part_count = dm.gap_count = 0;
    dm.uefi = 0;
    while (*p) {
        const char *line = p;
        uint64_t len = 0;
        while (p[len] && p[len] != '\n') len++;
        p += len;
        if (*p == '\n') p++;
        if (len == 0) continue;
        if (line[0] != ' ') {
            if (key_is(line, len, "devices:")) section = 0;
            else if (key_is(line, len, "partitions:")) section = 1;
            else if (key_is(line, len, "free:")) section = 2;
            else if (key_is(line, len, "firmware=uefi")) section = -1, dm.uefi = 1;
            else section = -1;
            continue;
        }
        if (section == 2) parse_fields(line + 2, len - 2, 2);
        else if (section >= 0 && len > 2 && line[2] >= '0' && line[2] <= '9') parse_fields(line + 2, len - 2, section);
    }
}

static void build_segments(void) {
    disk_t *d = cur_disk();
    dm.seg_count = 0;
    if (!d) return;
    for (;;) {
        uint64_t best = ~0ULL;
        int best_part = -1, best_gap = -1;
        uint64_t after = dm.seg_count ? dm.segs[dm.seg_count - 1].start + 1 : 0;
        for (int i = 0; i < dm.part_count; i++) {
            if (!s_eq(dm.parts[i].dev, d->name)) continue;
            if (dm.parts[i].start >= after && dm.parts[i].start < best) {
                best = dm.parts[i].start;
                best_part = i;
                best_gap = -1;
            }
        }
        for (int i = 0; i < dm.gap_count; i++) {
            if (dm.gaps[i].dev != d->index) continue;
            if (dm.gaps[i].start >= after && dm.gaps[i].start < best) {
                best = dm.gaps[i].start;
                best_gap = i;
                best_part = -1;
            }
        }
        if ((best_part < 0 && best_gap < 0) || dm.seg_count >= MAX_SEG) break;
        {
            seg_t *s = &dm.segs[dm.seg_count++];
            ic_memzero(s, sizeof(*s));
            if (best_part >= 0) {
                s->ref = best_part;
                s->start = dm.parts[best_part].start;
                s->sectors = dm.parts[best_part].sectors;
            } else {
                s->is_free = 1;
                s->ref = best_gap;
                s->start = dm.gaps[best_gap].start;
                s->sectors = dm.gaps[best_gap].sectors;
            }
        }
    }
    if (dm.selected_seg >= dm.seg_count) dm.selected_seg = dm.seg_count - 1;
    if (dm.selected_seg < 0 && dm.seg_count) dm.selected_seg = 0;
}

static void load_usage(part_t *p) {
    icda_disk_edit_t req;
    if (!p || p->usage_known || !s_eq(p->fs, "fat32")) return;
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_USAGE;
    req.partition = (uint32_t)p->index;
    if (icda_disk_edit(&req) == 0) {
        p->used = req.start_lba;
        p->total = req.sectors;
        p->usage_known = 1;
    } else {
        p->usage_known = -1;
    }
}

static void refresh_keep(uint64_t keep_start) {
    long n = (long)icda_storage_info(dm.info, sizeof(dm.info) - 1);
    uint64_t rt;
    if (n < 0 || (uint64_t)n >= sizeof(dm.info)) {
        set_status("The storage query failed", 0);
        dm.disk_count = 0;
        return;
    }
    dm.info[n] = 0;
    parse_info();
    rt = icda_runtime_device();
    dm.runtime_device = rt == (uint64_t)-1 ? -1 : (int)rt;
    if (dm.selected_disk < 0 || dm.selected_disk >= dm.disk_count) dm.selected_disk = 0;
    build_segments();
    if (keep_start != ~0ULL) {
        for (int i = 0; i < dm.seg_count; i++) {
            if (dm.segs[i].start == keep_start) dm.selected_seg = i;
        }
    }
    load_usage(seg_part(cur_seg()));
}

static void refresh(void) {
    seg_t *s = cur_seg();
    refresh_keep(s ? s->start : ~0ULL);
}

static void select_disk(int i) {
    if (i < 0 || i >= dm.disk_count) return;
    dm.selected_disk = i;
    dm.selected_seg = 0;
    build_segments();
    load_usage(seg_part(cur_seg()));
}

static ic_rect_t sidebar_rect(ic_app_t *app) { return ic_rect_make(0, 0, SIDEBAR_W, app->height - 26); }
static ic_rect_t content_rect(ic_app_t *app) { return ic_rect_make(SIDEBAR_W, 0, app->width - SIDEBAR_W, app->height - 26); }
static ic_rect_t status_rect(ic_app_t *app) { return ic_rect_make(0, app->height - 26, app->width, 26); }

static ic_rect_t disk_row_rect(ic_app_t *app, int i) {
    ic_rect_t s = sidebar_rect(app);
    return ic_rect_make(s.x + IC_SP_2, s.y + 44 + i * (DEV_ROW_H + 4), s.w - 2 * IC_SP_2, DEV_ROW_H);
}

static ic_rect_t refresh_rect(ic_app_t *app) {
    ic_rect_t s = sidebar_rect(app);
    return ic_rect_make(s.x + s.w - IC_SP_3 - 28, s.y + 10, 28, 24);
}

static ic_rect_t install_rect(ic_app_t *app) {
    ic_rect_t c = content_rect(app);
    int w = ic_ui_button_width("Install ICDA...", IC_SYM_DISK);
    return ic_rect_make(c.x + c.w - IC_SP_6 - 30 - IC_SP_2 - w, c.y + IC_SP_6 + 16, w, IC_H_CONTROL);
}

static ic_rect_t diskmenu_rect(ic_app_t *app) {
    ic_rect_t c = content_rect(app);
    return ic_rect_make(c.x + c.w - IC_SP_6 - 30, c.y + IC_SP_6 + 16, 30, IC_H_CONTROL);
}

static ic_rect_t map_rect(ic_app_t *app) {
    ic_rect_t c = content_rect(app);
    return ic_rect_make(c.x + IC_SP_6, c.y + 144, c.w - 2 * IC_SP_6, MAP_H);
}

static ic_rect_t tool_rect(ic_app_t *app, int i) {
    ic_rect_t m = map_rect(app);
    return ic_rect_make(m.x + i * 34, m.y + m.h + IC_SP_3, 30, 26);
}

static ic_rect_t details_rect(ic_app_t *app) {
    ic_rect_t m = map_rect(app);
    ic_rect_t c = content_rect(app);
    int y = m.y + m.h + IC_SP_3 + 26 + IC_SP_4;
    return ic_rect_make(m.x, y, m.w, c.y + c.h - y - IC_SP_4);
}

static void layout_segments(ic_app_t *app) {
    ic_rect_t m = map_rect(app);
    uint64_t total = 0, big_total = 0;
    int min_w = 34, n = dm.seg_count, small = 0, x, avail;
    int widths[MAX_SEG];
    for (int i = 0; i < n; i++) total += dm.segs[i].sectors;
    if (!n || !total) return;
    for (int i = 0; i < n; i++) {
        int w = (int)((uint64_t)m.w * dm.segs[i].sectors / total);
        widths[i] = w;
        if (w < min_w) small++;
        else big_total += dm.segs[i].sectors;
    }
    avail = m.w - small * min_w - (n - 1) * 2;
    for (int i = 0; i < n; i++) {
        if (widths[i] < min_w || !big_total) widths[i] = min_w;
        else widths[i] = (int)((uint64_t)avail * dm.segs[i].sectors / big_total);
    }
    x = m.x;
    for (int i = 0; i < n; i++) {
        int w = i == n - 1 ? m.x + m.w - x : widths[i];
        dm.segs[i].r = ic_rect_make(x, m.y, w < 4 ? 4 : w, m.h);
        x += widths[i] + 2;
    }
}

static void draw_drive_icon(ic_canvas_t *c, int x, int y, int size, int kind) {
    float s = (float)size / 48.0f;
    if (kind == DRIVE_NVME) {
        int w = size, h = (int)(18 * s), top = y + (size - h) / 2;
        ic_gfx_shadow(c, x, top, w, h, 3 * s, (int)(4 * s) + 1, (int)(2 * s), 70);
        ic_gfx_rrect(c, x, top, w, h, 3 * s, ic_volume_color(IC_VOL_PCB));
        for (int i = 0; i < 3; i++) {
            ic_gfx_rrect(c, x + (int)((12 + i * 11) * s), top + (int)(3 * s), (int)(9 * s), h - (int)(6 * s), 1.5f * s,
                         ic_volume_color(IC_VOL_CHIP));
        }
        for (int i = 0; i < 4; i++) {
            ic_gfx_fill(c, x + (int)(2 * s), top + (int)((3 + i * 3.5f) * s), (int)(6 * s), (int)(2 * s),
                        ic_volume_color(IC_VOL_GOLD));
        }
        ic_gfx_circle(c, (float)x + w - 4 * s, (float)top + h / 2.0f, 2 * s, ic_volume_color(IC_VOL_METAL));
        return;
    }
    if (kind == DRIVE_USB) {
        int w = (int)(26 * s), h = (int)(38 * s), bx = x + (size - w) / 2, by = y + (int)(10 * s);
        ic_gfx_rrect(c, bx + (int)(5 * s), y + (int)(1 * s), w - (int)(10 * s), (int)(11 * s), 1.5f * s,
                     ic_volume_color(IC_VOL_METAL));
        ic_gfx_shadow(c, bx, by, w, h, 6 * s, (int)(4 * s) + 1, (int)(2 * s), 70);
        ic_gfx_rrect_gradient_v(c, bx, by, w, h, 6 * s, ic_palette()->accent, ic_palette()->accent_pressed);
        ic_gfx_circle(c, (float)bx + w / 2.0f, (float)by + h - 8 * s, 2.2f * s, IC_WHITE_A(200));
        return;
    }
    {
        int w = size, h = (int)(34 * s), top = y + (size - h) / 2;
        ic_gfx_shadow(c, x, top, w, h, 6 * s, (int)(5 * s) + 1, (int)(2 * s), 80);
        ic_gfx_rrect_gradient_v(c, x, top, w, h, 6 * s, ic_volume_color(IC_VOL_METAL), ic_volume_color(IC_VOL_METAL_DARK));
        ic_gfx_rrect_stroke(c, x, top, w, h, 6 * s, 1.0f, IC_BLACK_A(50));
        ic_gfx_fill(c, x + (int)(4 * s), top + h - (int)(11 * s), w - (int)(8 * s), 1, IC_BLACK_A(50));
        ic_gfx_circle(c, (float)x + w - 9 * s, (float)top + h - 5.5f * s, 2.2f * s, ic_palette()->success);
        ic_gfx_circle(c, (float)x + 6 * s, (float)top + 6 * s, 1.4f * s, IC_BLACK_A(70));
        ic_gfx_circle(c, (float)x + w - 6 * s, (float)top + 6 * s, 1.4f * s, IC_BLACK_A(70));
    }
}

static void draw_sidebar(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t s = sidebar_rect(app);
    ic_ui_sidebar_bg(c, s);
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(s.x + IC_SP_4, s.y + 12, 120, 20), "Disks",
                    p->label, IC_ALIGN_LEFT);
    ic_ui_icon_button(c, refresh_rect(app), IC_SYM_RELOAD, dm.hover == H_REFRESH ? IC_STATE_HOVER : IC_STATE_NORMAL);
    for (int i = 0; i < dm.disk_count; i++) {
        ic_rect_t r = disk_row_rect(app, i);
        disk_t *d = &dm.disks[i];
        char title[48], sub[64];
        int sel = i == dm.selected_disk;
        if (sel) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_ROW, p->accent_soft);
        else if (i == dm.hover_disk) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_ROW, p->fill_hover);
        draw_drive_icon(c, r.x + IC_SP_2, r.y + (r.h - 36) / 2, 36, disk_kind(d));
        disk_title(d, title, sizeof(title));
        ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(r.x + 54, r.y + 10, r.w - 60, 18), title,
                        p->label, IC_ALIGN_LEFT);
        sub[0] = 0;
        ic_strlcat(sub, d->name, sizeof(sub));
        ic_strlcat(sub, s_eq(d->table, "gpt") ? "  GPT" : s_eq(d->table, "mbr") ? "  MBR" : "  Empty", sizeof(sub));
        if (disk_is_runtime(d)) ic_strlcat(sub, "  In use", sizeof(sub));
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(r.x + 54, r.y + 30, r.w - 60, 16), sub,
                        p->label_secondary, IC_ALIGN_LEFT);
    }
    if (!dm.disk_count) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(s.x + IC_SP_4, s.y + 50, s.w - 32, 18),
                        "No disks found", p->label_secondary, IC_ALIGN_LEFT);
    }
}

static void draw_hatch(ic_canvas_t *c, ic_rect_t r, ic_color_t base, ic_color_t line) {
    ic_rect_t saved;
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 6.0f, base);
    ic_canvas_push_clip(c, r.x, r.y, r.w, r.h, &saved);
    for (int k = -r.h; k < r.w; k += 9) {
        ic_gfx_line(c, (float)(r.x + k), (float)(r.y + r.h), (float)(r.x + k + r.h), (float)r.y, 1.2f, line);
    }
    ic_canvas_pop_clip(c, &saved);
}

static void draw_map(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    layout_segments(app);
    for (int i = 0; i < dm.seg_count; i++) {
        seg_t *s = &dm.segs[i];
        part_t *pt = seg_part(s);
        ic_rect_t r = s->r;
        char size[24];
        size_text(s->sectors * 512ULL, size, sizeof(size));
        if (s->is_free) {
            draw_hatch(c, r, ic_volume_color(IC_VOL_FREE), ic_color_with_alpha(p->label_tertiary, 60));
            if (r.w > 64) {
                ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(r.x + 8, r.y + 20, r.w - 16, 16),
                                "Free Space", p->label_secondary, IC_ALIGN_LEFT);
                ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(r.x + 8, r.y + 38, r.w - 16, 16), size,
                                p->label_secondary, IC_ALIGN_LEFT);
            }
        } else {
            ic_color_t col = ic_volume_color(part_volume(pt));
            ic_gfx_rrect_gradient_v(c, r.x, r.y, r.w, r.h, 6.0f, ic_color_mix(col, IC_WHITE, 0.12f), col);
            if (pt->usage_known == 1 && pt->total) {
                int uh = (int)((uint64_t)(r.h - 8) * pt->used / pt->total);
                if (uh > 0) ic_gfx_rrect(c, r.x + 3, r.y + r.h - 4 - uh, 4, uh, 2.0f, IC_WHITE_A(150));
            }
            if (r.w > 64) {
                ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(r.x + 10, r.y + 20, r.w - 18, 16),
                                pt->name, IC_WHITE, IC_ALIGN_LEFT);
                ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(r.x + 10, r.y + 38, r.w - 18, 16), size,
                                IC_WHITE_A(210), IC_ALIGN_LEFT);
            }
        }
        if (i == dm.hover_seg && i != dm.selected_seg) {
            ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 6.0f, IC_WHITE_A(28));
        }
        if (i == dm.selected_seg) {
            ic_gfx_rrect_stroke(c, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 8.0f, 2.5f, p->accent);
        }
    }
    if (!dm.seg_count) {
        ic_rect_t m = map_rect(app);
        draw_hatch(c, m, ic_volume_color(IC_VOL_FREE), ic_color_with_alpha(p->label_tertiary, 60));
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), m, "This disk has no partition table", p->label_secondary,
                        IC_ALIGN_CENTER);
    }
}

static void detail_row(ic_canvas_t *c, ic_rect_t d, int *y, const char *label, const char *value) {
    const ic_palette_t *p = ic_palette();
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(d.x, *y, 110, 20), label, p->label_secondary,
                    IC_ALIGN_RIGHT);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(d.x + 124, *y, d.w - 124, 20), value, p->label,
                    IC_ALIGN_LEFT);
    *y += 26;
}

static int can_edit_disk(void) {
    disk_t *d = cur_disk();
    return d && !disk_is_runtime(d);
}

static int can_create(void) {
    seg_t *s = cur_seg();
    return can_edit_disk() && s && s->is_free;
}

static int can_remove(void) {
    seg_t *s = cur_seg();
    return can_edit_disk() && s && !s->is_free;
}

static void draw_details(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t d = details_rect(app);
    seg_t *s = cur_seg();
    part_t *pt = seg_part(s);
    char a[96], b[32];
    int y = d.y;
    ic_ui_icon_button(c, tool_rect(app, 0), IC_SYM_PLUS,
                      !can_create() ? IC_STATE_DISABLED : dm.hover == H_ADD ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, tool_rect(app, 1), IC_SYM_MINUS,
                      !can_remove() ? IC_STATE_DISABLED : dm.hover == H_REMOVE ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, tool_rect(app, 2), IC_SYM_GEAR,
                      !can_remove() ? IC_STATE_DISABLED : dm.hover == H_GEAR ? IC_STATE_HOVER : IC_STATE_NORMAL);
    if (!s) return;
    size_text(s->sectors * 512ULL, a, sizeof(a));
    ic_strlcat(a, "  (", sizeof(a));
    ic_uint_to_str(s->sectors, b, sizeof(b));
    ic_strlcat(a, b, sizeof(a));
    ic_strlcat(a, " sectors)", sizeof(a));
    detail_row(c, d, &y, "Size", a);
    if (s->is_free) {
        detail_row(c, d, &y, "Contents", "Unallocated space");
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(d.x + 124, y + 4, d.w - 124, 18),
                        "Use + to create a partition here, or Install ICDA to put ICDA here.",
                        p->label_secondary, IC_ALIGN_LEFT);
        y += 26;
    } else {
        a[0] = 0;
        ic_strlcat(a, fs_name(pt), sizeof(a));
        if (pt->usage_known == 1) {
            char used[24], total[24];
            size_text(pt->used, used, sizeof(used));
            size_text(pt->total, total, sizeof(total));
            ic_strlcat(a, "  -  ", sizeof(a));
            ic_strlcat(a, used, sizeof(a));
            ic_strlcat(a, " used of ", sizeof(a));
            ic_strlcat(a, total, sizeof(a));
        }
        detail_row(c, d, &y, "Contents", a);
        if (pt->usage_known == 1 && pt->total) {
            float f = (float)pt->used / (float)pt->total;
            ic_ui_progress(c, ic_rect_make(d.x + 124, y - 4, 220, 6), f, f > 0.9f ? p->danger : p->accent);
            y += 10;
        }
        detail_row(c, d, &y, "Type", part_type_name(pt));
        detail_row(c, d, &y, "Name", pt->name);
        detail_row(c, d, &y, "Disk", pt->dev);
    }
    a[0] = 0;
    ic_strlcat(a, "Starts at ", sizeof(a));
    size_text(s->start * 512ULL, b, sizeof(b));
    ic_strlcat(a, b, sizeof(a));
    detail_row(c, d, &y, "Location", a);
}

static void draw_content(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t cr = content_rect(app);
    disk_t *d = cur_disk();
    char t[64], sub[128];
    ic_gfx_fill(c, cr.x, cr.y, cr.w, cr.h, p->content);
    if (!d) {
        ic_ui_empty_state(c, cr, IC_SYM_DISK, "No disks", "Connect a disk and press refresh.");
        return;
    }
    draw_drive_icon(c, cr.x + IC_SP_6, cr.y + IC_SP_6, 64, disk_kind(d));
    disk_title(d, t, sizeof(t));
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE2), ic_rect_make(cr.x + 104, cr.y + 30, cr.w - 360, 24), t, p->label,
                    IC_ALIGN_LEFT);
    sub[0] = 0;
    ic_strlcat(sub, d->name, sizeof(sub));
    ic_strlcat(sub, "  -  ", sizeof(sub));
    ic_strlcat(sub, table_name(d), sizeof(sub));
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(cr.x + 104, cr.y + 58, cr.w - 360, 18), sub,
                    p->label_secondary, IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(cr.x + 104, cr.y + 76, cr.w - 360, 18),
                    dm.uefi ? "Started with UEFI firmware" : "Started with legacy BIOS", p->label_tertiary,
                    IC_ALIGN_LEFT);
    if (disk_is_runtime(d)) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(cr.x + 104, cr.y + 94, cr.w - 360, 18),
                        "ICDA is running from this disk, so it cannot be changed.", p->warning, IC_ALIGN_LEFT);
    }
    ic_ui_button(c, install_rect(app), "Install ICDA...", IC_SYM_DISK, IC_BUTTON_PRIMARY,
                 !can_edit_disk() ? IC_STATE_DISABLED : dm.hover == H_INSTALL ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_icon_button(c, diskmenu_rect(app), IC_SYM_CHEVRON_DOWN,
                      !can_edit_disk() ? IC_STATE_DISABLED : dm.hover == H_DISKMENU ? IC_STATE_HOVER : IC_STATE_NORMAL);
    ic_ui_section_header(c, cr.x + IC_SP_6, cr.y + 118, "Volumes");
    draw_map(app, c);
    draw_details(app, c);
}

static ic_rect_t modal_rect(ic_app_t *app) {
    int h = dm.modal == M_INSTALL ? 330 : dm.modal == M_PROGRESS ? 170 : dm.modal == M_RESULT ? 190 :
            dm.modal == M_INSTALL_MODE ? 280 : dm.modal == M_INSTALL_MANUAL ? 420 : 230;
    int w = dm.modal == M_INSTALL || dm.modal == M_INSTALL_MODE ? 520 : dm.modal == M_INSTALL_MANUAL ? 540 : 460;
    if (h > app->height - 16) h = app->height - 16;
    return ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
}

static ic_rect_t mode_card_rect(ic_app_t *app, int i) {
    ic_rect_t m = ic_rect_make((app->width - 520) / 2, (app->height - 280) / 2, 520, 280);
    return ic_rect_make(m.x + IC_SP_5, m.y + 60 + i * 82, m.w - 2 * IC_SP_5, 72);
}

#define MAN_ROWS 4

static ic_rect_t manual_row_rect(ic_app_t *app, int list, int i) {
    ic_rect_t m = modal_rect(app);
    int top = list == 0 ? m.y + 84 : m.y + 222;
    return ic_rect_make(m.x + IC_SP_5, top + i * 28, m.w - 2 * IC_SP_5, 26);
}

static ic_rect_t manual_toggle_rect(ic_app_t *app) {
    ic_rect_t m = modal_rect(app);
    return ic_rect_make(m.x + IC_SP_5, m.y + 340, m.w - 2 * IC_SP_5, 24);
}

static void build_manual_lists(void) {
    disk_t *d = cur_disk();
    dm.efi_n = dm.root_n = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; d && i < dm.part_count; i++) {
            part_t *p = &dm.parts[i];
            int is_efi = s_eq(p->role, "efi");
            if (!s_eq(p->dev, d->name) || !s_eq(p->fs, "fat32")) continue;
            if ((pass == 0) == is_efi && dm.efi_n < MAN_ROWS) dm.efi_list[dm.efi_n++] = i;
            if (pass == 1 && !is_efi && dm.root_n < MAN_ROWS) dm.root_list[dm.root_n++] = i;
        }
    }
    dm.man_efi = dm.efi_n ? dm.efi_list[0] : -1;
    dm.man_root = -1;
    for (int i = 0; i < dm.root_n; i++) {
        if (s_eq(dm.parts[dm.root_list[i]].role, "system")) dm.man_root = dm.root_list[i];
    }
    if (dm.man_root < 0 && dm.root_n) dm.man_root = dm.root_list[0];
    if (dm.man_root == dm.man_efi) dm.man_root = -1;
    dm.man_format = 1;
}

static int manual_ready(void) {
    return dm.man_efi >= 0 && dm.man_root >= 0 && dm.man_efi != dm.man_root;
}

static void part_row_label(const part_t *p, char *out, uint64_t cap) {
    char size[24];
    out[0] = 0;
    ic_strlcat(out, p->name, cap);
    ic_strlcat(out, "   ", cap);
    size_text(p->sectors * 512ULL, size, sizeof(size));
    ic_strlcat(out, size, cap);
    ic_strlcat(out, "   ", cap);
    ic_strlcat(out, part_type_name(p), cap);
}

static void draw_radio_row(ic_canvas_t *c, ic_rect_t r, const char *label, int selected, int hover, int enabled) {
    const ic_palette_t *p = ic_palette();
    if (selected) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_ROW, p->accent_soft);
    else if (hover) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_ROW, p->fill_hover);
    ic_gfx_ring(c, (float)r.x + 14, (float)r.y + r.h / 2.0f, 6.5f, 1.5f, p->label_secondary);
    if (selected) ic_gfx_circle(c, (float)r.x + 14, (float)r.y + r.h / 2.0f, 3.5f, p->accent);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + 30, r.y, r.w - 36, r.h), label,
                    enabled ? p->label : p->label_disabled, IC_ALIGN_LEFT);
}

static ic_rect_t modal_button(ic_app_t *app, int primary) {
    ic_rect_t m = modal_rect(app);
    int bw = 104, by = m.y + m.h - IC_H_CONTROL - IC_SP_5;
    return primary ? ic_rect_make(m.x + m.w - IC_SP_5 - bw, by, bw, IC_H_CONTROL)
                   : ic_rect_make(m.x + m.w - IC_SP_5 - 2 * bw - IC_SP_2, by, bw, IC_H_CONTROL);
}

static ic_rect_t modal_slider(ic_app_t *app) {
    ic_rect_t m = modal_rect(app);
    if (dm.modal == M_INSTALL) return ic_rect_make(m.x + IC_SP_5 + 28, m.y + 168, m.w - 2 * IC_SP_5 - 28, 22);
    return ic_rect_make(m.x + IC_SP_5, m.y + 92, m.w - 2 * IC_SP_5, 22);
}

static ic_rect_t modal_segmented(ic_app_t *app) {
    ic_rect_t m = modal_rect(app);
    return ic_rect_make(m.x + IC_SP_5, m.y + (dm.modal == M_FORMAT ? 92 : 136), m.w - 2 * IC_SP_5, IC_H_CONTROL);
}

static ic_rect_t install_card(ic_app_t *app, int i) {
    ic_rect_t m = modal_rect(app);
    return i == 0 ? ic_rect_make(m.x + IC_SP_5, m.y + 60, m.w - 2 * IC_SP_5, 140)
                  : ic_rect_make(m.x + IC_SP_5, m.y + 210, m.w - 2 * IC_SP_5, 58);
}

static const char *const create_kinds[4] = { "FAT32", "exFAT", "Swap", "Empty" };
static const char *const format_kinds[2] = { "FAT32", "exFAT" };

static gap_t *largest_gap(void) {
    disk_t *d = cur_disk();
    gap_t *best = 0;
    for (int i = 0; d && i < dm.gap_count; i++) {
        if (dm.gaps[i].dev == d->index && (!best || dm.gaps[i].sectors > best->sectors)) best = &dm.gaps[i];
    }
    return best;
}

static int disk_has_esp(void) {
    disk_t *d = cur_disk();
    for (int i = 0; d && i < dm.part_count; i++) {
        if (s_eq(dm.parts[i].dev, d->name) && s_eq(dm.parts[i].role, "efi") && s_eq(dm.parts[i].fs, "fat32")) return 1;
    }
    return 0;
}

static uint64_t alongside_max_mb(void) {
    gap_t *g = largest_gap();
    uint64_t mb = g ? g->sectors / SECTORS_MB : 0;
    if (!disk_has_esp()) mb = mb > ESP_MB ? mb - ESP_MB : 0;
    return mb;
}

static int alongside_possible(void) {
    disk_t *d = cur_disk();
    return d && dm.uefi && s_eq(d->table, "gpt") && alongside_max_mb() >= ICDA_MIN_MB;
}

static uint64_t slider_mb(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return lo;
    return lo + (uint64_t)(dm.slider * (float)(hi - lo) + 0.5f);
}

static uint64_t resize_min_mb(part_t *p) {
    uint64_t min = 64;
    if (p && p->usage_known == 1) min = p->used / MB + p->used / MB / 8 + 64;
    return min;
}

static uint64_t resize_max_mb(seg_t *s) {
    uint64_t max = s->sectors / SECTORS_MB;
    int i = dm.selected_seg + 1;
    if (i < dm.seg_count && dm.segs[i].is_free) max += dm.segs[i].sectors / SECTORS_MB;
    return max;
}

static void draw_modal(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_rect_t m = modal_rect(app);
    seg_t *s = cur_seg();
    part_t *pt = seg_part(s);
    char t[96], u[48];
    const char *title = "", *ok = "OK", *msg = 0;
    ic_button_style_t ok_style = IC_BUTTON_PRIMARY;
    int show_cancel = 1;
    ic_gfx_fill(c, 0, 0, app->width, app->height, IC_BLACK_A(70));
    ic_ui_panel(c, m, IC_R_PANEL, IC_ELEV_MENU, 0, 0);
    ic_gfx_rrect(c, m.x, m.y, m.w, m.h, IC_R_PANEL, p->window);
    ic_gfx_rrect_stroke(c, m.x, m.y, m.w, m.h, IC_R_PANEL, 1.0f, p->separator);
    switch (dm.modal) {
    case M_CREATE: {
        uint64_t max = s ? s->sectors / SECTORS_MB : 0, mb = slider_mb(16, max);
        title = "Create Partition";
        ok = "Create";
        size_text(mb * MB, t, sizeof(t));
        ic_strlcat(t, " of ", sizeof(t));
        size_text(max * MB, u, sizeof(u));
        ic_strlcat(t, u, sizeof(t));
        ic_strlcat(t, " free space", sizeof(t));
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(m.x + IC_SP_5, m.y + 64, m.w - 40, 20), t, p->label,
                        IC_ALIGN_LEFT);
        ic_ui_slider(c, modal_slider(app), dm.slider, dm.hover == H_M_SLIDER ? IC_STATE_HOVER : IC_STATE_NORMAL);
        ic_ui_segmented(c, modal_segmented(app), create_kinds, 4, (float)dm.seg_choice, -1);
        break;
    }
    case M_RESIZE: {
        uint64_t lo = resize_min_mb(pt), hi = s ? resize_max_mb(s) : lo, mb = slider_mb(lo, hi);
        title = "Resize Partition";
        ok = "Resize";
        size_text(mb * MB, t, sizeof(t));
        ic_strlcat(t, "   (", sizeof(t));
        size_text(lo * MB, u, sizeof(u));
        ic_strlcat(t, u, sizeof(t));
        ic_strlcat(t, " minimum, ", sizeof(t));
        size_text(hi * MB, u, sizeof(u));
        ic_strlcat(t, u, sizeof(t));
        ic_strlcat(t, " maximum)", sizeof(t));
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(m.x + IC_SP_5, m.y + 64, m.w - 40, 20), t, p->label,
                        IC_ALIGN_LEFT);
        ic_ui_slider(c, modal_slider(app), dm.slider, dm.hover == H_M_SLIDER ? IC_STATE_HOVER : IC_STATE_NORMAL);
        ic_text_draw_wrapped(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(m.x + IC_SP_5, m.y + 128, m.w - 40, 40),
                             "Shrinking keeps your files. Files near the end of the volume can limit how far "
                             "it can shrink.", p->label_secondary, 2);
        break;
    }
    case M_RESIZE_NTFS:
        title = "Shrink Windows from Windows";
        show_cancel = 0;
        msg = "ICDA does not resize NTFS volumes, to keep your Windows files safe. Start Windows, open Disk "
              "Management, right-click this volume and choose Shrink Volume. The free space then appears here "
              "and Install ICDA can use it.";
        break;
    case M_FORMAT:
        title = "Format Partition";
        ok = "Format";
        ok_style = IC_BUTTON_DESTRUCTIVE;
        ic_ui_segmented(c, modal_segmented(app), format_kinds, 2, (float)dm.seg_choice, -1);
        msg = "Formatting erases everything on this partition.";
        break;
    case M_DELETE:
        title = "Delete Partition?";
        ok = "Delete";
        ok_style = IC_BUTTON_DESTRUCTIVE;
        msg = "All data on this partition will be lost. The space becomes free space.";
        break;
    case M_ERASE_GPT:
    case M_ERASE_MBR:
        title = dm.modal == M_ERASE_GPT ? "Erase disk with a new GPT table?" : "Erase disk with a new MBR table?";
        ok = "Erase";
        ok_style = IC_BUTTON_DESTRUCTIVE;
        msg = "Every partition and file on this disk will be deleted.";
        break;
    case M_INSTALL: {
        int can_a = alongside_possible();
        ic_rect_t a = install_card(app, 0), b = install_card(app, 1);
        title = "Install ICDA";
        ok = "Install";
        if (dm.card == 1) ok_style = IC_BUTTON_DESTRUCTIVE;
        ic_gfx_rrect(c, a.x, a.y, a.w, a.h, IC_R_GROUP, dm.card == 0 ? p->accent_soft : p->group);
        ic_gfx_rrect_stroke(c, a.x, a.y, a.w, a.h, IC_R_GROUP, dm.card == 0 ? 2.0f : 1.0f,
                            dm.card == 0 ? p->accent : p->group_stroke);
        ic_gfx_ring(c, (float)a.x + 16, (float)a.y + 20, 7, 1.5f, p->label_secondary);
        if (dm.card == 0) ic_gfx_circle(c, (float)a.x + 16, (float)a.y + 20, 4, p->accent);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(a.x + 32, a.y + 10, a.w - 40, 20),
                        "Install alongside the other systems", can_a ? p->label : p->label_disabled, IC_ALIGN_LEFT);
        if (can_a) {
            uint64_t mb = slider_mb(ICDA_MIN_MB, alongside_max_mb());
            t[0] = 0;
            ic_strlcat(t, "Uses ", sizeof(t));
            size_text(mb * MB, u, sizeof(u));
            ic_strlcat(t, u, sizeof(t));
            ic_strlcat(t, " of free space. Nothing is deleted.", sizeof(t));
            ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x + 32, a.y + 32, a.w - 40, 18), t,
                            p->label_secondary, IC_ALIGN_LEFT);
            ic_ui_slider(c, modal_slider(app), dm.slider, dm.hover == H_M_SLIDER ? IC_STATE_HOVER : IC_STATE_NORMAL);
            ic_text_draw_wrapped(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x + 32, a.y + 58, a.w - 44, 46),
                                 disk_has_esp() ? "Reuses the existing EFI partition and adds ICDA to the UEFI "
                                                  "boot menu. Windows stays in the boot menu."
                                                : "Creates a 260 MB EFI partition and adds ICDA to the UEFI boot "
                                                  "menu.", p->label_secondary, 2);
        } else {
            ic_text_draw_wrapped(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(a.x + 32, a.y + 34, a.w - 44, 90),
                                 !dm.uefi ? "This computer started in legacy BIOS mode. Installing alongside "
                                            "another system needs UEFI."
                                          : "Needs a GPT disk with at least 1 GB of free space. To make room "
                                            "next to Windows, shrink its volume in Windows Disk Management first.",
                                 p->label_secondary, 2);
        }
        ic_gfx_rrect(c, b.x, b.y, b.w, b.h, IC_R_GROUP, dm.card == 1 ? ic_color_with_alpha(p->danger, 40) : p->group);
        ic_gfx_rrect_stroke(c, b.x, b.y, b.w, b.h, IC_R_GROUP, dm.card == 1 ? 2.0f : 1.0f,
                            dm.card == 1 ? p->danger : p->group_stroke);
        ic_gfx_ring(c, (float)b.x + 16, (float)b.y + 20, 7, 1.5f, p->label_secondary);
        if (dm.card == 1) ic_gfx_circle(c, (float)b.x + 16, (float)b.y + 20, 4, p->danger);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(b.x + 32, b.y + 10, b.w - 40, 20),
                        "Erase disk and install ICDA", p->label, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(b.x + 32, b.y + 32, b.w - 40, 18),
                        "Deletes every partition and file on this disk.", p->danger, IC_ALIGN_LEFT);
        break;
    }
    case M_INSTALL_MODE: {
        static const char *const heads[2] = { "Automatic (recommended)", "Manual" };
        static const char *const subs[2] = {
            "ICDA picks the space: next to your other systems in free space, or the whole disk.",
            "Choose the EFI partition and the ICDA partition yourself. For advanced setups."
        };
        title = "How do you want to install ICDA?";
        ok = "Continue";
        for (int i = 0; i < 2; i++) {
            ic_rect_t r = mode_card_rect(app, i);
            int sel = dm.mode_card == i;
            ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_GROUP, sel ? p->accent_soft : p->group);
            ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_GROUP, sel ? 2.0f : 1.0f, sel ? p->accent : p->group_stroke);
            ic_gfx_ring(c, (float)r.x + 16, (float)r.y + 20, 7, 1.5f, p->label_secondary);
            if (sel) ic_gfx_circle(c, (float)r.x + 16, (float)r.y + 20, 4, p->accent);
            ic_symbol_draw(c, i == 0 ? IC_SYM_CHECK : IC_SYM_GEAR, (float)r.x + r.w - 26, (float)r.y + 22, 16,
                           p->label_secondary);
            ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(r.x + 32, r.y + 10, r.w - 72, 20), heads[i],
                            p->label, IC_ALIGN_LEFT);
            ic_text_draw_wrapped(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + 32, r.y + 32, r.w - 60, 36),
                                 subs[i], p->label_secondary, 2);
        }
        break;
    }
    case M_INSTALL_MANUAL: {
        title = "Manual installation";
        ok = "Install";
        ok_style = dm.man_format ? IC_BUTTON_DESTRUCTIVE : IC_BUTTON_PRIMARY;
        ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(m.x + IC_SP_5, m.y + 58, m.w - 40, 20),
                        "EFI system partition (boot files)", p->label, IC_ALIGN_LEFT);
        for (int i = 0; i < dm.efi_n; i++) {
            part_row_label(&dm.parts[dm.efi_list[i]], t, sizeof(t));
            draw_radio_row(c, manual_row_rect(app, 0, i), t, dm.man_efi == dm.efi_list[i], dm.hover == H_M_EFI_ROW + i, 1);
        }
        if (!dm.efi_n) {
            ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), manual_row_rect(app, 0, 0),
                            "No FAT32 partition on this disk. Create one with + first.", p->label_secondary, IC_ALIGN_LEFT);
        }
        ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(m.x + IC_SP_5, m.y + 196, m.w - 40, 20),
                        "ICDA system partition", p->label, IC_ALIGN_LEFT);
        for (int i = 0; i < dm.root_n; i++) {
            int idx = dm.root_list[i];
            part_row_label(&dm.parts[idx], t, sizeof(t));
            draw_radio_row(c, manual_row_rect(app, 1, i), t, dm.man_root == idx, dm.hover == H_M_ROOT_ROW + i,
                           idx != dm.man_efi);
        }
        if (!dm.root_n) {
            ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), manual_row_rect(app, 1, 0),
                            "No other FAT32 partition. Create a 1 GB or larger one with + first.",
                            p->label_secondary, IC_ALIGN_LEFT);
        }
        {
            ic_rect_t tg = manual_toggle_rect(app);
            ic_gfx_rrect(c, tg.x + 4, tg.y + 4, 16, 16, 4.0f, dm.man_format ? p->accent : p->control);
            ic_gfx_rrect_stroke(c, tg.x + 4, tg.y + 4, 16, 16, 4.0f, 1.0f, p->control_stroke);
            if (dm.man_format) ic_symbol_draw(c, IC_SYM_CHECK, (float)tg.x + 12, (float)tg.y + 12, 12, IC_WHITE);
            ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(tg.x + 30, tg.y, tg.w - 30, tg.h),
                            dm.man_format ? "Format the ICDA partition first (erases it)"
                                          : "Keep the files already on the ICDA partition",
                            dm.man_format ? p->danger : p->label, IC_ALIGN_LEFT);
        }
        break;
    }
    case M_PROGRESS:
        title = "Installing ICDA";
        show_cancel = 0;
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(m.x + IC_SP_5, m.y + 62, m.w - 40, 20),
                        dm.progress_stage[0] ? dm.progress_stage : "Starting", p->label, IC_ALIGN_LEFT);
        ic_ui_progress(c, ic_rect_make(m.x + IC_SP_5, m.y + 92, m.w - 40, 8), dm.progress, p->accent);
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(m.x + IC_SP_5, m.y + 112, m.w - 40, 18),
                        "Keep the computer on until this finishes.", p->label_secondary, IC_ALIGN_LEFT);
        break;
    case M_RESULT:
        title = dm.result_title;
        show_cancel = 0;
        msg = dm.result_text;
        break;
    default:
        break;
    }
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE3), ic_rect_make(m.x + IC_SP_5, m.y + IC_SP_5, m.w - 40, 22), title,
                    p->label, IC_ALIGN_LEFT);
    if (msg) {
        ic_text_draw_wrapped(c, ic_font(IC_FONT_BODY), ic_rect_make(m.x + IC_SP_5, m.y + (dm.modal == M_FORMAT ? 134 : 60),
                                                                    m.w - 40, 90), msg, p->label_secondary, 3);
    }
    if (dm.modal != M_PROGRESS) {
        int ok_enabled = !(dm.modal == M_INSTALL && dm.card == 0 && !alongside_possible()) &&
                         !(dm.modal == M_INSTALL_MANUAL && !manual_ready());
        ic_ui_button(c, modal_button(app, 1), ok, IC_SYM_NONE, ok_style,
                     !ok_enabled ? IC_STATE_DISABLED : dm.hover == H_M_OK ? IC_STATE_HOVER : IC_STATE_NORMAL);
        if (show_cancel) {
            ic_ui_button(c, modal_button(app, 0), "Cancel", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                         dm.hover == H_M_CANCEL ? IC_STATE_HOVER : IC_STATE_NORMAL);
        }
    }
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    dm_app = app;
    draw_sidebar(app, c);
    draw_content(app, c);
    ic_ui_statusbar(c, status_rect(app), dm.status);
    if (dm.menu_open) ic_ui_menu(c, &dm.menu, dm.menu_x, dm.menu_y, 0, 0);
    if (dm.modal) draw_modal(app, c);
    (void)p;
}

static void report(long rc, const char *ok_text, const char *fail_text) {
    char n[24];
    if (rc >= 0) {
        set_status(ok_text, 0);
        return;
    }
    set_status(fail_text, " (error ");
    ic_uint_to_str((uint64_t)(-rc), n, sizeof(n));
    ic_strlcat(dm.status, n, STATUS_CAP);
    ic_strlcat(dm.status, ")", STATUS_CAP);
}

static void do_create(void) {
    seg_t *s = cur_seg();
    disk_t *d = cur_disk();
    icda_disk_edit_t req;
    uint64_t mb;
    long rc;
    if (!s || !d || !s->is_free) return;
    mb = slider_mb(16, s->sectors / SECTORS_MB);
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_CREATE;
    req.device = (uint32_t)d->index;
    req.start_lba = s->start;
    req.sectors = mb * SECTORS_MB;
    if (req.sectors > s->sectors) req.sectors = s->sectors / SECTORS_MB * SECTORS_MB;
    req.role = dm.seg_choice == 2 ? ICDA_ROLE_SWAP : ICDA_ROLE_DATA;
    req.fs = dm.seg_choice == 0 ? FS_FAT32 : dm.seg_choice == 1 ? FS_EXFAT : 0;
    ic_strcpy(req.name, dm.seg_choice == 2 ? "Swap" : "Data", sizeof(req.name));
    rc = icda_disk_edit(&req);
    report(rc, "Partition created", rc == -18 ? "The partition was created but formatting failed" : "Could not create the partition");
    refresh_keep(s->start);
}

static void do_delete(void) {
    seg_t *s = cur_seg();
    part_t *p = seg_part(s);
    icda_disk_edit_t req;
    long rc;
    if (!p) return;
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_DELETE;
    req.partition = (uint32_t)p->index;
    rc = icda_disk_edit(&req);
    report(rc, "Partition deleted", "Could not delete the partition");
    refresh_keep(s->start);
}

static void do_resize(void) {
    seg_t *s = cur_seg();
    part_t *p = seg_part(s);
    icda_disk_edit_t req;
    long rc;
    uint64_t start;
    if (!p) return;
    start = s->start;
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_RESIZE;
    req.partition = (uint32_t)p->index;
    req.sectors = slider_mb(resize_min_mb(p), resize_max_mb(s)) * SECTORS_MB;
    rc = icda_disk_edit(&req);
    report(rc, "Partition resized", rc == -24 ? "Files near the end of the volume prevent shrinking that far"
                                              : "Could not resize the partition");
    refresh_keep(start);
}

static void do_format(void) {
    seg_t *s = cur_seg();
    part_t *p = seg_part(s);
    long rc;
    if (!p) return;
    rc = (long)icda_format_partition(p->index, dm.seg_choice == 1 ? FS_EXFAT : FS_FAT32);
    report(rc, "Partition formatted", "Could not format the partition");
    refresh_keep(s->start);
}

static void do_set_type(int role) {
    seg_t *s = cur_seg();
    part_t *p = seg_part(s);
    long rc;
    if (!p) return;
    rc = (long)icda_set_partition_role(p->index, (uint64_t)role);
    report(rc, "Partition type changed", "Could not change the type (GPT disks only)");
    refresh_keep(s->start);
}

static void do_erase(int layout) {
    disk_t *d = cur_disk();
    long rc;
    if (!d) return;
    rc = (long)icda_format_device(d->index, (uint64_t)layout);
    report(rc, "The disk was erased", "Could not erase the disk");
    dm.selected_seg = 0;
    refresh_keep(~0ULL);
}

static int find_part_at(uint64_t dev_index, uint64_t start) {
    const char *name = 0;
    for (int i = 0; i < dm.disk_count; i++) {
        if (dm.disks[i].index == dev_index) name = dm.disks[i].name;
    }
    for (int i = 0; name && i < dm.part_count; i++) {
        if (s_eq(dm.parts[i].dev, name) && dm.parts[i].start == start) return (int)dm.parts[i].index;
    }
    return -1;
}

static void start_progress(const char *args) {
    icda_disk_edit_t req;
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_INSTALL_STATUS;
    icda_disk_edit(&req);
    dm.install_finished_base = req.partition;
    dm.progress = 0.0f;
    dm.progress_stage[0] = 0;
    dm.install_started = ic_time_ms();
    dm.install_pid = icda_spawn_args("/apps/diskman.app", args);
    if ((long)dm.install_pid < 0) {
        dm.modal = M_RESULT;
        dm.result_ok = 0;
        ic_strcpy(dm.result_title, "Install could not start", sizeof(dm.result_title));
        ic_strcpy(dm.result_text, "The installer process could not be launched.", sizeof(dm.result_text));
        return;
    }
    dm.modal = M_PROGRESS;
}

static void fail_result(const char *title, const char *text) {
    dm.modal = M_RESULT;
    dm.result_ok = 0;
    ic_strcpy(dm.result_title, title, sizeof(dm.result_title));
    ic_strcpy(dm.result_text, text, sizeof(dm.result_text));
}

static void do_install(void) {
    disk_t *d = cur_disk();
    char args[64], n[24];
    if (!d) return;
    if (dm.card == 1) {
        args[0] = 0;
        ic_strlcat(args, "--install-device ", sizeof(args));
        ic_uint_to_str(d->index, n, sizeof(n));
        ic_strlcat(args, n, sizeof(args));
        start_progress(args);
        return;
    }
    {
        gap_t *g = largest_gap();
        icda_disk_edit_t req;
        uint64_t start, mb = slider_mb(ICDA_MIN_MB, alongside_max_mb());
        int esp = -1, root;
        if (!g || !alongside_possible()) return;
        start = g->start;
        for (int i = 0; i < dm.part_count; i++) {
            if (s_eq(dm.parts[i].dev, d->name) && s_eq(dm.parts[i].role, "efi") && s_eq(dm.parts[i].fs, "fat32")) {
                esp = (int)dm.parts[i].index;
                break;
            }
        }
        if (esp < 0) {
            ic_memzero(&req, sizeof(req));
            req.op = ICDA_DISK_CREATE;
            req.device = (uint32_t)d->index;
            req.start_lba = start;
            req.sectors = ESP_MB * SECTORS_MB;
            req.role = ICDA_ROLE_EFI;
            req.fs = FS_FAT32;
            ic_strcpy(req.name, "EFI System", sizeof(req.name));
            if (icda_disk_edit(&req) < 0) {
                fail_result("Install failed", "Could not create the EFI system partition.");
                return;
            }
            refresh_keep(~0ULL);
            esp = find_part_at(d->index, start);
            start += ESP_MB * SECTORS_MB;
        }
        ic_memzero(&req, sizeof(req));
        req.op = ICDA_DISK_CREATE;
        req.device = (uint32_t)d->index;
        req.start_lba = start;
        req.sectors = mb * SECTORS_MB;
        req.role = ICDA_ROLE_SYSTEM;
        req.fs = FS_FAT32;
        ic_strcpy(req.name, "ICDA System", sizeof(req.name));
        if (icda_disk_edit(&req) < 0) {
            fail_result("Install failed", "Could not create the ICDA partition in the free space.");
            refresh_keep(~0ULL);
            return;
        }
        refresh_keep(start);
        root = find_part_at(d->index, start);
        if (esp < 0 || root < 0) {
            fail_result("Install failed", "The new partitions could not be found after creating them.");
            return;
        }
        args[0] = 0;
        ic_strlcat(args, "--install ", sizeof(args));
        ic_uint_to_str((uint64_t)esp, n, sizeof(n));
        ic_strlcat(args, n, sizeof(args));
        ic_strlcat(args, " ", sizeof(args));
        ic_uint_to_str((uint64_t)root, n, sizeof(n));
        ic_strlcat(args, n, sizeof(args));
        start_progress(args);
    }
}

static void do_install_manual(void) {
    char args[64], n[24];
    part_t *efi, *root;
    if (!manual_ready()) return;
    efi = &dm.parts[dm.man_efi];
    root = &dm.parts[dm.man_root];
    if (dm.man_format && (long)icda_format_partition(root->index, FS_FAT32) < 0) {
        fail_result("Install failed", "The ICDA partition could not be formatted.");
        return;
    }
    args[0] = 0;
    ic_strlcat(args, "--install ", sizeof(args));
    ic_uint_to_str(efi->index, n, sizeof(n));
    ic_strlcat(args, n, sizeof(args));
    ic_strlcat(args, " ", sizeof(args));
    ic_uint_to_str(root->index, n, sizeof(n));
    ic_strlcat(args, n, sizeof(args));
    dm.card = 0;
    start_progress(args);
}

static void tick(ic_app_t *app) {
    icda_disk_edit_t req;
    uint32_t now;
    if (dm.modal != M_PROGRESS) return;
    now = ic_time_ms();
    if (now - dm.last_poll < 150) return;
    dm.last_poll = now;
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_INSTALL_STATUS;
    if (icda_disk_edit(&req) != 0) return;
    ic_strcpy(dm.progress_stage, req.name, sizeof(dm.progress_stage));
    dm.progress = req.sectors ? (float)req.start_lba / (float)req.sectors : 0.0f;
    if (dm.progress > 1.0f) dm.progress = 1.0f;
    if (req.partition != dm.install_finished_base) {
        int rc = (int)req.fs;
        icda_waitpid(dm.install_pid);
        dm.modal = M_RESULT;
        dm.result_ok = rc >= 0;
        if (rc >= 0) {
            ic_strcpy(dm.result_title, "ICDA is installed", sizeof(dm.result_title));
            ic_strcpy(dm.result_text, dm.uefi ? "ICDA was added to the UEFI boot menu. When you restart, the ICDA "
                                                "boot menu also lists Windows and other systems it finds."
                                              : "Restart and boot from this disk to start ICDA.",
                      sizeof(dm.result_text));
        } else {
            char n[24];
            ic_strcpy(dm.result_title, "Install failed", sizeof(dm.result_title));
            ic_strcpy(dm.result_text, "The installer stopped with error ", sizeof(dm.result_text));
            ic_uint_to_str((uint64_t)(-rc), n, sizeof(n));
            ic_strlcat(dm.result_text, n, sizeof(dm.result_text));
            ic_strlcat(dm.result_text, dm.card == 0 ? ". Your existing partitions were not touched." : ".",
                       sizeof(dm.result_text));
        }
        refresh_keep(~0ULL);
    } else if (!req.role && now - dm.install_started > 15000 && !dm.progress_stage[0]) {
        fail_result("Install failed", "The installer did not start.");
    }
    ic_app_invalidate(app);
}

static void open_menu(int disk_menu, int x, int y) {
    int n = 0;
    disk_t *d = cur_disk();
    seg_t *s = cur_seg();
    part_t *p = seg_part(s);
    ic_memzero(&dm.menu, sizeof(dm.menu));
    dm.menu.hover = -1;
    if (disk_menu) {
        dm.menu.labels[n] = "Erase with a new GPT table...";
        dm.menu_actions[n++] = MA_ERASE_GPT;
        dm.menu.labels[n] = "Erase with a new MBR table...";
        dm.menu_actions[n++] = MA_ERASE_MBR;
    } else if (p) {
        int gpt = d && s_eq(d->table, "gpt");
        dm.menu.labels[n] = "Format...";
        dm.menu_actions[n++] = MA_FORMAT;
        dm.menu.labels[n] = "Resize...";
        dm.menu_actions[n++] = MA_RESIZE;
        dm.menu.labels[n] = IC_MENU_SEPARATOR;
        dm.menu_actions[n++] = 0;
        dm.menu.labels[n] = "Type: EFI System";
        dm.menu.disabled[n] = (uint8_t)!gpt;
        dm.menu.checked[n] = s_eq(p->role, "efi") ? IC_MENU_CHECK_ON : IC_MENU_CHECK_OFF;
        dm.menu_actions[n++] = MA_TYPE_EFI;
        dm.menu.labels[n] = "Type: ICDA System";
        dm.menu.disabled[n] = (uint8_t)!gpt;
        dm.menu.checked[n] = s_eq(p->role, "system") ? IC_MENU_CHECK_ON : IC_MENU_CHECK_OFF;
        dm.menu_actions[n++] = MA_TYPE_SYSTEM;
        dm.menu.labels[n] = "Type: Swap";
        dm.menu.disabled[n] = (uint8_t)!gpt;
        dm.menu.checked[n] = s_eq(p->role, "swap") ? IC_MENU_CHECK_ON : IC_MENU_CHECK_OFF;
        dm.menu_actions[n++] = MA_TYPE_SWAP;
        dm.menu.labels[n] = "Type: Basic Data";
        dm.menu.disabled[n] = (uint8_t)!gpt;
        dm.menu.checked[n] = s_eq(p->role, "data") ? IC_MENU_CHECK_ON : IC_MENU_CHECK_OFF;
        dm.menu_actions[n++] = MA_TYPE_DATA;
    }
    dm.menu.count = n;
    if (!n) return;
    if (x + ic_ui_menu_width(&dm.menu) > dm_app->width - 8) x = dm_app->width - 8 - ic_ui_menu_width(&dm.menu);
    dm.menu_x = x;
    dm.menu_y = y;
    dm.menu_open = 1;
}

static void open_modal(int modal) {
    dm.modal = modal;
    if (modal == M_INSTALL_MODE) dm.mode_card = 0;
    dm.slider = 1.0f;
    dm.seg_choice = 0;
    dm.dragging = 0;
    dm.hover = H_NONE;
    if (modal == M_INSTALL) {
        dm.card = alongside_possible() ? 0 : 1;
        dm.slider = 1.0f;
    }
}

static void menu_action(int action) {
    seg_t *s = cur_seg();
    part_t *p = seg_part(s);
    dm.menu_open = 0;
    switch (action) {
    case MA_FORMAT: open_modal(M_FORMAT); break;
    case MA_RESIZE:
        if (p && s_eq(p->fs, "fat32")) open_modal(M_RESIZE);
        else if (p && (s_eq(p->fs, "ntfs") || s_eq(p->role, "data") || s_eq(p->role, "recovery"))) open_modal(M_RESIZE_NTFS);
        else open_modal(M_RESIZE);
        break;
    case MA_TYPE_EFI: do_set_type(1); break;
    case MA_TYPE_SYSTEM: do_set_type(2); break;
    case MA_TYPE_SWAP: do_set_type(3); break;
    case MA_TYPE_DATA: do_set_type(4); break;
    case MA_ERASE_GPT: open_modal(M_ERASE_GPT); break;
    case MA_ERASE_MBR: open_modal(M_ERASE_MBR); break;
    default: break;
    }
}

static void modal_ok(void) {
    int m = dm.modal;
    if (m == M_INSTALL && dm.card == 0 && !alongside_possible()) return;
    if (m == M_INSTALL_MANUAL && !manual_ready()) return;
    if (m == M_INSTALL_MODE) {
        if (dm.mode_card == 0) {
            open_modal(M_INSTALL);
        } else {
            open_modal(M_INSTALL_MANUAL);
            build_manual_lists();
        }
        return;
    }
    dm.modal = M_NONE;
    switch (m) {
    case M_CREATE: do_create(); break;
    case M_RESIZE: do_resize(); break;
    case M_FORMAT: do_format(); break;
    case M_DELETE: do_delete(); break;
    case M_ERASE_GPT: do_erase(LAYOUT_GPT); break;
    case M_ERASE_MBR: do_erase(LAYOUT_MBR); break;
    case M_INSTALL: do_install(); break;
    case M_INSTALL_MANUAL: do_install_manual(); break;
    default: break;
    }
}

static int modal_hover(ic_app_t *app, int x, int y) {
    if (ic_ui_hit(modal_button(app, 1), x, y)) return H_M_OK;
    if (dm.modal != M_RESULT && dm.modal != M_RESIZE_NTFS && ic_ui_hit(modal_button(app, 0), x, y)) return H_M_CANCEL;
    if ((dm.modal == M_CREATE || dm.modal == M_RESIZE || (dm.modal == M_INSTALL && dm.card == 0 && alongside_possible())) &&
        ic_ui_hit(modal_slider(app), x, y)) {
        return H_M_SLIDER;
    }
    if ((dm.modal == M_CREATE || dm.modal == M_FORMAT) && ic_ui_hit(modal_segmented(app), x, y)) return H_M_SEG;
    if (dm.modal == M_INSTALL_MODE && ic_ui_hit(mode_card_rect(app, 0), x, y)) return H_M_CARD_A;
    if (dm.modal == M_INSTALL_MODE && ic_ui_hit(mode_card_rect(app, 1), x, y)) return H_M_CARD_B;
    if (dm.modal == M_INSTALL_MANUAL) {
        for (int i = 0; i < dm.efi_n; i++) {
            if (ic_ui_hit(manual_row_rect(app, 0, i), x, y)) return H_M_EFI_ROW + i;
        }
        for (int i = 0; i < dm.root_n; i++) {
            if (ic_ui_hit(manual_row_rect(app, 1, i), x, y)) return H_M_ROOT_ROW + i;
        }
        if (ic_ui_hit(manual_toggle_rect(app), x, y)) return H_M_TOGGLE;
    }
    if (dm.modal == M_INSTALL && ic_ui_hit(install_card(app, 0), x, y)) return H_M_CARD_A;
    if (dm.modal == M_INSTALL && ic_ui_hit(install_card(app, 1), x, y)) return H_M_CARD_B;
    return H_NONE;
}

static int seg_at(int x, int y) {
    for (int i = 0; i < dm.seg_count; i++) {
        if (ic_ui_hit(dm.segs[i].r, x, y)) return i;
    }
    return -1;
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    dm_app = app;
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        if (dm.modal) {
            dm.hover = modal_hover(app, ev->x, ev->y);
            if (dm.dragging) dm.slider = ic_ui_slider_value(modal_slider(app), ev->x);
            break;
        }
        if (dm.menu_open) {
            dm.menu.hover = ic_ui_menu_hit(&dm.menu, dm.menu_x, dm.menu_y, ev->x, ev->y);
            break;
        }
        dm.hover = H_NONE;
        dm.hover_disk = -1;
        dm.hover_seg = seg_at(ev->x, ev->y);
        for (int i = 0; i < dm.disk_count; i++) {
            if (ic_ui_hit(disk_row_rect(app, i), ev->x, ev->y)) dm.hover_disk = i;
        }
        if (ic_ui_hit(install_rect(app), ev->x, ev->y)) dm.hover = H_INSTALL;
        else if (ic_ui_hit(diskmenu_rect(app), ev->x, ev->y)) dm.hover = H_DISKMENU;
        else if (ic_ui_hit(tool_rect(app, 0), ev->x, ev->y)) dm.hover = H_ADD;
        else if (ic_ui_hit(tool_rect(app, 1), ev->x, ev->y)) dm.hover = H_REMOVE;
        else if (ic_ui_hit(tool_rect(app, 2), ev->x, ev->y)) dm.hover = H_GEAR;
        else if (ic_ui_hit(refresh_rect(app), ev->x, ev->y)) dm.hover = H_REFRESH;
        break;
    case IC_EV_MOUSE_UP:
        dm.dragging = 0;
        break;
    case IC_EV_MOUSE_DOWN:
        if (ev->button != GUI_BTN_LEFT) break;
        if (dm.modal) {
            int h = modal_hover(app, ev->x, ev->y);
            if (dm.modal == M_PROGRESS) break;
            if (h == H_M_OK) {
                if (dm.modal == M_RESULT || dm.modal == M_RESIZE_NTFS) dm.modal = M_NONE;
                else modal_ok();
            } else if (h == H_M_CANCEL) {
                dm.modal = M_NONE;
            } else if (h == H_M_SLIDER) {
                dm.dragging = 1;
                dm.slider = ic_ui_slider_value(modal_slider(app), ev->x);
            } else if (h == H_M_SEG) {
                int i = ic_ui_segmented_hit(modal_segmented(app), dm.modal == M_FORMAT ? 2 : 4, ev->x, ev->y);
                if (i >= 0) dm.seg_choice = i;
            } else if (dm.modal == M_INSTALL_MODE && (h == H_M_CARD_A || h == H_M_CARD_B)) {
                dm.mode_card = h == H_M_CARD_A ? 0 : 1;
            } else if (dm.modal == M_INSTALL_MANUAL && h >= H_M_EFI_ROW && h < H_M_EFI_ROW + MAN_ROWS) {
                dm.man_efi = dm.efi_list[h - H_M_EFI_ROW];
                if (dm.man_root == dm.man_efi) dm.man_root = -1;
            } else if (dm.modal == M_INSTALL_MANUAL && h >= H_M_ROOT_ROW && h < H_M_ROOT_ROW + MAN_ROWS) {
                if (dm.root_list[h - H_M_ROOT_ROW] != dm.man_efi) dm.man_root = dm.root_list[h - H_M_ROOT_ROW];
            } else if (dm.modal == M_INSTALL_MANUAL && h == H_M_TOGGLE) {
                dm.man_format = !dm.man_format;
            } else if (h == H_M_CARD_A && alongside_possible()) {
                dm.card = 0;
            } else if (h == H_M_CARD_B) {
                dm.card = 1;
            }
            break;
        }
        if (dm.menu_open) {
            int hit = ic_ui_menu_hit(&dm.menu, dm.menu_x, dm.menu_y, ev->x, ev->y);
            if (hit >= 0 && !dm.menu.disabled[hit]) menu_action(dm.menu_actions[hit]);
            else dm.menu_open = 0;
            break;
        }
        if (dm.hover == H_REFRESH) {
            refresh();
            set_status("Refreshed", 0);
            break;
        }
        if (dm.hover == H_INSTALL && can_edit_disk()) {
            open_modal(M_INSTALL_MODE);
            break;
        }
        if (dm.hover == H_DISKMENU && can_edit_disk()) {
            ic_rect_t r = diskmenu_rect(app);
            open_menu(1, r.x, r.y + r.h + 4);
            break;
        }
        if (dm.hover == H_ADD && can_create()) {
            open_modal(M_CREATE);
            break;
        }
        if (dm.hover == H_REMOVE && can_remove()) {
            open_modal(M_DELETE);
            break;
        }
        if (dm.hover == H_GEAR && can_remove()) {
            ic_rect_t r = tool_rect(app, 2);
            open_menu(0, r.x, r.y + r.h + 4);
            break;
        }
        for (int i = 0; i < dm.disk_count; i++) {
            if (ic_ui_hit(disk_row_rect(app, i), ev->x, ev->y)) select_disk(i);
        }
        {
            int s = seg_at(ev->x, ev->y);
            if (s >= 0) {
                dm.selected_seg = s;
                load_usage(seg_part(cur_seg()));
            }
        }
        break;
    case IC_EV_KEY:
        if (dm.modal) {
            if (ev->key == IC_KEY_ESCAPE && dm.modal != M_PROGRESS) dm.modal = M_NONE;
            else if (ev->key == IC_KEY_ENTER && dm.modal != M_PROGRESS) {
                if (dm.modal == M_RESULT || dm.modal == M_RESIZE_NTFS) dm.modal = M_NONE;
                else modal_ok();
            }
            break;
        }
        if (dm.menu_open && ev->key == IC_KEY_ESCAPE) {
            dm.menu_open = 0;
            break;
        }
        if (ev->key == IC_KEY_RIGHT && dm.selected_seg + 1 < dm.seg_count) dm.selected_seg++;
        else if (ev->key == IC_KEY_LEFT && dm.selected_seg > 0) dm.selected_seg--;
        else if (ev->key == IC_KEY_DOWN && dm.selected_disk + 1 < dm.disk_count) select_disk(dm.selected_disk + 1);
        else if (ev->key == IC_KEY_UP && dm.selected_disk > 0) select_disk(dm.selected_disk - 1);
        else if (ev->key == IC_KEY_DELETE && can_remove()) open_modal(M_DELETE);
        else if (ev->key == 'r' || ev->key == 'R') refresh();
        load_usage(seg_part(cur_seg()));
        break;
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    dm_app = app;
    dm.selected_disk = 0;
    dm.selected_seg = 0;
    dm.hover_disk = -1;
    dm.hover_seg = -1;
    dm.runtime_device = -1;
    set_status("Select a disk to see its volumes", 0);
    refresh_keep(~0ULL);
    for (int i = 0; i < dm.disk_count; i++) {
        if (!disk_is_runtime(&dm.disks[i])) {
            select_disk(i);
            break;
        }
    }
}

static int parse_arg(const char *s, uint64_t *out) {
    uint64_t v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return 1;
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Disk Utility", WIN_W, WIN_H, init, draw, event, tick };
    if (argc >= 4 && s_eq(argv[1], "--install")) {
        icda_install_plan_t plan;
        uint64_t files = 0, bytes = 0, e = 0, r = 0;
        if (!parse_arg(argv[2], &e) || !parse_arg(argv[3], &r)) return 2;
        plan.efi_partition = e;
        plan.root_partition = r;
        plan.swap_partition = -1;
        return (long)icda_install_partitions(&plan, &files, &bytes) < 0 ? 1 : 0;
    }
    if (argc >= 3 && s_eq(argv[1], "--install-device")) {
        uint64_t files = 0, bytes = 0, dev = 0;
        if (!parse_arg(argv[2], &dev)) return 2;
        (void)icda_format_device(dev, LAYOUT_ICDA);
        return (long)icda_install_device(dev, &files, &bytes) < 0 ? 1 : 0;
    }
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("disk utility requires the desktop\n");
        return 1;
    }
    return 0;
}
