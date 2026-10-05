#include "fatfs.h"
#include "../memory/heap.h"
#include "../drivers/rtc/rtc.h"

#define SECTOR        512U
#define EOC           0x0FFFFFFFU
#define EOC_MIN       0x0FFFFFF8U
#define ATTR_LFN      0x0F
#define ATTR_VOLUME   0x08
#define SLOT_FREE     0xE5
#define LFN_CHARS     13U

static void mem_zero(void *p, uint64_t n) {
    uint8_t *d = (uint8_t *)p;
    for (uint64_t i = 0; i < n; i++) d[i] = 0;
}

static void mem_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

static void wr16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    wr16(p, v & 0xFFFFU);
    wr16(p + 2, v >> 16);
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (up(*a) != up(*b)) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int dev_read(fatfs_t *v, uint64_t rel, uint32_t n, void *buf) {
    if (rel + n > v->sectors) return -1;
    return v->dev->read(v->dev->context, v->base_lba + rel, n, buf);
}

static int dev_write(fatfs_t *v, uint64_t rel, uint32_t n, const void *buf) {
    if (!v->dev->write || rel + n > v->sectors) return -1;
    v->touched = 1;
    return v->dev->write(v->dev->context, v->base_lba + rel, n, buf);
}

static int valid_cluster(const fatfs_t *v, uint32_t c) {
    return c >= 2 && c < v->cluster_count + 2;
}

static uint64_t cluster_lba(const fatfs_t *v, uint32_t c) {
    return v->data_lba + (uint64_t)(c - 2) * v->sectors_per_cluster;
}

static int fat_cache_flush(fatfs_t *v) {
    if (!v->cache_dirty) return 0;
    for (uint32_t f = 0; f < v->fat_count; f++) {
        if (dev_write(v, v->fat_lba + (uint64_t)f * v->fat_sectors + (uint64_t)v->cache_sector, 1, v->cache) != 0) {
            return -1;
        }
    }
    v->cache_dirty = 0;
    return 0;
}

static int fat_cache_load(fatfs_t *v, uint32_t sector) {
    if (v->cache_sector == (int64_t)sector) return 0;
    if (fat_cache_flush(v) != 0) return -1;
    if (dev_read(v, v->fat_lba + sector, 1, v->cache) != 0) {
        v->cache_sector = -1;
        return -1;
    }
    v->cache_sector = sector;
    return 0;
}

static uint32_t fat_get(fatfs_t *v, uint32_t c) {
    if (fat_cache_load(v, (c * 4U) / SECTOR) != 0) return EOC;
    return rd32(v->cache + (c * 4U) % SECTOR) & 0x0FFFFFFFU;
}

static int fat_set(fatfs_t *v, uint32_t c, uint32_t value) {
    uint8_t *p;
    if (fat_cache_load(v, (c * 4U) / SECTOR) != 0) return -1;
    p = v->cache + (c * 4U) % SECTOR;
    wr32(p, (rd32(p) & 0xF0000000U) | (value & 0x0FFFFFFFU));
    v->cache_dirty = 1;
    return 0;
}

int fatfs_flush(fatfs_t *v) {
    uint8_t s[SECTOR];
    if (fat_cache_flush(v) != 0) return -1;
    if (!v->touched || v->fsinfo_sector == 0 || v->fsinfo_sector >= v->reserved) return 0;
    if (dev_read(v, v->fsinfo_sector, 1, s) != 0) return -1;
    if (rd32(s) != 0x41615252U || rd32(s + 484) != 0x61417272U) return 0;
    wr32(s + 488, 0xFFFFFFFFU);
    wr32(s + 492, v->next_free);
    v->touched = 0;
    return dev_write(v, v->fsinfo_sector, 1, s);
}

int fatfs_mount(fatfs_t *v, block_device_t *dev, uint64_t start_lba, uint64_t sector_count) {
    uint8_t s[SECTOR];
    uint32_t spc, total, data_sectors;
    if (!v || !dev || dev->sector_size != SECTOR) return -1;
    mem_zero(v, sizeof(*v));
    v->dev = dev;
    v->base_lba = start_lba;
    v->sectors = sector_count;
    v->cache_sector = -1;
    if (dev_read(v, 0, 1, s) != 0) return -1;
    if (s[510] != 0x55 || s[511] != 0xAA || rd16(s + 11) != SECTOR) return -1;
    spc = s[13];
    if (spc == 0 || (spc & (spc - 1)) != 0) return -1;
    v->sectors_per_cluster = spc;
    v->cluster_bytes = spc * SECTOR;
    v->reserved = rd16(s + 14);
    v->fat_count = s[16];
    v->fat_sectors = rd32(s + 36);
    v->root_cluster = rd32(s + 44);
    v->fsinfo_sector = rd16(s + 48);
    total = rd16(s + 19) ? rd16(s + 19) : rd32(s + 32);
    if (v->reserved == 0 || v->fat_count == 0 || v->fat_count > 4 || v->fat_sectors == 0 || rd16(s + 22) != 0) return -1;
    if (total == 0 || total > sector_count) return -1;
    v->sectors = total;
    v->fat_lba = v->reserved;
    v->data_lba = v->reserved + (uint64_t)v->fat_count * v->fat_sectors;
    if (v->data_lba >= total) return -1;
    data_sectors = total - (uint32_t)v->data_lba;
    v->cluster_count = data_sectors / spc;
    if (v->cluster_count < 65525U) return -1;
    if ((uint64_t)(v->cluster_count + 2) * 4U > (uint64_t)v->fat_sectors * SECTOR) {
        v->cluster_count = v->fat_sectors * (SECTOR / 4U) - 2;
    }
    if (!valid_cluster(v, v->root_cluster)) return -1;
    v->next_free = 2;
    return 0;
}

int fatfs_mount_part(fatfs_t *v, const partition_info_t *part) {
    if (!part || !part->device) return -1;
    return fatfs_mount(v, part->device, part->start_lba, part->sector_count);
}

static int alloc_one(fatfs_t *v, uint32_t *out) {
    uint32_t total = v->cluster_count;
    uint32_t c = valid_cluster(v, v->next_free) ? v->next_free : 2;
    for (uint32_t n = 0; n < total; n++) {
        if (fat_get(v, c) == 0) {
            if (fat_set(v, c, EOC) != 0) return -1;
            v->next_free = c + 1;
            *out = c;
            return 0;
        }
        c++;
        if (!valid_cluster(v, c)) c = 2;
    }
    return -1;
}

static void free_chain(fatfs_t *v, uint32_t c) {
    uint32_t guard = v->cluster_count;
    while (valid_cluster(v, c) && guard--) {
        uint32_t next = fat_get(v, c);
        fat_set(v, c, 0);
        if (c < v->next_free) v->next_free = c;
        if (next >= EOC_MIN) break;
        c = next;
    }
}

static int alloc_chain(fatfs_t *v, uint32_t count, uint32_t *first) {
    uint32_t prev = 0, c = 0;
    *first = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (alloc_one(v, &c) != 0) {
            if (*first) free_chain(v, *first);
            *first = 0;
            return -1;
        }
        if (prev) fat_set(v, prev, c);
        else *first = c;
        prev = c;
    }
    return 0;
}

static int zero_cluster(fatfs_t *v, uint32_t c) {
    uint8_t z[SECTOR];
    mem_zero(z, sizeof(z));
    for (uint32_t i = 0; i < v->sectors_per_cluster; i++) {
        if (dev_write(v, cluster_lba(v, c) + i, 1, z) != 0) return -1;
    }
    return 0;
}

static int write_chain(fatfs_t *v, uint32_t first, const uint8_t *data, uint64_t size) {
    uint8_t *buf;
    uint32_t c = first;
    uint32_t max_run = 64;
    uint64_t done = 0;
    buf = (uint8_t *)kmalloc((uint64_t)v->cluster_bytes * max_run);
    if (!buf) return -1;
    while (done < size && valid_cluster(v, c)) {
        uint32_t run = 1, last = c;
        uint64_t bytes;
        while (run < max_run && done + (uint64_t)run * v->cluster_bytes < size) {
            uint32_t next = fat_get(v, last);
            if (next != last + 1) break;
            last = next;
            run++;
        }
        bytes = (uint64_t)run * v->cluster_bytes;
        mem_zero(buf, bytes);
        mem_copy(buf, data + done, size - done < bytes ? size - done : bytes);
        if (dev_write(v, cluster_lba(v, c), run * v->sectors_per_cluster, buf) != 0) {
            kfree(buf);
            return -1;
        }
        done += bytes;
        c = fat_get(v, last);
    }
    kfree(buf);
    return done >= size ? 0 : -1;
}

static int slot_locate(fatfs_t *v, uint32_t dir, uint32_t slot, uint64_t *lba, uint32_t *off) {
    uint32_t per = v->cluster_bytes / 32U;
    uint32_t c = dir;
    for (uint32_t k = slot / per; k > 0; k--) {
        c = fat_get(v, c);
        if (!valid_cluster(v, c)) return -1;
    }
    if (!valid_cluster(v, c)) return -1;
    *lba = cluster_lba(v, c) + ((slot % per) * 32U) / SECTOR;
    *off = ((slot % per) * 32U) % SECTOR;
    return 0;
}

static int slot_write(fatfs_t *v, uint32_t dir, uint32_t slot, const uint8_t *entry) {
    uint8_t s[SECTOR];
    uint64_t lba;
    uint32_t off;
    if (slot_locate(v, dir, slot, &lba, &off) != 0) return -1;
    if (dev_read(v, lba, 1, s) != 0) return -1;
    mem_copy(s + off, entry, 32);
    return dev_write(v, lba, 1, s);
}

static int slot_read(fatfs_t *v, uint32_t dir, uint32_t slot, uint8_t *entry) {
    uint8_t s[SECTOR];
    uint64_t lba;
    uint32_t off;
    if (slot_locate(v, dir, slot, &lba, &off) != 0) return -1;
    if (dev_read(v, lba, 1, s) != 0) return -1;
    mem_copy(entry, s + off, 32);
    return 0;
}

static uint8_t short_checksum(const uint8_t *raw) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + raw[i]);
    return sum;
}

static uint32_t utf16_to_utf8(const uint16_t *in, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n && in[i]; i++) {
        uint32_t ch = in[i];
        if (ch < 0x80) {
            if (o + 1 >= cap) break;
            out[o++] = (char)ch;
        } else if (ch < 0x800) {
            if (o + 2 >= cap) break;
            out[o++] = (char)(0xC0 | (ch >> 6));
            out[o++] = (char)(0x80 | (ch & 0x3F));
        } else {
            if (o + 3 >= cap) break;
            out[o++] = (char)(0xE0 | (ch >> 12));
            out[o++] = (char)(0x80 | ((ch >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (ch & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

static uint32_t utf8_to_utf16(const char *in, uint16_t *out, uint32_t cap) {
    const uint8_t *s = (const uint8_t *)in;
    uint32_t n = 0;
    while (*s && n < cap) {
        uint32_t ch;
        if (s[0] < 0x80) {
            ch = *s++;
        } else if ((s[0] & 0xE0) == 0xC0 && s[1]) {
            ch = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            s += 2;
        } else if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
            ch = ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            s += 3;
        } else {
            ch = '_';
            s++;
        }
        out[n++] = (uint16_t)ch;
    }
    return n;
}

static void short_to_name(const uint8_t *raw, uint8_t ntres, int is_dir, char *out) {
    uint32_t o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        char c = (char)(i == 0 && raw[0] == 0x05 ? 0xE5 : raw[i]);
        out[o++] = (ntres & 0x08) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (raw[8] != ' ' && !(is_dir && raw[8] == ' ')) {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            char c = (char)raw[i];
            out[o++] = (ntres & 0x10) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    out[o] = 0;
}

int fatfs_iterate(fatfs_t *v, uint32_t dir, fatfs_iter_fn fn, void *ctx) {
    uint8_t s[SECTOR];
    uint16_t lfn[260];
    uint32_t lfn_total = 0, lfn_seen = 0, lfn_first = 0;
    uint8_t lfn_sum = 0;
    uint32_t c = dir, slot = 0, guard = v->cluster_count;
    fatfs_entry_t e;
    while (valid_cluster(v, c) && guard--) {
        for (uint32_t sec = 0; sec < v->sectors_per_cluster; sec++) {
            if (dev_read(v, cluster_lba(v, c) + sec, 1, s) != 0) return -1;
            for (uint32_t off = 0; off < SECTOR; off += 32, slot++) {
                uint8_t *d = s + off;
                if (d[0] == 0x00) return 0;
                if (d[0] == SLOT_FREE) {
                    lfn_total = 0;
                    continue;
                }
                if ((d[11] & 0x3F) == ATTR_LFN) {
                    uint32_t seq = d[0] & 0x1FU;
                    if (d[0] & 0x40) {
                        lfn_total = seq;
                        lfn_seen = 0;
                        lfn_sum = d[13];
                        lfn_first = slot;
                        for (int i = 0; i < 260; i++) lfn[i] = 0;
                    }
                    if (lfn_total && seq >= 1 && seq <= 20 && d[13] == lfn_sum) {
                        uint16_t *dst = lfn + (seq - 1) * LFN_CHARS;
                        static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                        for (int i = 0; i < 13; i++) {
                            uint16_t ch = (uint16_t)rd16(d + pos[i]);
                            dst[i] = ch == 0xFFFF ? 0 : ch;
                        }
                        lfn_seen++;
                    } else {
                        lfn_total = 0;
                    }
                    continue;
                }
                if (d[11] & ATTR_VOLUME) {
                    lfn_total = 0;
                    continue;
                }
                mem_zero(&e, sizeof(e));
                mem_copy(e.raw, d, 11);
                e.attr = d[11];
                e.cluster = (rd16(d + 20) << 16) | rd16(d + 26);
                e.size = rd32(d + 28);
                e.dir_cluster = dir;
                e.short_slot = slot;
                e.first_slot = slot;
                if (lfn_total && lfn_seen == lfn_total && lfn_sum == short_checksum(d)) {
                    utf16_to_utf8(lfn, lfn_total * LFN_CHARS, e.name, sizeof(e.name));
                    e.first_slot = lfn_first;
                } else {
                    short_to_name(d, d[12], (d[11] & FATFS_ATTR_DIR) != 0, e.name);
                }
                lfn_total = 0;
                if ((e.name[0] == '.' && e.name[1] == 0) || (e.name[0] == '.' && e.name[1] == '.' && e.name[2] == 0)) {
                    continue;
                }
                {
                    int r = fn(&e, ctx);
                    if (r) return r;
                }
            }
        }
        c = fat_get(v, c);
        if (c >= EOC_MIN) break;
    }
    return 0;
}

typedef struct {
    const char *name;
    uint32_t len;
    fatfs_entry_t *out;
} find_ctx_t;

static int find_cb(const fatfs_entry_t *e, void *p) {
    find_ctx_t *f = (find_ctx_t *)p;
    char tmp[FATFS_NAME_MAX];
    uint32_t i;
    for (i = 0; i < f->len && i + 1 < sizeof(tmp); i++) tmp[i] = f->name[i];
    tmp[i] = 0;
    if (!ci_eq(e->name, tmp)) return 0;
    mem_copy(f->out, e, sizeof(*e));
    return 1;
}

static int find_in_dir(fatfs_t *v, uint32_t dir, const char *name, uint32_t len, fatfs_entry_t *out) {
    find_ctx_t f = { name, len, out };
    return fatfs_iterate(v, dir, find_cb, &f) == 1 ? 0 : -1;
}

static void root_entry(fatfs_t *v, fatfs_entry_t *out) {
    mem_zero(out, sizeof(*out));
    out->attr = FATFS_ATTR_DIR;
    out->cluster = v->root_cluster;
    out->name[0] = '/';
}

int fatfs_lookup(fatfs_t *v, const char *path, fatfs_entry_t *out) {
    fatfs_entry_t cur;
    root_entry(v, &cur);
    while (*path) {
        uint32_t len = 0;
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        while (path[len] && path[len] != '/' && path[len] != '\\') len++;
        if (!(cur.attr & FATFS_ATTR_DIR)) return -1;
        if (find_in_dir(v, cur.cluster ? cur.cluster : v->root_cluster, path, len, &cur) != 0) return -1;
        path += len;
    }
    mem_copy(out, &cur, sizeof(cur));
    return 0;
}

static int valid_name(const char *n) {
    uint32_t len = 0;
    if (!n || !*n || (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2])))) return 0;
    for (; n[len]; len++) {
        char c = n[len];
        if ((uint8_t)c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            return 0;
        }
    }
    return len < 255;
}

static int short_char_ok(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return c == '$' || c == '%' || c == '\'' || c == '-' || c == '_' || c == '@' || c == '~' ||
           c == '`' || c == '!' || c == '(' || c == ')' || c == '{' || c == '}' || c == '^' ||
           c == '#' || c == '&';
}

static int exact_short(const char *name, uint8_t *raw) {
    uint32_t i = 0, j = 0;
    for (int k = 0; k < 11; k++) raw[k] = ' ';
    while (name[i] && name[i] != '.') {
        if (j >= 8 || !short_char_ok(name[i])) return 0;
        raw[j++] = (uint8_t)name[i++];
    }
    if (j == 0) return 0;
    if (name[i] == '.') {
        i++;
        j = 8;
        if (!name[i]) return 0;
        while (name[i]) {
            if (j >= 11 || !short_char_ok(name[i])) return 0;
            raw[j++] = (uint8_t)name[i++];
        }
    }
    return 1;
}

typedef struct {
    const uint8_t *raw;
} raw_ctx_t;

static int raw_cb(const fatfs_entry_t *e, void *p) {
    const uint8_t *raw = ((raw_ctx_t *)p)->raw;
    for (int i = 0; i < 11; i++) {
        if (e->raw[i] != raw[i]) return 0;
    }
    return 1;
}

static int make_alias(fatfs_t *v, uint32_t dir, const char *name, uint8_t *raw) {
    const char *dot = 0;
    char base[8], ext[3];
    uint32_t nb = 0, ne = 0;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    for (const char *p = name; *p && p != dot && nb < 6; p++) {
        char c = up(*p);
        if (c == ' ' || c == '.') continue;
        base[nb++] = short_char_ok(c) ? c : '_';
    }
    if (nb == 0) base[nb++] = '_';
    if (dot) {
        for (const char *p = dot + 1; *p && ne < 3; p++) {
            char c = up(*p);
            if (c == ' ') continue;
            ext[ne++] = short_char_ok(c) ? c : '_';
        }
    }
    for (uint32_t n = 1; n < 100000; n++) {
        char num[8];
        uint32_t nl = 0, t = n, keep;
        raw_ctx_t rc;
        while (t) {
            num[nl++] = (char)('0' + t % 10);
            t /= 10;
        }
        keep = 7 - nl < nb ? 7 - nl : nb;
        for (int k = 0; k < 11; k++) raw[k] = ' ';
        for (uint32_t k = 0; k < keep; k++) raw[k] = (uint8_t)base[k];
        raw[keep] = '~';
        for (uint32_t k = 0; k < nl; k++) raw[keep + 1 + k] = (uint8_t)num[nl - 1 - k];
        for (uint32_t k = 0; k < ne; k++) raw[8 + k] = (uint8_t)ext[k];
        rc.raw = raw;
        if (fatfs_iterate(v, dir, raw_cb, &rc) != 1) return 0;
    }
    return -1;
}

static void stamp(uint8_t *d) {
    rtc_time_t t;
    uint32_t date = (1U << 5) | 1U, time = 0;
    if (rtc_read(&t) == 0 && t.year >= 1980) {
        date = ((uint32_t)(t.year - 1980) << 9) | ((uint32_t)t.month << 5) | t.day;
        time = ((uint32_t)t.hour << 11) | ((uint32_t)t.minute << 5) | (t.second / 2U);
    }
    wr16(d + 14, time);
    wr16(d + 16, date);
    wr16(d + 18, date);
    wr16(d + 22, time);
    wr16(d + 24, date);
}

static int find_free_run(fatfs_t *v, uint32_t dir, uint32_t need, uint32_t *start) {
    uint8_t s[SECTOR];
    uint32_t per = v->cluster_bytes / 32U;
    for (int attempt = 0; attempt < 32; attempt++) {
        uint32_t c = dir, slot = 0, run = 0, run_start = 0, last = dir, guard = v->cluster_count;
        while (valid_cluster(v, c) && guard--) {
            for (uint32_t sec = 0; sec < v->sectors_per_cluster; sec++) {
                if (dev_read(v, cluster_lba(v, c) + sec, 1, s) != 0) return -1;
                for (uint32_t off = 0; off < SECTOR; off += 32, slot++) {
                    if (s[off] == 0x00 || s[off] == SLOT_FREE) {
                        if (!run) run_start = slot;
                        if (++run >= need) {
                            *start = run_start;
                            return 0;
                        }
                    } else {
                        run = 0;
                    }
                }
            }
            last = c;
            c = fat_get(v, c);
            if (c >= EOC_MIN) break;
        }
        {
            uint32_t fresh;
            if (alloc_one(v, &fresh) != 0) return -1;
            if (zero_cluster(v, fresh) != 0 || fat_set(v, last, fresh) != 0) return -1;
        }
        (void)per;
    }
    return -1;
}

static int create_entry(fatfs_t *v, uint32_t dir, const char *name, uint8_t attr, uint32_t cluster,
                        uint32_t size) {
    uint8_t raw[11], d[32];
    uint16_t u[260];
    uint32_t ulen = 0, lfn_n = 0, start;
    if (!valid_name(name)) return -1;
    if (!exact_short(name, raw)) {
        ulen = utf8_to_utf16(name, u, 255);
        lfn_n = (ulen + LFN_CHARS - 1) / LFN_CHARS;
        if (make_alias(v, dir, name, raw) != 0) return -1;
    }
    if (find_free_run(v, dir, lfn_n + 1, &start) != 0) return -1;
    for (uint32_t i = 0; i < lfn_n; i++) {
        uint32_t seq = lfn_n - i;
        static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        mem_zero(d, sizeof(d));
        d[0] = (uint8_t)(seq | (i == 0 ? 0x40 : 0));
        d[11] = ATTR_LFN;
        d[13] = short_checksum(raw);
        for (uint32_t k = 0; k < LFN_CHARS; k++) {
            uint32_t idx = (seq - 1) * LFN_CHARS + k;
            uint32_t ch = idx < ulen ? u[idx] : (idx == ulen ? 0 : 0xFFFF);
            wr16(d + pos[k], ch);
        }
        if (slot_write(v, dir, start + i, d) != 0) return -1;
    }
    mem_zero(d, sizeof(d));
    mem_copy(d, raw, 11);
    d[11] = attr;
    stamp(d);
    wr16(d + 20, cluster >> 16);
    wr16(d + 26, cluster & 0xFFFFU);
    wr32(d + 28, size);
    return slot_write(v, dir, start + lfn_n, d);
}

static int split_parent(fatfs_t *v, const char *path, fatfs_entry_t *parent, const char **leaf) {
    char buf[512];
    uint32_t n = 0, cut = 0;
    while (path[n] && n + 1 < sizeof(buf)) {
        buf[n] = path[n] == '\\' ? '/' : path[n];
        n++;
    }
    while (n > 0 && buf[n - 1] == '/') n--;
    buf[n] = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (buf[i] == '/') cut = i + 1;
    }
    *leaf = path + cut;
    if (!**leaf) return -1;
    buf[cut] = 0;
    if (fatfs_lookup(v, buf, parent) != 0 || !(parent->attr & FATFS_ATTR_DIR)) return -1;
    if (!parent->cluster) parent->cluster = v->root_cluster;
    return 0;
}

static uint32_t leaf_len(const char *leaf) {
    uint32_t n = 0;
    while (leaf[n] && leaf[n] != '/' && leaf[n] != '\\') n++;
    return n;
}

int fatfs_mkdir(fatfs_t *v, const char *path) {
    fatfs_entry_t cur, next;
    root_entry(v, &cur);
    while (*path) {
        char name[FATFS_NAME_MAX];
        uint32_t len = 0;
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        while (path[len] && path[len] != '/' && path[len] != '\\') len++;
        if (len + 1 > sizeof(name)) return -1;
        mem_copy(name, path, len);
        name[len] = 0;
        if (find_in_dir(v, cur.cluster, path, len, &next) == 0) {
            if (!(next.attr & FATFS_ATTR_DIR)) return -1;
        } else {
            uint32_t c;
            uint8_t d[32];
            if (alloc_one(v, &c) != 0 || zero_cluster(v, c) != 0) return -1;
            mem_zero(d, sizeof(d));
            for (int k = 0; k < 11; k++) d[k] = ' ';
            d[0] = '.';
            d[11] = FATFS_ATTR_DIR;
            stamp(d);
            wr16(d + 20, c >> 16);
            wr16(d + 26, c & 0xFFFFU);
            if (slot_write(v, c, 0, d) != 0) return -1;
            d[1] = '.';
            {
                uint32_t parent = cur.cluster == v->root_cluster ? 0 : cur.cluster;
                wr16(d + 20, parent >> 16);
                wr16(d + 26, parent & 0xFFFFU);
            }
            if (slot_write(v, c, 1, d) != 0) return -1;
            if (create_entry(v, cur.cluster, name, FATFS_ATTR_DIR, c, 0) != 0) {
                free_chain(v, c);
                return -1;
            }
            if (find_in_dir(v, cur.cluster, path, len, &next) != 0) return -1;
        }
        if (!next.cluster) next.cluster = v->root_cluster;
        cur = next;
        path += len;
    }
    return fatfs_flush(v);
}

int fatfs_write(fatfs_t *v, const char *path, const void *data, uint64_t size) {
    fatfs_entry_t parent, old;
    const char *leaf;
    uint32_t first = 0, len;
    int exists;
    if (size > 0xFFFFFFFFULL) return -1;
    if (split_parent(v, path, &parent, &leaf) != 0) return -1;
    len = leaf_len(leaf);
    exists = find_in_dir(v, parent.cluster, leaf, len, &old) == 0;
    if (exists && (old.attr & (FATFS_ATTR_DIR | FATFS_ATTR_RO))) return -1;
    if (size) {
        uint32_t clusters = (uint32_t)((size + v->cluster_bytes - 1) / v->cluster_bytes);
        if (alloc_chain(v, clusters, &first) != 0) return -1;
        if (fat_cache_flush(v) != 0 || write_chain(v, first, (const uint8_t *)data, size) != 0) {
            free_chain(v, first);
            fatfs_flush(v);
            return -1;
        }
    }
    if (exists) {
        uint8_t d[32];
        if (slot_read(v, parent.cluster, old.short_slot, d) != 0) return -1;
        wr16(d + 20, first >> 16);
        wr16(d + 26, first & 0xFFFFU);
        wr32(d + 28, (uint32_t)size);
        stamp(d);
        if (slot_write(v, parent.cluster, old.short_slot, d) != 0) return -1;
        if (old.cluster) free_chain(v, old.cluster);
    } else {
        char name[FATFS_NAME_MAX];
        if (len + 1 > sizeof(name)) return -1;
        mem_copy(name, leaf, len);
        name[len] = 0;
        if (create_entry(v, parent.cluster, name, 0x20, first, (uint32_t)size) != 0) {
            if (first) free_chain(v, first);
            fatfs_flush(v);
            return -1;
        }
    }
    return fatfs_flush(v);
}

int fatfs_read_entry(fatfs_t *v, const fatfs_entry_t *e, char **data_out) {
    char *buf;
    uint8_t *tmp;
    uint32_t c = e->cluster;
    uint64_t done = 0;
    buf = (char *)kmalloc((uint64_t)e->size + 1);
    if (!buf) return -1;
    tmp = (uint8_t *)kmalloc(v->cluster_bytes);
    if (!tmp) {
        kfree(buf);
        return -1;
    }
    while (done < e->size && valid_cluster(v, c)) {
        uint64_t take = e->size - done < v->cluster_bytes ? e->size - done : v->cluster_bytes;
        if (dev_read(v, cluster_lba(v, c), v->sectors_per_cluster, tmp) != 0) break;
        mem_copy(buf + done, tmp, take);
        done += take;
        c = fat_get(v, c);
    }
    kfree(tmp);
    if (done < e->size) {
        kfree(buf);
        return -1;
    }
    buf[e->size] = 0;
    *data_out = buf;
    return 0;
}

int fatfs_read(fatfs_t *v, const char *path, char **data_out, uint64_t *size_out) {
    fatfs_entry_t e;
    if (fatfs_lookup(v, path, &e) != 0 || (e.attr & FATFS_ATTR_DIR)) return -1;
    if (fatfs_read_entry(v, &e, data_out) != 0) return -1;
    if (size_out) *size_out = e.size;
    return 0;
}

typedef struct {
    char   **names;
    uint32_t count;
    uint32_t cap;
} list_ctx_t;

static int list_cb(const fatfs_entry_t *e, void *p) {
    list_ctx_t *l = (list_ctx_t *)p;
    uint32_t n = 0;
    char *copy;
    if (l->count >= l->cap) return 2;
    while (e->name[n]) n++;
    copy = (char *)kmalloc(n + 1);
    if (!copy) return 2;
    mem_copy(copy, e->name, n + 1);
    l->names[l->count++] = copy;
    return 0;
}

static int remove_entry(fatfs_t *v, const char *path, int depth) {
    fatfs_entry_t e;
    uint8_t d[32];
    if (depth > 32 || fatfs_lookup(v, path, &e) != 0 || e.name[0] == '/') return -1;
    if (e.attr & FATFS_ATTR_DIR) {
        list_ctx_t l;
        char child[512];
        int rc = 0;
        l.cap = 1024;
        l.count = 0;
        l.names = (char **)kmalloc(sizeof(char *) * l.cap);
        if (!l.names) return -1;
        if (fatfs_iterate(v, e.cluster, list_cb, &l) == 2) rc = -1;
        for (uint32_t i = 0; i < l.count; i++) {
            uint32_t n = 0, k = 0;
            while (path[n] && n + 1 < sizeof(child)) {
                child[n] = path[n];
                n++;
            }
            if (n + 1 < sizeof(child)) child[n++] = '/';
            while (l.names[i][k] && n + 1 < sizeof(child)) child[n++] = l.names[i][k++];
            child[n] = 0;
            if (rc == 0 && remove_entry(v, child, depth + 1) != 0) rc = -1;
            kfree(l.names[i]);
        }
        kfree(l.names);
        if (rc != 0) return -1;
        if (fatfs_lookup(v, path, &e) != 0) return -1;
    }
    for (uint32_t s = e.first_slot; s <= e.short_slot; s++) {
        if (slot_read(v, e.dir_cluster, s, d) != 0) return -1;
        d[0] = SLOT_FREE;
        if (slot_write(v, e.dir_cluster, s, d) != 0) return -1;
    }
    if (e.cluster) free_chain(v, e.cluster);
    return 0;
}

int fatfs_remove(fatfs_t *v, const char *path) {
    int rc = remove_entry(v, path, 0);
    if (fatfs_flush(v) != 0) return -1;
    return rc;
}

int fatfs_usage(fatfs_t *v, uint32_t *free_clusters, uint32_t *highest_used) {
    uint32_t free_n = 0, high = 0;
    for (uint32_t c = 2; c < v->cluster_count + 2; c++) {
        if (fat_get(v, c) == 0) free_n++;
        else high = c;
    }
    if (free_clusters) *free_clusters = free_n;
    if (highest_used) *highest_used = high;
    return 0;
}

int64_t fatfs_read_range(fatfs_t *v, const fatfs_entry_t *e, uint64_t off, void *buf, uint64_t len,
                         fatfs_hint_t *hint) {
    uint8_t *tmp;
    uint8_t *out = (uint8_t *)buf;
    uint64_t done = 0, pos = 0;
    uint32_t c = e->cluster;
    if (off >= e->size) return 0;
    if (len > e->size - off) len = e->size - off;
    if (hint && hint->cluster && hint->first == e->cluster && hint->off <= off) {
        pos = hint->off;
        c = hint->cluster;
    }
    while (valid_cluster(v, c) && pos + v->cluster_bytes <= off) {
        c = fat_get(v, c);
        pos += v->cluster_bytes;
    }
    tmp = (uint8_t *)kmalloc(v->cluster_bytes);
    if (!tmp) return -1;
    while (done < len && valid_cluster(v, c)) {
        uint64_t in = off + done - pos;
        uint64_t take = v->cluster_bytes - in;
        if (take > len - done) take = len - done;
        if (dev_read(v, cluster_lba(v, c), v->sectors_per_cluster, tmp) != 0) break;
        mem_copy(out + done, tmp + in, take);
        done += take;
        if (in + take == v->cluster_bytes) {
            if (hint) {
                hint->first = e->cluster;
                hint->cluster = c;
                hint->off = pos;
            }
            c = fat_get(v, c);
            pos += v->cluster_bytes;
        }
    }
    if (hint && done && valid_cluster(v, c)) {
        hint->first = e->cluster;
        hint->cluster = c;
        hint->off = pos;
    }
    kfree(tmp);
    return (int64_t)done;
}

static struct {
    uint32_t first;
    uint32_t idx;
    uint32_t cluster;
} seek_hint;

static uint32_t chain_seek(fatfs_t *v, uint32_t first, uint32_t idx) {
    uint32_t c = first, i = 0;
    if (seek_hint.first == first && seek_hint.cluster && seek_hint.idx <= idx) {
        c = seek_hint.cluster;
        i = seek_hint.idx;
    }
    while (i < idx && valid_cluster(v, c)) {
        c = fat_get(v, c);
        i++;
    }
    if (valid_cluster(v, c)) {
        seek_hint.first = first;
        seek_hint.idx = idx;
        seek_hint.cluster = c;
    }
    return c;
}

static int write_range(fatfs_t *v, uint32_t first, uint64_t pos, const uint8_t *src, uint64_t len) {
    uint8_t *tmp = (uint8_t *)kmalloc(v->cluster_bytes);
    int rc = 0;
    if (!tmp) return -1;
    while (len && rc == 0) {
        uint32_t c = chain_seek(v, first, (uint32_t)(pos / v->cluster_bytes));
        uint64_t in = pos % v->cluster_bytes;
        uint64_t take = v->cluster_bytes - in;
        if (take > len) take = len;
        if (!valid_cluster(v, c)) {
            rc = -1;
            break;
        }
        if (in != 0 || take != v->cluster_bytes) {
            if (dev_read(v, cluster_lba(v, c), v->sectors_per_cluster, tmp) != 0) {
                rc = -1;
                break;
            }
        }
        if (src) mem_copy(tmp + in, src, take);
        else mem_zero(tmp + in, take);
        if (dev_write(v, cluster_lba(v, c), v->sectors_per_cluster, tmp) != 0) rc = -1;
        pos += take;
        len -= take;
        if (src) src += take;
    }
    kfree(tmp);
    return rc;
}

static int update_dirent(fatfs_t *v, const fatfs_entry_t *e, uint32_t first, uint32_t size) {
    uint8_t d[32];
    if (slot_read(v, e->dir_cluster, e->short_slot, d) != 0) return -1;
    wr16(d + 20, first >> 16);
    wr16(d + 26, first & 0xFFFFU);
    wr32(d + 28, size);
    stamp(d);
    return slot_write(v, e->dir_cluster, e->short_slot, d);
}

static int grow_to(fatfs_t *v, fatfs_entry_t *e, uint64_t new_size) {
    uint32_t have = e->cluster ? (uint32_t)((e->size + v->cluster_bytes - 1) / v->cluster_bytes) : 0;
    uint32_t need = (uint32_t)((new_size + v->cluster_bytes - 1) / v->cluster_bytes);
    uint32_t fresh;
    if (e->cluster && have == 0) have = 1;
    if (need <= have) return 0;
    if (alloc_chain(v, need - have, &fresh) != 0) return -1;
    if (!e->cluster) {
        e->cluster = fresh;
    } else {
        uint32_t last = chain_seek(v, e->cluster, have - 1);
        if (!valid_cluster(v, last) || fat_set(v, last, fresh) != 0) return -1;
    }
    return fat_cache_flush(v);
}

int fatfs_write_at(fatfs_t *v, const char *path, uint64_t off, const void *data, uint64_t len) {
    fatfs_entry_t e;
    uint64_t new_size;
    int rc;
    seek_hint.cluster = 0;
    if (fatfs_lookup(v, path, &e) != 0 || (e.attr & (FATFS_ATTR_DIR | FATFS_ATTR_RO))) return -1;
    new_size = off + len > e.size ? off + len : e.size;
    if (new_size > 0xFFFFFFFFULL) return -1;
    if (grow_to(v, &e, new_size) != 0) {
        fatfs_flush(v);
        return -1;
    }
    rc = 0;
    if (off > e.size) rc = write_range(v, e.cluster, e.size, 0, off - e.size);
    if (rc == 0 && len) rc = write_range(v, e.cluster, off, (const uint8_t *)data, len);
    if (rc == 0) rc = update_dirent(v, &e, e.cluster, (uint32_t)new_size);
    if (fatfs_flush(v) != 0) rc = -1;
    return rc;
}

int fatfs_truncate(fatfs_t *v, const char *path, uint64_t len) {
    fatfs_entry_t e;
    int rc = 0;
    if (fatfs_lookup(v, path, &e) != 0 || (e.attr & (FATFS_ATTR_DIR | FATFS_ATTR_RO))) return -1;
    if (len > 0xFFFFFFFFULL) return -1;
    if (len == e.size) return 0;
    seek_hint.cluster = 0;
    if (len > e.size) {
        uint64_t old = e.size;
        if (grow_to(v, &e, len) != 0) rc = -1;
        if (rc == 0) rc = write_range(v, e.cluster, old, 0, len - old);
    } else {
        uint32_t keep = (uint32_t)((len + v->cluster_bytes - 1) / v->cluster_bytes);
        if (keep == 0) {
            if (e.cluster) free_chain(v, e.cluster);
            e.cluster = 0;
        } else if (e.cluster) {
            uint32_t last = chain_seek(v, e.cluster, keep - 1);
            uint32_t next = valid_cluster(v, last) ? fat_get(v, last) : EOC;
            if (valid_cluster(v, last)) fat_set(v, last, EOC);
            if (valid_cluster(v, next)) free_chain(v, next);
        }
        seek_hint.cluster = 0;
    }
    if (rc == 0) rc = update_dirent(v, &e, e.cluster, (uint32_t)len);
    if (fatfs_flush(v) != 0) rc = -1;
    return rc;
}

/* Renames or moves an entry within the volume: a new directory entry takes
 * over the same cluster chain and the old slots are released.  A moved
 * folder gets its ".." entry pointed at the new parent. */
int fatfs_rename(fatfs_t *v, const char *from, const char *to) {
    fatfs_entry_t e, parent, existing;
    const char *leaf;
    char name[FATFS_NAME_MAX];
    uint32_t len;
    uint8_t d[32];
    if (fatfs_lookup(v, from, &e) != 0 || e.name[0] == '/') return -1;
    if (split_parent(v, to, &parent, &leaf) != 0) return -1;
    len = leaf_len(leaf);
    if (!len || len + 1 > sizeof(name)) return -1;
    mem_copy(name, leaf, len);
    name[len] = 0;
    if (find_in_dir(v, parent.cluster, leaf, len, &existing) == 0) {
        if ((existing.attr & FATFS_ATTR_DIR) || (e.attr & FATFS_ATTR_DIR)) return -1;
        if (remove_entry(v, to, 0) != 0) return -1;
        if (fatfs_lookup(v, from, &e) != 0) return -1;
    }
    if (create_entry(v, parent.cluster, name, e.attr, e.cluster, e.size) != 0) {
        fatfs_flush(v);
        return -1;
    }
    for (uint32_t s = e.first_slot; s <= e.short_slot; s++) {
        if (slot_read(v, e.dir_cluster, s, d) != 0) return -1;
        d[0] = SLOT_FREE;
        if (slot_write(v, e.dir_cluster, s, d) != 0) return -1;
    }
    if ((e.attr & FATFS_ATTR_DIR) && e.cluster && e.dir_cluster != parent.cluster &&
        slot_read(v, e.cluster, 1, d) == 0 && d[0] == '.' && d[1] == '.') {
        uint32_t up = parent.cluster == v->root_cluster ? 0 : parent.cluster;
        wr16(d + 20, up >> 16);
        wr16(d + 26, up & 0xFFFFU);
        slot_write(v, e.cluster, 1, d);
    }
    return fatfs_flush(v);
}
