#include "exfatfs.h"
#include "../memory/heap.h"
#include "../drivers/rtc/rtc.h"

#define SECTOR       512U
#define EOC          0xFFFFFFFFU
#define ENT_EOD      0x00
#define ENT_BITMAP   0x81
#define ENT_UPCASE   0x82
#define ENT_FILE     0x85
#define ENT_STREAM   0xC0
#define ENT_NAME     0xC1
#define FLAG_ALLOC   0x01
#define FLAG_NOFAT   0x02
#define NAME_PER_ENT 15U

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
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v & 0xFFFFU); wr16(p + 2, v >> 16); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

static int dev_read(exfat_t *v, uint64_t rel, uint32_t n, void *buf) {
    if (rel + n > v->sectors) return -1;
    return v->dev->read(v->dev->context, v->base_lba + rel, n, buf);
}

static int dev_write(exfat_t *v, uint64_t rel, uint32_t n, const void *buf) {
    if (!v->writable || !v->dev->write || rel + n > v->sectors) return -1;
    return v->dev->write(v->dev->context, v->base_lba + rel, n, buf);
}

static int valid_cluster(const exfat_t *v, uint32_t c) {
    return c >= 2 && c < v->cluster_count + 2;
}

static uint64_t cluster_lba(const exfat_t *v, uint32_t c) {
    return v->heap_offset + (uint64_t)(c - 2) * v->sectors_per_cluster;
}

static int fat_flush(exfat_t *v) {
    if (!v->fat_dirty) return 0;
    if (dev_write(v, v->fat_offset + (uint64_t)v->fat_cache_sector, 1, v->fat_cache) != 0) return -1;
    v->fat_dirty = 0;
    return 0;
}

static int fat_load(exfat_t *v, uint32_t sector) {
    if (v->fat_cache_sector == (int64_t)sector) return 0;
    if (fat_flush(v) != 0) return -1;
    if (dev_read(v, v->fat_offset + sector, 1, v->fat_cache) != 0) {
        v->fat_cache_sector = -1;
        return -1;
    }
    v->fat_cache_sector = sector;
    return 0;
}

static uint32_t fat_get(exfat_t *v, uint32_t c) {
    if (fat_load(v, (c * 4U) / SECTOR) != 0) return EOC;
    return rd32(v->fat_cache + (c * 4U) % SECTOR);
}

static int fat_set(exfat_t *v, uint32_t c, uint32_t value) {
    if (fat_load(v, (c * 4U) / SECTOR) != 0) return -1;
    wr32(v->fat_cache + (c * 4U) % SECTOR, value);
    v->fat_dirty = 1;
    return 0;
}

static int bmp_flush(exfat_t *v) {
    if (!v->bmp_dirty) return 0;
    if (dev_write(v, cluster_lba(v, v->bitmap_cluster) + (uint64_t)v->bmp_cache_sector, 1, v->bmp_cache) != 0) {
        return -1;
    }
    v->bmp_dirty = 0;
    return 0;
}

static int bmp_load(exfat_t *v, uint32_t sector) {
    if (v->bmp_cache_sector == (int64_t)sector) return 0;
    if (bmp_flush(v) != 0) return -1;
    if (dev_read(v, cluster_lba(v, v->bitmap_cluster) + sector, 1, v->bmp_cache) != 0) {
        v->bmp_cache_sector = -1;
        return -1;
    }
    v->bmp_cache_sector = sector;
    return 0;
}

static int bmp_get(exfat_t *v, uint32_t c) {
    uint32_t bit = c - 2;
    if (bmp_load(v, bit / (SECTOR * 8U)) != 0) return 1;
    return (v->bmp_cache[(bit / 8U) % SECTOR] >> (bit % 8U)) & 1;
}

static int bmp_set(exfat_t *v, uint32_t c, int used) {
    uint32_t bit = c - 2;
    uint8_t *b;
    if (bmp_load(v, bit / (SECTOR * 8U)) != 0) return -1;
    b = &v->bmp_cache[(bit / 8U) % SECTOR];
    if (used) *b = (uint8_t)(*b | (1U << (bit % 8U)));
    else *b = (uint8_t)(*b & ~(1U << (bit % 8U)));
    v->bmp_dirty = 1;
    return 0;
}

int exfat_flush(exfat_t *v) {
    if (fat_flush(v) != 0) return -1;
    return bmp_flush(v);
}

static uint32_t boot_checksum(const uint8_t *region) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 11U * SECTOR; i++) {
        if (i == 106 || i == 107 || i == 112) continue;
        sum = ((sum & 1U) ? 0x80000000U : 0U) + (sum >> 1) + region[i];
    }
    return sum;
}

static uint16_t set_checksum(const uint8_t *set, uint32_t count) {
    uint16_t sum = 0;
    for (uint32_t i = 0; i < count * 32U; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum & 1U) ? 0x8000U : 0U) + (sum >> 1) + set[i]);
    }
    return sum;
}

static uint16_t up_char(const exfat_t *v, uint16_t c) {
    if (v->upcase) return v->upcase[c];
    if (c >= 'a' && c <= 'z') return (uint16_t)(c - 32);
    return c;
}

static uint16_t name_hash(const exfat_t *v, const uint16_t *name, uint32_t len) {
    uint16_t hash = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint16_t c = up_char(v, name[i]);
        hash = (uint16_t)(((hash & 1U) ? 0x8000U : 0U) + (hash >> 1) + (c & 0xFFU));
        hash = (uint16_t)(((hash & 1U) ? 0x8000U : 0U) + (hash >> 1) + (c >> 8));
    }
    return hash;
}

static uint32_t utf8_to_utf16(const char *in, uint32_t in_len, uint16_t *out, uint32_t cap) {
    const uint8_t *s = (const uint8_t *)in;
    const uint8_t *end = s + in_len;
    uint32_t n = 0;
    while (s < end && *s && n < cap) {
        uint32_t ch;
        if (s[0] < 0x80) {
            ch = *s++;
        } else if ((s[0] & 0xE0) == 0xC0 && s + 1 < end) {
            ch = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            s += 2;
        } else if ((s[0] & 0xF0) == 0xE0 && s + 2 < end) {
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

static void utf16_to_utf8(const uint16_t *in, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
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
}

static uint32_t exfat_time(void) {
    rtc_time_t t;
    if (rtc_read(&t) != 0 || t.year < 1980) return (1U << 21) | (1U << 16);
    return ((uint32_t)(t.year - 1980) << 25) | ((uint32_t)t.month << 21) | ((uint32_t)t.day << 16) |
           ((uint32_t)t.hour << 11) | ((uint32_t)t.minute << 5) | (t.second / 2U);
}

void exfat_root_dir(const exfat_t *v, exfat_dir_t *out) {
    mem_zero(out, sizeof(*out));
    out->cluster = v->root_cluster;
    out->is_root = 1;
}

void exfat_entry_dir(const exfat_entry_t *e, exfat_dir_t *out) {
    mem_zero(out, sizeof(*out));
    out->cluster = e->cluster;
    out->nofat = e->nofat;
    out->size = e->size;
    out->parent_cluster = e->dir.cluster;
    out->parent_nofat = e->dir.nofat;
    out->parent_size = e->dir.size;
    out->parent_is_root = e->dir.is_root;
    out->set_slot = e->set_slot;
}

static void parent_dir(const exfat_dir_t *d, exfat_dir_t *out) {
    mem_zero(out, sizeof(*out));
    out->cluster = d->parent_cluster;
    out->nofat = d->parent_nofat;
    out->size = d->parent_size;
    out->is_root = d->parent_is_root;
}

typedef struct {
    const exfat_dir_t *dir;
    uint32_t ci;
    uint32_t c;
    int64_t  lba;
    uint8_t  sec[SECTOR];
} cursor_t;

static void cursor_init(cursor_t *k, const exfat_dir_t *dir) {
    k->dir = dir;
    k->ci = 0;
    k->c = dir->cluster;
    k->lba = -1;
}

static uint32_t cursor_cluster(exfat_t *v, cursor_t *k, uint32_t idx) {
    const exfat_dir_t *d = k->dir;
    if (d->nofat) {
        if (!d->is_root && (uint64_t)(idx + 1) * v->cluster_bytes > d->size) return 0;
        return d->cluster + idx;
    }
    if (idx < k->ci) {
        k->ci = 0;
        k->c = d->cluster;
    }
    while (k->ci < idx) {
        if (!valid_cluster(v, k->c)) return 0;
        k->c = fat_get(v, k->c);
        k->ci++;
    }
    return valid_cluster(v, k->c) ? k->c : 0;
}

static int slot_rw(exfat_t *v, cursor_t *k, uint32_t slot, uint8_t *ent, int write) {
    uint32_t per = v->cluster_bytes / 32U;
    uint32_t c = cursor_cluster(v, k, slot / per);
    uint64_t lba;
    uint32_t off;
    if (!c) return -1;
    lba = cluster_lba(v, c) + ((slot % per) * 32U) / SECTOR;
    off = ((slot % per) * 32U) % SECTOR;
    if (k->lba != (int64_t)lba) {
        if (dev_read(v, lba, 1, k->sec) != 0) return -1;
        k->lba = (int64_t)lba;
    }
    if (!write) {
        mem_copy(ent, k->sec + off, 32);
        return 0;
    }
    mem_copy(k->sec + off, ent, 32);
    return dev_write(v, lba, 1, k->sec);
}

static int read_set(exfat_t *v, cursor_t *k, uint32_t slot, uint8_t *set, uint32_t *count) {
    uint32_t n;
    if (slot_rw(v, k, slot, set, 0) != 0 || set[0] != ENT_FILE) return -1;
    n = (uint32_t)set[1] + 1U;
    if (n < 3 || n > 19) return -1;
    for (uint32_t i = 1; i < n; i++) {
        if (slot_rw(v, k, slot + i, set + i * 32U, 0) != 0) return -1;
    }
    *count = n;
    return 0;
}

static void decode_set(exfat_t *v, const uint8_t *set, uint32_t count, exfat_entry_t *e) {
    uint16_t name[EXFATFS_NAME_MAX];
    uint32_t len = set[32 + 3], got = 0;
    (void)v;
    e->attr = (uint16_t)rd16(set + 4);
    e->nofat = (set[32 + 1] & FLAG_NOFAT) != 0;
    e->valid_size = rd64(set + 32 + 8);
    e->cluster = rd32(set + 32 + 20);
    e->size = rd64(set + 32 + 24);
    e->set_count = count;
    for (uint32_t i = 2; i < count && got < len; i++) {
        const uint8_t *ne = set + i * 32U;
        if (ne[0] != ENT_NAME) break;
        for (uint32_t j = 0; j < NAME_PER_ENT && got < len; j++) name[got++] = (uint16_t)rd16(ne + 2 + j * 2);
    }
    utf16_to_utf8(name, got, e->name, sizeof(e->name));
}

int exfat_iterate(exfat_t *v, const exfat_dir_t *dir, exfat_iter_fn fn, void *ctx) {
    cursor_t *k = (cursor_t *)kmalloc(sizeof(cursor_t));
    uint8_t *set = (uint8_t *)kmalloc(19 * 32);
    exfat_entry_t *e = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
    int rc = 0;
    if (!k || !set || !e) {
        rc = -1;
        goto out;
    }
    cursor_init(k, dir);
    for (uint32_t slot = 0; slot < 0x200000U; slot++) {
        uint8_t ent[32];
        uint32_t count;
        if (slot_rw(v, k, slot, ent, 0) != 0 || ent[0] == ENT_EOD) break;
        if (ent[0] != ENT_FILE) continue;
        if (read_set(v, k, slot, set, &count) != 0 || set[32] != ENT_STREAM) continue;
        mem_zero(e, sizeof(*e));
        decode_set(v, set, count, e);
        e->set_slot = slot;
        e->dir = *dir;
        rc = fn(e, ctx);
        if (rc) break;
        slot += count - 1;
    }
out:
    if (k) kfree(k);
    if (set) kfree(set);
    if (e) kfree(e);
    return rc;
}

typedef struct {
    exfat_t       *v;
    const char    *name;
    uint32_t       len;
    exfat_entry_t *out;
} find_ctx_t;

static int name_equal(exfat_t *v, const char *a, uint32_t alen, const char *b) {
    uint16_t x[EXFATFS_NAME_MAX], y[EXFATFS_NAME_MAX];
    uint32_t nx = utf8_to_utf16(a, alen, x, EXFATFS_NAME_MAX);
    uint32_t blen = 0, ny;
    while (b[blen]) blen++;
    ny = utf8_to_utf16(b, blen, y, EXFATFS_NAME_MAX);
    if (nx != ny) return 0;
    for (uint32_t i = 0; i < nx; i++) {
        if (up_char(v, x[i]) != up_char(v, y[i])) return 0;
    }
    return 1;
}

static int find_cb(const exfat_entry_t *e, void *p) {
    find_ctx_t *f = (find_ctx_t *)p;
    if (!name_equal(f->v, f->name, f->len, e->name)) return 0;
    mem_copy(f->out, e, sizeof(*e));
    return 1;
}

static int find_in_dir(exfat_t *v, const exfat_dir_t *dir, const char *name, uint32_t len, exfat_entry_t *out) {
    find_ctx_t f;
    f.v = v;
    f.name = name;
    f.len = len;
    f.out = out;
    return exfat_iterate(v, dir, find_cb, &f) == 1 ? 0 : -1;
}

int exfat_lookup(exfat_t *v, const char *path, exfat_entry_t *out) {
    exfat_dir_t dir;
    exfat_root_dir(v, &dir);
    mem_zero(out, sizeof(*out));
    out->attr = EXFATFS_ATTR_DIR;
    out->cluster = v->root_cluster;
    out->name[0] = '/';
    out->dir = dir;
    while (*path) {
        uint32_t len = 0;
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        while (path[len] && path[len] != '/' && path[len] != '\\') len++;
        if (!(out->attr & EXFATFS_ATTR_DIR)) return -1;
        if (out->name[0] != '/' || out->name[1]) exfat_entry_dir(out, &dir);
        if (find_in_dir(v, &dir, path, len, out) != 0) return -1;
        path += len;
    }
    return 0;
}

static int lookup_dir(exfat_t *v, const char *path, exfat_dir_t *dir) {
    exfat_entry_t *e = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
    int rc = -1;
    if (!e) return -1;
    if (exfat_lookup(v, path, e) == 0 && (e->attr & EXFATFS_ATTR_DIR)) {
        if (e->name[0] == '/' && !e->name[1]) exfat_root_dir(v, dir);
        else exfat_entry_dir(e, dir);
        rc = 0;
    }
    kfree(e);
    return rc;
}

static uint32_t chain_cluster(exfat_t *v, uint32_t first, int nofat, uint32_t idx) {
    uint32_t c = first;
    if (nofat) return first + idx;
    for (uint32_t i = 0; i < idx && valid_cluster(v, c); i++) c = fat_get(v, c);
    return c;
}

int64_t exfat_read_range(exfat_t *v, const exfat_entry_t *e, uint64_t off, void *buf, uint64_t len,
                         exfat_hint_t *hint) {
    uint8_t *tmp, *out = (uint8_t *)buf;
    uint64_t done = 0, limit = e->valid_size < e->size ? e->valid_size : e->size;
    uint32_t idx, c;
    if (off >= e->size) return 0;
    if (len > e->size - off) len = e->size - off;
    idx = (uint32_t)(off / v->cluster_bytes);
    if (!e->nofat && hint && hint->first == e->cluster && hint->cluster && hint->off <= off &&
        (uint32_t)(hint->off / v->cluster_bytes) <= idx) {
        c = hint->cluster;
        for (uint32_t i = (uint32_t)(hint->off / v->cluster_bytes); i < idx && valid_cluster(v, c); i++) {
            c = fat_get(v, c);
        }
    } else {
        c = chain_cluster(v, e->cluster, e->nofat, idx);
    }
    tmp = (uint8_t *)kmalloc(v->cluster_bytes);
    if (!tmp) return -1;
    while (done < len && valid_cluster(v, c)) {
        uint64_t pos = off + done, in = pos % v->cluster_bytes;
        uint64_t take = v->cluster_bytes - in;
        if (take > len - done) take = len - done;
        if (pos >= limit) {
            mem_zero(out + done, take);
        } else if (dev_read(v, cluster_lba(v, c), v->sectors_per_cluster, tmp) != 0) {
            break;
        } else {
            mem_copy(out + done, tmp + in, take);
            if (pos + take > limit) mem_zero(out + done + (limit - pos), pos + take - limit);
        }
        done += take;
        if (hint) {
            hint->first = e->cluster;
            hint->cluster = c;
            hint->off = (pos / v->cluster_bytes) * v->cluster_bytes;
        }
        if (in + take == v->cluster_bytes) {
            idx++;
            c = e->nofat ? e->cluster + idx : fat_get(v, c);
        }
    }
    kfree(tmp);
    return (int64_t)done;
}

static int alloc_clusters(exfat_t *v, uint32_t count, uint32_t *first, int *nofat) {
    uint32_t total = v->cluster_count, run = 0, start = 0;
    uint32_t c = valid_cluster(v, v->next_free) ? v->next_free : 2;
    for (uint32_t n = 0; n < total + count && run < count; n++) {
        if (!valid_cluster(v, c)) {
            c = 2;
            run = 0;
        }
        if (!bmp_get(v, c)) {
            if (!run) start = c;
            run++;
        } else {
            run = 0;
        }
        c++;
    }
    if (run >= count) {
        for (uint32_t i = 0; i < count; i++) {
            if (bmp_set(v, start + i, 1) != 0) return -1;
        }
        *first = start;
        *nofat = 1;
        v->next_free = start + count;
        return 0;
    }
    {
        uint32_t prev = 0, got = 0;
        c = 2;
        for (uint32_t n = 0; n < total && got < count; n++, c++) {
            if (bmp_get(v, c)) continue;
            if (bmp_set(v, c, 1) != 0) return -1;
            if (prev) fat_set(v, prev, c);
            else *first = c;
            prev = c;
            got++;
        }
        if (got < count) {
            uint32_t k = *first;
            for (uint32_t i = 0; i < got && valid_cluster(v, k); i++) {
                uint32_t nx = fat_get(v, k);
                bmp_set(v, k, 0);
                k = nx;
            }
            return -1;
        }
        fat_set(v, prev, EOC);
        *nofat = 0;
    }
    return 0;
}

static void free_clusters(exfat_t *v, uint32_t first, int nofat, uint64_t size) {
    uint32_t n = (uint32_t)((size + v->cluster_bytes - 1) / v->cluster_bytes);
    uint32_t c = first;
    if (!valid_cluster(v, first)) return;
    for (uint32_t i = 0; i < n && valid_cluster(v, c); i++) {
        uint32_t next = nofat ? c + 1 : fat_get(v, c);
        bmp_set(v, c, 0);
        if (!nofat) fat_set(v, c, 0);
        if (c < v->next_free) v->next_free = c;
        c = next;
    }
}

static int zero_cluster(exfat_t *v, uint32_t c) {
    uint8_t z[SECTOR];
    mem_zero(z, sizeof(z));
    for (uint32_t i = 0; i < v->sectors_per_cluster; i++) {
        if (dev_write(v, cluster_lba(v, c) + i, 1, z) != 0) return -1;
    }
    return 0;
}

static int write_data(exfat_t *v, uint32_t first, int nofat, const uint8_t *data, uint64_t size) {
    uint8_t *buf = (uint8_t *)kmalloc(v->cluster_bytes);
    uint32_t c = first;
    uint64_t done = 0;
    if (!buf) return -1;
    while (done < size && valid_cluster(v, c)) {
        uint64_t take = size - done < v->cluster_bytes ? size - done : v->cluster_bytes;
        mem_zero(buf, v->cluster_bytes);
        mem_copy(buf, data + done, take);
        if (dev_write(v, cluster_lba(v, c), v->sectors_per_cluster, buf) != 0) break;
        done += take;
        c = nofat ? c + 1 : fat_get(v, c);
    }
    kfree(buf);
    return done >= size ? 0 : -1;
}

static int update_set(exfat_t *v, const exfat_dir_t *dir, uint32_t slot, uint32_t first, int nofat,
                      uint64_t size) {
    cursor_t *k = (cursor_t *)kmalloc(sizeof(cursor_t));
    uint8_t *set = (uint8_t *)kmalloc(19 * 32);
    uint32_t count;
    int rc = -1;
    if (!k || !set) goto out;
    cursor_init(k, dir);
    if (read_set(v, k, slot, set, &count) != 0) goto out;
    set[32 + 1] = (uint8_t)(FLAG_ALLOC | (nofat ? FLAG_NOFAT : 0));
    wr64(set + 32 + 8, size);
    wr32(set + 32 + 20, first);
    wr64(set + 32 + 24, size);
    wr32(set + 12, exfat_time());
    wr32(set + 16, exfat_time());
    wr16(set + 2, set_checksum(set, count));
    rc = 0;
    for (uint32_t i = 0; i < count && rc == 0; i++) rc = slot_rw(v, k, slot + i, set + i * 32U, 1);
out:
    if (k) kfree(k);
    if (set) kfree(set);
    return rc;
}

static int extend_dir(exfat_t *v, exfat_dir_t *dir) {
    uint32_t used = (uint32_t)(dir->size / v->cluster_bytes), last, fresh;
    int nofat;
    if (dir->is_root) {
        uint32_t c = dir->cluster;
        while (valid_cluster(v, c)) {
            uint32_t nx = fat_get(v, c);
            if (!valid_cluster(v, nx)) break;
            c = nx;
        }
        if (alloc_clusters(v, 1, &fresh, &nofat) != 0) return -1;
        fat_set(v, fresh, EOC);
        fat_set(v, c, fresh);
        return zero_cluster(v, fresh);
    }
    if (used == 0) return -1;
    last = chain_cluster(v, dir->cluster, dir->nofat, used - 1);
    if (dir->nofat && valid_cluster(v, last + 1) && !bmp_get(v, last + 1)) {
        fresh = last + 1;
        bmp_set(v, fresh, 1);
    } else {
        if (dir->nofat) {
            for (uint32_t i = 0; i < used; i++) fat_set(v, dir->cluster + i, i + 1 < used ? dir->cluster + i + 1 : EOC);
            dir->nofat = 0;
        }
        if (alloc_clusters(v, 1, &fresh, &nofat) != 0) return -1;
        fat_set(v, fresh, EOC);
        fat_set(v, last, fresh);
    }
    if (zero_cluster(v, fresh) != 0) return -1;
    dir->size += v->cluster_bytes;
    {
        exfat_dir_t parent;
        parent_dir(dir, &parent);
        return update_set(v, &parent, dir->set_slot, dir->cluster, dir->nofat, dir->size);
    }
}

static int find_free_run(exfat_t *v, exfat_dir_t *dir, uint32_t need, uint32_t *start) {
    for (int attempt = 0; attempt < 8; attempt++) {
        cursor_t *k = (cursor_t *)kmalloc(sizeof(cursor_t));
        uint32_t run = 0, run_start = 0, slot;
        uint8_t ent[32];
        if (!k) return -1;
        cursor_init(k, dir);
        for (slot = 0; slot_rw(v, k, slot, ent, 0) == 0; slot++) {
            if (ent[0] == ENT_EOD || !(ent[0] & 0x80)) {
                if (!run) run_start = slot;
                if (++run >= need) {
                    kfree(k);
                    *start = run_start;
                    return 0;
                }
            } else {
                run = 0;
            }
        }
        kfree(k);
        if (extend_dir(v, dir) != 0) return -1;
    }
    return -1;
}

static int create_set(exfat_t *v, exfat_dir_t *dir, const char *name, uint32_t name_len, uint16_t attr,
                      uint32_t first, int nofat, uint64_t size) {
    uint16_t uname[EXFATFS_NAME_MAX];
    uint32_t ulen = utf8_to_utf16(name, name_len, uname, 255), names, count, start;
    uint8_t *set;
    cursor_t *k;
    int rc = 0;
    uint32_t now = exfat_time();
    if (ulen == 0) return -1;
    for (uint32_t i = 0; i < ulen; i++) {
        uint16_t ch = uname[i];
        if (ch < 32 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|') {
            return -1;
        }
    }
    names = (ulen + NAME_PER_ENT - 1) / NAME_PER_ENT;
    count = 2 + names;
    if (find_free_run(v, dir, count, &start) != 0) return -1;
    set = (uint8_t *)kmalloc(count * 32U);
    k = (cursor_t *)kmalloc(sizeof(cursor_t));
    if (!set || !k) {
        if (set) kfree(set);
        if (k) kfree(k);
        return -1;
    }
    mem_zero(set, count * 32U);
    set[0] = ENT_FILE;
    set[1] = (uint8_t)(count - 1);
    wr16(set + 4, attr);
    wr32(set + 8, now);
    wr32(set + 12, now);
    wr32(set + 16, now);
    set[32] = ENT_STREAM;
    set[32 + 1] = (uint8_t)(FLAG_ALLOC | (nofat ? FLAG_NOFAT : 0));
    set[32 + 3] = (uint8_t)ulen;
    wr16(set + 32 + 4, name_hash(v, uname, ulen));
    wr64(set + 32 + 8, size);
    wr32(set + 32 + 20, first);
    wr64(set + 32 + 24, size);
    for (uint32_t i = 0; i < names; i++) {
        uint8_t *ne = set + (2 + i) * 32U;
        ne[0] = ENT_NAME;
        for (uint32_t j = 0; j < NAME_PER_ENT; j++) {
            uint32_t idx = i * NAME_PER_ENT + j;
            wr16(ne + 2 + j * 2, idx < ulen ? uname[idx] : 0);
        }
    }
    wr16(set + 2, set_checksum(set, count));
    cursor_init(k, dir);
    for (uint32_t i = 0; i < count && rc == 0; i++) rc = slot_rw(v, k, start + i, set + i * 32U, 1);
    kfree(set);
    kfree(k);
    return rc;
}

static int split_path(exfat_t *v, const char *path, exfat_dir_t *parent, const char **leaf, uint32_t *leaf_len) {
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
    *leaf_len = n - cut;
    if (*leaf_len == 0) return -1;
    buf[cut] = 0;
    return lookup_dir(v, buf, parent);
}

int exfat_write(exfat_t *v, const char *path, const void *data, uint64_t size) {
    exfat_dir_t parent;
    exfat_entry_t *old;
    const char *leaf;
    uint32_t leaf_len, first = 0;
    int nofat = 1, exists, rc = -1;
    if (!v->writable || split_path(v, path, &parent, &leaf, &leaf_len) != 0) return -1;
    old = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
    if (!old) return -1;
    exists = find_in_dir(v, &parent, leaf, leaf_len, old) == 0;
    if (exists && (old->attr & (EXFATFS_ATTR_DIR | EXFATFS_ATTR_RO))) goto out;
    if (size) {
        uint32_t n = (uint32_t)((size + v->cluster_bytes - 1) / v->cluster_bytes);
        if (alloc_clusters(v, n, &first, &nofat) != 0) goto out;
        if (exfat_flush(v) != 0 || write_data(v, first, nofat, (const uint8_t *)data, size) != 0) {
            free_clusters(v, first, nofat, size);
            exfat_flush(v);
            goto out;
        }
    }
    if (exists) {
        rc = update_set(v, &parent, old->set_slot, first, nofat, size);
        if (rc == 0 && old->size) free_clusters(v, old->cluster, old->nofat, old->size);
    } else {
        rc = create_set(v, &parent, leaf, leaf_len, 0x20, first, nofat, size);
        if (rc != 0 && size) free_clusters(v, first, nofat, size);
    }
out:
    kfree(old);
    if (exfat_flush(v) != 0) rc = -1;
    return rc;
}

int exfat_mkdir(exfat_t *v, const char *path) {
    exfat_dir_t dir;
    exfat_entry_t *e = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
    int rc = 0;
    if (!e) return -1;
    if (!v->writable) {
        kfree(e);
        return -1;
    }
    exfat_root_dir(v, &dir);
    while (*path && rc == 0) {
        uint32_t len = 0;
        while (*path == '/' || *path == '\\') path++;
        if (!*path) break;
        while (path[len] && path[len] != '/' && path[len] != '\\') len++;
        if (find_in_dir(v, &dir, path, len, e) == 0) {
            if (!(e->attr & EXFATFS_ATTR_DIR)) rc = -1;
        } else {
            uint32_t c;
            int nofat;
            if (alloc_clusters(v, 1, &c, &nofat) != 0 || zero_cluster(v, c) != 0 ||
                create_set(v, &dir, path, len, EXFATFS_ATTR_DIR, c, 1, v->cluster_bytes) != 0 ||
                find_in_dir(v, &dir, path, len, e) != 0) {
                rc = -1;
            }
        }
        if (rc == 0) exfat_entry_dir(e, &dir);
        path += len;
    }
    kfree(e);
    if (exfat_flush(v) != 0) rc = -1;
    return rc;
}

typedef struct {
    char   **names;
    uint32_t count;
    uint32_t cap;
} list_ctx_t;

static int list_cb(const exfat_entry_t *e, void *p) {
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

static int remove_path(exfat_t *v, const char *path, int depth) {
    exfat_entry_t *e = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
    cursor_t *k = 0;
    int rc = -1;
    if (!e) return -1;
    if (depth > 32 || exfat_lookup(v, path, e) != 0 || (e->name[0] == '/' && !e->name[1])) goto out;
    if (e->attr & EXFATFS_ATTR_DIR) {
        exfat_dir_t dir;
        list_ctx_t l;
        char *child = (char *)kmalloc(512);
        int crc = 0;
        l.cap = 1024;
        l.count = 0;
        l.names = (char **)kmalloc(sizeof(char *) * l.cap);
        if (!l.names || !child) {
            if (l.names) kfree(l.names);
            if (child) kfree(child);
            goto out;
        }
        exfat_entry_dir(e, &dir);
        if (exfat_iterate(v, &dir, list_cb, &l) == 2) crc = -1;
        for (uint32_t i = 0; i < l.count; i++) {
            uint32_t n = 0, m = 0;
            while (path[n] && n + 1 < 512) {
                child[n] = path[n];
                n++;
            }
            if (n + 1 < 512) child[n++] = '/';
            while (l.names[i][m] && n + 1 < 512) child[n++] = l.names[i][m++];
            child[n] = 0;
            if (crc == 0 && remove_path(v, child, depth + 1) != 0) crc = -1;
            kfree(l.names[i]);
        }
        kfree(l.names);
        kfree(child);
        if (crc != 0 || exfat_lookup(v, path, e) != 0) goto out;
    }
    k = (cursor_t *)kmalloc(sizeof(cursor_t));
    if (!k) goto out;
    cursor_init(k, &e->dir);
    rc = 0;
    for (uint32_t i = 0; i < e->set_count && rc == 0; i++) {
        uint8_t ent[32];
        rc = slot_rw(v, k, e->set_slot + i, ent, 0);
        if (rc == 0) {
            ent[0] = (uint8_t)(ent[0] & 0x7F);
            rc = slot_rw(v, k, e->set_slot + i, ent, 1);
        }
    }
    if (rc == 0 && e->size && e->cluster) free_clusters(v, e->cluster, e->nofat, e->size);
out:
    if (k) kfree(k);
    kfree(e);
    return rc;
}

int exfat_remove(exfat_t *v, const char *path) {
    int rc;
    if (!v->writable) return -1;
    rc = remove_path(v, path, 0);
    if (exfat_flush(v) != 0) return -1;
    return rc;
}

int exfat_usage(exfat_t *v, uint32_t *free_clusters_out) {
    uint32_t n = 0;
    for (uint32_t c = 2; c < v->cluster_count + 2; c++) {
        if (!bmp_get(v, c)) n++;
    }
    *free_clusters_out = n;
    return 0;
}

int exfat_load_upcase(exfat_t *v, uint16_t **table_out) {
    uint16_t *table = (uint16_t *)kmalloc(65536U * 2U);
    uint8_t *raw;
    uint64_t pos = 0;
    uint32_t idx = 0, c = v->upcase_cluster;
    if (!table) return -1;
    for (uint32_t i = 0; i < 65536U; i++) table[i] = (uint16_t)i;
    raw = (uint8_t *)kmalloc(v->cluster_bytes);
    if (!raw) {
        kfree(table);
        return -1;
    }
    while (pos < v->upcase_bytes && valid_cluster(v, c) && idx < 65536U) {
        uint64_t take = v->upcase_bytes - pos < v->cluster_bytes ? v->upcase_bytes - pos : v->cluster_bytes;
        if (dev_read(v, cluster_lba(v, c), v->sectors_per_cluster, raw) != 0) break;
        for (uint64_t i = 0; i + 1 < take && idx < 65536U; i += 2) {
            uint16_t ch = (uint16_t)rd16(raw + i);
            if (ch == 0xFFFF && i + 3 < take) {
                idx += rd16(raw + i + 2);
                i += 2;
            } else {
                table[idx++] = ch;
            }
        }
        pos += take;
        c = fat_get(v, c);
    }
    kfree(raw);
    *table_out = table;
    return 0;
}

int exfat_mount(exfat_t *v, block_device_t *dev, uint64_t start_lba, uint64_t sector_count) {
    uint8_t *region;
    uint32_t sum;
    int ok = 0;
    if (!v || !dev || dev->sector_size != SECTOR) return -1;
    mem_zero(v, sizeof(*v));
    v->dev = dev;
    v->base_lba = start_lba;
    v->sectors = sector_count;
    v->fat_cache_sector = -1;
    v->bmp_cache_sector = -1;
    region = (uint8_t *)kmalloc(12U * SECTOR);
    if (!region) return -1;
    if (dev_read(v, 0, 12, region) == 0 && region[3] == 'E' && region[4] == 'X' && region[5] == 'F' &&
        region[6] == 'A' && region[7] == 'T' && region[108] == 9 && region[109] <= 16) {
        sum = boot_checksum(region);
        ok = rd32(region + 11U * SECTOR) == sum;
        if (ok) {
            v->sectors_per_cluster = 1U << region[109];
            v->cluster_bytes = v->sectors_per_cluster * SECTOR;
            v->fat_offset = rd32(region + 80);
            v->fat_length = rd32(region + 84);
            v->heap_offset = rd32(region + 88);
            v->cluster_count = rd32(region + 92);
            v->root_cluster = rd32(region + 96);
            v->writable = region[110] == 1 && dev->write != 0;
            if (rd64(region + 72) < v->sectors) v->sectors = rd64(region + 72);
            ok = valid_cluster(v, v->root_cluster) && v->heap_offset < v->sectors;
        }
    }
    kfree(region);
    if (!ok) return -1;
    {
        exfat_dir_t root;
        cursor_t *k = (cursor_t *)kmalloc(sizeof(cursor_t));
        uint8_t ent[32];
        if (!k) return -1;
        exfat_root_dir(v, &root);
        cursor_init(k, &root);
        for (uint32_t slot = 0; slot < 4096 && slot_rw(v, k, slot, ent, 0) == 0 && ent[0] != ENT_EOD; slot++) {
            if (ent[0] == ENT_BITMAP && !(ent[1] & 1)) {
                v->bitmap_cluster = rd32(ent + 20);
                v->bitmap_bytes = rd64(ent + 24);
            } else if (ent[0] == ENT_UPCASE) {
                v->upcase_cluster = rd32(ent + 20);
                v->upcase_bytes = rd64(ent + 24);
            }
        }
        kfree(k);
    }
    if (!valid_cluster(v, v->bitmap_cluster) || v->bitmap_bytes * 8U < v->cluster_count) v->writable = 0;
    if (v->writable) {
        uint32_t need = (uint32_t)((v->bitmap_bytes + v->cluster_bytes - 1) / v->cluster_bytes);
        uint32_t c = v->bitmap_cluster;
        for (uint32_t i = 0; i + 1 < need && v->writable; i++) {
            uint32_t nx = fat_get(v, c);
            if (nx != c + 1 && nx != 0) v->writable = 0;
            c = c + 1;
        }
    }
    v->next_free = 2;
    return 0;
}

static int link_run(exfat_t *v, uint32_t start, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (fat_set(v, start + i, i + 1 < count ? start + i + 1 : EOC) != 0) return -1;
    }
    return 0;
}

static uint32_t clusters_for(const exfat_t *v, uint64_t size) {
    return (uint32_t)((size + v->cluster_bytes - 1) / v->cluster_bytes);
}

static int grow_entry(exfat_t *v, exfat_entry_t *e, uint64_t new_size) {
    uint32_t have = e->cluster ? clusters_for(v, e->size) : 0;
    uint32_t need = clusters_for(v, new_size), extra, fresh = 0, last;
    int fresh_nofat = 1, contiguous = 1;
    if (e->cluster && have == 0) have = 1;
    if (need <= have) return 0;
    extra = need - have;
    if (!e->cluster) {
        if (alloc_clusters(v, extra, &fresh, &fresh_nofat) != 0) return -1;
        e->cluster = fresh;
        e->nofat = fresh_nofat;
        return exfat_flush(v);
    }
    if (e->nofat) {
        for (uint32_t i = 0; i < extra && contiguous; i++) {
            uint32_t c = e->cluster + have + i;
            if (!valid_cluster(v, c) || bmp_get(v, c)) contiguous = 0;
        }
        if (contiguous) {
            for (uint32_t i = 0; i < extra; i++) {
                if (bmp_set(v, e->cluster + have + i, 1) != 0) return -1;
            }
            return exfat_flush(v);
        }
    }
    if (alloc_clusters(v, extra, &fresh, &fresh_nofat) != 0) return -1;
    if (fresh_nofat && link_run(v, fresh, extra) != 0) return -1;
    if (e->nofat) {
        if (link_run(v, e->cluster, have) != 0) return -1;
        e->nofat = 0;
    }
    last = chain_cluster(v, e->cluster, 0, have - 1);
    if (!valid_cluster(v, last) || fat_set(v, last, fresh) != 0) return -1;
    return exfat_flush(v);
}

static int write_span(exfat_t *v, const exfat_entry_t *e, uint64_t pos, const uint8_t *src, uint64_t len) {
    uint8_t *tmp;
    uint32_t idx = (uint32_t)(pos / v->cluster_bytes);
    uint32_t c = chain_cluster(v, e->cluster, e->nofat, idx);
    int rc = 0;
    if (!len) return 0;
    tmp = (uint8_t *)kmalloc(v->cluster_bytes);
    if (!tmp) return -1;
    while (len && rc == 0) {
        uint64_t in = pos % v->cluster_bytes;
        uint64_t take = v->cluster_bytes - in;
        if (take > len) take = len;
        if (!valid_cluster(v, c)) {
            rc = -1;
            break;
        }
        if ((in != 0 || take != v->cluster_bytes) &&
            dev_read(v, cluster_lba(v, c), v->sectors_per_cluster, tmp) != 0) {
            rc = -1;
            break;
        }
        if (src) mem_copy(tmp + in, src, take);
        else mem_zero(tmp + in, take);
        if (dev_write(v, cluster_lba(v, c), v->sectors_per_cluster, tmp) != 0) rc = -1;
        pos += take;
        len -= take;
        if (src) src += take;
        idx++;
        c = e->nofat ? e->cluster + idx : fat_get(v, c);
    }
    kfree(tmp);
    return rc;
}

static int zero_between(exfat_t *v, const exfat_entry_t *e, uint64_t from, uint64_t to) {
    return to > from ? write_span(v, e, from, 0, to - from) : 0;
}

int exfat_write_at(exfat_t *v, const char *path, uint64_t off, const void *data, uint64_t len) {
    exfat_entry_t *e;
    uint64_t new_size, valid, end = off + len;
    int rc = -1;
    if (!v->writable || end < off) return -1;
    e = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
    if (!e) return -1;
    if (exfat_lookup(v, path, e) != 0 || (e->attr & (EXFATFS_ATTR_DIR | EXFATFS_ATTR_RO))) goto out;
    valid = e->valid_size < e->size ? e->valid_size : e->size;
    new_size = end > e->size ? end : e->size;
    if (grow_entry(v, e, new_size) != 0) goto out;
    rc = zero_between(v, e, valid, off);
    if (rc == 0) rc = write_span(v, e, off, (const uint8_t *)data, len);
    if (rc == 0) rc = zero_between(v, e, end > valid ? end : valid, e->size);
    if (rc == 0) rc = update_set(v, &e->dir, e->set_slot, e->cluster, e->nofat, new_size);
out:
    kfree(e);
    if (exfat_flush(v) != 0) rc = -1;
    return rc;
}

int exfat_truncate(exfat_t *v, const char *path, uint64_t len) {
    exfat_entry_t *e;
    uint64_t valid;
    int rc = -1;
    if (!v->writable) return -1;
    e = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
    if (!e) return -1;
    if (exfat_lookup(v, path, e) != 0 || (e->attr & (EXFATFS_ATTR_DIR | EXFATFS_ATTR_RO))) goto out;
    valid = e->valid_size < e->size ? e->valid_size : e->size;
    if (len > e->size) {
        if (grow_entry(v, e, len) != 0) goto out;
        rc = zero_between(v, e, valid, len);
    } else {
        uint32_t have = e->cluster ? clusters_for(v, e->size) : 0;
        uint32_t keep = clusters_for(v, len);
        rc = 0;
        if (keep == 0) {
            if (e->cluster) free_clusters(v, e->cluster, e->nofat, e->size);
            e->cluster = 0;
            e->nofat = 1;
        } else if (keep < have && e->nofat) {
            free_clusters(v, e->cluster + keep, 1, (uint64_t)(have - keep) * v->cluster_bytes);
        } else if (keep < have) {
            uint32_t last = chain_cluster(v, e->cluster, 0, keep - 1);
            uint32_t next = valid_cluster(v, last) ? fat_get(v, last) : EOC;
            if (valid_cluster(v, last)) fat_set(v, last, EOC);
            if (valid_cluster(v, next)) free_clusters(v, next, 0, (uint64_t)(have - keep) * v->cluster_bytes);
        }
        if (keep && valid < len) rc = zero_between(v, e, valid, len);
    }
    if (rc == 0) rc = update_set(v, &e->dir, e->set_slot, e->cluster, e->nofat, len);
out:
    kfree(e);
    if (exfat_flush(v) != 0) rc = -1;
    return rc;
}
