#include "volumes.h"
#include "vfs.h"
#include "fatfs.h"
#include "exfatfs.h"
#include "ntfsfs.h"
#include "../drivers/storage/partition.h"
#include "../memory/heap.h"

#define VOL_PATH_CAP  512U
#define VOL_MAX       16U
#define VOL_DEPTH_MAX 16U
#define VOL_NAME_MAX  255U

typedef struct {
    int             used;
    int             fs;
    int             writable;
    char            path[64];
    block_device_t *dev;
    uint64_t        start;
    uint64_t        sectors;
    uint16_t       *upcase;
    ntfs_t         *ntfs;
} volume_t;

static volume_t volumes[VOL_MAX];
static uint32_t volume_counts[4];
static fatfs_hint_t fat_hint;
static exfat_hint_t ex_hint;
static char hint_path[VOL_PATH_CAP];
static uint8_t hint_mount;

static uint64_t str_len(const char *text) {
    uint64_t len = 0;
    while (text && text[len]) len++;
    return len;
}

static void copy_text(char *dst, const char *src, uint32_t cap) {
    uint32_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void append_text(char *dst, const char *src, uint32_t cap) {
    uint32_t len = (uint32_t)str_len(dst);
    if (len >= cap) return;
    copy_text(dst + len, src, cap - len);
}

static void append_u32(char *dst, uint32_t cap, uint32_t value) {
    char tmp[12];
    uint32_t n = 0;
    do {
        tmp[n++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value && n < sizeof(tmp));
    while (n) {
        char one[2] = { tmp[--n], 0 };
        append_text(dst, one, cap);
    }
}

static int same_text(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int rel_path(uint8_t id, const char *path, volume_t **out, const char **rel) {
    volume_t *m;
    uint64_t prefix;
    if (id == 0 || id > VOL_MAX) return -1;
    m = &volumes[id - 1];
    if (!m->used) return -1;
    prefix = str_len(m->path);
    for (uint64_t i = 0; i < prefix; i++) {
        if (path[i] != m->path[i]) return -1;
    }
    if (path[prefix] != '/') return -1;
    *out = m;
    *rel = path + prefix;
    return 0;
}

/* ---- FAT32 file reads ---------------------------------------------------------
 * Reading a file at an offset used to mount the volume, look the path up
 * directory by directory, walk the cluster chain from the file's start and
 * read one cluster per disk command.  Programs' libraries are read this way
 * (the page cache asks for 256 KB at a time, all over files of tens of MB),
 * so a file's cluster chain is remembered here as runs of contiguous
 * clusters: a read is then one disk command per run.  Any write to a volume
 * forgets what is remembered about it. */
#define RC_FILES   64
#define RC_FAT_BUF 64U                   /* FAT sectors read at once */

typedef struct { uint32_t cluster, count; } rc_run_t;

typedef struct {
    uint8_t   id;
    uint32_t  gen;
    uint64_t  use;
    uint32_t  size;
    uint32_t  nruns;
    rc_run_t *runs;
    char      path[VOL_PATH_CAP];
} rc_file_t;

static rc_file_t rc_files[RC_FILES];
static uint32_t  rc_gen[VOL_MAX];
static uint64_t  rc_clock;
static struct {
    uint32_t gen;
    int      ok;
    fatfs_t  v;
} rc_vol[VOL_MAX];

static void rc_forget(uint8_t id) {
    if (id >= 1 && id <= VOL_MAX) rc_gen[id - 1]++;
}

/* the volume's geometry, read once per generation */
static fatfs_t *rc_volume(uint8_t id, volume_t *m) {
    uint32_t i = id - 1u;
    if (!rc_vol[i].ok || rc_vol[i].gen != rc_gen[i]) {
        rc_vol[i].ok = fatfs_mount(&rc_vol[i].v, m->dev, m->start, m->sectors) == 0;
        rc_vol[i].gen = rc_gen[i];
    }
    return rc_vol[i].ok ? &rc_vol[i].v : 0;
}

/* the cluster runs of a file, from its first cluster (FAT read in batches) */
static int rc_build(fatfs_t *v, uint32_t first, uint32_t size, rc_file_t *f) {
    uint64_t clusters = ((uint64_t)size + v->cluster_bytes - 1) / v->cluster_bytes;
    uint32_t cap = 16, n = 0, c = first;
    uint64_t have = 0, buf_first = ~0ULL;
    uint8_t *fat = (uint8_t *)kmalloc(RC_FAT_BUF * 512U);
    rc_run_t *runs = (rc_run_t *)kmalloc(cap * sizeof(rc_run_t));
    if (!fat || !runs) {
        kfree(fat);
        kfree(runs);
        return -1;
    }
    while (have < clusters && c >= 2 && c < v->cluster_count + 2) {
        uint64_t sec = ((uint64_t)c * 4U) / 512U, at;
        if (n && runs[n - 1].cluster + runs[n - 1].count == c) {
            runs[n - 1].count++;
        } else {
            if (n == cap) {
                rc_run_t *bigger = (rc_run_t *)kmalloc(cap * 2 * sizeof(rc_run_t));
                if (!bigger) break;
                for (uint32_t k = 0; k < n; k++) bigger[k] = runs[k];
                kfree(runs);
                runs = bigger;
                cap *= 2;
            }
            runs[n].cluster = c;
            runs[n].count = 1;
            n++;
        }
        have++;
        if (have == clusters) break;
        if (buf_first == ~0ULL || sec < buf_first || sec >= buf_first + RC_FAT_BUF) {
            uint32_t count = RC_FAT_BUF;
            if (sec + count > v->fat_sectors) count = (uint32_t)(v->fat_sectors - sec);
            if (!count || v->dev->read(v->dev->context, v->base_lba + v->fat_lba + sec, count, fat) != 0) break;
            buf_first = sec;
        }
        at = ((uint64_t)c * 4U) - buf_first * 512U;
        c = ((uint32_t)fat[at] | (uint32_t)fat[at + 1] << 8 | (uint32_t)fat[at + 2] << 16 | (uint32_t)fat[at + 3] << 24) &
            0x0FFFFFFFU;
    }
    kfree(fat);
    if (have != clusters) {
        kfree(runs);
        return -1;
    }
    f->runs = runs;
    f->nruns = n;
    f->size = size;
    return 0;
}

static rc_file_t *rc_find(uint8_t id, volume_t *m, const char *path, const char *rel, fatfs_t *v) {
    rc_file_t *slot = 0;
    fatfs_entry_t *e;
    for (uint32_t i = 0; i < RC_FILES; i++) {
        rc_file_t *f = &rc_files[i];
        int stale = !f->runs || f->gen != rc_gen[f->id - 1];
        if (!stale && f->id == id && same_text(f->path, path)) {
            f->use = ++rc_clock;
            return f;
        }
        if (stale) f->use = 0;           /* reused first */
        if (!slot || f->use < slot->use) slot = f;
    }
    (void)m;
    e = (fatfs_entry_t *)kmalloc(sizeof(fatfs_entry_t));
    if (!e) return 0;
    if (fatfs_lookup(v, rel, e) != 0 || (e->attr & FATFS_ATTR_DIR)) {
        kfree(e);
        return 0;
    }
    if (slot->runs) {
        kfree(slot->runs);
        slot->runs = 0;
    }
    if (e->size && rc_build(v, e->cluster, e->size, slot) != 0) {
        kfree(e);
        return 0;
    }
    if (!e->size) {                      /* empty: no clusters, nothing to read */
        slot->runs = (rc_run_t *)kmalloc(sizeof(rc_run_t));
        if (!slot->runs) {
            kfree(e);
            return 0;
        }
        slot->nruns = 0;
        slot->size = 0;
    }
    kfree(e);
    slot->id = id;
    slot->gen = rc_gen[id - 1];
    slot->use = ++rc_clock;
    copy_text(slot->path, path, sizeof(slot->path));
    return slot;
}

/* Small reads (programs reading files a few KB at a time) go through a cache
 * of 64 KB disk blocks, so they cost a copy instead of a disk command.  Big
 * reads (the page cache's 256 KB chunks) go straight to the disk. */
#define BC_BLOCKS  256
#define BC_SECTORS 128U                  /* 64 KB */

static struct {
    uint8_t  id;
    uint32_t gen;
    uint64_t lba;                        /* first sector, BC_SECTORS aligned */
    uint64_t use;
    uint8_t *data;
} bc[BC_BLOCKS];

static int bc_read(uint8_t id, fatfs_t *v, uint64_t lba, uint32_t count, uint8_t *out) {
    while (count) {
        uint64_t base = lba & ~(uint64_t)(BC_SECTORS - 1), in = lba - base;
        uint32_t take = (uint32_t)(BC_SECTORS - in), slot = 0;
        int hit = -1;
        if (take > count) take = count;
        for (uint32_t i = 0; i < BC_BLOCKS; i++) {
            if (bc[i].data && bc[i].id == id && bc[i].lba == base && bc[i].gen == rc_gen[id - 1]) {
                hit = (int)i;
                break;
            }
            if (!bc[i].data || bc[i].use < bc[slot].use) slot = i;
        }
        if (hit < 0) {
            uint64_t end = v->base_lba + v->sectors, n = BC_SECTORS;
            if (base + n > end) n = end > base ? end - base : 0;
            if (!bc[slot].data) bc[slot].data = (uint8_t *)kmalloc(BC_SECTORS * 512U);
            if (!bc[slot].data || n < in + take) return v->dev->read(v->dev->context, lba, count, out);
            bc[slot].lba = ~0ULL;
            if (v->dev->read(v->dev->context, base, (uint32_t)n, bc[slot].data) != 0) return -1;
            bc[slot].id = id;
            bc[slot].gen = rc_gen[id - 1];
            bc[slot].lba = base;
            hit = (int)slot;
        }
        bc[hit].use = ++rc_clock;
        {
            const uint8_t *src = bc[hit].data + in * 512U;
            uint64_t bytes = (uint64_t)take * 512U;
            uint8_t *dst = out;
            __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(bytes) : : "memory");
        }
        out += (uint64_t)take * 512U;
        lba += take;
        count -= take;
    }
    return 0;
}

/* reads [off, off + len) of a cached file: whole sectors straight into buf,
 * partial ones through a sector buffer */
static int64_t rc_read(uint8_t id, fatfs_t *v, rc_file_t *f, uint64_t off, char *buf, uint64_t len) {
    int small = len < 65536;
    uint64_t done = 0, run_start = 0;   /* byte offset where the current run begins */
    uint32_t r = 0;
    uint8_t sec[512];
    if (off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;
    while (r < f->nruns && run_start + (uint64_t)f->runs[r].count * v->cluster_bytes <= off) {
        run_start += (uint64_t)f->runs[r].count * v->cluster_bytes;
        r++;
    }
    while (done < len && r < f->nruns) {
        uint64_t run_bytes = (uint64_t)f->runs[r].count * v->cluster_bytes;
        uint64_t in = off + done - run_start;               /* offset inside the run */
        uint64_t lba = v->base_lba + v->data_lba + (uint64_t)(f->runs[r].cluster - 2) * v->sectors_per_cluster + in / 512U;
        uint64_t avail = run_bytes - in, want = len - done;
        if (want > avail) want = avail;
        if (in % 512U || want < 512U) {                     /* a partial sector */
            uint64_t skip = in % 512U, take = 512U - skip;
            if (take > want) take = want;
            if ((small ? bc_read(id, v, lba, 1, sec) : v->dev->read(v->dev->context, lba, 1, sec)) != 0) break;
            for (uint64_t i = 0; i < take; i++) buf[done + i] = (char)sec[skip + i];
            done += take;
        } else {
            uint64_t sectors = want / 512U;
            if (sectors > 2048U) sectors = 2048U;           /* 1 MB per command */
            if ((small ? bc_read(id, v, lba, (uint32_t)sectors, (uint8_t *)buf + done)
                       : v->dev->read(v->dev->context, lba, (uint32_t)sectors, buf + done)) != 0) break;
            done += sectors * 512U;
        }
        if (off + done - run_start >= run_bytes) {
            run_start += run_bytes;
            r++;
        }
    }
    return done ? (int64_t)done : -1;
}

static int volume_external(int op, uint8_t id, const char *path, const char *data, uint64_t size, uint64_t off) {
    volume_t *m;
    const char *rel, *dst_rel = 0;
    int rc = -1;
    if (id == 0 || id > VOL_MAX || !volumes[id - 1].used) return 0;
    rc_forget(id);
    if (rel_path(id, path, &m, &rel) != 0 || !m->writable) return -1;
    if (op == VFS_EXT_RENAME) {
        volume_t *m2;
        if (rel_path(id, data, &m2, &dst_rel) != 0 || m2 != m) return -1;
    }
    fat_hint.cluster = 0;
    ex_hint.cluster = 0;
    if (m->fs == VOLUME_FAT32) {
        fatfs_t *v = (fatfs_t *)kmalloc(sizeof(fatfs_t));
        if (!v) return -1;
        if (fatfs_mount(v, m->dev, m->start, m->sectors) == 0) {
            if (op == VFS_EXT_MKDIR) rc = fatfs_mkdir(v, rel);
            else if (op == VFS_EXT_WRITE) rc = fatfs_write(v, rel, data, size);
            else if (op == VFS_EXT_REMOVE) rc = fatfs_remove(v, rel);
            else if (op == VFS_EXT_WRITE_AT) rc = fatfs_write_at(v, rel, off, data, size);
            else if (op == VFS_EXT_TRUNCATE) rc = fatfs_truncate(v, rel, size);
            else if (op == VFS_EXT_RENAME) rc = fatfs_rename(v, rel, dst_rel);
        }
        kfree(v);
    } else {
        exfat_t *v = (exfat_t *)kmalloc(sizeof(exfat_t));
        if (!v) return -1;
        if (exfat_mount(v, m->dev, m->start, m->sectors) == 0) {
            v->upcase = m->upcase;
            if (op == VFS_EXT_MKDIR) rc = exfat_mkdir(v, rel);
            else if (op == VFS_EXT_WRITE) rc = exfat_write(v, rel, data, size);
            else if (op == VFS_EXT_REMOVE) rc = exfat_remove(v, rel);
            else if (op == VFS_EXT_WRITE_AT) rc = exfat_write_at(v, rel, off, data, size);
            else if (op == VFS_EXT_TRUNCATE) rc = exfat_truncate(v, rel, size);
            else if (op == VFS_EXT_RENAME) rc = exfat_rename(v, rel, dst_rel);
        }
        kfree(v);
    }
    return rc == 0 ? 0 : -1;
}

static int64_t volume_loader(uint8_t id, const char *path, uint64_t ref, uint64_t off, char *buf, uint64_t len) {
    volume_t *m;
    const char *rel;
    int64_t got = -1;
    if (rel_path(id, path, &m, &rel) != 0) return -1;
    if (m->fs == VOLUME_NTFS) return m->ntfs ? ntfs_read_file(m->ntfs, ref, off, buf, len) : -1;
    if (hint_mount != id || !same_text(hint_path, path)) {
        fat_hint.cluster = 0;
        ex_hint.cluster = 0;
        hint_mount = id;
        copy_text(hint_path, path, sizeof(hint_path));
    }
    if (m->fs == VOLUME_FAT32) {
        fatfs_t *cv = rc_volume(id, m);
        rc_file_t *f = cv ? rc_find(id, m, path, rel, cv) : 0;
        if (f) return rc_read(id, cv, f, off, buf, len);
    }
    if (m->fs == VOLUME_FAT32) {
        fatfs_t *v = (fatfs_t *)kmalloc(sizeof(fatfs_t));
        fatfs_entry_t *e = (fatfs_entry_t *)kmalloc(sizeof(fatfs_entry_t));
        if (v && e && fatfs_mount(v, m->dev, m->start, m->sectors) == 0 && fatfs_lookup(v, rel, e) == 0 &&
            !(e->attr & FATFS_ATTR_DIR)) {
            got = fatfs_read_range(v, e, off, buf, len, &fat_hint);
        }
        if (v) kfree(v);
        if (e) kfree(e);
    } else {
        exfat_t *v = (exfat_t *)kmalloc(sizeof(exfat_t));
        exfat_entry_t *e = (exfat_entry_t *)kmalloc(sizeof(exfat_entry_t));
        if (v && e && exfat_mount(v, m->dev, m->start, m->sectors) == 0) {
            v->upcase = m->upcase;
            if (exfat_lookup(v, rel, e) == 0 && !(e->attr & EXFATFS_ATTR_DIR)) {
                got = exfat_read_range(v, e, off, buf, len, &ex_hint);
            }
        }
        if (v) kfree(v);
        if (e) kfree(e);
    }
    return got;
}

typedef struct {
    void    *vol;
    char     path[VOL_PATH_CAP];
    uint32_t depth;
    uint8_t  readonly;
} import_ctx_t;

static int import_fat_dir(fatfs_t *v, uint32_t cluster, const char *path, uint32_t depth, uint8_t ro);
static int import_ex_dir(exfat_t *v, const exfat_dir_t *dir, const char *path, uint32_t depth, uint8_t ro);

static int child_path(const import_ctx_t *ctx, const char *name, char *out) {
    if (str_len(name) > VOL_NAME_MAX) return -1;
    copy_text(out, ctx->path, VOL_PATH_CAP);
    append_text(out, "/", VOL_PATH_CAP);
    append_text(out, name, VOL_PATH_CAP);
    return str_len(out) + 1 >= VOL_PATH_CAP ? -1 : 0;
}

static int fat_import_cb(const fatfs_entry_t *e, void *p) {
    import_ctx_t *ctx = (import_ctx_t *)p;
    char path[VOL_PATH_CAP];
    if (child_path(ctx, e->name, path) != 0) return 0;
    if (e->attr & FATFS_ATTR_DIR) {
        if (vfs_import_node(path, VFS_NODE_DIR, ctx->readonly, 0, 0, 0, 0, 0) != 0) return 0;
        if (e->cluster >= 2) (void)import_fat_dir((fatfs_t *)ctx->vol, e->cluster, path, ctx->depth + 1, ctx->readonly);
    } else {
        (void)vfs_import_lazy(path, e->size, (uint8_t)(ctx->readonly || (e->attr & FATFS_ATTR_RO)));
    }
    return 0;
}

static int ex_import_cb(const exfat_entry_t *e, void *p) {
    import_ctx_t *ctx = (import_ctx_t *)p;
    char path[VOL_PATH_CAP];
    if (child_path(ctx, e->name, path) != 0) return 0;
    if (e->attr & EXFATFS_ATTR_DIR) {
        exfat_dir_t dir;
        if (vfs_import_node(path, VFS_NODE_DIR, ctx->readonly, 0, 0, 0, 0, 0) != 0) return 0;
        exfat_entry_dir(e, &dir);
        (void)import_ex_dir((exfat_t *)ctx->vol, &dir, path, ctx->depth + 1, ctx->readonly);
    } else {
        (void)vfs_import_lazy(path, e->size, (uint8_t)(ctx->readonly || (e->attr & EXFATFS_ATTR_RO)));
    }
    return 0;
}

static import_ctx_t *new_ctx(void *vol, const char *path, uint32_t depth, uint8_t ro) {
    import_ctx_t *ctx;
    if (depth > VOL_DEPTH_MAX) return 0;
    ctx = (import_ctx_t *)kmalloc(sizeof(import_ctx_t));
    if (!ctx) return 0;
    ctx->vol = vol;
    ctx->depth = depth;
    ctx->readonly = ro;
    copy_text(ctx->path, path, sizeof(ctx->path));
    return ctx;
}

static int import_fat_dir(fatfs_t *v, uint32_t cluster, const char *path, uint32_t depth, uint8_t ro) {
    import_ctx_t *ctx = new_ctx(v, path, depth, ro);
    int rc;
    if (!ctx) return 0;
    rc = fatfs_iterate(v, cluster, fat_import_cb, ctx);
    kfree(ctx);
    return rc < 0 ? -1 : 0;
}

static int import_ex_dir(exfat_t *v, const exfat_dir_t *dir, const char *path, uint32_t depth, uint8_t ro) {
    import_ctx_t *ctx = new_ctx(v, path, depth, ro);
    int rc;
    if (!ctx) return 0;
    rc = exfat_iterate(v, dir, ex_import_cb, ctx);
    kfree(ctx);
    return rc < 0 ? -1 : 0;
}

typedef struct {
    char path[VOL_PATH_CAP];
} ntfs_ctx_t;

static int ntfs_child_cb(const char *name, uint64_t record, int is_dir, uint64_t size, void *p) {
    ntfs_ctx_t *ctx = (ntfs_ctx_t *)p;
    char path[VOL_PATH_CAP];
    if (str_len(name) > VOL_NAME_MAX) return 0;
    copy_text(path, ctx->path, sizeof(path));
    append_text(path, "/", sizeof(path));
    append_text(path, name, sizeof(path));
    if (str_len(path) + 1 >= sizeof(path)) return 0;
    (void)vfs_import_ref(path, is_dir ? VFS_NODE_DIR : VFS_NODE_FILE, size, 1, record);
    return 0;
}

static int volume_dir_loader(uint8_t id, const char *path, uint64_t ref) {
    volume_t *m;
    const char *rel;
    ntfs_ctx_t *ctx;
    int rc;
    if (rel_path(id, path, &m, &rel) != 0 && !(id && id <= VOL_MAX && volumes[id - 1].used &&
                                                same_text(volumes[id - 1].path, path))) {
        return -1;
    }
    m = &volumes[id - 1];
    if (m->fs != VOLUME_NTFS || !m->ntfs) return -1;
    ctx = (ntfs_ctx_t *)kmalloc(sizeof(ntfs_ctx_t));
    if (!ctx) return -1;
    copy_text(ctx->path, path, sizeof(ctx->path));
    rc = ntfs_list_dir(m->ntfs, ref, ntfs_child_cb, ctx);
    kfree(ctx);
    return rc;
}

static volume_t *claim_slot(uint8_t *id) {
    for (uint32_t i = 0; i < VOL_MAX; i++) {
        if (!volumes[i].used) {
            *id = (uint8_t)(i + 1);
            return &volumes[i];
        }
    }
    return 0;
}

static int mount_part(const partition_info_t *part, const char *mount_path) {
    volume_t *m;
    uint8_t id = 0;
    int rc = -1, fs_writable = 0;
    void *vol = 0;
    if (!part || !mount_path || !*mount_path) return -1;
    if (part->fs_hint != PARTITION_FS_FAT32 && part->fs_hint != PARTITION_FS_EXFAT &&
        part->fs_hint != PARTITION_FS_NTFS) {
        return -1;
    }
    m = claim_slot(&id);
    if (!m) return -1;
    if (part->fs_hint == PARTITION_FS_NTFS) {
        ntfs_t *n = (ntfs_t *)kmalloc(sizeof(ntfs_t));
        if (!n || ntfs_mount(n, part->device, part->start_lba, part->sector_count) != 0) {
            if (n) kfree(n);
            return -1;
        }
        m->used = 1;
        m->fs = VOLUME_NTFS;
        m->ntfs = n;
        m->writable = 0;
        m->dev = part->device;
        m->start = part->start_lba;
        m->sectors = part->sector_count;
        copy_text(m->path, mount_path, sizeof(m->path));
        (void)vfs_detach_tree(mount_path);
        if (vfs_import_node(mount_path, VFS_NODE_DIR, 1, 0, 0, 0, 0, 0) != 0 || vfs_set_mount(mount_path, id) != 0 ||
            vfs_import_ref(mount_path, VFS_NODE_DIR, 0, 1, NTFS_ROOT_RECORD) != 0) {
            kfree(n);
            m->ntfs = 0;
            m->used = 0;
            return -1;
        }
        return 0;
    }
    if (part->fs_hint == PARTITION_FS_FAT32) {
        fatfs_t *v = (fatfs_t *)kmalloc(sizeof(fatfs_t));
        if (!v || fatfs_mount_part(v, part) != 0) {
            if (v) kfree(v);
            return -1;
        }
        vol = v;
        fs_writable = 1;
        m->fs = VOLUME_FAT32;
    } else {
        exfat_t *v = (exfat_t *)kmalloc(sizeof(exfat_t));
        if (!v || exfat_mount(v, part->device, part->start_lba, part->sector_count) != 0) {
            if (v) kfree(v);
            return -1;
        }
        m->upcase = 0;
        if (v->upcase_cluster) (void)exfat_load_upcase(v, &m->upcase);
        v->upcase = m->upcase;
        vol = v;
        fs_writable = v->writable;
        m->fs = VOLUME_EXFAT;
    }
    m->used = 1;
    m->dev = part->device;
    m->start = part->start_lba;
    m->sectors = part->sector_count;
    m->writable = fs_writable && part->device->write && part->role != PARTITION_ROLE_EFI &&
                  part->role != PARTITION_ROLE_SYSTEM;
    copy_text(m->path, mount_path, sizeof(m->path));
    (void)vfs_detach_tree(mount_path);
    if (vfs_import_node(mount_path, VFS_NODE_DIR, 1, 0, 0, 0, 0, 0) == 0 && vfs_set_mount(mount_path, id) == 0) {
        if (m->fs == VOLUME_FAT32) {
            rc = import_fat_dir((fatfs_t *)vol, ((fatfs_t *)vol)->root_cluster, mount_path, 0, (uint8_t)!m->writable);
        } else {
            exfat_dir_t root;
            exfat_root_dir((exfat_t *)vol, &root);
            rc = import_ex_dir((exfat_t *)vol, &root, mount_path, 0, (uint8_t)!m->writable);
        }
    } else {
        m->used = 0;
    }
    kfree(vol);
    return rc;
}

static void install_hooks(void) {
    vfs_set_external_hook(volume_external);
    vfs_set_loader(volume_loader);
    vfs_set_dir_loader(volume_dir_loader);
}

int volumes_mount_all(void) {
    install_hooks();
    for (uint32_t i = 0; i < VOL_MAX; i++) {
        if (volumes[i].used) (void)vfs_detach_tree(volumes[i].path);
        if (volumes[i].upcase) kfree(volumes[i].upcase);
        if (volumes[i].ntfs) kfree(volumes[i].ntfs);
        volumes[i].upcase = 0;
        volumes[i].ntfs = 0;
        volumes[i].used = 0;
    }
    fat_hint.cluster = 0;
    ex_hint.cluster = 0;
    hint_mount = 0;
    volume_counts[VOLUME_FAT32] = 0;
    volume_counts[VOLUME_EXFAT] = 0;
    volume_counts[VOLUME_NTFS] = 0;
    (void)vfs_mkdir(vfs_root(), "/volumes");
    for (uint32_t i = 0; i < partition_count(); i++) {
        const partition_info_t *part = partition_get(i);
        char mount_path[64];
        int fs;
        if (!part) continue;
        if (part->fs_hint == PARTITION_FS_FAT32) fs = VOLUME_FAT32;
        else if (part->fs_hint == PARTITION_FS_EXFAT) fs = VOLUME_EXFAT;
        else if (part->fs_hint == PARTITION_FS_NTFS) fs = VOLUME_NTFS;
        else continue;
        copy_text(mount_path, fs == VOLUME_FAT32 ? "/volumes/fat32-" : fs == VOLUME_EXFAT ? "/volumes/exfat-" : "/volumes/ntfs-",
                  sizeof(mount_path));
        append_u32(mount_path, sizeof(mount_path), volume_counts[fs]);
        if (mount_part(part, mount_path) == 0) volume_counts[fs]++;
    }
    return 0;
}

uint32_t volumes_count(int fs) {
    return fs >= VOLUME_FAT32 && fs <= VOLUME_NTFS ? volume_counts[fs] : 0;
}

int volumes_mount_partition(uint32_t partition_index, const char *mount_path) {
    install_hooks();
    return mount_part(partition_get(partition_index), mount_path);
}

int volumes_is_writable(uint32_t index, int fs) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < VOL_MAX; i++) {
        if (volumes[i].used && volumes[i].fs == fs) {
            if (n == index) return volumes[i].writable;
            n++;
        }
    }
    return 0;
}
