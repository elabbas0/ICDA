/* Install ICDA: a guide in four steps (the disk, how, a last look, the
 * copying) and what to do next.  A live start offers it first; Disk
 * Utility's Install ICDA opens it too (--disk N picks the disk).
 *
 * The disk work runs in a second copy of this program (--erase DEV, or
 * --alongside DEV START MB), so the window keeps drawing meanwhile: the
 * kernel reports how far the copying is (ICDA_DISK_INSTALL_STATUS), and the
 * helper leaves its result in RESULT_PATH ("ok", or "fail <code> <what>").
 * Nothing on a disk changes before the last step's button. */
#include "libicda.h"

#define WIN_W       760
#define WIN_H       520
#define RAIL_W      200
#define PAD         28
#define BODY_Y      116
#define INFO_CAP    8192
#define MAX_DEV     8
#define MAX_PART    32
#define MAX_FREE    32
#define MB          (1024ULL * 1024ULL)
#define GB          (1024ULL * MB)
#define SECTORS_MB  2048ULL
#define ICDA_MIN_MB 1024ULL            /* the smallest ICDA partition */
#define ERASE_MIN_MB 2048ULL           /* ICDA's own layout: EFI, swap and ICDA */
#define ESP_MB      260ULL
#define LAYOUT_EFI_MB  256ULL           /* what ICDA's layout makes (kernel/fs/diskfmt.c) */
#define LAYOUT_SWAP_MB 512ULL
#define FS_FAT32    1
#define LAYOUT_ICDA 4
#define RESULT_PATH "/tmp/.installer-result"

enum { S_DISK = 0, S_HOW, S_REVIEW, S_INSTALL, S_DONE };
enum { HOW_ALONGSIDE = 0, HOW_ERASE, HOW_MANUAL, HOW_COUNT };
enum { H_NONE = 0, H_BACK, H_NEXT, H_SLIDER, H_CONFIRM, H_RELOAD, H_DISK0 = 100, H_HOW0 = 200 };
enum { DRIVE_HDD = 0, DRIVE_NVME, DRIVE_USB };

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
} part_t;

typedef struct {
    uint64_t dev, start, sectors;
} gap_t;

static struct {
    char     info[INFO_CAP];
    disk_t   disks[MAX_DEV];
    part_t   parts[MAX_PART];
    gap_t    gaps[MAX_FREE];
    int      ndisks, nparts, ngaps;
    int      uefi, live, medium, runtime;   /* medium, runtime: device index or -1 */
    int      step;
    uint32_t step_ms;                      /* when the step was entered (its slide-in) */
    int      disk;                         /* the chosen disk (index into disks[]) */
    int      how;
    float    size;                         /* alongside: 0..1 between the smallest and the free space */
    int      confirm;                      /* erase: the box is ticked */
    int      hover, dragging;
    uint32_t click_ms;
    int      click_id;
    /* installing */
    uint64_t pid;
    uint32_t started, last_poll, last_change;
    uint32_t base_finished;
    char     stage[40];
    uint64_t cur, total;
    int      phase;                        /* 0 the disk, 1 copying, 2 startup, 3 finishing */
    float    progress, shown;
    int      ok;
    char     fail_title[64];
    char     fail_text[240];
    char     done_disk[48];
} st;

/* ---- small helpers -------------------------------------------------------------------- */

static uint64_t s_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int s_eq(const char *a, const char *b) { return ic_streq(a ? a : "", b ? b : ""); }

static int s_prefix(const char *s, const char *p) {
    while (*p)
        if (*s++ != *p++) return 0;
    return 1;
}

static void cat(char *out, uint64_t cap, const char *s) { ic_strlcat(out, s, cap); }

static void cat_num(char *out, uint64_t cap, uint64_t v) {
    char n[24];
    ic_uint_to_str(v, n, sizeof(n));
    ic_strlcat(out, n, cap);
}

/* 40.0 GB, 260 MB: decimal units, as disks are sold */
static void size_text(uint64_t bytes, char *out, uint64_t cap) {
    static const char *const units[5] = { "B", "KB", "MB", "GB", "TB" };
    uint64_t whole = bytes, frac = 0;
    int unit = 0;
    while (whole >= 1000 && unit < 4) {
        frac = whole % 1000;
        whole /= 1000;
        unit++;
    }
    out[0] = 0;
    cat_num(out, cap, whole);
    if (unit > 0 && whole < 100) {
        cat(out, cap, ".");
        cat_num(out, cap, frac / 100);
    }
    cat(out, cap, " ");
    cat(out, cap, units[unit]);
}

/* a sine good enough for a turning arc (as wm_assistant.c's) */
static float fsin(float x) {
    const float pi = 3.14159265f, tau = 6.2831853f;
    float y;
    x -= tau * (float)(int)(x / tau);
    if (x > pi) x -= tau;
    if (x < -pi) x += tau;
    y = 1.2732395f * x - 0.4052847f * x * (x < 0 ? -x : x);
    return 0.225f * (y * (y < 0 ? -y : y) - y) + y;
}

static float ease_out(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

/* ---- what the kernel says about the disks ---------------------------------------------- */

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

static int key_is(const char *s, uint64_t n, const char *key) { return n == s_len(key) && s_prefix(s, key); }

/* "  0: nvme0 table=gpt sectors=..." and the like (kernel/syscall/syscall.c sys_storage_info) */
static void parse_fields(const char *line, uint64_t len, int section) {
    disk_t *d = 0;
    part_t *p = 0;
    gap_t *g = 0;
    uint64_t pos = 0;
    int field = 0;
    if (section == 0 && st.ndisks < MAX_DEV) {
        d = &st.disks[st.ndisks++];
        ic_memzero(d, sizeof(*d));
        d->sector_size = 512;
    } else if (section == 1 && st.nparts < MAX_PART) {
        p = &st.parts[st.nparts++];
        ic_memzero(p, sizeof(*p));
    } else if (section == 2 && st.ngaps < MAX_FREE) {
        g = &st.gaps[st.ngaps++];
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
            uint64_t cap = d ? sizeof(d->name) : sizeof(p->name), at = s_len(name);
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
    if (d && !d->sector_size) d->sector_size = 512;
    if (d && d->name[0] && d->name[s_len(d->name) - 1] == ':') d->name[s_len(d->name) - 1] = 0;
    if (p && p->name[0] && p->name[s_len(p->name) - 1] == ':') p->name[s_len(p->name) - 1] = 0;
}

static void load_disks(void) {
    const char *p = st.info;
    int section = -1;
    uint64_t rt;
    ic_memzero(st.info, sizeof(st.info));
    st.ndisks = st.nparts = st.ngaps = 0;
    st.uefi = st.live = 0;
    st.medium = -1;
    (void)icda_storage_info(st.info, sizeof(st.info) - 1);
    while (*p) {
        const char *line = p;
        uint64_t len = 0;
        while (p[len] && p[len] != '\n') len++;
        p += len;
        if (*p == '\n') p++;
        if (!len) continue;
        if (line[0] != ' ') {
            section = -1;
            if (key_is(line, len, "devices:")) section = 0;
            else if (key_is(line, len, "partitions:")) section = 1;
            else if (key_is(line, len, "free:")) section = 2;
            else if (key_is(line, len, "firmware=uefi")) st.uefi = 1;
            else if (key_is(line, len, "boot=live")) st.live = 1;
            else if (len > 7 && s_prefix(line, "medium=")) st.medium = (int)span_u64(line + 7, len - 7);
            continue;
        }
        if (section == 2) parse_fields(line + 2, len - 2, 2);
        else if (section >= 0 && len > 2 && line[2] >= '0' && line[2] <= '9') parse_fields(line + 2, len - 2, section);
    }
    rt = icda_runtime_device();
    st.runtime = rt == (uint64_t)-1 ? -1 : (int)rt;
}

static disk_t *cur_disk(void) { return st.disk >= 0 && st.disk < st.ndisks ? &st.disks[st.disk] : 0; }

static uint64_t disk_bytes(const disk_t *d) { return d->sectors * d->sector_size; }

static int disk_kind(const disk_t *d) {
    if (s_prefix(d->name, "nvme")) return DRIVE_NVME;
    if (s_prefix(d->name, "usb")) return DRIVE_USB;
    return DRIVE_HDD;
}

static const char *disk_kind_name(const disk_t *d) {
    if (s_prefix(d->name, "nvme")) return "NVMe SSD";
    if (s_prefix(d->name, "usb")) return "USB drive";
    if (s_prefix(d->name, "ata")) return "IDE disk";
    return "SATA disk";
}

static void disk_title(const disk_t *d, char *out, uint64_t cap) {
    size_text(disk_bytes(d), out, cap);
    cat(out, cap, " ");
    cat(out, cap, disk_kind_name(d));
}

/* why ICDA cannot go on this disk, or 0 */
static const char *disk_blocked(const disk_t *d) {
    if (st.medium >= 0 && d->index == (uint64_t)st.medium) return "ICDA is running from this drive";
    if (st.runtime >= 0 && d->index == (uint64_t)st.runtime) return "ICDA is installed and running here";
    if (d->sector_size != 512) return "Its sector size is not supported";
    if (disk_bytes(d) < ICDA_MIN_MB * MB) return "Too small for ICDA";
    return 0;
}

static int part_on(const part_t *p, const disk_t *d) { return s_eq(p->dev, d->name); }

static const char *part_kind(const part_t *p) {
    if (s_eq(p->role, "efi")) return "EFI";
    if (s_eq(p->role, "system")) return "ICDA";
    if (s_eq(p->role, "swap")) return "Swap";
    if (s_eq(p->role, "msr")) return 0;
    if (s_eq(p->role, "recovery")) return "Windows Recovery";
    if (s_eq(p->role, "linux")) return "Linux";
    if (s_eq(p->role, "data")) return s_eq(p->fs, "ntfs") ? "Windows" : s_eq(p->fs, "exfat") ? "exFAT data" : "Data";
    return "Other";
}

static ic_volume_t part_volume(const part_t *p) {
    if (s_eq(p->role, "efi")) return IC_VOL_EFI;
    if (s_eq(p->role, "system")) return IC_VOL_ICDA;
    if (s_eq(p->role, "swap")) return IC_VOL_SWAP;
    if (s_eq(p->role, "linux")) return IC_VOL_LINUX;
    if (s_eq(p->role, "msr") || s_eq(p->role, "recovery")) return IC_VOL_RESERVED;
    if (s_eq(p->role, "data")) return s_eq(p->fs, "fat32") || s_eq(p->fs, "exfat") ? IC_VOL_DATA : IC_VOL_WINDOWS;
    return IC_VOL_UNKNOWN;
}

static uint64_t free_sectors(const disk_t *d) {
    uint64_t n = 0;
    for (int i = 0; i < st.ngaps; i++)
        if (st.gaps[i].dev == d->index) n += st.gaps[i].sectors;
    return n;
}

/* what is on a disk, in a line: "Windows · EFI · 12.4 GB free" */
static void disk_summary(const disk_t *d, char *out, uint64_t cap) {
    const char *seen[8];
    int nseen = 0;
    uint64_t fr = free_sectors(d);
    out[0] = 0;
    for (int i = 0; i < st.nparts; i++) {
        const char *k;
        int dup = 0;
        if (!part_on(&st.parts[i], d) || !(k = part_kind(&st.parts[i]))) continue;
        for (int j = 0; j < nseen; j++) dup |= s_eq(seen[j], k);
        if (dup || nseen == 8) continue;
        seen[nseen++] = k;
        if (out[0]) cat(out, cap, " \xC2\xB7 ");
        cat(out, cap, k);
    }
    if (!out[0]) cat(out, cap, s_eq(d->table, "none") || !d->table[0] ? "Empty, no partitions" : "No partitions");
    if (out[0] && nseen && fr >= 200 * SECTORS_MB) {
        char t[24];
        size_text(fr * 512ULL, t, sizeof(t));
        cat(out, cap, " \xC2\xB7 ");
        cat(out, cap, t);
        cat(out, cap, " free");
    }
}

/* the systems on a disk, for "Install alongside Windows" */
static const char *others_name(const disk_t *d) {
    int win = 0, lin = 0, icda = 0;
    for (int i = 0; i < st.nparts; i++) {
        const part_t *p = &st.parts[i];
        if (!part_on(p, d)) continue;
        win |= s_eq(p->role, "data") && s_eq(p->fs, "ntfs");
        lin |= s_eq(p->role, "linux");
        icda |= s_eq(p->role, "system");
    }
    if (win && lin) return "Windows and Linux";
    if (win) return "Windows";
    if (lin) return "Linux";
    if (icda) return "ICDA";
    return "what is on the disk";
}

static gap_t *largest_gap(const disk_t *d) {
    gap_t *best = 0;
    for (int i = 0; d && i < st.ngaps; i++)
        if (st.gaps[i].dev == d->index && (!best || st.gaps[i].sectors > best->sectors)) best = &st.gaps[i];
    return best;
}

static int disk_esp(const disk_t *d) {
    for (int i = 0; d && i < st.nparts; i++)
        if (part_on(&st.parts[i], d) && s_eq(st.parts[i].role, "efi") && s_eq(st.parts[i].fs, "fat32")) return i;
    return -1;
}

static uint64_t alongside_max_mb(const disk_t *d) {
    gap_t *g = largest_gap(d);
    uint64_t mb = g ? g->sectors / SECTORS_MB : 0;
    if (disk_esp(d) < 0) mb = mb > ESP_MB ? mb - ESP_MB : 0;
    return mb;
}

/* why ICDA cannot go next to what is there, or 0 */
static int part_count_on(const disk_t *d) {
    int n = 0;
    for (int i = 0; d && i < st.nparts; i++) n += part_on(&st.parts[i], d);
    return n;
}

static const char *alongside_blocked(const disk_t *d) {
    if (!d) return "No disk";
    if (!part_count_on(d)) return "There is nothing on this disk to keep: ICDA can have all of it.";
    if (!st.uefi) return "This computer started in legacy BIOS mode; installing next to another system needs UEFI.";
    if (!s_eq(d->table, "gpt")) return "Needs a GPT disk. This one has none, or an MBR table.";
    if (alongside_max_mb(d) < ICDA_MIN_MB)
        return s_eq(others_name(d), "Windows") || s_eq(others_name(d), "Windows and Linux")
                   ? "Needs 1 GB of free space. Shrink Windows' volume in its Disk Management to make room, "
                     "then start ICDA again."
                   : "Needs 1 GB of free space on the disk, and it has less.";
    return 0;
}

static const char *erase_blocked(const disk_t *d) {
    if (!d) return "No disk";
    if (disk_bytes(d) < ERASE_MIN_MB * MB) return "ICDA needs a disk of at least 2 GB.";
    return 0;
}

static int how_ok(int how) {
    disk_t *d = cur_disk();
    if (how == HOW_ALONGSIDE) return !alongside_blocked(d);
    if (how == HOW_ERASE) return !erase_blocked(d);
    return d != 0;
}

static uint64_t alongside_mb(void) {
    disk_t *d = cur_disk();
    uint64_t hi = d ? alongside_max_mb(d) : 0, lo = ICDA_MIN_MB;
    if (hi <= lo) return hi < lo ? hi : lo;
    return lo + (uint64_t)(st.size * (float)(hi - lo) + 0.5f);
}

/* ---- geometry -------------------------------------------------------------------------- */

static int slide_dx(void) {
    float t = (float)(ic_time_ms() - st.step_ms) / 220.0f;
    return (int)((1.0f - ease_out(t)) * 22.0f);
}

static int cx0(ic_app_t *app) { (void)app; return RAIL_W + PAD + slide_dx(); }
static int cw(ic_app_t *app) { return app->width - RAIL_W - 2 * PAD; }
static int body_bottom(ic_app_t *app) { return app->height - 72; }

static const char *next_label(void) {
    switch (st.step) {
    case S_HOW:    return st.how == HOW_MANUAL ? "Open Disk Utility" : "Continue";
    case S_REVIEW: return st.how == HOW_ERASE ? "Erase and install" : "Install";
    case S_DONE:   return st.ok ? "Restart now" : "Try again";
    default:       return "Continue";
    }
}

static ic_rect_t next_rect(ic_app_t *app) {
    int w = ic_ui_button_width(next_label(), IC_SYM_NONE);
    if (w < 120) w = 120;
    return ic_rect_make(app->width - PAD - w, app->height - 52, w, 32);
}

static int has_back(void) {
    return st.step == S_HOW || st.step == S_REVIEW || st.step == S_DONE;
}

static const char *back_label(void) {
    if (st.step == S_DONE) return st.ok ? (st.live ? "Keep trying ICDA" : "Close") : "Close";
    return "Back";
}

static ic_rect_t back_rect(ic_app_t *app) {
    ic_rect_t n = next_rect(app);
    int w = ic_ui_button_width(back_label(), IC_SYM_NONE);
    if (w < 96) w = 96;
    return ic_rect_make(n.x - 12 - w, n.y, w, 32);
}

static ic_rect_t disk_row(ic_app_t *app, int i) {
    int avail = body_bottom(app) - BODY_Y, h = 64;
    if (st.ndisks > 0 && st.ndisks * (h + 8) > avail) h = avail / st.ndisks - 8;
    return ic_rect_make(cx0(app), BODY_Y + i * (h + 8), cw(app), h);
}

#define HOW_Y 80                       /* the method step has a title only */

static int how_h(int i) {
    if (i == HOW_ALONGSIDE) return how_ok(HOW_ALONGSIDE) ? 112 : 70;
    return i == HOW_ERASE ? 64 : 56;
}

static ic_rect_t how_card(ic_app_t *app, int i) {
    int y = HOW_Y;
    for (int k = 0; k < i; k++) y += how_h(k) + 8;
    return ic_rect_make(cx0(app), y, cw(app), how_h(i));
}

static ic_rect_t slider_rect(ic_app_t *app) {
    ic_rect_t c = how_card(app, HOW_ALONGSIDE);
    return ic_rect_make(c.x + 40, c.y + 76, c.w - 40 - 140, 22);
}

static ic_rect_t confirm_rect(ic_app_t *app) {
    return ic_rect_make(cx0(app), body_bottom(app) - 30, cw(app), 24);
}

static ic_rect_t reload_rect(ic_app_t *app) {
    return ic_rect_make(cx0(app) + (cw(app) - 140) / 2, BODY_Y + 150, 140, 32);
}

static int hit(ic_app_t *app, int x, int y) {
    if (st.step != S_INSTALL && ic_ui_hit(next_rect(app), x, y)) return H_NEXT;
    if (has_back() && ic_ui_hit(back_rect(app), x, y)) return H_BACK;
    if (st.step == S_DISK) {
        if (!st.ndisks && ic_ui_hit(reload_rect(app), x, y)) return H_RELOAD;
        for (int i = 0; i < st.ndisks; i++)
            if (ic_ui_hit(disk_row(app, i), x, y)) return H_DISK0 + i;
    } else if (st.step == S_HOW) {
        if (st.how == HOW_ALONGSIDE && how_ok(HOW_ALONGSIDE) && ic_ui_hit(ic_rect_inset(slider_rect(app), -4, -6), x, y))
            return H_SLIDER;
        for (int i = 0; i < HOW_COUNT; i++)
            if (ic_ui_hit(how_card(app, i), x, y)) return H_HOW0 + i;
    } else if (st.step == S_REVIEW && st.how == HOW_ERASE && ic_ui_hit(confirm_rect(app), x, y)) {
        return H_CONFIRM;
    }
    return H_NONE;
}

static int next_enabled(void) {
    disk_t *d = cur_disk();
    switch (st.step) {
    case S_DISK:   return d && !disk_blocked(d);
    case S_HOW:    return how_ok(st.how);
    case S_REVIEW: return st.how != HOW_ERASE || st.confirm;
    case S_DONE:   return 1;
    default:       return 0;
    }
}

/* ---- drawing ---------------------------------------------------------------------------- */

static void draw_drive_icon(ic_canvas_t *c, int x, int y, int size, int kind) {
    float s = (float)size / 48.0f;
    if (kind == DRIVE_NVME) {
        int w = size, h = (int)(18 * s), top = y + (size - h) / 2;
        ic_gfx_shadow(c, x, top, w, h, 3 * s, (int)(4 * s) + 1, (int)(2 * s), 70);
        ic_gfx_rrect(c, x, top, w, h, 3 * s, ic_volume_color(IC_VOL_PCB));
        for (int i = 0; i < 3; i++)
            ic_gfx_rrect(c, x + (int)((12 + i * 11) * s), top + (int)(3 * s), (int)(9 * s), h - (int)(6 * s), 1.5f * s,
                         ic_volume_color(IC_VOL_CHIP));
        for (int i = 0; i < 4; i++)
            ic_gfx_fill(c, x + (int)(2 * s), top + (int)((3 + i * 3.5f) * s), (int)(6 * s), (int)(2 * s),
                        ic_volume_color(IC_VOL_GOLD));
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
    }
}

static void draw_rail(ic_app_t *app, ic_canvas_t *c) {
    static const char *const names[4] = { "Disk", "Method", "Review", "Install" };
    const ic_palette_t *p = ic_palette();
    const ic_icon_t *icon = ic_icon_builtin("install");
    int cur = st.step > S_INSTALL ? 4 : st.step;
    ic_ui_sidebar_bg(c, ic_rect_make(0, 0, RAIL_W, app->height));
    if (icon && ic_icon_valid(icon)) ic_gfx_image_rgba(c, 22, 22, 44, 44, icon->rgba, icon->w, icon->h, 255);
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(22, 76, RAIL_W - 30, 22), "Install ICDA", p->label,
                    IC_ALIGN_LEFT);
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(22, 98, RAIL_W - 30, 16), ic_version_label(),
                    p->label_secondary, IC_ALIGN_LEFT);
    for (int i = 0; i < 4; i++) {
        float x = 36.0f, y = 156.0f + (float)i * 46.0f;
        char num[4] = { (char)('1' + i), 0, 0, 0 };
        int done = i < cur || (st.step == S_DONE && st.ok);
        if (i < 3) {
            ic_color_t line = done ? p->success : p->separator;
            ic_gfx_fill(c, (int)x - 1, (int)y + 14, 2, 18, line);
        }
        if (done) {
            ic_gfx_circle(c, x, y, 12.0f, p->success);
            ic_symbol_draw(c, IC_SYM_CHECK, x, y, 12.0f, IC_WHITE);
        } else if (i == cur) {
            ic_gfx_circle(c, x, y, 12.0f, st.step == S_DONE ? p->danger : p->accent);
            ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make((int)x - 12, (int)y - 12, 24, 24), num,
                            p->label_on_accent, IC_ALIGN_CENTER);
        } else {
            ic_gfx_ring(c, x, y, 11.0f, 1.5f, p->label_tertiary);
            ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make((int)x - 12, (int)y - 12, 24, 24), num,
                            p->label_tertiary, IC_ALIGN_CENTER);
        }
        ic_text_draw_in(c, ic_font(i == cur ? IC_FONT_BODY_EMPH : IC_FONT_BODY), ic_rect_make((int)x + 22, (int)y - 11, 120, 22),
                        names[i], i == cur || done ? p->label : p->label_secondary, IC_ALIGN_LEFT);
    }
    if (st.live) {
        ic_symbol_draw(c, IC_SYM_INFO, 30.0f, (float)app->height - 66.0f, 14.0f, p->label_tertiary);
        ic_text_draw_wrapped(c, ic_font(IC_FONT_CAPTION), ic_rect_make(44, app->height - 74, RAIL_W - 54, 60),
                             "Running from the USB drive. Nothing is kept until ICDA is installed.",
                             p->label_tertiary, 2);
    }
}

static void draw_header(ic_app_t *app, ic_canvas_t *c, const char *title, const char *sub) {
    const ic_palette_t *p = ic_palette();
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE2), ic_rect_make(cx0(app), 26, cw(app), 30), title, p->label, IC_ALIGN_LEFT);
    if (sub)
        ic_text_draw_wrapped(c, ic_font(IC_FONT_BODY), ic_rect_make(cx0(app), 62, cw(app), 44), sub, p->label_secondary, 3);
}

/* a selectable card: the accent when chosen, the danger colour for erasing */
static void card_bg(ic_canvas_t *c, ic_rect_t r, int selected, int hovered, int enabled, int danger) {
    const ic_palette_t *p = ic_palette();
    ic_color_t edge = danger ? p->danger : p->accent;
    ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_GROUP,
                 selected ? (danger ? ic_color_with_alpha(p->danger, 36) : p->accent_soft) : p->group);
    if (hovered && enabled && !selected) ic_gfx_rrect(c, r.x, r.y, r.w, r.h, IC_R_GROUP, p->fill_hover);
    ic_gfx_rrect_stroke(c, r.x, r.y, r.w, r.h, IC_R_GROUP, selected ? 2.0f : 1.0f, selected ? edge : p->group_stroke);
}

static void radio(ic_canvas_t *c, float x, float y, int on, int enabled, int danger) {
    const ic_palette_t *p = ic_palette();
    ic_gfx_ring(c, x, y, 8.0f, 1.5f, enabled ? p->label_secondary : p->label_disabled);
    if (on) ic_gfx_circle(c, x, y, 4.5f, danger ? p->danger : p->accent);
}

static void draw_disk_step(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    draw_header(app, c, "Where should ICDA go?",
                "Choose a disk. Nothing on it changes until you confirm on the last step.");
    if (!st.ndisks) {
        ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(cx0(app), BODY_Y + 60, cw(app), 24), "No disk found",
                        p->label, IC_ALIGN_CENTER);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(cx0(app), BODY_Y + 90, cw(app), 22),
                        "Connect a disk, then look again.", p->label_secondary, IC_ALIGN_CENTER);
        ic_ui_button(c, reload_rect(app), "Look again", IC_SYM_RELOAD, IC_BUTTON_DEFAULT,
                     st.hover == H_RELOAD ? IC_STATE_HOVER : IC_STATE_NORMAL);
        return;
    }
    for (int i = 0; i < st.ndisks; i++) {
        disk_t *d = &st.disks[i];
        ic_rect_t r = disk_row(app, i);
        const char *why = disk_blocked(d);
        char t[64], s[160];
        int sel = st.disk == i && !why, icon = r.h - 16 > 44 ? 44 : r.h - 16;
        card_bg(c, r, sel, st.hover == H_DISK0 + i, !why, 0);
        draw_drive_icon(c, r.x + 14, r.y + (r.h - icon) / 2, icon, disk_kind(d));
        disk_title(d, t, sizeof(t));
        ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(r.x + 72, r.y + r.h / 2 - 20, r.w - 220, 20), t,
                        why ? p->label_disabled : p->label, IC_ALIGN_LEFT);
        disk_summary(d, s, sizeof(s));
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + 72, r.y + r.h / 2 + 2, r.w - 90, 18), why ? why : s,
                        why ? p->label_tertiary : p->label_secondary, IC_ALIGN_LEFT);
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(r.x + r.w - 150, r.y + r.h / 2 - 18, 134, 16), d->name,
                        p->label_tertiary, IC_ALIGN_RIGHT);
        if (sel) ic_symbol_draw(c, IC_SYM_CHECK, (float)(r.x + r.w - 24), (float)(r.y + r.h / 2 + 8), 14.0f, p->accent);
    }
}

/* a disk drawn as a bar of its partitions, sized by what they hold; ICDA's
 * new partitions are lit.  kind: 0 as it is, 1 with ICDA next to it, 2 erased */
typedef struct { uint64_t sectors; ic_volume_t vol; const char *label; int fresh; } seg_t;

static int plan_segments(const disk_t *d, int kind, seg_t *out, int cap) {
    int n = 0;
    if (!d) return 0;
    if (kind == 2) {
        uint64_t total = d->sectors, efi = LAYOUT_EFI_MB * SECTORS_MB, swap = LAYOUT_SWAP_MB * SECTORS_MB;
        out[n++] = (seg_t){ efi, IC_VOL_EFI, "EFI", 1 };
        out[n++] = (seg_t){ swap, IC_VOL_SWAP, "Swap", 1 };
        out[n++] = (seg_t){ total > efi + swap ? total - efi - swap : 1, IC_VOL_ICDA, "ICDA", 1 };
        return n;
    }
    {
        /* partitions and gaps in disk order */
        gap_t *big = kind == 1 ? largest_gap(d) : 0;
        uint64_t at = 0;
        for (;;) {
            uint64_t best = ~0ULL;
            int bp = -1, bg = -1;
            for (int i = 0; i < st.nparts; i++)
                if (part_on(&st.parts[i], d) && st.parts[i].start >= at && st.parts[i].start < best) best = st.parts[i].start, bp = i, bg = -1;
            for (int i = 0; i < st.ngaps; i++)
                if (st.gaps[i].dev == d->index && st.gaps[i].start >= at && st.gaps[i].start < best) best = st.gaps[i].start, bg = i, bp = -1;
            if ((bp < 0 && bg < 0) || n >= cap - 3) break;
            if (bp >= 0) {
                part_t *pt = &st.parts[bp];
                out[n++] = (seg_t){ pt->sectors, part_volume(pt), part_kind(pt) ? part_kind(pt) : "Reserved", 0 };
                at = pt->start + pt->sectors;
            } else {
                gap_t *g = &st.gaps[bg];
                uint64_t left = g->sectors;
                if (g == big) {
                    uint64_t icda = alongside_mb() * SECTORS_MB;
                    if (disk_esp(d) < 0) {
                        out[n++] = (seg_t){ ESP_MB * SECTORS_MB, IC_VOL_EFI, "EFI", 1 };
                        left = left > ESP_MB * SECTORS_MB ? left - ESP_MB * SECTORS_MB : 0;
                    }
                    if (icda > left) icda = left;
                    out[n++] = (seg_t){ icda, IC_VOL_ICDA, "ICDA", 1 };
                    left -= icda;
                }
                if (left >= SECTORS_MB) out[n++] = (seg_t){ left, IC_VOL_FREE, "Free", 0 };
                at = g->start + g->sectors;
            }
        }
    }
    return n;
}

static void draw_bar(ic_canvas_t *c, ic_rect_t r, const disk_t *d, int kind) {
    const ic_palette_t *p = ic_palette();
    seg_t segs[MAX_PART + MAX_FREE];
    int n = plan_segments(d, kind, segs, (int)(sizeof(segs) / sizeof(segs[0]))), x = r.x, minw = 6;
    uint64_t total = 0;
    int widths[MAX_PART + MAX_FREE], spare;
    for (int i = 0; i < n; i++) total += segs[i].sectors;
    if (!n || !total) {
        ic_gfx_rrect(c, r.x, r.y, r.w, r.h, 6.0f, ic_volume_color(IC_VOL_FREE));
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), r, "Empty", p->label_secondary, IC_ALIGN_CENTER);
        return;
    }
    spare = r.w - 2 * (n - 1) - n * minw;
    if (spare < 0) spare = 0;
    for (int i = 0; i < n; i++) widths[i] = minw + (int)((uint64_t)spare * segs[i].sectors / total);
    for (int i = 0; i < n; i++) {
        int w = i == n - 1 ? r.x + r.w - x : widths[i];
        ic_color_t col = ic_volume_color(segs[i].vol);
        ic_rect_t s = ic_rect_make(x, r.y, w, r.h);
        if (segs[i].vol == IC_VOL_FREE) {
            ic_gfx_rrect(c, s.x, s.y, s.w, s.h, 5.0f, ic_color_with_alpha(p->label_tertiary, 40));
        } else {
            ic_gfx_rrect_gradient_v(c, s.x, s.y, s.w, s.h, 5.0f, ic_color_mix(col, IC_WHITE, 0.12f), col);
            if (!segs[i].fresh && kind != 0) ic_gfx_rrect(c, s.x, s.y, s.w, s.h, 5.0f, IC_BLACK_A(60));
        }
        if (segs[i].fresh) ic_gfx_rrect_stroke(c, s.x - 1, s.y - 1, s.w + 2, s.h + 2, 6.0f, 2.0f, p->accent);
        if (w > 46) {
            char t[24];
            size_text(segs[i].sectors * 512ULL, t, sizeof(t));
            ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(s.x + 6, s.y + 3, s.w - 10, 14), segs[i].label,
                            segs[i].vol == IC_VOL_FREE ? p->label_secondary : IC_WHITE, IC_ALIGN_LEFT);
            if (s.h >= 32)
                ic_text_draw_in(c, ic_font(IC_FONT_CAPTION), ic_rect_make(s.x + 6, s.y + 17, s.w - 10, 14), t,
                                segs[i].vol == IC_VOL_FREE ? p->label_tertiary : IC_WHITE_A(210), IC_ALIGN_LEFT);
        }
        x += w + 2;
    }
}

static void draw_how_step(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    disk_t *d = cur_disk();
    char title[96], t[160], u[32];
    title[0] = 0;
    cat(title, sizeof(title), "How should ICDA go on the ");
    if (d) disk_title(d, t, sizeof(t));
    cat(title, sizeof(title), d ? t : "disk");
    cat(title, sizeof(title), "?");
    draw_header(app, c, title, 0);
    for (int i = 0; i < HOW_COUNT; i++) {
        ic_rect_t r = how_card(app, i);
        int ok = how_ok(i), sel = st.how == i, danger = i == HOW_ERASE;
        const char *head = "", *text = "";
        card_bg(c, r, sel && ok, st.hover == H_HOW0 + i, ok, danger);
        radio(c, (float)r.x + 20, (float)r.y + 22, sel && ok, ok, danger);
        if (i == HOW_ALONGSIDE) {
            t[0] = 0;
            cat(t, sizeof(t), "Install alongside ");
            cat(t, sizeof(t), others_name(d));
            head = t;
            text = ok ? "Uses free space on the disk; nothing is deleted. Both start from the boot menu."
                      : alongside_blocked(d);
        } else if (i == HOW_ERASE) {
            head = "Erase the disk and install ICDA";
            text = ok ? "Everything on the disk is deleted. ICDA gets all of it, with its swap partition."
                      : erase_blocked(d);
        } else {
            head = "Choose partitions myself";
            text = "Opens Disk Utility to pick the EFI and ICDA partitions. For special setups.";
        }
        ic_text_draw_in(c, ic_font(IC_FONT_BODY_EMPH), ic_rect_make(r.x + 40, r.y + 12, r.w - 56, 20), head,
                        ok ? (danger && sel ? p->danger : p->label) : p->label_disabled, IC_ALIGN_LEFT);
        ic_text_draw_wrapped(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(r.x + 40, r.y + 32, r.w - 56, r.h - 34),
                             text, ok ? p->label_secondary : p->label_tertiary, 2);
        if (i == HOW_ALONGSIDE && ok) {
            ic_rect_t sr = slider_rect(app);
            uint64_t mb = alongside_mb();
            ic_ui_slider(c, sr, st.size, !sel ? IC_STATE_DISABLED : st.hover == H_SLIDER ? IC_STATE_HOVER : IC_STATE_NORMAL);
            t[0] = 0;
            cat(t, sizeof(t), "ICDA gets ");
            size_text(mb * MB, u, sizeof(u));
            cat(t, sizeof(t), u);
            ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(sr.x + sr.w + 14, sr.y + 2, 128, 18), t,
                            sel ? p->label : p->label_secondary, IC_ALIGN_LEFT);
        }
    }
    {
        /* what the disk will hold, where there is room for it */
        ic_rect_t last = how_card(app, HOW_COUNT - 1);
        int y = last.y + last.h + 14;
        if (how_ok(st.how) && st.how != HOW_MANUAL && y + 20 + 30 <= body_bottom(app)) {
            ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(cx0(app), y, cw(app), 16), "AFTER INSTALLING",
                            p->label_tertiary, IC_ALIGN_LEFT);
            draw_bar(c, ic_rect_make(cx0(app), y + 20, cw(app), 30), d, st.how == HOW_ERASE ? 2 : 1);
        }
    }
}

static int add_bullet(ic_canvas_t *c, ic_app_t *app, int y, const char *text, ic_color_t col) {
    const ic_palette_t *p = ic_palette();
    ic_gfx_circle(c, (float)cx0(app) + 4.0f, (float)y + 9.0f, 2.5f, p->label_secondary);
    ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(cx0(app) + 16, y, cw(app) - 16, 20), text, col, IC_ALIGN_LEFT);
    return y + 24;
}

static void draw_review_step(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    disk_t *d = cur_disk();
    char t[200], u[48];
    int y = BODY_Y;
    if (!d) return;
    draw_header(app, c, "Ready to install",
                st.how == HOW_ERASE ? "Check this last: erasing cannot be undone."
                                    : "Check what happens, then install. Your other systems and files stay.");
    disk_title(d, t, sizeof(t));
    cat(t, sizeof(t), "  (");
    cat(t, sizeof(t), d->name);
    cat(t, sizeof(t), ")");
    ic_text_draw_in(c, ic_font(IC_FONT_HEADLINE), ic_rect_make(cx0(app), y, cw(app), 22), t, p->label, IC_ALIGN_LEFT);
    y += 30;
    if (st.how == HOW_ERASE) {
        char s[160];
        disk_summary(d, s, sizeof(s));
        t[0] = 0;
        if (part_count_on(d)) {
            cat(t, sizeof(t), "Deletes everything on it: ");
            cat(t, sizeof(t), s);
            y = add_bullet(c, app, y, t, p->danger);
        } else {
            y = add_bullet(c, app, y, "The disk is empty: nothing on it is lost", p->label);
        }
        y = add_bullet(c, app, y, "Makes an EFI partition, a 512 MB swap partition and ICDA's partition", p->label);
    } else {
        if (disk_esp(d) < 0) y = add_bullet(c, app, y, "Makes a 260 MB EFI partition for starting up", p->label);
        t[0] = 0;
        cat(t, sizeof(t), "Makes a ");
        size_text(alongside_mb() * MB, u, sizeof(u));
        cat(t, sizeof(t), u);
        cat(t, sizeof(t), " ICDA partition in the free space");
        y = add_bullet(c, app, y, t, p->label);
        t[0] = 0;
        cat(t, sizeof(t), "Leaves ");
        cat(t, sizeof(t), others_name(d));
        cat(t, sizeof(t), " as it is");
        y = add_bullet(c, app, y, t, p->label);
    }
    y = add_bullet(c, app, y, st.uefi ? "Adds ICDA to the computer's boot menu" : "Makes the disk start ICDA", p->label);
    y += 8;
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(cx0(app), y, 80, 16), "NOW", p->label_tertiary, IC_ALIGN_LEFT);
    draw_bar(c, ic_rect_make(cx0(app) + 56, y - 4, cw(app) - 56, 26), d, 0);
    y += 34;
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH), ic_rect_make(cx0(app), y, 80, 16), "AFTER", p->label_tertiary, IC_ALIGN_LEFT);
    draw_bar(c, ic_rect_make(cx0(app) + 56, y - 4, cw(app) - 56, 26), d, st.how == HOW_ERASE ? 2 : 1);
    y += 40;
    ic_text_draw_wrapped(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(cx0(app), y, cw(app), 36),
                         "Afterwards: restart and remove the USB drive. On its first start ICDA asks you to make your "
                         "account.", p->label_secondary, 2);
    if (st.how == HOW_ERASE) {
        ic_rect_t r = confirm_rect(app);
        ic_gfx_rrect(c, r.x + 2, r.y + 3, 18, 18, 4.0f, st.confirm ? p->danger : p->control);
        ic_gfx_rrect_stroke(c, r.x + 2, r.y + 3, 18, 18, 4.0f, 1.0f, st.hover == H_CONFIRM ? p->danger : p->control_stroke);
        if (st.confirm) ic_symbol_draw(c, IC_SYM_CHECK, (float)r.x + 11, (float)r.y + 12, 12.0f, IC_WHITE);
        t[0] = 0;
        cat(t, sizeof(t), "Yes, erase everything on the ");
        disk_title(d, u, sizeof(u));
        cat(t, sizeof(t), u);
        ic_text_draw_in(c, ic_font(IC_FONT_BODY), ic_rect_make(r.x + 30, r.y + 2, r.w - 30, 20), t, p->label, IC_ALIGN_LEFT);
    }
}

static const char *const phase_names[4] = { "Preparing the disk", "Copying ICDA", "Setting up startup", "Finishing" };

static void draw_install_step(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    uint32_t now = ic_time_ms(), secs = (now - st.started) / 1000;
    char t[96], n[24];
    int y = BODY_Y + 4;
    draw_header(app, c, "Installing ICDA", "Keep the computer on and the USB drive in. This takes a few minutes.");
    for (int i = 0; i < 4; i++, y += 38) {
        float x = (float)cx0(app) + 12.0f, cy = (float)y + 12.0f;
        if (i < st.phase) {
            ic_gfx_circle(c, x, cy, 11.0f, p->success);
            ic_symbol_draw(c, IC_SYM_CHECK, x, cy, 11.0f, IC_WHITE);
        } else if (i == st.phase) {
            /* a turning arc */
            float a = (float)(now % 1000) / 1000.0f * 6.2832f;
            ic_gfx_ring(c, x, cy, 10.0f, 2.0f, ic_color_with_alpha(p->accent, 60));
            for (int k = 0; k < 10; k++) {
                float b = a + (float)k * 0.16f;
                ic_gfx_circle(c, x + 10.0f * fsin(b + 1.5707963f), cy + 10.0f * fsin(b), 1.6f, p->accent);
            }
        } else {
            ic_gfx_ring(c, x, cy, 10.0f, 1.5f, p->label_tertiary);
        }
        ic_text_draw_in(c, ic_font(i == st.phase ? IC_FONT_BODY_EMPH : IC_FONT_BODY), ic_rect_make((int)x + 22, y + 2, cw(app) - 40, 20),
                        phase_names[i], i <= st.phase ? p->label : p->label_secondary, IC_ALIGN_LEFT);
    }
    y += 12;
    ic_ui_progress(c, ic_rect_make(cx0(app), y, cw(app) - 56, 8), st.shown, p->accent);
    t[0] = 0;
    cat_num(t, sizeof(t), (uint64_t)(st.shown * 100.0f + 0.5f));
    cat(t, sizeof(t), "%");
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(cx0(app) + cw(app) - 48, y - 6, 48, 18), t, p->label,
                    IC_ALIGN_RIGHT);
    y += 20;
    t[0] = 0;
    cat(t, sizeof(t), st.stage[0] ? st.stage : st.phase == 0 ? "Making ICDA's partitions" : "Working");
    cat(t, sizeof(t), " \xC2\xB7 ");
    cat_num(t, sizeof(t), secs / 60);
    cat(t, sizeof(t), ":");
    ic_uint_to_str(secs % 60, n, sizeof(n));
    if (secs % 60 < 10) cat(t, sizeof(t), "0");
    cat(t, sizeof(t), n);
    ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE), ic_rect_make(cx0(app), y, cw(app), 18), t, p->label_secondary,
                    IC_ALIGN_LEFT);
}

static void draw_done_step(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    float cx = (float)cx0(app) + cw(app) / 2.0f, cy = (float)BODY_Y + 24.0f;
    char t[240];
    ic_gfx_circle(c, cx, cy, 34.0f, st.ok ? p->success : p->danger);
    ic_symbol_draw(c, st.ok ? IC_SYM_CHECK : IC_SYM_WARNING, cx, cy, 30.0f, IC_WHITE);
    ic_text_draw_in(c, ic_font(IC_FONT_TITLE2), ic_rect_make(cx0(app), BODY_Y + 74, cw(app), 30),
                    st.ok ? "ICDA is installed" : st.fail_title, p->label, IC_ALIGN_CENTER);
    t[0] = 0;
    if (st.ok) {
        cat(t, sizeof(t), "ICDA is on the ");
        cat(t, sizeof(t), st.done_disk);
        cat(t, sizeof(t), st.live ? ". Restart, and remove the USB drive when the screen goes dark. "
                                  : ". Restart the computer to start it. ");
        cat(t, sizeof(t), st.uefi ? "ICDA is in the boot menu, with the other systems it found. "
                                  : "Start the computer from that disk. ");
        cat(t, sizeof(t), "On its first start it asks you to make your account.");
    } else {
        cat(t, sizeof(t), st.fail_text);
    }
    ic_text_draw_wrapped(c, ic_font(IC_FONT_BODY), ic_rect_make(cx0(app) + 20, BODY_Y + 112, cw(app) - 40, 100), t,
                         p->label_secondary, 3);
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    const ic_palette_t *p = ic_palette();
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_rail(app, c);
    switch (st.step) {
    case S_DISK:    draw_disk_step(app, c); break;
    case S_HOW:     draw_how_step(app, c); break;
    case S_REVIEW:  draw_review_step(app, c); break;
    case S_INSTALL: draw_install_step(app, c); break;
    default:        draw_done_step(app, c); break;
    }
    if (st.step != S_INSTALL) {
        int en = next_enabled();
        ic_button_style_t style = (st.step == S_REVIEW && st.how == HOW_ERASE) ? IC_BUTTON_DESTRUCTIVE : IC_BUTTON_PRIMARY;
        ic_gfx_hline(c, RAIL_W, app->height - 68, app->width - RAIL_W, p->separator);
        ic_ui_button(c, next_rect(app), next_label(), IC_SYM_NONE, style,
                     !en ? IC_STATE_DISABLED : st.hover == H_NEXT ? IC_STATE_HOVER : IC_STATE_NORMAL);
        if (has_back())
            ic_ui_button(c, back_rect(app), back_label(), IC_SYM_NONE, IC_BUTTON_DEFAULT,
                         st.hover == H_BACK ? IC_STATE_HOVER : IC_STATE_NORMAL);
    }
}

/* ---- the steps ---------------------------------------------------------------------------- */

static void go(ic_app_t *app, int step) {
    st.step = step;
    st.step_ms = ic_time_ms();
    st.hover = H_NONE;
    ic_app_animate(app);
}

/* the disk most likely meant: the biggest one ICDA may use, inside the computer */
static void pick_disk(int prefer) {
    int best = -1;
    for (int i = 0; i < st.ndisks; i++) {
        disk_t *d = &st.disks[i];
        if (disk_blocked(d)) continue;
        if (prefer >= 0 && d->index == (uint64_t)prefer) { best = i; break; }
        if (best < 0 || (disk_kind(&st.disks[best]) == DRIVE_USB && disk_kind(d) != DRIVE_USB) ||
            ((disk_kind(&st.disks[best]) == DRIVE_USB) == (disk_kind(d) == DRIVE_USB) && disk_bytes(d) > disk_bytes(&st.disks[best])))
            best = i;
    }
    st.disk = best;
}

static void pick_how(void) {
    st.how = how_ok(HOW_ALONGSIDE) ? HOW_ALONGSIDE : how_ok(HOW_ERASE) ? HOW_ERASE : HOW_MANUAL;
    st.size = 1.0f;                                     /* all the free space, to start with */
}

static void fail(const char *title, const char *text) {
    st.ok = 0;
    ic_strcpy(st.fail_title, title, sizeof(st.fail_title));
    ic_strcpy(st.fail_text, text, sizeof(st.fail_text));
}

static void start_install(ic_app_t *app) {
    disk_t *d = cur_disk();
    icda_disk_edit_t req;
    char args[96];
    if (!d) return;
    args[0] = 0;
    if (st.how == HOW_ERASE) {
        cat(args, sizeof(args), "--erase ");
        cat_num(args, sizeof(args), d->index);
    } else {
        gap_t *g = largest_gap(d);
        if (!g) return;
        cat(args, sizeof(args), "--alongside ");
        cat_num(args, sizeof(args), d->index);
        cat(args, sizeof(args), " ");
        cat_num(args, sizeof(args), g->start);
        cat(args, sizeof(args), " ");
        cat_num(args, sizeof(args), alongside_mb());
    }
    disk_title(d, st.done_disk, sizeof(st.done_disk));
    (void)icda_write_file(RESULT_PATH, "run\n", 4);
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_INSTALL_STATUS;
    (void)icda_disk_edit(&req);
    st.base_finished = req.partition;
    st.stage[0] = 0;
    st.phase = 0;
    st.progress = st.shown = 0.0f;
    st.started = st.last_change = ic_time_ms();
    st.pid = icda_spawn_args("/apps/installer.app", args);
    if ((long)st.pid < 0) {
        fail("The installer could not start", "Its helper could not be started. Nothing on the disk was changed.");
        go(app, S_DONE);
        return;
    }
    go(app, S_INSTALL);
}

static void open_manual(ic_app_t *app) {
    disk_t *d = cur_disk();
    char args[48];
    args[0] = 0;
    cat(args, sizeof(args), "--manual ");
    cat_num(args, sizeof(args), d ? d->index : 0);
    (void)icda_spawn_args("/apps/diskman.app", args);
    ic_app_quit(app);
}

static void next(ic_app_t *app) {
    if (!next_enabled()) return;
    switch (st.step) {
    case S_DISK:
        pick_how();
        go(app, S_HOW);
        break;
    case S_HOW:
        if (st.how == HOW_MANUAL) {
            open_manual(app);
            return;
        }
        st.confirm = 0;
        go(app, S_REVIEW);
        break;
    case S_REVIEW:
        start_install(app);
        break;
    case S_DONE:
        if (st.ok) {
            (void)icda_sync();
            (void)icda_power(1);
        } else {
            load_disks();
            go(app, S_DISK);
        }
        break;
    default:
        break;
    }
}

static void back(ic_app_t *app) {
    if (st.step == S_HOW) go(app, S_DISK);
    else if (st.step == S_REVIEW) go(app, S_HOW);
    else if (st.step == S_DONE) ic_app_quit(app);
}

/* the phase and the share done, from what the kernel's install says */
static void read_stage(const char *stage, uint64_t cur, uint64_t total) {
    float frac = total ? (float)cur / (float)total : 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    if (s_prefix(stage, "Finalizing")) {
        st.phase = 3;
        st.progress = 0.96f + 0.04f * frac;
    } else if (s_prefix(stage, "Preparing boot") || s_prefix(stage, "Writing boot") || s_prefix(stage, "Registering")) {
        st.phase = 2;
        st.progress = 0.88f;
    } else if (s_prefix(stage, "Installing WebKit")) {
        st.phase = 1;
        st.progress = 0.62f + 0.24f * frac;
    } else if (s_prefix(stage, "Preparing root")) {
        st.phase = 1;
        st.progress = 0.60f;
    } else if (stage[0]) {
        st.phase = 1;
        st.progress = 0.06f + 0.52f * frac;
    }
}

static void finish_install(ic_app_t *app, const char *res) {
    (void)icda_waitpid(st.pid);
    if (s_prefix(res, "ok")) {
        st.ok = 1;
        st.phase = 4;
        st.progress = st.shown = 1.0f;
    } else {
        /* "fail <code> <what>" */
        const char *w = res + 5;
        uint64_t code = 0;
        char t[240];
        while (*w >= '0' && *w <= '9') code = code * 10 + (uint64_t)(*w++ - '0');
        while (*w == ' ') w++;
        t[0] = 0;
        if (s_prefix(w, "format")) {
            cat(t, sizeof(t), "The disk could not be prepared (error ");
            cat_num(t, sizeof(t), code);
            cat(t, sizeof(t), "). It may be in use: ICDA cannot erase a disk it runs or swaps from.");
            fail("The disk could not be erased", t);
        } else if (s_prefix(w, "partition")) {
            cat(t, sizeof(t), "ICDA's partitions could not be made in the free space (error ");
            cat_num(t, sizeof(t), code);
            cat(t, sizeof(t), "). Your other partitions were not touched.");
            fail("ICDA could not be installed", t);
        } else {
            cat(t, sizeof(t), "Copying ICDA to the disk stopped with error ");
            cat_num(t, sizeof(t), code);
            cat(t, sizeof(t), ". The disk may be faulty or too full. You can try again, or choose another disk.");
            fail("ICDA could not be installed", t);
        }
    }
    load_disks();
    go(app, S_DONE);
}

static void tick(ic_app_t *app) {
    uint32_t now = ic_time_ms();
    int anim = now - st.step_ms < 260;
    if (st.step == S_INSTALL) {
        anim = 1;
        if (now - st.last_poll >= 150) {
            icda_disk_edit_t req;
            char res[64];
            long n;
            st.last_poll = now;
            ic_memzero(&req, sizeof(req));
            req.op = ICDA_DISK_INSTALL_STATUS;
            if (icda_disk_edit(&req) == 0 && req.role) {
                if (!s_eq(st.stage, req.name) || st.cur != req.start_lba) st.last_change = now;
                ic_strcpy(st.stage, req.name, sizeof(st.stage));
                st.cur = req.start_lba;
                st.total = req.sectors;
                read_stage(st.stage, st.cur, st.total);
            } else if (st.phase == 0) {
                /* the partitions are being made: creep, so it is seen to be working */
                float t = (float)(now - st.started) / 60000.0f;
                st.progress = 0.05f * (t > 1.0f ? 1.0f : t);
            }
            ic_memzero(res, sizeof(res));
            n = (long)icda_read_file(RESULT_PATH, res, sizeof(res) - 1);
            if (n > 0 && (s_prefix(res, "ok") || s_prefix(res, "fail"))) {
                finish_install(app, res);
                return;
            }
            if (now - st.last_change > 15U * 60U * 1000U) {
                fail("The installer stopped answering", "Nothing has happened for 15 minutes. Restart the computer "
                     "and try again; if the disk was being erased, erase it again.");
                go(app, S_DONE);
                return;
            }
        }
    }
    if (st.shown < st.progress) {
        st.shown += (st.progress - st.shown) * 0.12f + 0.001f;
        if (st.shown > st.progress) st.shown = st.progress;
        anim = 1;
    }
    if (anim) {
        ic_app_invalidate(app);
        ic_app_animate(app);
    }
}

/* ---- input -------------------------------------------------------------------------------- */

static void set_slider(ic_app_t *app, int x) {
    st.size = ic_ui_slider_value(slider_rect(app), x);
    if (st.size < 0.0f) st.size = 0.0f;
    if (st.size > 1.0f) st.size = 1.0f;
}

static void move_disk(int dir) {
    for (int k = 1; k <= st.ndisks; k++) {
        int i = (st.disk + dir * k + st.ndisks * 4) % st.ndisks;
        if (!disk_blocked(&st.disks[i])) {
            st.disk = i;
            return;
        }
    }
}

static void move_how(int dir) {
    for (int k = 1; k <= HOW_COUNT; k++) {
        int i = (st.how + dir * k + HOW_COUNT * 4) % HOW_COUNT;
        if (how_ok(i)) {
            st.how = i;
            return;
        }
    }
}

static void click(ic_app_t *app, int h, int x) {
    uint32_t now = ic_time_ms();
    int dbl = h == st.click_id && now - st.click_ms < 400;
    st.click_id = h;
    st.click_ms = now;
    if (h == H_NEXT) next(app);
    else if (h == H_BACK) back(app);
    else if (h == H_RELOAD) {
        load_disks();
        pick_disk(-1);
    } else if (h >= H_DISK0 && h < H_DISK0 + st.ndisks) {
        if (!disk_blocked(&st.disks[h - H_DISK0])) {
            st.disk = h - H_DISK0;
            if (dbl) next(app);
        }
    } else if (h == H_SLIDER) {
        st.how = HOW_ALONGSIDE;
        st.dragging = 1;
        set_slider(app, x);
    } else if (h >= H_HOW0 && h < H_HOW0 + HOW_COUNT) {
        if (how_ok(h - H_HOW0)) {
            st.how = h - H_HOW0;
            if (dbl) next(app);
        }
    } else if (h == H_CONFIRM) {
        st.confirm = !st.confirm;
    }
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE: {
        int h = hit(app, ev->x, ev->y);
        if (st.dragging) set_slider(app, ev->x);
        if (h != st.hover || st.dragging) {
            st.hover = h;
            ic_app_invalidate(app);
        }
        break;
    }
    case IC_EV_MOUSE_LEAVE:
        st.hover = H_NONE;
        ic_app_invalidate(app);
        break;
    case IC_EV_MOUSE_DOWN:
        if (ev->button == 1) click(app, hit(app, ev->x, ev->y), ev->x);
        ic_app_invalidate(app);
        break;
    case IC_EV_MOUSE_UP:
        st.dragging = 0;
        break;
    case IC_EV_KEY:
        if (st.step == S_INSTALL) break;
        if (ev->key == IC_KEY_ENTER) next(app);
        else if (ev->key == IC_KEY_ESCAPE || (ev->key == IC_KEY_BACKSPACE && st.step != S_DONE)) back(app);
        else if (ev->key == IC_KEY_UP || ev->key == IC_KEY_DOWN || ev->key == IC_KEY_TAB) {
            int dir = ev->key == IC_KEY_UP ? -1 : 1;
            if (st.step == S_DISK) move_disk(dir);
            else if (st.step == S_HOW) move_how(dir);
        } else if ((ev->key == IC_KEY_LEFT || ev->key == IC_KEY_RIGHT) && st.step == S_HOW && st.how == HOW_ALONGSIDE) {
            disk_t *d = cur_disk();
            uint64_t range = d && alongside_max_mb(d) > ICDA_MIN_MB ? alongside_max_mb(d) - ICDA_MIN_MB : 1;
            float stepv = (float)((ev->mods & IC_MOD_SHIFT) ? 10240 : 1024) / (float)range;
            st.size += ev->key == IC_KEY_RIGHT ? stepv : -stepv;
            if (st.size < 0.0f) st.size = 0.0f;
            if (st.size > 1.0f) st.size = 1.0f;
        } else if (ev->key == ' ' && st.step == S_REVIEW && st.how == HOW_ERASE) {
            st.confirm = !st.confirm;
        }
        ic_app_invalidate(app);
        break;
    default:
        break;
    }
}

static int prefer_disk = -1;

static void init(ic_app_t *app) {
    load_disks();
    pick_disk(prefer_disk);
    go(app, S_DISK);
}

/* ---- the helper: the disk work, in its own process ------------------------------------------ */

static int parse_num(const char *s, uint64_t *out) {
    uint64_t v = 0;
    if (!s || !*s) return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return 1;
}

static int result(long rc, const char *what) {
    char line[64];
    line[0] = 0;
    if (rc >= 0) {
        cat(line, sizeof(line), "ok\n");
    } else {
        cat(line, sizeof(line), "fail ");
        cat_num(line, sizeof(line), (uint64_t)(-rc));
        cat(line, sizeof(line), " ");
        cat(line, sizeof(line), what);
        cat(line, sizeof(line), "\n");
    }
    (void)icda_write_file(RESULT_PATH, line, s_len(line));
    return rc >= 0 ? 0 : 1;
}

static int find_part(uint64_t dev, uint64_t start) {
    const char *name = 0;
    for (int i = 0; i < st.ndisks; i++)
        if (st.disks[i].index == dev) name = st.disks[i].name;
    for (int i = 0; name && i < st.nparts; i++)
        if (s_eq(st.parts[i].dev, name) && st.parts[i].start == start) return (int)st.parts[i].index;
    return -1;
}

static int create_part(uint64_t dev, uint64_t start, uint64_t sectors, uint32_t role, const char *name) {
    icda_disk_edit_t req;
    ic_memzero(&req, sizeof(req));
    req.op = ICDA_DISK_CREATE;
    req.device = (uint32_t)dev;
    req.start_lba = start;
    req.sectors = sectors;
    req.role = role;
    req.fs = FS_FAT32;
    ic_strcpy(req.name, name, sizeof(req.name));
    return (int)icda_disk_edit(&req);
}

static int helper_erase(uint64_t dev) {
    uint64_t files = 0, bytes = 0;
    long rc = (long)icda_format_device(dev, LAYOUT_ICDA);
    if (rc < 0) return result(rc, "format");
    return result((long)icda_install_device(dev, &files, &bytes), "install");
}

static int helper_alongside(uint64_t dev, uint64_t start, uint64_t mb) {
    icda_install_plan_t plan;
    uint64_t files = 0, bytes = 0;
    disk_t *d = 0;
    int esp, root, rc;
    load_disks();
    for (int i = 0; i < st.ndisks; i++)
        if (st.disks[i].index == dev) d = &st.disks[i];
    if (!d) return result(-19, "partition");
    esp = disk_esp(d);
    esp = esp >= 0 ? (int)st.parts[esp].index : -1;
    if (esp < 0) {
        if ((rc = create_part(dev, start, ESP_MB * SECTORS_MB, ICDA_ROLE_EFI, "EFI System")) < 0) return result(rc, "partition");
        load_disks();
        esp = find_part(dev, start);
        start += ESP_MB * SECTORS_MB;
    }
    if ((rc = create_part(dev, start, mb * SECTORS_MB, ICDA_ROLE_SYSTEM, "ICDA System")) < 0) return result(rc, "partition");
    load_disks();
    root = find_part(dev, start);
    if (esp < 0 || root < 0) return result(-20, "partition");
    plan.efi_partition = (uint64_t)esp;
    plan.root_partition = (uint64_t)root;
    plan.swap_partition = -1;
    return result((long)icda_install_partitions(&plan, &files, &bytes), "install");
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Install ICDA", WIN_W, WIN_H, init, draw, event, tick };
    uint64_t a = 0, b = 0, c = 0;
    if (argc >= 3 && s_eq(argv[1], "--erase")) return parse_num(argv[2], &a) ? helper_erase(a) : 2;
    if (argc >= 5 && s_eq(argv[1], "--alongside"))
        return parse_num(argv[2], &a) && parse_num(argv[3], &b) && parse_num(argv[4], &c) ? helper_alongside(a, b, c) : 2;
    if (argc >= 3 && s_eq(argv[1], "--disk") && parse_num(argv[2], &a)) prefer_disk = (int)a;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("Install ICDA needs the desktop\n");
        return 1;
    }
    return 0;
}
