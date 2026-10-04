#include "ntfsfs.h"
#include "../memory/heap.h"

#define DEV_SECTOR      512U
#define ATTR_LIST       0x20U
#define ATTR_FILE_NAME  0x30U
#define ATTR_DATA       0x80U
#define ATTR_INDEX_ROOT 0x90U
#define ATTR_INDEX_ALLOC 0xA0U
#define ATTR_END        0xFFFFFFFFU
#define FLAG_COMPRESSED 0x0001U
#define FLAG_ENCRYPTED  0x4000U
#define REF_MASK        0x0000FFFFFFFFFFFFULL

static const uint16_t name_i30[4] = { '$', 'I', '3', '0' };

typedef struct {
    int         resident;
    uint8_t    *data;
    uint64_t    size;
    uint64_t    init_size;
    uint16_t    flags;
    ntfs_run_t *runs;
    uint32_t    nruns;
    int         found;
} attr_data_t;

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

static int dev_read_bytes(ntfs_t *v, uint64_t off, void *buf, uint64_t len) {
    uint8_t *out = (uint8_t *)buf;
    uint8_t sec[DEV_SECTOR];
    while (len) {
        uint64_t lba = off / DEV_SECTOR, in = off % DEV_SECTOR;
        if (in == 0 && len >= DEV_SECTOR) {
            uint64_t n = len / DEV_SECTOR;
            if (n > 128) n = 128;
            if (lba + n > v->sectors) return -1;
            if (v->dev->read(v->dev->context, v->base_lba + lba, (uint32_t)n, out) != 0) return -1;
            out += n * DEV_SECTOR;
            off += n * DEV_SECTOR;
            len -= n * DEV_SECTOR;
        } else {
            uint64_t take = DEV_SECTOR - in;
            if (take > len) take = len;
            if (lba >= v->sectors || v->dev->read(v->dev->context, v->base_lba + lba, 1, sec) != 0) return -1;
            mem_copy(out, sec + in, take);
            out += take;
            off += take;
            len -= take;
        }
    }
    return 0;
}

static int stream_read(ntfs_t *v, const ntfs_run_t *runs, uint32_t nruns, uint64_t off, void *buf, uint64_t len) {
    uint8_t *out = (uint8_t *)buf;
    while (len) {
        uint64_t vcn = off / v->cluster_bytes, in = off % v->cluster_bytes;
        uint64_t take = v->cluster_bytes - in;
        const ntfs_run_t *r = 0;
        if (take > len) take = len;
        for (uint32_t i = 0; i < nruns; i++) {
            if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].len) {
                r = &runs[i];
                break;
            }
        }
        if (!r) return -1;
        {
            uint64_t avail = (r->vcn + r->len - vcn) * v->cluster_bytes - in;
            if (take < avail && len > take) take = avail < len ? avail : len;
        }
        if (r->sparse) {
            mem_zero(out, take);
        } else if (dev_read_bytes(v, (r->lcn + (vcn - r->vcn)) * v->cluster_bytes + in, out, take) != 0) {
            return -1;
        }
        out += take;
        off += take;
        len -= take;
    }
    return 0;
}

static int apply_fixups(uint8_t *buf, uint32_t size, uint32_t sector) {
    uint32_t usa = rd16(buf + 4), count = rd16(buf + 6);
    uint32_t usn;
    if (count == 0 || usa + count * 2U > size) return -1;
    usn = rd16(buf + usa);
    for (uint32_t i = 1; i < count; i++) {
        uint32_t pos = i * sector - 2U;
        if (pos + 2U > size) break;
        if (rd16(buf + pos) != usn) return -1;
        buf[pos] = buf[usa + i * 2U];
        buf[pos + 1] = buf[usa + i * 2U + 1];
    }
    return 0;
}

static int read_record(ntfs_t *v, uint64_t rec, uint8_t *buf) {
    if ((rec + 1) * v->record_bytes > v->mft_size) return -1;
    if (stream_read(v, v->mft_runs, v->mft_run_count, rec * v->record_bytes, buf, v->record_bytes) != 0) return -1;
    if (buf[0] != 'F' || buf[1] != 'I' || buf[2] != 'L' || buf[3] != 'E') return -1;
    if (apply_fixups(buf, v->record_bytes, v->bytes_per_sector) != 0) return -1;
    return (rd16(buf + 22) & 1U) ? 0 : -1;
}

static int parse_runs(const uint8_t *attr, uint32_t attr_len, ntfs_run_t *runs, uint32_t *n, uint32_t cap) {
    uint32_t off = rd16(attr + 32);
    uint64_t vcn = rd64(attr + 16);
    int64_t lcn = 0;
    while (off < attr_len && attr[off]) {
        uint32_t hdr = attr[off], lsz = hdr & 0xFU, osz = hdr >> 4;
        uint64_t length = 0;
        int64_t delta = 0;
        if (lsz == 0 || lsz > 8 || osz > 8 || off + 1 + lsz + osz > attr_len) return -1;
        for (uint32_t i = 0; i < lsz; i++) length |= (uint64_t)attr[off + 1 + i] << (8 * i);
        for (uint32_t i = 0; i < osz; i++) delta |= (int64_t)((uint64_t)attr[off + 1 + lsz + i] << (8 * i));
        if (osz && (attr[off + lsz + osz] & 0x80)) delta |= (int64_t)(~0ULL << (8 * osz));
        if (*n >= cap) return -1;
        runs[*n].vcn = vcn;
        runs[*n].len = length;
        runs[*n].sparse = osz == 0;
        if (osz) {
            lcn += delta;
            runs[*n].lcn = (uint64_t)lcn;
        } else {
            runs[*n].lcn = 0;
        }
        (*n)++;
        vcn += length;
        off += 1 + lsz + osz;
    }
    return 0;
}

static int attr_name_is(const uint8_t *attr, const uint16_t *name, uint32_t name_len) {
    uint32_t len = attr[9], off = rd16(attr + 10);
    if (len != name_len) return 0;
    for (uint32_t i = 0; i < len; i++) {
        if (rd16(attr + off + i * 2) != name[i]) return 0;
    }
    return 1;
}

static void take_attr(const uint8_t *a, attr_data_t *out) {
    uint32_t alen = rd32(a + 4);
    if (!a[8]) {
        uint32_t vlen = rd32(a + 16), voff = rd16(a + 20);
        if (out->found || voff + vlen > alen) return;
        out->resident = 1;
        out->data = (uint8_t *)kmalloc(vlen + 1);
        if (!out->data) return;
        mem_copy(out->data, a + voff, vlen);
        out->size = vlen;
        out->init_size = vlen;
        out->flags = (uint16_t)rd16(a + 12);
        out->found = 1;
        return;
    }
    if (!out->runs) {
        out->runs = (ntfs_run_t *)kmalloc(sizeof(ntfs_run_t) * NTFS_RUNS_MAX);
        if (!out->runs) return;
    }
    if (rd64(a + 16) == 0) {
        out->size = rd64(a + 48);
        out->init_size = rd64(a + 56);
        out->flags = (uint16_t)rd16(a + 12);
    }
    (void)parse_runs(a, alen, out->runs, &out->nruns, NTFS_RUNS_MAX);
    out->found = 1;
}

static void scan_record(const ntfs_t *v, const uint8_t *rec, uint32_t type, const uint16_t *name, uint32_t name_len,
                        attr_data_t *out) {
    uint32_t off = rd16(rec + 20);
    while (off + 16 <= v->record_bytes) {
        uint32_t t = rd32(rec + off), len = rd32(rec + off + 4);
        if (t == ATTR_END || len < 16 || off + len > v->record_bytes) break;
        if (t == type && attr_name_is(rec + off, name, name_len)) take_attr(rec + off, out);
        off += len;
    }
}

static void free_attr(attr_data_t *a) {
    if (a->data) kfree(a->data);
    if (a->runs) kfree(a->runs);
    mem_zero(a, sizeof(*a));
}

static int get_attr(ntfs_t *v, uint64_t record, uint32_t type, const uint16_t *name, uint32_t name_len,
                    attr_data_t *out) {
    uint8_t *rec = (uint8_t *)kmalloc(v->record_bytes);
    attr_data_t list;
    int rc = -1;
    mem_zero(out, sizeof(*out));
    mem_zero(&list, sizeof(list));
    if (!rec || read_record(v, record, rec) != 0) goto out;
    scan_record(v, rec, ATTR_LIST, 0, 0, &list);
    if (!list.found) {
        scan_record(v, rec, type, name, name_len, out);
        rc = out->found ? 0 : -1;
        goto out;
    }
    {
        uint8_t *raw = list.data;
        uint64_t size = list.size;
        uint64_t last = ~0ULL;
        uint8_t *ext = (uint8_t *)kmalloc(v->record_bytes);
        if (!list.resident) {
            raw = (uint8_t *)kmalloc(size + 1);
            if (!raw || stream_read(v, list.runs, list.nruns, 0, raw, size) != 0) {
                if (raw) kfree(raw);
                raw = 0;
            }
        }
        for (uint64_t p = 0; raw && ext && p + 26 <= size;) {
            uint32_t et = rd32(raw + p), elen = rd16(raw + p + 4), nlen = raw[p + 6], noff = raw[p + 7];
            uint64_t seg = rd64(raw + p + 16) & REF_MASK;
            int match = et == type && nlen == name_len;
            if (elen < 26) break;
            for (uint32_t i = 0; match && i < nlen; i++) match = rd16(raw + p + noff + i * 2) == name[i];
            if (match && seg != last) {
                const uint8_t *src = seg == record ? rec : 0;
                if (!src && read_record(v, seg, ext) == 0) src = ext;
                if (src) scan_record(v, src, type, name, name_len, out);
                last = seg;
            }
            p += elen;
        }
        if (raw && raw != list.data) kfree(raw);
        if (ext) kfree(ext);
        rc = out->found ? 0 : -1;
    }
out:
    free_attr(&list);
    if (rec) kfree(rec);
    return rc;
}

int ntfs_mount(ntfs_t *v, block_device_t *dev, uint64_t start_lba, uint64_t sector_count) {
    uint8_t boot[DEV_SECTOR];
    uint8_t *rec;
    int32_t cpr, cpi;
    uint32_t spc;
    if (!v || !dev || dev->sector_size != DEV_SECTOR) return -1;
    mem_zero(v, sizeof(*v));
    v->dev = dev;
    v->base_lba = start_lba;
    v->sectors = sector_count;
    if (dev->read(dev->context, start_lba, 1, boot) != 0) return -1;
    if (boot[3] != 'N' || boot[4] != 'T' || boot[5] != 'F' || boot[6] != 'S') return -1;
    v->bytes_per_sector = rd16(boot + 11);
    if (v->bytes_per_sector < 512 || (v->bytes_per_sector & (v->bytes_per_sector - 1))) return -1;
    spc = boot[13];
    spc = spc > 0x80 ? 1U << (256U - spc) : spc;
    if (spc == 0) return -1;
    v->cluster_bytes = v->bytes_per_sector * spc;
    cpr = (int8_t)boot[64];
    cpi = (int8_t)boot[68];
    v->record_bytes = cpr > 0 ? (uint32_t)cpr * v->cluster_bytes : 1U << (-cpr);
    v->index_bytes = cpi > 0 ? (uint32_t)cpi * v->cluster_bytes : 1U << (-cpi);
    if (v->record_bytes < 512 || v->record_bytes > 65536 || v->index_bytes < 512 || v->index_bytes > 65536) return -1;
    rec = (uint8_t *)kmalloc(v->record_bytes);
    if (!rec) return -1;
    if (dev_read_bytes(v, rd64(boot + 48) * v->cluster_bytes, rec, v->record_bytes) != 0 ||
        apply_fixups(rec, v->record_bytes, v->bytes_per_sector) != 0) {
        kfree(rec);
        return -1;
    }
    {
        attr_data_t data;
        mem_zero(&data, sizeof(data));
        scan_record(v, rec, ATTR_DATA, 0, 0, &data);
        if (data.found && !data.resident && data.nruns) {
            mem_copy(v->mft_runs, data.runs, sizeof(ntfs_run_t) * data.nruns);
            v->mft_run_count = data.nruns;
            v->mft_size = data.size;
        }
        free_attr(&data);
    }
    kfree(rec);
    return v->mft_run_count ? 0 : -1;
}

static void name_to_utf8(const uint8_t *p, uint32_t n, char *out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ch = rd16(p + i * 2);
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

static int walk_entries(const uint8_t *start, const uint8_t *end, ntfs_dir_fn fn, void *ctx) {
    const uint8_t *e = start;
    char name[256];
    while (e + 16 <= end) {
        uint32_t len = rd16(e + 8), key = rd16(e + 10), flags = rd16(e + 12);
        if (flags & 2U) break;
        if (len < 16 || e + len > end) break;
        if (key >= 0x42) {
            const uint8_t *f = e + 16;
            uint32_t nlen = f[0x40], ns = f[0x41];
            uint64_t ref = rd64(e) & REF_MASK;
            if (ns != 2 && 0x42U + nlen * 2U <= key) {
                name_to_utf8(f + 0x42, nlen, name, sizeof(name));
                if (name[0] && name[0] != '$' && !(name[0] == '.' && !name[1])) {
                    int rc = fn(name, ref, (rd32(f + 0x38) & 0x10000000U) != 0, rd64(f + 0x30), ctx);
                    if (rc) return rc;
                }
            }
        }
        e += len;
    }
    return 0;
}

int ntfs_list_dir(ntfs_t *v, uint64_t record, ntfs_dir_fn fn, void *ctx) {
    attr_data_t root, alloc;
    int rc = 0;
    if (get_attr(v, record, ATTR_INDEX_ROOT, name_i30, 4, &root) != 0 || !root.resident || root.size < 32) {
        free_attr(&root);
        return -1;
    }
    {
        const uint8_t *node = root.data + 16;
        const uint8_t *end = node + rd32(node + 4);
        if (end > root.data + root.size) end = root.data + root.size;
        rc = walk_entries(node + rd32(node), end, fn, ctx);
    }
    free_attr(&root);
    if (rc) return rc;
    if (get_attr(v, record, ATTR_INDEX_ALLOC, name_i30, 4, &alloc) == 0 && !alloc.resident) {
        uint8_t *blk = (uint8_t *)kmalloc(v->index_bytes);
        for (uint64_t off = 0; blk && off + v->index_bytes <= alloc.size && rc == 0; off += v->index_bytes) {
            const uint8_t *node;
            if (stream_read(v, alloc.runs, alloc.nruns, off, blk, v->index_bytes) != 0) continue;
            if (blk[0] != 'I' || blk[1] != 'N' || blk[2] != 'D' || blk[3] != 'X') continue;
            if (apply_fixups(blk, v->index_bytes, v->bytes_per_sector) != 0) continue;
            node = blk + 0x18;
            {
                const uint8_t *end = node + rd32(node + 4);
                if (end > blk + v->index_bytes) end = blk + v->index_bytes;
                rc = walk_entries(node + rd32(node), end, fn, ctx);
            }
        }
        if (blk) kfree(blk);
    }
    free_attr(&alloc);
    return rc;
}

int64_t ntfs_file_size(ntfs_t *v, uint64_t record) {
    attr_data_t data;
    int64_t size = -1;
    if (get_attr(v, record, ATTR_DATA, 0, 0, &data) == 0) size = (int64_t)data.size;
    free_attr(&data);
    return size;
}

int64_t ntfs_read_file(ntfs_t *v, uint64_t record, uint64_t off, void *buf, uint64_t len) {
    attr_data_t data;
    int64_t got = -1;
    if (get_attr(v, record, ATTR_DATA, 0, 0, &data) != 0) {
        free_attr(&data);
        return -1;
    }
    if (data.flags & (FLAG_COMPRESSED | FLAG_ENCRYPTED)) {
        free_attr(&data);
        return -1;
    }
    if (off >= data.size) {
        free_attr(&data);
        return 0;
    }
    if (len > data.size - off) len = data.size - off;
    if (data.resident) {
        mem_copy(buf, data.data + off, len);
        got = (int64_t)len;
    } else {
        uint64_t readable = off < data.init_size ? data.init_size - off : 0;
        if (readable > len) readable = len;
        if (readable == 0 || stream_read(v, data.runs, data.nruns, off, buf, readable) == 0) {
            mem_zero((uint8_t *)buf + readable, len - readable);
            got = (int64_t)len;
        }
    }
    free_attr(&data);
    return got;
}
