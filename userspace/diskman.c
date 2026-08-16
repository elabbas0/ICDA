#include "gui.h"
#include "icda_sys.h"
#include "libicda.h"
#include "font.h"

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

static uint64_t diskman_console_main(void) {
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

/* ============================ GUI mode ============================ */

#define DG_W 780
#define DG_H 460
#define DG_HEAD_H 64
#define DG_PANEL_Y 76
#define DG_LEFT_X 12
#define DG_LEFT_W 280
#define DG_RIGHT_X 300
#define DG_BTN_H 28
#define DG_ROW_A_Y (DG_H - 94)
#define DG_ROW_B_Y (DG_H - 58)
#define DG_STATUS_Y (DG_H - 30)
#define DG_DEV_ROW_H 26
#define DG_PART_ROW_H 18

enum {
    DG_KEY_UP = 1,
    DG_KEY_DOWN = 2,
    DG_KEY_LEFT = 3,
    DG_KEY_RIGHT = 4
};

static int dg_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}

static void dg_text(int x, int y, const char *text, uint32_t fg, uint32_t bg, int max_px) {
    int cx = x;
    if (max_px <= 0) return;
    while (text && *text && cx + FONT_CELL_WIDTH <= x + max_px) {
        gui_draw_char(cx, y, *text, fg, bg);
        cx += FONT_CELL_WIDTH;
        text++;
    }
}

static void dg_button(int x, int y, int w, int h, const char *label, int active) {
    uint32_t top = active ? 0x003D8BFF : 0x00FFFFFF;
    uint32_t bottom = active ? 0x001F5EBE : 0x00DDEBFF;
    uint32_t edge = active ? 0x000C3C88 : 0x006EA6E8;
    for (int row = 0; row < h; row++) {
        int r = (int)(((top >> 16) & 0xFF) + ((((bottom >> 16) & 0xFF) - ((top >> 16) & 0xFF)) * row) / (h - 1));
        int g = (int)(((top >> 8) & 0xFF) + ((((bottom >> 8) & 0xFF) - ((top >> 8) & 0xFF)) * row) / (h - 1));
        int b = (int)((top & 0xFF) + (((bottom & 0xFF) - (top & 0xFF)) * row) / (h - 1));
        gui_fill_rect(x, y + row, w, 1, (uint32_t)((r << 16) | (g << 8) | b));
    }
    gui_draw_rect_outline(x, y, w, h, edge);
    dg_text(x + 6, y + 6, label, active ? 0x00FFFFFF : 0x001D3F66, bottom, w - 12);
}

static void dg_status_set(const char *text) {
    copy_text(diskman_status, text ? text : "", sizeof(diskman_status));
}

static void dg_status_append(const char *text) {
    copy_text(diskman_status + str_len(diskman_status), text,
              sizeof(diskman_status) - str_len(diskman_status));
}

static void dg_size_text(uint64_t sectors, uint64_t sector_size, char *out, uint64_t cap) {
    uint64_t bytes = sectors * sector_size;
    uint64_t gib = 1024ULL * 1024ULL * 1024ULL;
    uint64_t tib = gib * 1024ULL;
    out[0] = 0;
    if (bytes >= tib) {
        append_uint_text(out, cap, bytes / tib);
        copy_text(out + str_len(out), " TiB", cap - str_len(out));
    } else if (bytes >= gib) {
        append_uint_text(out, cap, bytes / gib);
        copy_text(out + str_len(out), " GiB", cap - str_len(out));
    } else {
        append_uint_text(out, cap, bytes / (1024ULL * 1024ULL));
        copy_text(out + str_len(out), " MiB", cap - str_len(out));
    }
}

static int dg_part_of_selected(uint64_t i) {
    uint64_t n;
    if (diskman_selected >= diskman_device_count) return 0;
    if (str_len(diskman_parts[i].dev) == 0) return 0;
    n = str_len(diskman_devices[diskman_selected].name);
    if (n == 0 || str_len(diskman_parts[i].dev) != n) return 0;
    for (uint64_t j = 0; j < n; j++) {
        if (diskman_parts[i].dev[j] != diskman_devices[diskman_selected].name[j]) return 0;
    }
    return 1;
}

static uint64_t dg_selected_part_index(void) {
    for (uint64_t i = 0; i < diskman_part_count; i++) {
        if (dg_part_of_selected(i) && i == diskman_selected_part) return i;
    }
    for (uint64_t i = 0; i < diskman_part_count; i++) {
        if (dg_part_of_selected(i)) return i;
    }
    return 0;
}

static void dg_format_device_action(uint64_t fs_type) {
    long rc;
    const char *name = diskman_action_name(fs_type);
    if (diskman_device_count == 0) {
        dg_status_set("no device selected");
        return;
    }
    if (diskman_runtime_device >= 0 &&
        diskman_devices[diskman_selected].index == (uint64_t)diskman_runtime_device) {
        dg_status_set("runtime persistence disk protected");
        return;
    }
    dg_status_set("running ");
    dg_status_append(name);
    dg_status_append("...");
    rc = (long)icda_format_device(diskman_devices[diskman_selected].index, fs_type);
    if (rc < 0) {
        dg_status_set(name);
        dg_status_append(" failed (err=");
        append_uint_text(diskman_status, sizeof(diskman_status), (uint64_t)(-rc));
        dg_status_append(")");
    } else {
        dg_status_set(name);
        dg_status_append(" complete");
        diskman_refresh();
    }
}

static void dg_format_partition_action(uint64_t fs_type) {
    long rc;
    const char *name = diskman_action_name(fs_type);
    uint64_t part = dg_selected_part_index();
    if (diskman_part_count == 0) {
        dg_status_set("no partition selected");
        return;
    }
    dg_status_set("running ");
    dg_status_append(name);
    dg_status_append(" on partition...");
    rc = (long)icda_format_partition(diskman_parts[part].index, fs_type);
    if (rc < 0) {
        dg_status_set(name);
        dg_status_append(" failed (err=");
        append_uint_text(diskman_status, sizeof(diskman_status), (uint64_t)(-rc));
        dg_status_append(")");
    } else {
        dg_status_set(name);
        dg_status_append(" complete");
        diskman_refresh();
    }
}

static void dg_set_role_action(uint64_t role, const char *label) {
    long rc;
    uint64_t part = dg_selected_part_index();
    if (diskman_part_count == 0) {
        dg_status_set("no partition selected");
        return;
    }
    rc = (long)icda_set_partition_role(diskman_parts[part].index, role);
    if (rc < 0) {
        dg_status_set("set role failed (err=");
        append_uint_text(diskman_status, sizeof(diskman_status), (uint64_t)(-rc));
        dg_status_append(")");
    } else {
        dg_status_set("partition role updated (");
        dg_status_append(label);
        dg_status_append(")");
        diskman_refresh();
    }
}

static void dg_draw(void) {
    int w = gui_window_width();
    int h = gui_window_height();
    int max_dev_rows = (h - DG_PANEL_Y - 90) / DG_DEV_ROW_H;
    int max_part_rows = (h - DG_PANEL_Y - 92) / DG_PART_ROW_H;
    char buf[64];
    int shown = 0;

    if (max_dev_rows < 1) max_dev_rows = 1;
    if (max_part_rows < 1) max_part_rows = 1;

    gui_fill_rect(0, 0, w, h, 0x00E7F1FF);
    for (int row = 0; row < DG_HEAD_H; row++) {
        int r = (int)(0x3D + ((0x1F - 0x3D) * row) / (DG_HEAD_H - 1));
        int g = (int)(0x8B + ((0x5E - 0x8B) * row) / (DG_HEAD_H - 1));
        int b = (int)(0xFF + ((0xBE - 0xFF) * row) / (DG_HEAD_H - 1));
        gui_fill_rect(0, row, w, 1, (uint32_t)((r << 16) | (g << 8) | b));
    }
    gui_fill_rect(0, DG_HEAD_H - 1, w, 1, 0x0015449C);
    dg_text(16, 12, "ICDA Disk Manager", 0x00FFFFFF, 0x002C73D2, 220);
    if (diskman_device_count && diskman_selected < diskman_device_count) {
        dg_text(240, 12, "Disk: ", 0x00EAF2FF, 0x002C73D2, 60);
        dg_text(288, 12, diskman_devices[diskman_selected].name, 0x00FFFFFF, 0x002C73D2, 100);
        dg_size_text(diskman_devices[diskman_selected].sectors,
                     diskman_devices[diskman_selected].sector_size, buf, sizeof(buf));
        dg_text(390, 12, "Size: ", 0x00EAF2FF, 0x002C73D2, 60);
        dg_text(438, 12, buf, 0x00FFFFFF, 0x002C73D2, 90);
    }
    dg_text(16, 36, "TAB focus  arrows move  M MBR  G GPT  F FAT32  X exFAT  I ICDA  C clear  R refresh  Q close",
            0x00EAF2FF, 0x002C73D2, w - 32);

    /* Devices panel */
    gui_fill_rect(DG_LEFT_X, DG_PANEL_Y, DG_LEFT_W, h - DG_PANEL_Y - 92, 0x00FFFFFF);
    gui_draw_rect_outline(DG_LEFT_X, DG_PANEL_Y, DG_LEFT_W, h - DG_PANEL_Y - 92, 0x0092B7E8);
    dg_text(DG_LEFT_X + 8, DG_PANEL_Y + 4, "Devices", 0x001D3F66, 0x00FFFFFF, DG_LEFT_W - 16);
    if (diskman_device_count == 0) {
        dg_text(DG_LEFT_X + 8, DG_PANEL_Y + 26, "(none detected)", 0x0064758B, 0x00FFFFFF, DG_LEFT_W - 16);
    } else {
        for (uint64_t i = 0; i < diskman_device_count && (int)i < max_dev_rows; i++) {
            int y = DG_PANEL_Y + 22 + (int)i * DG_DEV_ROW_H;
            int selected = (uint64_t)diskman_selected == i;
            if (selected) {
                gui_fill_rect(DG_LEFT_X + 1, y, DG_LEFT_W - 2, DG_DEV_ROW_H - 1, 0x00CFE6FF);
            }
            buf[0] = 0;
            append_uint_text(buf, sizeof(buf), diskman_devices[i].index);
            copy_text(buf + str_len(buf), ": ", sizeof(buf) - str_len(buf));
            copy_text(buf + str_len(buf), diskman_devices[i].name, sizeof(buf) - str_len(buf));
            copy_text(buf + str_len(buf), "  ", sizeof(buf) - str_len(buf));
            copy_text(buf + str_len(buf), diskman_devices[i].table, sizeof(buf) - str_len(buf));
            if (diskman_runtime_device >= 0 && diskman_devices[i].index == (uint64_t)diskman_runtime_device) {
                copy_text(buf + str_len(buf), "  protected", sizeof(buf) - str_len(buf));
            }
            dg_text(DG_LEFT_X + 8, y + 4, buf, selected ? 0x001F2937 : 0x0024334A,
                    selected ? 0x00CFE6FF : 0x00FFFFFF, DG_LEFT_W - 16);
        }
    }

    /* Partitions panel */
    gui_fill_rect(DG_RIGHT_X, DG_PANEL_Y, w - DG_RIGHT_X - 14, h - DG_PANEL_Y - 92, 0x00FFFFFF);
    gui_draw_rect_outline(DG_RIGHT_X, DG_PANEL_Y, w - DG_RIGHT_X - 14, h - DG_PANEL_Y - 92, 0x0092B7E8);
    dg_text(DG_RIGHT_X + 8, DG_PANEL_Y + 4, "Partitions on selected device", 0x001D3F66, 0x00FFFFFF, w - DG_RIGHT_X - 40);
    dg_text(DG_RIGHT_X + 8, DG_PANEL_Y + 22, "Id  Name      FS        Role      Start        Sectors", 0x00334455, 0x00FFFFFF, w - DG_RIGHT_X - 40);
    {
        uint64_t part = dg_selected_part_index();
        for (uint64_t i = 0; i < diskman_part_count && shown < max_part_rows; i++) {
            int y = DG_PANEL_Y + 40 + shown * DG_PART_ROW_H;
            int selected = (i == part);
            if (!dg_part_of_selected(i)) continue;
            if (selected) {
                gui_fill_rect(DG_RIGHT_X + 1, y, w - DG_RIGHT_X - 16, DG_PART_ROW_H - 1, 0x00DDF0FF);
            }
            buf[0] = 0;
            append_uint_text(buf, sizeof(buf), diskman_parts[i].index);
            copy_text(buf + str_len(buf), "  ", sizeof(buf) - str_len(buf));
            copy_text(buf + str_len(buf), diskman_parts[i].name, sizeof(buf) - str_len(buf));
            dg_text(DG_RIGHT_X + 8, y + 2, buf, 0x001F2937, selected ? 0x00DDF0FF : 0x00FFFFFF, 130);
            dg_text(DG_RIGHT_X + 116, y + 2, diskman_parts[i].fs[0] ? diskman_parts[i].fs : "unknown",
                    0x001F2937, selected ? 0x00DDF0FF : 0x00FFFFFF, 90);
            dg_text(DG_RIGHT_X + 196, y + 2, diskman_parts[i].role[0] ? diskman_parts[i].role : "unknown",
                    0x001F2937, selected ? 0x00DDF0FF : 0x00FFFFFF, 90);
            buf[0] = 0;
            append_uint_text(buf, sizeof(buf), diskman_parts[i].start);
            dg_text(DG_RIGHT_X + 276, y + 2, buf, 0x001F2937, selected ? 0x00DDF0FF : 0x00FFFFFF, 110);
            buf[0] = 0;
            append_uint_text(buf, sizeof(buf), diskman_parts[i].sectors);
            dg_text(DG_RIGHT_X + 336, y + 2, buf, 0x001F2937, selected ? 0x00DDF0FF : 0x00FFFFFF, 120);
            shown++;
        }
        if (shown == 0) {
            dg_text(DG_RIGHT_X + 8, DG_PANEL_Y + 40, "(no partitions on this device)", 0x0064758B, 0x00FFFFFF, w - DG_RIGHT_X - 40);
        }
    }

    /* Buttons */
    dg_button(12, DG_ROW_A_Y, 62, DG_BTN_H, "MBR", 1);
    dg_button(82, DG_ROW_A_Y, 62, DG_BTN_H, "GPT", 1);
    dg_button(152, DG_ROW_A_Y, 74, DG_BTN_H, "FAT32", 1);
    dg_button(234, DG_ROW_A_Y, 74, DG_BTN_H, "exFAT", 1);
    dg_button(316, DG_ROW_A_Y, 74, DG_BTN_H, "ICDA", 1);
    dg_button(398, DG_ROW_A_Y, 74, DG_BTN_H, "Clear", 1);

    dg_button(12, DG_ROW_B_Y, 62, DG_BTN_H, "EFI", diskman_focus_parts);
    dg_button(82, DG_ROW_B_Y, 62, DG_BTN_H, "Root", diskman_focus_parts);
    dg_button(152, DG_ROW_B_Y, 62, DG_BTN_H, "Swap", diskman_focus_parts);
    dg_button(222, DG_ROW_B_Y, 82, DG_BTN_H, "Refresh", 1);
    dg_button(312, DG_ROW_B_Y, 62, DG_BTN_H, "Quit", 1);

    /* Status bar */
    gui_fill_rect(0, DG_STATUS_Y, w, 30, 0x00EAF2FF);
    gui_draw_hline(0, DG_STATUS_Y, w, 0x0092B7E8);
    dg_text(12, DG_STATUS_Y + 8, diskman_status[0] ? diskman_status : "ready", 0x00334455, 0x00EAF2FF, w - 24);
}

static void dg_key_action(uint32_t key) {
    if (key == 'q' || key == 'Q' || key == 24) {
        gui_close_window();
        icda_exit(0);
        return;
    }
    if (key == '\t') {
        diskman_focus_parts = !diskman_focus_parts;
        return;
    }
    if (key == DG_KEY_UP) {
        if (diskman_focus_parts) {
            uint64_t part = dg_selected_part_index();
            uint64_t prev = 0;
            int found = 0;
            for (uint64_t i = 0; i < diskman_part_count; i++) {
                if (!dg_part_of_selected(i)) continue;
                if (i < part) {
                    prev = i;
                    found = 1;
                }
            }
            if (found) diskman_selected_part = prev;
        } else if (diskman_selected > 0) {
            diskman_selected--;
            diskman_refresh();
        }
        return;
    }
    if (key == DG_KEY_DOWN) {
        if (diskman_focus_parts) {
            uint64_t part = dg_selected_part_index();
            int found = 0;
            for (uint64_t i = part + 1; i < diskman_part_count; i++) {
                if (dg_part_of_selected(i)) {
                    diskman_selected_part = i;
                    found = 1;
                    break;
                }
            }
            (void)found;
        } else if (diskman_selected + 1 < diskman_device_count) {
            diskman_selected++;
            diskman_refresh();
        }
        return;
    }
    if (key == 'r' || key == 'R') { diskman_refresh(); return; }
    if (key == 'f' || key == 'F') {
        if (diskman_focus_parts) dg_format_partition_action(DISKMAN_FS_FAT32);
        else dg_format_device_action(DISKMAN_FS_FAT32);
        return;
    }
    if (key == 'x' || key == 'X') {
        if (diskman_focus_parts) dg_format_partition_action(DISKMAN_FS_EXFAT);
        else dg_format_device_action(DISKMAN_FS_EXFAT);
        return;
    }
    if (key == 'm' || key == 'M') { dg_format_device_action(DISKMAN_LAYOUT_MBR); return; }
    if (key == 'g' || key == 'G') { dg_format_device_action(DISKMAN_LAYOUT_GPT); return; }
    if (key == 'e' || key == 'E') { dg_set_role_action(1, "efi"); return; }
    if (key == 'n' || key == 'N') { dg_set_role_action(2, "system"); return; }
    if (key == 'w' || key == 'W') { dg_set_role_action(3, "swap"); return; }
    if (key == 'i' || key == 'I') { dg_format_device_action(DISKMAN_LAYOUT_ICDA); return; }
    if (key == 'c' || key == 'C') { dg_format_device_action(DISKMAN_LAYOUT_CLEAR); return; }
}

static void dg_mouse_click(int mx, int my) {
    int w = gui_window_width();
    int h = gui_window_height();
    int max_dev_rows = (h - DG_PANEL_Y - 90) / DG_DEV_ROW_H;
    int max_part_rows = (h - DG_PANEL_Y - 92) / DG_PART_ROW_H;
    int i;

    if (max_dev_rows < 1) max_dev_rows = 1;
    if (max_part_rows < 1) max_part_rows = 1;

    if (dg_hit(mx, my, 12, DG_ROW_A_Y, 62, DG_BTN_H)) { dg_format_device_action(DISKMAN_LAYOUT_MBR); return; }
    if (dg_hit(mx, my, 82, DG_ROW_A_Y, 62, DG_BTN_H)) { dg_format_device_action(DISKMAN_LAYOUT_GPT); return; }
    if (dg_hit(mx, my, 152, DG_ROW_A_Y, 74, DG_BTN_H)) {
        if (diskman_focus_parts) dg_format_partition_action(DISKMAN_FS_FAT32);
        else dg_format_device_action(DISKMAN_FS_FAT32);
        return;
    }
    if (dg_hit(mx, my, 234, DG_ROW_A_Y, 74, DG_BTN_H)) {
        if (diskman_focus_parts) dg_format_partition_action(DISKMAN_FS_EXFAT);
        else dg_format_device_action(DISKMAN_FS_EXFAT);
        return;
    }
    if (dg_hit(mx, my, 316, DG_ROW_A_Y, 74, DG_BTN_H)) { dg_format_device_action(DISKMAN_LAYOUT_ICDA); return; }
    if (dg_hit(mx, my, 398, DG_ROW_A_Y, 74, DG_BTN_H)) { dg_format_device_action(DISKMAN_LAYOUT_CLEAR); return; }

    if (dg_hit(mx, my, 12, DG_ROW_B_Y, 62, DG_BTN_H)) { dg_set_role_action(1, "efi"); return; }
    if (dg_hit(mx, my, 82, DG_ROW_B_Y, 62, DG_BTN_H)) { dg_set_role_action(2, "system"); return; }
    if (dg_hit(mx, my, 152, DG_ROW_B_Y, 62, DG_BTN_H)) { dg_set_role_action(3, "swap"); return; }
    if (dg_hit(mx, my, 222, DG_ROW_B_Y, 82, DG_BTN_H)) { diskman_refresh(); return; }
    if (dg_hit(mx, my, 312, DG_ROW_B_Y, 62, DG_BTN_H)) {
        gui_close_window();
        icda_exit(0);
        return;
    }

    if (dg_hit(mx, my, DG_LEFT_X, DG_PANEL_Y + 22, DG_LEFT_W, max_dev_rows * DG_DEV_ROW_H)) {
        i = (my - DG_PANEL_Y - 22) / DG_DEV_ROW_H;
        if (i >= 0 && (uint64_t)i < diskman_device_count) {
            diskman_focus_parts = 0;
            diskman_selected = (uint64_t)i;
            diskman_refresh();
        }
        return;
    }
    if (dg_hit(mx, my, DG_RIGHT_X, DG_PANEL_Y + 40, w - DG_RIGHT_X - 14, max_part_rows * DG_PART_ROW_H)) {
        int shown = 0;
        for (uint64_t p = 0; p < diskman_part_count; p++) {
            if (!dg_part_of_selected(p)) continue;
            if (shown == (my - DG_PANEL_Y - 40) / DG_PART_ROW_H) {
                diskman_focus_parts = 1;
                diskman_selected_part = p;
                return;
            }
            shown++;
        }
        return;
    }
    (void)w;
}

static uint64_t diskman_gui_main(void) {
    int key_seq = 0;

    if (gui_open_window("Disk Manager", DG_W, DG_H) != 0) {
        return 1;
    }
    dg_status_set("ready");
    diskman_focus_parts = 0;
    diskman_refresh();
    dg_draw();
    gui_flush();

    for (;;) {
        gui_msg_t msg;
        int changed = 0;
        while (gui_poll_event(&msg)) {
            changed = 1;
            if (msg.type == GUI_MSG_MOUSE_EVENT && (msg.mouse.buttons & GUI_BTN_LEFT)) {
                dg_mouse_click(msg.mouse.x, msg.mouse.y);
            } else if (msg.type == GUI_MSG_KEY_EVENT && msg.key.pressed) {
                uint32_t code = msg.key.keycode;
                if (key_seq == 0 && code == 27) {
                    key_seq = 1;
                } else if (key_seq == 1 && code == '[') {
                    key_seq = 2;
                } else if (key_seq == 2) {
                    key_seq = 0;
                    if (code == 'A') dg_key_action(DG_KEY_UP);
                    else if (code == 'B') dg_key_action(DG_KEY_DOWN);
                } else {
                    key_seq = 0;
                    dg_key_action(code);
                }
            } else if (msg.type == GUI_MSG_CLOSE_WINDOW) {
                gui_close_window();
                return 0;
            }
        }
        if (changed) {
            dg_draw();
            gui_flush();
        }
        icda_sleep(1);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (icda_gui_available()) {
        return (int)diskman_gui_main();
    }
    return (int)diskman_console_main();
}
