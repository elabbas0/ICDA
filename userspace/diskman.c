#include "icda_sys.h"

#include <stdint.h>

#define DISKMAN_BUF_CAP 4096
#define DISKMAN_MAX_DEVICES 8
#define DISKMAN_MAX_PARTS 32
#define DISKMAN_FS_FAT32 1
#define DISKMAN_FS_EXFAT 2
#define DISKMAN_LAYOUT_CLEAR 3
#define DISKMAN_LAYOUT_ICDA 4
#define DISKMAN_LAYOUT_MBR 5
#define DISKMAN_LAYOUT_GPT 6
#define DISKMAN_MAX_W 120
#define DISKMAN_MAX_H 32
#define CP437_BLOCK_FULL ((char)0xDB)
#define CP437_BLOCK_MED  ((char)0xB2)
#define CP437_LINE_H     ((char)0xC4)

enum {
    KEY_SPECIAL_BASE = 256,
    KEY_UP,
    KEY_DOWN
};

typedef struct {
    uint64_t index;
    char name[16];
    char table[16];
    uint64_t sectors;
    uint64_t sector_size;
} diskman_device_t;

typedef struct {
    uint64_t index;
    char name[48];
    char dev[16];
    char fs[16];
    char role[16];
    uint64_t start;
    uint64_t sectors;
} diskman_part_t;

static char diskman_info[DISKMAN_BUF_CAP];
static diskman_device_t diskman_devices[DISKMAN_MAX_DEVICES];
static diskman_part_t diskman_parts[DISKMAN_MAX_PARTS];
static uint64_t diskman_device_count = 0;
static uint64_t diskman_part_count = 0;
static uint64_t diskman_selected = 0;
static uint64_t diskman_selected_part = 0;
static uint64_t diskman_focus_parts = 0;
static char diskman_status[96];
static uint64_t diskman_left = 0;
static uint64_t diskman_top = 0;
static uint64_t diskman_width = 68;
static uint64_t diskman_height = 16;
static char diskman_prev_lines[DISKMAN_MAX_H][DISKMAN_MAX_W + 1];
static int diskman_prev_valid = 0;
static int64_t diskman_runtime_device = -1;

static void copy_text(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static uint64_t str_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void write_uint(uint64_t v) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0) {
        icda_write("0");
        return;
    }
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    icda_write(&buf[i]);
}

static void append_uint_text(char *dst, uint64_t cap, uint64_t value) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    uint64_t at = str_len(dst);

    if (at + 2 >= cap) return;
    buf[i] = 0;
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value && i > 0) {
            buf[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    copy_text(dst + at, &buf[i], cap - at);
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

static void line_reset(char *line, uint64_t width) {
    for (uint64_t i = 0; i < width; i++) line[i] = ' ';
    line[width] = 0;
}

static void line_put_text(char *line, uint64_t width, uint64_t col, const char *text) {
    uint64_t i = 0;
    while (text && text[i] && col + i < width) {
        line[col + i] = text[i];
        i++;
    }
}

static void line_put_uint(char *line, uint64_t width, uint64_t col, uint64_t value, uint64_t min_width) {
    char buf[32];
    uint64_t i = sizeof(buf) - 1;
    uint64_t len;

    buf[i] = 0;
    if (value == 0) {
        buf[--i] = '0';
    } else {
        while (value && i > 0) {
            buf[--i] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    len = str_len(&buf[i]);
    while (len < min_width && col < width) {
        line[col++] = ' ';
        len++;
    }
    while (buf[i] && col < width) {
        line[col++] = buf[i++];
    }
}

static void line_fill(char *line, uint64_t width, char ch) {
    for (uint64_t i = 0; i < width; i++) line[i] = ch;
    line[width] = 0;
}

static void write_field(char *line, uint64_t width, uint64_t col, const char *text, uint64_t field_width) {
    uint64_t len = str_len(text);
    if (len > field_width) len = field_width;
    for (uint64_t i = 0; i < len && col + i < width; i++) {
        line[col + i] = text[i];
    }
}

static void write_size_brief_into(char *line, uint64_t width, uint64_t col, uint64_t sectors, uint64_t sector_size) {
    uint64_t bytes = sectors * sector_size;
    uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    uint64_t tib = gib * 1024ULL;
    if (bytes >= tib) {
        line_put_uint(line, width, col, bytes / tib, 0);
        line_put_text(line, width, str_len(line), " TiB");
    } else if (bytes >= gib) {
        line_put_uint(line, width, col, bytes / gib, 0);
        line_put_text(line, width, str_len(line), " GiB");
    } else {
        line_put_uint(line, width, col, bytes / (1024ULL * 1024ULL), 0);
        line_put_text(line, width, str_len(line), " MiB");
    }
}

static long diskman_read_key(void) {
    long c = icda_read_char_timeout(0);
    if (c != 27) return c;
    {
        long c1 = icda_read_char_timeout(2);
        if (c1 != '[') return c;
        switch (icda_read_char_timeout(2)) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            default: return c;
        }
    }
}

static void diskman_layout_refresh(void) {
    uint64_t cols = 80;
    uint64_t rows = 25;

    if (icda_console_size(&cols, &rows) != 0) {
        cols = 80;
        rows = 25;
    }
    if (cols > DISKMAN_MAX_W) cols = DISKMAN_MAX_W;
    if (rows > DISKMAN_MAX_H) rows = DISKMAN_MAX_H;
    diskman_width = cols > 12 ? cols - 12 : cols;
    if (diskman_width < 48) diskman_width = 48;
    diskman_height = rows / 2;
    if (diskman_height < 14) diskman_height = 14;
    if (diskman_height > 24) diskman_height = 24;
    diskman_left = cols > diskman_width ? (cols - diskman_width) / 2 : 0;
    diskman_top = rows > diskman_height ? (rows - diskman_height) / 2 : 0;
}

static void diskman_move_row(uint64_t row) {
    icda_set_cursor(diskman_left, diskman_top + row);
}

static void diskman_parse_devices(void) {
    uint64_t pos = 0;
    diskman_device_count = 0;
    diskman_part_count = 0;
    while (diskman_info[pos]) {
        if (diskman_info[pos] == ' ' && diskman_info[pos + 1] == ' ' &&
            diskman_info[pos + 2] >= '0' && diskman_info[pos + 2] <= '9') {
            uint64_t idx = 0;
            uint64_t p = pos + 2;
            uint64_t name_out = 0;
            if (!parse_uint64(&diskman_info[p], &idx)) break;
            while (diskman_info[p] >= '0' && diskman_info[p] <= '9') p++;
            if (diskman_info[p] == ':' && diskman_info[p + 1] == ' ') {
                p += 2;
                if (diskman_info[p] == 'a') {
                    if (diskman_device_count < DISKMAN_MAX_DEVICES) {
                        diskman_devices[diskman_device_count].index = idx;
                        while (diskman_info[p] && diskman_info[p] != ' ' && diskman_info[p] != '\n' &&
                               name_out + 1 < sizeof(diskman_devices[diskman_device_count].name)) {
                            diskman_devices[diskman_device_count].name[name_out++] = diskman_info[p++];
                        }
                        diskman_devices[diskman_device_count].name[name_out] = 0;
                        copy_text(diskman_devices[diskman_device_count].table, "unknown", sizeof(diskman_devices[diskman_device_count].table));
                        diskman_devices[diskman_device_count].sectors = 0;
                        diskman_devices[diskman_device_count].sector_size = 512;

                        while (diskman_info[p] && diskman_info[p] != '\n') {
                            if (diskman_info[p] == 's' && diskman_info[p + 1] == 'e' && diskman_info[p + 2] == 'c' &&
                                diskman_info[p + 3] == 't' && diskman_info[p + 4] == 'o' && diskman_info[p + 5] == 'r' &&
                                diskman_info[p + 6] == 's' && diskman_info[p + 7] == '=') {
                                parse_uint64(&diskman_info[p + 8], &diskman_devices[diskman_device_count].sectors);
                            }
                            if (diskman_info[p] == 't' && diskman_info[p + 1] == 'a' && diskman_info[p + 2] == 'b' &&
                                diskman_info[p + 3] == 'l' && diskman_info[p + 4] == 'e' && diskman_info[p + 5] == '=') {
                                uint64_t j = 0;
                                p += 6;
                                while (diskman_info[p] && diskman_info[p] != ' ' && diskman_info[p] != '\n' &&
                                       j + 1 < sizeof(diskman_devices[diskman_device_count].table)) {
                                    diskman_devices[diskman_device_count].table[j++] = diskman_info[p++];
                                }
                                diskman_devices[diskman_device_count].table[j] = 0;
                                continue;
                            }
                            if (diskman_info[p] == 's' && diskman_info[p + 1] == 'e' && diskman_info[p + 2] == 'c' &&
                                diskman_info[p + 3] == 't' && diskman_info[p + 4] == 'o' && diskman_info[p + 5] == 'r' &&
                                diskman_info[p + 6] == '_' && diskman_info[p + 7] == 's' && diskman_info[p + 8] == 'i' &&
                                diskman_info[p + 9] == 'z' && diskman_info[p + 10] == 'e' && diskman_info[p + 11] == '=') {
                                parse_uint64(&diskman_info[p + 12], &diskman_devices[diskman_device_count].sector_size);
                            }
                            p++;
                        }
                        diskman_device_count++;
                    }
                } else if (diskman_info[p] != '(') {
                    if (diskman_part_count < DISKMAN_MAX_PARTS) {
                        uint64_t out = 0;
                        diskman_parts[diskman_part_count].index = idx;
                        while (diskman_info[p] && diskman_info[p] != ' ' && diskman_info[p] != '\n' &&
                               out + 1 < sizeof(diskman_parts[diskman_part_count].name)) {
                            diskman_parts[diskman_part_count].name[out++] = diskman_info[p++];
                        }
                        diskman_parts[diskman_part_count].name[out] = 0;
                        diskman_parts[diskman_part_count].dev[0] = 0;
                        diskman_parts[diskman_part_count].fs[0] = 0;
                        diskman_parts[diskman_part_count].role[0] = 0;
                        diskman_parts[diskman_part_count].start = 0;
                        diskman_parts[diskman_part_count].sectors = 0;
                        while (diskman_info[p] && diskman_info[p] != '\n') {
                            if (diskman_info[p] == 'd' && diskman_info[p + 1] == 'e' && diskman_info[p + 2] == 'v' && diskman_info[p + 3] == '=') {
                                uint64_t j = 0;
                                p += 4;
                                while (diskman_info[p] && diskman_info[p] != ' ' && diskman_info[p] != '\n' &&
                                       j + 1 < sizeof(diskman_parts[diskman_part_count].dev)) {
                                    diskman_parts[diskman_part_count].dev[j++] = diskman_info[p++];
                                }
                                diskman_parts[diskman_part_count].dev[j] = 0;
                                continue;
                            }
                            if (diskman_info[p] == 'f' && diskman_info[p + 1] == 's' && diskman_info[p + 2] == '=') {
                                uint64_t j = 0;
                                p += 3;
                                while (diskman_info[p] && diskman_info[p] != ' ' && diskman_info[p] != '\n' &&
                                       j + 1 < sizeof(diskman_parts[diskman_part_count].fs)) {
                                    diskman_parts[diskman_part_count].fs[j++] = diskman_info[p++];
                                }
                                diskman_parts[diskman_part_count].fs[j] = 0;
                                continue;
                            }
                            if (diskman_info[p] == 'r' && diskman_info[p + 1] == 'o' && diskman_info[p + 2] == 'l' &&
                                diskman_info[p + 3] == 'e' && diskman_info[p + 4] == '=') {
                                uint64_t j = 0;
                                p += 5;
                                while (diskman_info[p] && diskman_info[p] != ' ' && diskman_info[p] != '\n' &&
                                       j + 1 < sizeof(diskman_parts[diskman_part_count].role)) {
                                    diskman_parts[diskman_part_count].role[j++] = diskman_info[p++];
                                }
                                diskman_parts[diskman_part_count].role[j] = 0;
                                continue;
                            }
                            if (diskman_info[p] == 's' && diskman_info[p + 1] == 't' && diskman_info[p + 2] == 'a' &&
                                diskman_info[p + 3] == 'r' && diskman_info[p + 4] == 't' && diskman_info[p + 5] == '=') {
                                parse_uint64(&diskman_info[p + 6], &diskman_parts[diskman_part_count].start);
                            }
                            if (diskman_info[p] == 's' && diskman_info[p + 1] == 'e' && diskman_info[p + 2] == 'c' &&
                                diskman_info[p + 3] == 't' && diskman_info[p + 4] == 'o' && diskman_info[p + 5] == 'r' &&
                                diskman_info[p + 6] == 's' && diskman_info[p + 7] == '=') {
                                parse_uint64(&diskman_info[p + 8], &diskman_parts[diskman_part_count].sectors);
                            }
                            p++;
                        }
                        diskman_part_count++;
                    }
                }
            }
        }
        while (diskman_info[pos] && diskman_info[pos] != '\n') pos++;
        if (diskman_info[pos] == '\n') pos++;
        if (diskman_info[pos] == 'p' && diskman_info[pos + 1] == 'a') break;
    }
    if (diskman_selected >= diskman_device_count) {
        diskman_selected = diskman_device_count ? diskman_device_count - 1 : 0;
    }
    if (diskman_device_count > 1 && diskman_selected == 0) {
        diskman_selected = 1;
    }
}

static void diskman_refresh(void) {
    long ret = (long)icda_storage_info(diskman_info, sizeof(diskman_info));
    if (ret < 0) {
        copy_text(diskman_info, "storage query failed\n", sizeof(diskman_info));
        diskman_device_count = 0;
        copy_text(diskman_status, "storage query failed", sizeof(diskman_status));
        return;
    }
    diskman_parse_devices();
    {
        uint64_t runtime = icda_runtime_device();
        diskman_runtime_device = (runtime == (uint64_t)-1) ? -1 : (int64_t)runtime;
    }
    diskman_selected_part = 0;
    if (diskman_device_count && diskman_selected < diskman_device_count) {
        for (uint64_t i = 0; i < diskman_part_count; i++) {
            if (str_len(diskman_parts[i].dev) == str_len(diskman_devices[diskman_selected].name)) {
                int same = 1;
                for (uint64_t j = 0; j < str_len(diskman_devices[diskman_selected].name); j++) {
                    if (diskman_parts[i].dev[j] != diskman_devices[diskman_selected].name[j]) { same = 0; break; }
                }
                if (same) {
                    diskman_selected_part = i;
                    break;
                }
            }
        }
    }
}

static void diskman_emit_line(uint64_t row, const char *line) {
    diskman_move_row(row);
    icda_write(line);
}

static const char *diskman_action_name(uint64_t fs_type) {
    switch (fs_type) {
        case DISKMAN_FS_FAT32: return "FAT32 format";
        case DISKMAN_FS_EXFAT: return "exFAT format";
        case DISKMAN_LAYOUT_CLEAR: return "clear layout";
        case DISKMAN_LAYOUT_ICDA: return "ICDA layout";
        case DISKMAN_LAYOUT_MBR: return "init MBR";
        case DISKMAN_LAYOUT_GPT: return "init GPT";
        default: return "disk action";
    }
}

static void diskman_draw(void) {
    diskman_device_t *sel = (diskman_device_count && diskman_selected < diskman_device_count) ? &diskman_devices[diskman_selected] : 0;
    uint64_t shown_parts = 0;
    uint64_t device_rows;
    uint64_t partitions_title_row;
    uint64_t partitions_base_row;
    uint64_t footer_sep_row;
    uint64_t partition_rows;
    char lines[DISKMAN_MAX_H][DISKMAN_MAX_W + 1];

    diskman_layout_refresh();
    for (uint64_t row = 0; row < diskman_height; row++) {
        line_reset(lines[row], diskman_width);
    }

    device_rows = diskman_height > 18 ? 5 : 3;
    partitions_title_row = 4 + device_rows + 1;
    partitions_base_row = partitions_title_row + 4;
    footer_sep_row = diskman_height - 3;
    partition_rows = footer_sep_row > partitions_base_row ? footer_sep_row - partitions_base_row : 0;

    line_fill(lines[0], diskman_width, CP437_BLOCK_FULL);
    line_put_text(lines[1], diskman_width, 0, "\xDB ICDA Disk Manager");
    if (sel) {
        line_put_text(lines[1], diskman_width, 26, "Disk: ");
        line_put_text(lines[1], diskman_width, 32, sel->name);
        line_put_text(lines[1], diskman_width, 42, "Size: ");
        write_size_brief_into(lines[1], diskman_width, 48, sel->sectors, sel->sector_size);
    }
    line_fill(lines[2], diskman_width, CP437_BLOCK_FULL);

    line_put_text(lines[3], diskman_width, 0, " Devices:");
    if (diskman_device_count == 0) {
        line_put_text(lines[4], diskman_width, 0, "   (none detected)");
    } else {
        for (uint64_t i = 0; i < diskman_device_count && i < device_rows; i++) {
            uint64_t row = 4 + i;
        line_put_text(lines[row], diskman_width, 0, i == diskman_selected ? " >> " : "    ");
        line_put_uint(lines[row], diskman_width, 4, diskman_devices[i].index, 0);
        line_put_text(lines[row], diskman_width, 5 + str_len(&lines[row][5]), ": ");
        write_field(lines[row], diskman_width, 7, diskman_devices[i].name, 8);
        write_field(lines[row], diskman_width, 16, diskman_devices[i].table, 8);
        write_size_brief_into(lines[row], diskman_width, 28, diskman_devices[i].sectors, diskman_devices[i].sector_size);
        if (diskman_runtime_device >= 0 && diskman_devices[i].index == (uint64_t)diskman_runtime_device) {
            line_put_text(lines[row], diskman_width, 40, "protected");
        }
        }
    }

    line_fill(lines[partitions_title_row], diskman_width, CP437_BLOCK_MED);
    line_put_text(lines[partitions_title_row + 1], diskman_width, 0, " Partitions on selected device:");
    line_put_text(lines[partitions_title_row + 2], diskman_width, 0, "  Id  Name         FS       Role     Start        Sectors      Size");
    line_fill(lines[partitions_title_row + 3], diskman_width, CP437_LINE_H);

    if (sel) {
        for (uint64_t i = 0; i < diskman_part_count && shown_parts < partition_rows; i++) {
            uint64_t row = partitions_base_row + shown_parts;
            if (str_len(diskman_parts[i].dev) == 0 || str_len(sel->name) == 0) continue;
            if (str_len(diskman_parts[i].dev) == str_len(sel->name)) {
                int same = 1;
                for (uint64_t j = 0; j < str_len(sel->name); j++) {
                    if (diskman_parts[i].dev[j] != sel->name[j]) { same = 0; break; }
                }
                if (!same) continue;
            } else {
                continue;
            }
            line_put_text(lines[row], diskman_width, 0, (diskman_focus_parts && i == diskman_selected_part) ? " >" : "  ");
            line_put_uint(lines[row], diskman_width, 2, diskman_parts[i].index, 2);
            write_field(lines[row], diskman_width, 6, diskman_parts[i].name, 10);
            write_field(lines[row], diskman_width, 17, diskman_parts[i].fs[0] ? diskman_parts[i].fs : "unknown", 8);
            write_field(lines[row], diskman_width, 26, diskman_parts[i].role[0] ? diskman_parts[i].role : "unknown", 8);
            line_put_uint(lines[row], diskman_width, 35, diskman_parts[i].start, 12);
            line_put_uint(lines[row], diskman_width, 48, diskman_parts[i].sectors, 12);
            write_size_brief_into(lines[row], diskman_width, 61, diskman_parts[i].sectors, 512);
            shown_parts++;
        }
    }
    if (shown_parts == 0 && partitions_base_row < diskman_height) {
        line_put_text(lines[partitions_base_row], diskman_width, 0, "   (no partitions detected on selected device)");
    }

    line_fill(lines[footer_sep_row], diskman_width, CP437_BLOCK_MED);
    line_put_text(lines[footer_sep_row + 1], diskman_width, 0, " Status:");
    line_put_text(lines[footer_sep_row + 1], diskman_width, 8, diskman_status[0] ? diskman_status : "ready");
    line_put_text(lines[footer_sep_row + 2], diskman_width, 0, " [TAB] focus  [M] MBR  [G] GPT  [F] FAT32  [X] exFAT  [E] EFI  [N] Root  [W] Swap  [I] ICDA  [Q] quit");

    for (uint64_t row = 0; row < diskman_height; row++) {
        int changed = !diskman_prev_valid;
        if (!changed) {
            for (uint64_t col = 0; col < diskman_width; col++) {
                if (diskman_prev_lines[row][col] != lines[row][col]) {
                    changed = 1;
                    break;
                }
            }
        }
        if (changed) {
            diskman_emit_line(row, lines[row]);
            copy_text(diskman_prev_lines[row], lines[row], sizeof(diskman_prev_lines[row]));
        }
    }
    diskman_prev_valid = 1;
}

static void diskman_format_selected(uint64_t fs_type) {
    long rc;
    const char *name = diskman_action_name(fs_type);
    if (diskman_device_count == 0) {
        copy_text(diskman_status, "no device selected", sizeof(diskman_status));
        return;
    }
    if (diskman_runtime_device >= 0 &&
        diskman_devices[diskman_selected].index == (uint64_t)diskman_runtime_device) {
        copy_text(diskman_status, "runtime persistence disk protected", sizeof(diskman_status));
        return;
    }
    copy_text(diskman_status, "running ", sizeof(diskman_status));
    copy_text(diskman_status + str_len(diskman_status), name, sizeof(diskman_status) - str_len(diskman_status));
    copy_text(diskman_status + str_len(diskman_status), "...", sizeof(diskman_status) - str_len(diskman_status));
    diskman_draw();
    rc = (long)icda_format_device(diskman_devices[diskman_selected].index, fs_type);
    if (rc < 0) {
        copy_text(diskman_status, name, sizeof(diskman_status));
        copy_text(diskman_status + str_len(diskman_status), " failed", sizeof(diskman_status) - str_len(diskman_status));
        copy_text(diskman_status + str_len(diskman_status), " (err=", sizeof(diskman_status) - str_len(diskman_status));
        append_uint_text(diskman_status, sizeof(diskman_status), (uint64_t)(-rc));
        copy_text(diskman_status + str_len(diskman_status), ")", sizeof(diskman_status) - str_len(diskman_status));
    } else {
        copy_text(diskman_status, name, sizeof(diskman_status));
        copy_text(diskman_status + str_len(diskman_status), " complete", sizeof(diskman_status) - str_len(diskman_status));
        diskman_refresh();
    }
}

static void diskman_format_selected_partition(uint64_t fs_type) {
    long rc;
    const char *name = diskman_action_name(fs_type);
    if (diskman_part_count == 0) {
        copy_text(diskman_status, "no partition selected", sizeof(diskman_status));
        return;
    }
    copy_text(diskman_status, "running ", sizeof(diskman_status));
    copy_text(diskman_status + str_len(diskman_status), name, sizeof(diskman_status) - str_len(diskman_status));
    copy_text(diskman_status + str_len(diskman_status), " on partition...", sizeof(diskman_status) - str_len(diskman_status));
    diskman_draw();
    rc = (long)icda_format_partition(diskman_parts[diskman_selected_part].index, fs_type);
    if (rc < 0) {
        copy_text(diskman_status, name, sizeof(diskman_status));
        copy_text(diskman_status + str_len(diskman_status), " failed (err=", sizeof(diskman_status) - str_len(diskman_status));
        append_uint_text(diskman_status, sizeof(diskman_status), (uint64_t)(-rc));
        copy_text(diskman_status + str_len(diskman_status), ")", sizeof(diskman_status) - str_len(diskman_status));
    } else {
        copy_text(diskman_status, name, sizeof(diskman_status));
        copy_text(diskman_status + str_len(diskman_status), " complete", sizeof(diskman_status) - str_len(diskman_status));
        diskman_refresh();
    }
}

static void diskman_set_selected_partition_role(uint64_t role, const char *label) {
    long rc;
    if (diskman_part_count == 0) {
        copy_text(diskman_status, "no partition selected", sizeof(diskman_status));
        return;
    }
    copy_text(diskman_status, "setting role ", sizeof(diskman_status));
    copy_text(diskman_status + str_len(diskman_status), label, sizeof(diskman_status) - str_len(diskman_status));
    diskman_draw();
    rc = (long)icda_set_partition_role(diskman_parts[diskman_selected_part].index, role);
    if (rc < 0) {
        copy_text(diskman_status, "set role failed (err=", sizeof(diskman_status));
        append_uint_text(diskman_status, sizeof(diskman_status), (uint64_t)(-rc));
        copy_text(diskman_status + str_len(diskman_status), ")", sizeof(diskman_status) - str_len(diskman_status));
    } else {
        copy_text(diskman_status, "partition role updated", sizeof(diskman_status));
        diskman_refresh();
    }
}

uint64_t diskman_main(void) {
    copy_text(diskman_status, "ready", sizeof(diskman_status));
    diskman_refresh();
    icda_clear();
    diskman_prev_valid = 0;
    diskman_draw();
    for (;;) {
        long c = diskman_read_key();
        if (c < 0) continue;
        if (c == 'q' || c == 24) {
            icda_clear();
            icda_exit(0);
        }
        if (c == '\t') {
            diskman_focus_parts = !diskman_focus_parts;
            diskman_draw();
            continue;
        }
        if (c == KEY_UP) {
            if (diskman_focus_parts) {
                if (diskman_selected_part > 0) diskman_selected_part--;
            } else {
                if (diskman_selected > 0) diskman_selected--;
            }
        } else if (c == KEY_DOWN) {
            if (diskman_focus_parts) {
                if (diskman_selected_part + 1 < diskman_part_count) diskman_selected_part++;
            } else {
                if (diskman_selected + 1 < diskman_device_count) diskman_selected++;
            }
        } else if (c == 'r' || c == 'R') {
            diskman_refresh();
        } else if (c == 'f' || c == 'F') {
            if (diskman_focus_parts) diskman_format_selected_partition(DISKMAN_FS_FAT32);
            else diskman_format_selected(DISKMAN_FS_FAT32);
        } else if (c == 'x' || c == 'X') {
            if (diskman_focus_parts) diskman_format_selected_partition(DISKMAN_FS_EXFAT);
            else diskman_format_selected(DISKMAN_FS_EXFAT);
        } else if (c == 'm' || c == 'M') {
            diskman_format_selected(DISKMAN_LAYOUT_MBR);
        } else if (c == 'g' || c == 'G') {
            diskman_format_selected(DISKMAN_LAYOUT_GPT);
        } else if (c == 'e' || c == 'E') {
            diskman_set_selected_partition_role(1, "efi");
        } else if (c == 'n' || c == 'N') {
            diskman_set_selected_partition_role(2, "system");
        } else if (c == 'w' || c == 'W') {
            diskman_set_selected_partition_role(3, "swap");
        } else if (c == 'i' || c == 'I') {
            diskman_format_selected(DISKMAN_LAYOUT_ICDA);
        } else if (c == 'c' || c == 'C') {
            diskman_format_selected(DISKMAN_LAYOUT_CLEAR);
        }
        diskman_draw();
    }
}
