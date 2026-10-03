











#include "libicda.h"

#define WIN_W 800
#define WIN_H 480

#define DISKMAN_BUF_CAP  4096
#define DISKMAN_MAX_DEV  8
#define DISKMAN_MAX_PART 32
#define DISKMAN_STATUS_CAP 128

#define DISKMAN_FS_FAT32       1
#define DISKMAN_FS_EXFAT       2
#define DISKMAN_LAYOUT_CLEAR   3
#define DISKMAN_LAYOUT_ICDA    4
#define DISKMAN_LAYOUT_MBR     5
#define DISKMAN_LAYOUT_GPT     6


#define DISKMAN_ROLE_EFI     1
#define DISKMAN_ROLE_SYSTEM  2
#define DISKMAN_ROLE_SWAP    3


enum { DA_NONE = 0, DA_FORMAT_DEVICE, DA_FORMAT_PART, DA_ROLE, DA_LAYOUT };

typedef struct {
    uint64_t index;
    char     name[16];
    char     table[16];
    uint64_t sectors;
    uint64_t sector_size;
} dm_device_t;

typedef struct {
    uint64_t index;
    char     name[48];
    char     dev[16];
    char     fs[16];
    char     role[16];
    uint64_t start;
    uint64_t sectors;
} dm_part_t;

static struct {
    char      info[DISKMAN_BUF_CAP];
    dm_device_t devices[DISKMAN_MAX_DEV];
    dm_part_t  parts[DISKMAN_MAX_PART];
    int        device_count;
    int        part_count;
    int        selected;
    int        selected_part;
    int        focus_parts;      
    int        runtime_device;   

    
    int rows;
    int first_row;
    int last_row;

    
    int hover_device;
    int hover_refresh;
    int hover_fat32;
    int hover_exfat;
    int hover_gpt;
    int hover_role_efi;
    int hover_role_system;
    int hover_role_swap;
    int list_focused;

    
    int  action;                 
    int  action_arg;             
    int  alert_hover;            

    char status[DISKMAN_STATUS_CAP];
} dm;



static ic_rect_t toolbar_rect(ic_app_t *app) {
    return ic_rect_make(0, 0, app->width, IC_H_TOOLBAR);
}

static ic_rect_t sidebar_rect(ic_app_t *app) {
    return ic_rect_make(0, IC_H_TOOLBAR, IC_W_SIDEBAR,
                        app->height - IC_H_TOOLBAR - 24);
}

static ic_rect_t device_rect(ic_app_t *app, int i) {
    ic_rect_t s = sidebar_rect(app);
    return ic_rect_make(s.x, s.y + IC_SP_3 + 14 + IC_SP_2 + i * (IC_H_ROW + 2), s.w, IC_H_ROW);
}

static ic_rect_t detail_rect(ic_app_t *app) {
    ic_rect_t s = sidebar_rect(app);
    return ic_rect_make(s.x + s.w, s.y, app->width - s.x - s.w, s.h);
}

static ic_rect_t status_rect(ic_app_t *app) {
    return ic_rect_make(0, app->height - 24, app->width, 24);
}

static ic_rect_t refresh_rect(ic_app_t *app) {
    ic_rect_t b = toolbar_rect(app);
    return ic_rect_make(b.x + IC_SP_4, (b.h - IC_H_CONTROL_SM) / 2, 30, IC_H_CONTROL_SM);
}


static ic_rect_t action_rect(ic_app_t *app, int index) {
    static const char *const labels[6] = { "FAT32", "exFAT", "ICDA", "EFI", "System", "Swap" };
    ic_rect_t b = toolbar_rect(app);
    int x = refresh_rect(app).x + refresh_rect(app).w + IC_SP_3;
    for (int i = 0; i < index; i++) {
        x += ic_ui_button_width(labels[i], IC_SYM_NONE) + IC_SP_2;
    }
    return ic_rect_make(x, (b.h - IC_H_CONTROL_SM) / 2,
                        ic_ui_button_width(labels[index], IC_SYM_NONE), IC_H_CONTROL_SM);
}

static ic_rect_t alert_rect(ic_app_t *app) {
    int w = 440, h = ic_ui_alert_height(IC_SYM_WARNING, "");
    return ic_rect_make((app->width - w) / 2, (app->height - h) / 2, w, h);
}

static ic_rect_t alert_button_rect(ic_app_t *app, int index) {
    return ic_ui_alert_button_rect(alert_rect(app), index, 2);
}


static int device_is_runtime(int i) {
    return i >= 0 && i < dm.device_count && dm.runtime_device >= 0 &&
           dm.devices[i].index == (uint64_t)dm.runtime_device;
}

static int has_device(void) { return dm.device_count > 0; }

static int has_partition(void) {
    return dm.focus_parts && dm.part_count > 0;
}

static int can_set_role(void) {
    return has_partition() && has_device() && ic_streq(dm.devices[dm.selected].table, "gpt");
}



static int title_h(void) {
    return ic_font(IC_FONT_TITLE3)->line_h;
}

static int detail_header_h(ic_app_t *app) {
    ic_rect_t d = detail_rect(app);
    return d.y + IC_SP_4 + title_h() + IC_SP_4 + IC_H_ROW + IC_SP_1;
}

static void layout(ic_app_t *app) {
    ic_rect_t d = detail_rect(app);
    int y = detail_header_h(app);
    int rows = (d.y + d.h - IC_SP_4 - y) / IC_H_ROW;
    if (rows < 1) rows = 1;
    if (rows > dm.part_count) rows = dm.part_count;
    dm.rows = rows;
    if (dm.selected_part < 0) dm.selected_part = 0;
    if (dm.part_count == 0) { dm.first_row = 0; dm.last_row = 0; return; }
    if (dm.selected_part >= rows) {
        dm.first_row = dm.selected_part - rows + 1;
    } else {
        dm.first_row = 0;
    }
    if (dm.first_row + rows > dm.part_count) dm.first_row = dm.part_count - rows;
    if (dm.first_row < 0) dm.first_row = 0;
    dm.last_row = dm.first_row + rows;
}

static ic_rect_t part_rect(ic_app_t *app, int i) {
    ic_rect_t d = detail_rect(app);
    int y = detail_header_h(app);
    return ic_rect_make(d.x + IC_SP_4, y + (i - dm.first_row) * IC_H_ROW,
                        d.w - 2 * IC_SP_4, IC_H_ROW);
}



static uint64_t dm_strlen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void dm_copy(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) dst[i] = src[i], i++;
    dst[i] = 0;
}

static void dm_u64(uint64_t v, char *dst, uint64_t cap) {
    char tmp[24];
    int n = 0;
    int i = 0;
    if (cap == 0) return;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 24);
    while (n > 0 && (uint64_t)i + 1 < cap) dst[i++] = tmp[--n];
    dst[i] = 0;
}

static void dm_status(const char *text) {
    dm_copy(dm.status, text, DISKMAN_STATUS_CAP);
}


static void size_text(uint64_t sectors, uint64_t sector_size, char *out, uint64_t cap) {
    static const char *const units[4] = { "B", "KB", "MB", "GB" };
    uint64_t bytes = sectors * sector_size;
    uint64_t whole = bytes;
    uint64_t frac = 0;
    int unit = 0;
    if (cap == 0) return;
    while (whole >= 1000 && unit < 3) { frac = whole % 1000; whole /= 1000; unit++; }
    out[0] = 0;
    dm_u64(whole, out, cap);
    if (unit > 0 && whole < 100 && frac / 100 > 0) {
        char d[4];
        dm_u64(frac / 100, d, sizeof(d));
        ic_strlcat(out, ".", cap);
        ic_strlcat(out, d, cap);
    }
    ic_strlcat(out, " ", cap);
    ic_strlcat(out, units[unit], cap);
}


static void copy_span(char *out, uint64_t cap, const char *s, uint64_t n) {
    uint64_t i = 0;
    if (cap == 0) return;
    while (i < n && i + 1 < cap) { out[i] = s[i]; i++; }
    out[i] = 0;
}

static uint64_t span_u64(const char *s, uint64_t n) {
    uint64_t v = 0;
    for (uint64_t i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) {
        v = v * 10 + (uint64_t)(s[i] - '0');
    }
    return v;
}

static int key_is(const char *s, uint64_t n, const char *key) {
    if (n != dm_strlen(key)) return 0;
    for (uint64_t i = 0; i < n; i++) {
        if (s[i] != key[i]) return 0;
    }
    return 1;
}

static void parse_line(int section, const char *line, uint64_t len) {
    dm_device_t *d = 0;
    dm_part_t *pt = 0;
    uint64_t pos = 0;
    int field = 0;

    if (section == 0 && dm.device_count < DISKMAN_MAX_DEV) {
        d = &dm.devices[dm.device_count++];
        d->name[0] = 0;
        dm_copy(d->table, "unknown", sizeof(d->table));
        d->sectors = 0;
        d->sector_size = 512;
    } else if (section == 1 && dm.part_count < DISKMAN_MAX_PART) {
        pt = &dm.parts[dm.part_count++];
        pt->name[0] = pt->dev[0] = pt->fs[0] = pt->role[0] = 0;
        pt->start = 0;
        pt->sectors = 0;
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
            else pt->index = span_u64(t, n);
            field++;
            continue;
        }
        for (eq = 0; eq < n && t[eq] != '='; eq++) {}
        if (field == 1 && eq == n) {
            char *name = d ? d->name : pt->name;
            uint64_t cap = d ? sizeof(d->name) : sizeof(pt->name);
            uint64_t at = dm_strlen(name);
            if (at > 0 && at + 1 < cap) name[at++] = ' ';
            copy_span(name + at, cap - at, t, n);
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
        } else {
            if (key_is(t, eq, "dev")) copy_span(pt->dev, sizeof(pt->dev), v, vn);
            else if (key_is(t, eq, "fs")) copy_span(pt->fs, sizeof(pt->fs), v, vn);
            else if (key_is(t, eq, "role")) copy_span(pt->role, sizeof(pt->role), v, vn);
            else if (key_is(t, eq, "start")) pt->start = span_u64(v, vn);
            else if (key_is(t, eq, "sectors")) pt->sectors = span_u64(v, vn);
        }
    }
    if (d && d->sector_size == 0) d->sector_size = 512;
}

static void parse_storage(void) {
    const char *p = dm.info;
    int section = -1;
    dm.device_count = 0;
    dm.part_count = 0;
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
            else section = 2;
            continue;
        }
        if (len > 2 && line[2] >= '0' && line[2] <= '9') parse_line(section, line + 2, len - 2);
    }
}

static int part_belongs_to_selected(int i) {
    const char *want;
    if (dm.selected < 0 || dm.selected >= dm.device_count) return 0;
    want = dm.devices[dm.selected].name;
    if (dm_strlen(dm.parts[i].dev) != dm_strlen(want)) return 0;
    for (uint64_t j = 0; j < dm_strlen(want); j++) {
        if (dm.parts[i].dev[j] != want[j]) return 0;
    }
    return 1;
}

static void select_device(int i) {
    int n = 0;
    dm.selected = i;
    parse_storage();
    for (int k = 0; k < dm.part_count; k++) {
        if (part_belongs_to_selected(k)) dm.parts[n++] = dm.parts[k];
    }
    dm.part_count = n;
    dm.selected_part = 0;
    dm.first_row = 0;
}

static void refresh(void) {
    long n = (long)icda_storage_info(dm.info, sizeof(dm.info) - 1);
    uint64_t runtime;
    if (n < 0 || (uint64_t)n >= sizeof(dm.info)) {
        dm_status("The storage query failed");
        dm.device_count = 0;
        dm.part_count = 0;
        return;
    }
    dm.info[n] = 0;
    parse_storage();
    runtime = icda_runtime_device();
    dm.runtime_device = (runtime == (uint64_t)-1) ? -1 : (int64_t)runtime;

    
    if (dm.device_count > 1 && dm.selected == 0) dm.selected = 1;
    if (dm.selected >= dm.device_count) dm.selected = dm.device_count ? dm.device_count - 1 : 0;

    select_device(dm.selected);
    if (dm.device_count == 0) dm_status("No storage devices were reported");
}

static const char *fs_label(uint64_t fs_type) {
    switch (fs_type) {
        case DISKMAN_FS_FAT32:     return "FAT32";
        case DISKMAN_FS_EXFAT:     return "exFAT";
        case DISKMAN_LAYOUT_CLEAR: return "a new empty partition table";
        case DISKMAN_LAYOUT_ICDA:  return "the ICDA layout";
        case DISKMAN_LAYOUT_MBR:   return "a new MBR partition table";
        case DISKMAN_LAYOUT_GPT:   return "a new GPT partition table";
        default:                   return "a new file system";
    }
}

static const char *action_verb(uint64_t fs_type) {
    switch (fs_type) {
        case DISKMAN_FS_FAT32:
        case DISKMAN_FS_EXFAT:   return "Format";
        case DISKMAN_LAYOUT_ICDA: return "Write the ICDA layout";
        default:                 return "Write";
    }
}



static void do_format_device(uint64_t fs_type) {
    long rc;
    if (!has_device()) { dm_status("No device is selected"); return; }
    if (device_is_runtime(dm.selected)) {
        dm_status("That disk holds the running system and is protected");
        return;
    }
    rc = (long)icda_format_device(dm.devices[dm.selected].index, fs_type);
    if (rc < 0) {
        if (rc == -21 || rc == -31) {
            dm_status(fs_type == DISKMAN_LAYOUT_ICDA ? "The ICDA layout needs a disk of at least 1 GB"
                                                     : "This disk is too small for that layout");
            return;
        }
        char msg[DISKMAN_STATUS_CAP];
        char n[24];
        msg[0] = 0;
        ic_strlcat(msg, action_verb(fs_type), sizeof(msg));
        ic_strlcat(msg, " failed (error ", sizeof(msg));
        dm_u64((uint64_t)(-rc), n, sizeof(n));
        ic_strlcat(msg, n, sizeof(msg));
        ic_strlcat(msg, ")", sizeof(msg));
        dm_status(msg);
        return;
    }
    {
        char msg[DISKMAN_STATUS_CAP];
        msg[0] = 0;
        ic_strlcat(msg, dm.devices[dm.selected].name, sizeof(msg));
        ic_strlcat(msg, " was erased and now holds ", sizeof(msg));
        ic_strlcat(msg, fs_label(fs_type), sizeof(msg));
        dm_status(msg);
    }
    refresh();
}

static void reselect_part(uint64_t index) {
    for (int i = 0; i < dm.part_count; i++) {
        if (dm.parts[i].index == index) { dm.selected_part = i; dm.focus_parts = 1; return; }
    }
}

static void do_format_partition(uint64_t fs_type) {
    long rc;
    uint64_t keep;
    if (!has_partition()) { dm_status("No partition is selected"); return; }
    keep = dm.parts[dm.selected_part].index;
    rc = (long)icda_format_partition(dm.parts[dm.selected_part].index, fs_type);
    if (rc < 0) {
        char msg[DISKMAN_STATUS_CAP];
        char n[24];
        msg[0] = 0;
        ic_strlcat(msg, "Format failed on ", sizeof(msg));
        ic_strlcat(msg, dm.parts[dm.selected_part].name, sizeof(msg));
        ic_strlcat(msg, " (error ", sizeof(msg));
        dm_u64((uint64_t)(-rc), n, sizeof(n));
        ic_strlcat(msg, n, sizeof(msg));
        ic_strlcat(msg, ")", sizeof(msg));
        dm_status(msg);
        return;
    }
    {
        char msg[DISKMAN_STATUS_CAP];
        msg[0] = 0;
        ic_strlcat(msg, "Formatted ", sizeof(msg));
        ic_strlcat(msg, dm.parts[dm.selected_part].name, sizeof(msg));
        ic_strlcat(msg, " as ", sizeof(msg));
        ic_strlcat(msg, fs_label(fs_type), sizeof(msg));
        dm_status(msg);
    }
    refresh();
    reselect_part(keep);
}

static void do_set_role(int role) {
    long rc;
    uint64_t keep;
    static const char *const names[4] = { "", "EFI", "System", "Swap" };
    if (!has_partition()) { dm_status("No partition is selected"); return; }
    keep = dm.parts[dm.selected_part].index;
    rc = (long)icda_set_partition_role(dm.parts[dm.selected_part].index, (uint64_t)role);
    if (rc < 0) {
        char msg[DISKMAN_STATUS_CAP];
        char n[24];
        msg[0] = 0;
        ic_strlcat(msg, "Could not set the role (error ", sizeof(msg));
        dm_u64((uint64_t)(-rc), n, sizeof(n));
        ic_strlcat(msg, n, sizeof(msg));
        ic_strlcat(msg, ")", sizeof(msg));
        dm_status(msg);
        return;
    }
    {
        char msg[DISKMAN_STATUS_CAP];
        msg[0] = 0;
        ic_strlcat(msg, dm.parts[dm.selected_part].name, sizeof(msg));
        ic_strlcat(msg, " is now the ", sizeof(msg));
        ic_strlcat(msg, names[role], sizeof(msg));
        ic_strlcat(msg, " partition", sizeof(msg));
        dm_status(msg);
    }
    refresh();
    reselect_part(keep);
}

static void confirm(int action, int arg) {
    dm.action = action;
    dm.action_arg = arg;
    dm.alert_hover = -1;
}

static void resolve_alert(void) {
    int action = dm.action;
    int arg = dm.action_arg;
    dm.action = DA_NONE;
    dm.alert_hover = -1;
    if (action == DA_FORMAT_DEVICE) do_format_device((uint64_t)arg);
    else if (action == DA_FORMAT_PART) do_format_partition((uint64_t)arg);
    else if (action == DA_ROLE) do_set_role(arg);
}


static const char *alert_target(char *buf, uint64_t cap) {
    buf[0] = 0;
    if (dm.action == DA_FORMAT_DEVICE && has_device()) {
        ic_strlcat(buf, dm.devices[dm.selected].name, cap);
    } else if ((dm.action == DA_FORMAT_PART || dm.action == DA_ROLE) && dm.part_count > 0) {
        ic_strlcat(buf, dm.parts[dm.selected_part].name, cap);
    }
    return buf;
}

static void alert_text(char *title, uint64_t title_cap, char *msg, uint64_t msg_cap) {
    char target[48];
    alert_target(target, sizeof(target));
    title[0] = 0;
    msg[0] = 0;
    switch (dm.action) {
        case DA_FORMAT_DEVICE:
            ic_strlcat(title, "Erase this disk?", title_cap);
            ic_strlcat(msg, "Everything on ", msg_cap);
            ic_strlcat(msg, target, msg_cap);
            ic_strlcat(msg, " will be destroyed and replaced with ", msg_cap);
            ic_strlcat(msg, fs_label((uint64_t)dm.action_arg), msg_cap);
            ic_strlcat(msg, ". This cannot be undone.", msg_cap);
            break;
        case DA_FORMAT_PART:
            ic_strlcat(title, "Erase this partition?", title_cap);
            ic_strlcat(msg, "Everything on ", msg_cap);
            ic_strlcat(msg, target, msg_cap);
            ic_strlcat(msg, " will be destroyed and replaced with ", msg_cap);
            ic_strlcat(msg, fs_label((uint64_t)dm.action_arg), msg_cap);
            ic_strlcat(msg, ". This cannot be undone.", msg_cap);
            break;
        case DA_ROLE:
            ic_strlcat(title, "Change this role?", title_cap);
            ic_strlcat(msg, dm.action_arg == DISKMAN_ROLE_EFI ? "The partition will be marked as bootable."
                        : dm.action_arg == DISKMAN_ROLE_SYSTEM ? "The partition will be marked as the system partition."
                        : "The partition will be marked as the swap partition.", msg_cap);
            ic_strlcat(msg, " On the next install this changes which partition is used.", msg_cap);
            break;
        default:
            ic_strlcat(title, "Are you sure?", title_cap);
            break;
    }
}



static void draw_toolbar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t b = toolbar_rect(app);
    const ic_palette_t *p = ic_palette();
    int enabled = has_device() && !device_is_runtime(dm.selected);
    int part_enabled = can_set_role();
    const char *hint;

    ic_ui_toolbar(c, b);
    ic_ui_icon_button(c, refresh_rect(app), IC_SYM_RELOAD,
                      dm.hover_refresh ? IC_STATE_HOVER : IC_STATE_NORMAL);
    
    ic_ui_button(c, action_rect(app, 0), "FAT32", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !enabled ? IC_STATE_DISABLED
                          : (dm.hover_fat32 ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_button(c, action_rect(app, 1), "exFAT", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !enabled ? IC_STATE_DISABLED
                          : (dm.hover_exfat ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_button(c, action_rect(app, 2), "ICDA", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !enabled ? IC_STATE_DISABLED
                          : (dm.hover_gpt ? IC_STATE_HOVER : IC_STATE_NORMAL));
    
    ic_ui_button(c, action_rect(app, 3), "EFI", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !part_enabled ? IC_STATE_DISABLED
                               : (dm.hover_role_efi ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_button(c, action_rect(app, 4), "System", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !part_enabled ? IC_STATE_DISABLED
                               : (dm.hover_role_system ? IC_STATE_HOVER : IC_STATE_NORMAL));
    ic_ui_button(c, action_rect(app, 5), "Swap", IC_SYM_NONE, IC_BUTTON_DEFAULT,
                 !part_enabled ? IC_STATE_DISABLED
                               : (dm.hover_role_swap ? IC_STATE_HOVER : IC_STATE_NORMAL));
    if (!has_device()) hint = "Attach a disk to begin";
    else if (device_is_runtime(dm.selected)) hint = "System disk - protected";
    else if (has_partition() && !part_enabled) hint = "Roles need a GPT disk. ICDA writes one.";
    else if (has_partition()) hint = "Format the partition or set its role";
    else hint = "Erase the disk, or select a partition";
    {
        int right = action_rect(app, 5).x + action_rect(app, 5).w + IC_SP_3;
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(right, 0, b.w - right - IC_SP_3, b.h),
                        hint, p->label_tertiary, IC_ALIGN_LEFT);
    }
}

static void draw_sidebar(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = sidebar_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_ui_sidebar_bg(c, s);
    ic_text_draw_in(c, ic_font(IC_FONT_CAPTION_EMPH),
                    ic_rect_make(s.x + IC_SP_3, s.y + IC_SP_3, s.w - IC_SP_4, 14),
                    "DEVICES", p->label_tertiary, IC_ALIGN_LEFT);
    for (int i = 0; i < dm.device_count; i++) {
        ic_rect_t r = device_rect(app, i);
        float hover = (-100 - i) == dm.hover_device ? 1.0f : 0.0f;
        char size[24];
        char line[40];
        size_text(dm.devices[i].sectors, dm.devices[i].sector_size, size, sizeof(size));
        ic_ui_sidebar_item(c, r, i == dm.selected ? IC_SYM_DISK : IC_SYM_INFO,
                           dm.devices[i].name, i == dm.selected && !dm.focus_parts, hover);
        line[0] = 0;
        ic_strlcat(line, size, sizeof(line));
        ic_text_draw_in(c, ic_font(IC_FONT_CAPTION),
                        ic_rect_make(r.x + IC_SP_3, r.y, r.w - IC_SP_3 - IC_SP_4, r.h),
                        line, p->label_tertiary, IC_ALIGN_RIGHT);
    }
    if (dm.device_count == 0) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(s.x + IC_SP_3, s.y + IC_SP_6, s.w - IC_SP_4, 40),
                        "No storage devices", p->label_tertiary, IC_ALIGN_LEFT);
    }
}

static const char *pretty_token(const char *s) {
    if (ic_streq(s, "fat32")) return "FAT32";
    if (ic_streq(s, "exfat")) return "exFAT";
    if (ic_streq(s, "ntfs")) return "NTFS";
    if (ic_streq(s, "efi")) return "EFI";
    if (ic_streq(s, "system")) return "System";
    if (ic_streq(s, "swap")) return "Swap";
    if (ic_streq(s, "data")) return "Data";
    return "-";
}

static const char *table_label(const char *kind) {
    if (ic_streq(kind, "gpt")) return "GUID partition table";
    if (ic_streq(kind, "mbr")) return "Master boot record";
    return "No partition table";
}

static void draw_detail(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t d = detail_rect(app);
    const ic_palette_t *p = ic_palette();
    const ic_face_t *body = ic_font(IC_FONT_BODY);
    const ic_face_t *mono = ic_font(IC_FONT_MONO_SMALL);
    static const char *const titles[4] = { "Partition", "File System", "Role", "Size" };
    int widths[4] = { 0, 120, 90, 100 };
    char cell[32];

    layout(app);
    widths[0] = d.w - 2 * IC_SP_4 - widths[1] - widths[2] - widths[3];
    if (widths[0] < 80) widths[0] = 80;
    ic_ui_section_header(c, d.x + IC_SP_4, d.y + IC_SP_4,
                         has_device() ? dm.devices[dm.selected].name : "No device");
    if (has_device()) {
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(d.x + IC_SP_4, d.y + IC_SP_4 + title_h(), d.w - IC_SP_6,
                                     16),
                        table_label(dm.devices[dm.selected].table), p->label_tertiary,
                        IC_ALIGN_LEFT);
    }
    ic_ui_table_header(c, ic_rect_make(d.x + IC_SP_4, d.y + IC_SP_4 + title_h() + IC_SP_4,
                                       d.w - 2 * IC_SP_4, IC_H_ROW),
                       titles, widths, 4);

    for (int i = dm.first_row; i < dm.last_row; i++) {
        ic_rect_t r = part_rect(app, i);
        const dm_part_t *pt = &dm.parts[i];
        int sel = i == dm.selected_part && dm.focus_parts;
        ic_color_t text = ic_ui_list_row(c, r, sel, dm.list_focused,
                                         i == dm.hover_device ? 1.0f : 0.0f);
        int x = r.x + IC_SP_3;
        size_text(pt->sectors, 512, cell, sizeof(cell));
        ic_text_draw_in(c, body, ic_rect_make(x, r.y, widths[0] - IC_SP_2, r.h), pt->name,
                        text, IC_ALIGN_LEFT);
        ic_text_draw_in(c, body, ic_rect_make(x + widths[0], r.y, widths[1] - IC_SP_2, r.h),
                        pretty_token(pt->fs), text, IC_ALIGN_LEFT);
        ic_text_draw_in(c, body, ic_rect_make(x + widths[0] + widths[1], r.y,
                                              widths[2] - IC_SP_2, r.h),
                        pretty_token(pt->role), text, IC_ALIGN_LEFT);
        ic_text_draw_in(c, mono, ic_rect_make(x + widths[0] + widths[1] + widths[2], r.y,
                                              widths[3] - IC_SP_3 - IC_SP_2, r.h),
                        cell, text, IC_ALIGN_RIGHT);
    }

    if (dm.part_count == 0) {
        if (!has_device()) {
            ic_ui_empty_state(c, d, IC_SYM_DISK, "No storage devices",
                              "Attach a disk, then press Refresh.");
        } else {
            ic_ui_empty_state(c, d, IC_SYM_DISK, "No partitions",
                              "This disk has no partition table. Choose an action above to write one.");
        }
    } else if (dm.part_count > dm.rows) {
        ic_ui_scrollbar(c, d, dm.first_row * IC_H_ROW, (dm.part_count - dm.rows) * IC_H_ROW + d.h, 1.0f);
    }
}

static void draw_status(ic_app_t *app, ic_canvas_t *c) {
    ic_rect_t s = status_rect(app);
    const ic_palette_t *p = ic_palette();
    ic_ui_statusbar(c, s, dm.status);
    if (dm.focus_parts && has_partition()) {
        char where[64];
        where[0] = 0;
        ic_strlcat(where, "Editing ", sizeof(where));
        ic_strlcat(where, dm.parts[dm.selected_part].name, sizeof(where));
        ic_text_draw_in(c, ic_font(IC_FONT_FOOTNOTE),
                        ic_rect_make(s.x + s.w - 260, s.y, 260 - IC_SP_3, s.h), where,
                        p->accent, IC_ALIGN_RIGHT);
    }
}

static void draw_alert(ic_app_t *app, ic_canvas_t *c) {
    static const char *const labels[2] = { "Cancel", "Continue" };
    ic_rect_t rects[2];
    char title[64];
    char msg[224];
    const ic_symbol_t sym = dm.action == DA_ROLE ? IC_SYM_INFO : IC_SYM_WARNING;

    alert_text(title, sizeof(title), msg, sizeof(msg));
    ic_ui_alert(c, alert_rect(app), sym, title, msg, labels, 2, dm.alert_hover, rects);
}

static void draw(ic_app_t *app, ic_canvas_t *c) {
    ic_ui_window_bg(c, ic_rect_make(0, 0, app->width, app->height));
    draw_toolbar(app, c);
    draw_sidebar(app, c);
    draw_detail(app, c);
    draw_status(app, c);
    if (dm.action != DA_NONE) draw_alert(app, c);
    if (app->focused) ic_app_animate(app);
}



static int device_at(ic_app_t *app, int x, int y) {
    for (int i = 0; i < dm.device_count; i++) {
        if (ic_ui_hit(device_rect(app, i), x, y)) return i;
    }
    return -1;
}

static int part_at(ic_app_t *app, int x, int y) {
    for (int i = dm.first_row; i < dm.last_row; i++) {
        if (ic_ui_hit(part_rect(app, i), x, y)) return i;
    }
    return -1;
}

static void event(ic_app_t *app, const ic_event_t *ev) {
    switch (ev->type) {
    case IC_EV_MOUSE_MOVE:
        if (dm.action != DA_NONE) {
            dm.alert_hover = ic_ui_hit(alert_button_rect(app, 0), ev->x, ev->y) ? 0
                           : (ic_ui_hit(alert_button_rect(app, 1), ev->x, ev->y) ? 1 : -1);
            break;
        }
        dm.hover_refresh = ic_ui_hit(refresh_rect(app), ev->x, ev->y);
        dm.hover_fat32 = ic_ui_hit(action_rect(app, 0), ev->x, ev->y);
        dm.hover_exfat = ic_ui_hit(action_rect(app, 1), ev->x, ev->y);
        dm.hover_gpt = ic_ui_hit(action_rect(app, 2), ev->x, ev->y);
        dm.hover_role_efi = ic_ui_hit(action_rect(app, 3), ev->x, ev->y);
        dm.hover_role_system = ic_ui_hit(action_rect(app, 4), ev->x, ev->y);
        dm.hover_role_swap = ic_ui_hit(action_rect(app, 5), ev->x, ev->y);
        dm.hover_device = part_at(app, ev->x, ev->y);
        if (dm.hover_device < 0) {
            int d = device_at(app, ev->x, ev->y);
            if (d >= 0) dm.hover_device = -100 - d;    
        }
        break;
    case IC_EV_MOUSE_DOWN: {
        int i;
        if (ev->button != GUI_BTN_LEFT) break;
        if (dm.action != DA_NONE) {
            if (ic_ui_hit(alert_button_rect(app, 1), ev->x, ev->y)) resolve_alert();
            else if (ic_ui_hit(alert_button_rect(app, 0), ev->x, ev->y)) {
                dm.action = DA_NONE;
                dm.alert_hover = -1;
            }
            break;
        }
        if (dm.hover_refresh) { refresh(); break; }
        
        if (dm.hover_fat32) {
            if (dm.focus_parts) confirm(DA_FORMAT_PART, DISKMAN_FS_FAT32);
            else confirm(DA_FORMAT_DEVICE, DISKMAN_FS_FAT32);
            break;
        }
        if (dm.hover_exfat) {
            if (dm.focus_parts) confirm(DA_FORMAT_PART, DISKMAN_FS_EXFAT);
            else confirm(DA_FORMAT_DEVICE, DISKMAN_FS_EXFAT);
            break;
        }
        if (dm.hover_gpt) { confirm(DA_FORMAT_DEVICE, DISKMAN_LAYOUT_ICDA); break; }
        if (dm.hover_role_efi && can_set_role()) { confirm(DA_ROLE, DISKMAN_ROLE_EFI); break; }
        if (dm.hover_role_system && can_set_role()) { confirm(DA_ROLE, DISKMAN_ROLE_SYSTEM); break; }
        if (dm.hover_role_swap && can_set_role()) { confirm(DA_ROLE, DISKMAN_ROLE_SWAP); break; }
        i = part_at(app, ev->x, ev->y);
        if (i >= 0) {
            dm.selected_part = i;
            dm.focus_parts = 1;
            dm.list_focused = 1;
            break;
        }
        i = device_at(app, ev->x, ev->y);
        if (i >= 0) {
            select_device(i);
            dm.focus_parts = 0;
            dm.list_focused = 1;
        }
        break;
    }
    case IC_EV_MOUSE_LEAVE:
        dm.hover_device = -1;
        dm.hover_refresh = dm.hover_fat32 = dm.hover_exfat = dm.hover_gpt = 0;
        dm.hover_role_efi = dm.hover_role_system = dm.hover_role_swap = 0;
        break;
    case IC_EV_KEY:
        if (dm.action != DA_NONE) {
            if (ev->key == IC_KEY_ESCAPE) {
                dm.action = DA_NONE;
                dm.alert_hover = -1;
            } else if (ev->key == IC_KEY_ENTER || ev->key == IC_KEY_RIGHT) {
                resolve_alert();
            } else if (ev->key == IC_KEY_LEFT || ev->key == IC_KEY_TAB) {
                dm.alert_hover = dm.alert_hover == 0 ? 1 : 0;
            }
            break;
        }
        switch (ev->key) {
        case IC_KEY_TAB:
            if (dm.part_count > 0) dm.focus_parts = !dm.focus_parts;
            break;
        case IC_KEY_UP:
            if (dm.focus_parts) {
                if (dm.selected_part > 0) dm.selected_part--;
            } else if (dm.selected > 0) {
                select_device(dm.selected - 1);
            }
            break;
        case IC_KEY_DOWN:
            if (dm.focus_parts) {
                if (dm.selected_part + 1 < dm.part_count) dm.selected_part++;
            } else if (dm.selected + 1 < dm.device_count) {
                select_device(dm.selected + 1);
            }
            break;
        case IC_KEY_HOME: dm.selected_part = 0; break;
        case IC_KEY_END:  if (dm.part_count) dm.selected_part = dm.part_count - 1; break;
        case 'r': case 'R': refresh(); break;
        case 'f': case 'F':
            if (dm.focus_parts) confirm(DA_FORMAT_PART, DISKMAN_FS_FAT32);
            else confirm(DA_FORMAT_DEVICE, DISKMAN_FS_FAT32);
            break;
        case 'x': case 'X':
            if (dm.focus_parts) confirm(DA_FORMAT_PART, DISKMAN_FS_EXFAT);
            else confirm(DA_FORMAT_DEVICE, DISKMAN_FS_EXFAT);
            break;
        case 'g': case 'G': confirm(DA_FORMAT_DEVICE, DISKMAN_LAYOUT_GPT); break;
        case 'i': case 'I': confirm(DA_FORMAT_DEVICE, DISKMAN_LAYOUT_ICDA); break;
        case 'c': case 'C': confirm(DA_FORMAT_DEVICE, DISKMAN_LAYOUT_CLEAR); break;
        default: break;
        }
        break;
    case IC_EV_RESIZE:
        layout(app);
        break;
    case IC_EV_BLUR:
        dm.list_focused = 0;
        break;
    case IC_EV_FOCUS:
    case IC_EV_APPEARANCE:
    default:
        break;
    }
    ic_app_invalidate(app);
}

static void init(ic_app_t *app) {
    (void)app;
    dm.device_count = 0;
    dm.part_count = 0;
    dm.selected = 0;
    dm.selected_part = 0;
    dm.focus_parts = 0;
    dm.runtime_device = -1;
    dm.hover_device = -1;
    dm.hover_refresh = dm.hover_fat32 = dm.hover_exfat = dm.hover_gpt = 0;
    dm.hover_role_efi = dm.hover_role_system = dm.hover_role_swap = 0;
    dm.list_focused = 1;
    dm.action = DA_NONE;
    dm.alert_hover = -1;
    dm.status[0] = 0;
    refresh();
}

int main(int argc, char **argv) {
    static const ic_app_desc_t desc = { "Disk Utility", WIN_W, WIN_H, init, draw, event, 0 };
    (void)argc;
    (void)argv;
    if (ic_app_run(&desc, 0) != 0) {
        icda_write("disk utility requires the desktop (Ctrl+Alt+F1)\n");
        return 1;
    }
    return 0;
}
