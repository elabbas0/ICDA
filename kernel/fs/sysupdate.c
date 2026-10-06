/* System files and over-the-air patches (see sysupdate.h).
 *
 * The updater (/sbin/updated.app) verifies the release signature, downloads
 * only the files that changed and stages them here with "put".  "commit"
 * checks every staged file against the SHA-256 the signed manifest named and
 * records the patch.  It is installed by renames during the next restart,
 * after the old files were moved to \UPDATE\BACKUP.  GRUB counts boots of a
 * fresh patch in GRUBENV (icda_trial 1 -> 2); if the patched system never
 * reports a healthy desktop ("confirm"), the next boot sees 2, GRUB starts
 * the previous kernel with icda_trial=3 and that kernel restores the backup.
 *
 * Every operation mounts the volume afresh: the boot log and the overlay
 * also write to this partition, each with its own fatfs_t, and a cached FAT
 * sector must never outlive one operation. */
#include "sysupdate.h"
#include "fatfs.h"
#include "persistfs.h"
#include "vfs.h"
#include "../drivers/storage/partition.h"
#include "../drivers/serial/serial.h"
#include "../memory/heap.h"
#include "../crypto/sha256.h"
#include "version.h"

#define ENV_PATH      "/EFI/ICDA/GRUBENV"
#define KERNEL_PATH   "/EFI/ICDA/KERNEL.BIN"
#define PENDING_PATH  "/UPDATE/PENDING.TXT"
#define LIST_PATH     "/UPDATE/BACKUP/LIST.TXT"
#define STATE_PATH    "/UPDATE/STATE.TXT"
#define STAGE_DIR     "/UPDATE/STAGE/"
#define BACKUP_DIR    "/UPDATE/BACKUP/"
#define SYSTEM_DIR    "/SYSTEM"

#define REL_MAX       200
#define PATH_MAX      (REL_MAX + 24)
#define TEXT_MAX      (64U * 1024U)

static int  supported;
static int  trial;
static int  check_requested;
static char status_text[128] = "idle";
static char pending_version[24];
static char bad_version[24];
static char rolled_back[24];
static uint32_t system_files;

/* ---- small helpers --------------------------------------------------------- */

static uint64_t slen(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void scopy(char *dst, uint64_t cap, const char *src) {
    uint64_t i = 0;
    if (!cap) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void scat(char *dst, uint64_t cap, const char *src) {
    uint64_t n = slen(dst);
    if (n < cap) scopy(dst + n, cap - n, src);
}

static int sstarts(const char *s, const char *prefix) {
    while (*prefix)
        if (*s++ != *prefix++) return 0;
    return 1;
}

static int seq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void ulog(const char *a, const char *b) {
    serial_write("[update] ");
    serial_write(a);
    if (b) serial_write(b);
    serial_write("\n");
}

static int mount(fatfs_t *v) {
    int idx = persistfs_active_partition();
    const partition_info_t *p;
    if (idx < 0) return -1;
    p = partition_get((uint32_t)idx);
    if (!p || fatfs_mount_part(v, p) != 0) return -1;
    return 0;
}

static int exists(fatfs_t *v, const char *path) {
    fatfs_entry_t e;
    return fatfs_lookup(v, path, &e) == 0;
}

/* mkdir -p of the directory part of path */
static void make_parent(fatfs_t *v, const char *path) {
    char dir[PATH_MAX];
    uint64_t cut = 0;
    scopy(dir, sizeof(dir), path);
    for (uint64_t i = 0; dir[i]; i++)
        if (dir[i] == '/') cut = i;
    if (cut == 0) return;
    dir[cut] = 0;
    (void)fatfs_mkdir(v, dir);
}

static void join(char *out, const char *dir, const char *rel) {
    out[0] = 0;
    scat(out, PATH_MAX, dir);
    scat(out, PATH_MAX, rel);
}

/* Files a patch may replace: \SYSTEM\..., the kernel and the boot loader. */
static int rel_ok(const char *rel) {
    uint64_t n = slen(rel);
    if (n == 0 || n >= REL_MAX) return 0;
    if (!(sstarts(rel, "SYSTEM/") || seq(rel, "EFI/ICDA/KERNEL.BIN") || seq(rel, "EFI/ICDA/GRUBX64.EFI")))
        return 0;
    for (uint64_t i = 0; i < n; i++) {
        char c = rel[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '.' || c == '_' || c == '-' || c == '/';
        if (!ok) return 0;
        if (c == '/' && (i + 1 == n || rel[i + 1] == '/')) return 0;
        if (c == '.' && i + 1 < n && rel[i + 1] == '.') return 0;
    }
    return 1;
}

/* Next line of text into line (without the newline); returns 0 at the end. */
static int next_line(const char **cur, char *line, uint64_t cap) {
    const char *s = *cur;
    uint64_t n = 0;
    if (!s || !*s) return 0;
    while (*s && *s != '\n') {
        if (n + 1 < cap) line[n++] = *s;
        s++;
    }
    line[n] = 0;
    if (*s == '\n') s++;
    *cur = s;
    return 1;
}

/* "word rest": returns the rest after the first space, 0 if none */
static char *split_word(char *line) {
    while (*line && *line != ' ') line++;
    if (!*line) return 0;
    *line++ = 0;
    return line;
}

static char *read_text(fatfs_t *v, const char *path) {
    char *data = 0;
    uint64_t size = 0;
    char *text;
    if (fatfs_read(v, path, &data, &size) != 0 || !data) return 0;
    if (size > TEXT_MAX) size = TEXT_MAX;
    text = (char *)kmalloc((size_t)size + 1);
    if (text) {
        for (uint64_t i = 0; i < size; i++) text[i] = data[i];
        text[size] = 0;
    }
    kfree(data);
    return text;
}

/* ---- GRUB environment ------------------------------------------------------- */

static int read_trial(fatfs_t *v) {
    char *t = read_text(v, ENV_PATH);
    int value = 0;
    if (!t) return 0;
    for (char *p = t; *p; p++) {
        if (sstarts(p, "icda_trial=")) {
            p += 11;
            while (*p >= '0' && *p <= '9') value = value * 10 + (*p++ - '0');
            break;
        }
    }
    kfree(t);
    return value;
}

static int write_trial(fatfs_t *v, int value) {
    char b[1024];
    uint64_t n;
    for (int i = 0; i < 1024; i++) b[i] = '#';
    b[0] = 0;
    scat(b, sizeof(b), "# GRUB Environment Block\nicda_trial=");
    n = slen(b);
    b[n++] = (char)('0' + (value % 10));
    b[n++] = '\n';
    /* the rest stays '#' padding, as GRUB expects */
    return fatfs_write(v, ENV_PATH, b, sizeof(b));
}

/* ---- state file ---------------------------------------------------------- */

static void load_state(fatfs_t *v) {
    char *t = read_text(v, STATE_PATH);
    const char *cur = t;
    char line[96];
    bad_version[0] = 0;
    while (t && next_line(&cur, line, sizeof(line))) {
        char *rest = split_word(line);
        if (rest && seq(line, "bad")) scopy(bad_version, sizeof(bad_version), rest);
    }
    if (t) kfree(t);
}

static void save_state(fatfs_t *v) {
    char b[96];
    b[0] = 0;
    if (bad_version[0]) {
        scat(b, sizeof(b), "bad ");
        scat(b, sizeof(b), bad_version);
        scat(b, sizeof(b), "\n");
    }
    (void)fatfs_mkdir(v, "/UPDATE");
    (void)fatfs_write(v, STATE_PATH, b, slen(b));
}

static void load_pending_version(fatfs_t *v) {
    char *t = read_text(v, PENDING_PATH);
    const char *cur = t;
    char line[96];
    pending_version[0] = 0;
    while (t && next_line(&cur, line, sizeof(line))) {
        char *rest = split_word(line);
        if (rest && seq(line, "version")) {
            scopy(pending_version, sizeof(pending_version), rest);
            break;
        }
    }
    if (t) kfree(t);
}

/* ---- rollback ------------------------------------------------------------- */

static void rollback(fatfs_t *v) {
    char *t = read_text(v, LIST_PATH);
    const char *cur = t;
    char line[REL_MAX + 16], path[PATH_MAX], backup[PATH_MAX];
    char version[24];
    version[0] = 0;
    if (!t) {
        ulog("rollback requested but there is no backup list", 0);
        return;
    }
    while (next_line(&cur, line, sizeof(line))) {
        char *rel = split_word(line);
        if (!rel) continue;
        if (seq(line, "version")) {
            scopy(version, sizeof(version), rel);
            continue;
        }
        if (!rel_ok(rel)) continue;
        join(path, "/", rel);
        join(backup, BACKUP_DIR, rel);
        if (seq(line, "had")) {
            if (exists(v, backup)) {
                make_parent(v, path);
                (void)fatfs_rename(v, backup, path);
            }
        } else if (seq(line, "new")) {
            (void)fatfs_remove(v, path);
        }
    }
    kfree(t);
    (void)fatfs_remove(v, LIST_PATH);
    scopy(bad_version, sizeof(bad_version), version);
    scopy(rolled_back, sizeof(rolled_back), version);
    save_state(v);
    ulog("rolled back patch ", version);
}

/* ---- loading \SYSTEM ------------------------------------------------------ */

typedef struct {
    char     name[64];
    int      dir;
} child_t;

typedef struct {
    child_t *items;
    int      count, cap;
} children_t;

static int collect_cb(const fatfs_entry_t *e, void *ctx) {
    children_t *c = (children_t *)ctx;
    if (e->name[0] == '.' && (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0))) return 0;
    if (slen(e->name) >= sizeof(c->items[0].name) || c->count >= c->cap) return 0;
    scopy(c->items[c->count].name, sizeof(c->items[0].name), e->name);
    c->items[c->count].dir = (e->attr & FATFS_ATTR_DIR) != 0;
    c->count++;
    return 0;
}

static void seed_dir(fatfs_t *v, const char *fat_dir, const char *vfs_dir, int depth) {
    fatfs_entry_t d;
    children_t c;
    char fpath[PATH_MAX], vpath[PATH_MAX];

    if (depth > 8 || fatfs_lookup(v, fat_dir, &d) != 0 || !(d.attr & FATFS_ATTR_DIR)) return;
    c.cap = 128;
    c.count = 0;
    c.items = (child_t *)kmalloc(sizeof(child_t) * (size_t)c.cap);
    if (!c.items) return;
    (void)fatfs_iterate(v, d.cluster, collect_cb, &c);
    for (int i = 0; i < c.count; i++) {
        join(fpath, fat_dir, "/");
        scat(fpath, sizeof(fpath), c.items[i].name);
        join(vpath, vfs_dir, "/");
        scat(vpath, sizeof(vpath), c.items[i].name);
        if (c.items[i].dir) {
            seed_dir(v, fpath, vpath, depth + 1);
        } else {
            char *data = 0;
            uint64_t size = 0;
            if (fatfs_read(v, fpath, &data, &size) == 0) {
                if (vfs_seed_readonly(vpath, data ? data : "", size) == 0) system_files++;
                else ulog("could not load ", vpath);
                if (data) kfree(data);
            } else {
                ulog("could not read ", fpath);
            }
        }
    }
    kfree(c.items);
}

void sysupdate_boot(void) {
    fatfs_t v;
    if (mount(&v) != 0) return;
    supported = exists(&v, KERNEL_PATH);
    if (!supported) return;
    trial = read_trial(&v);
    load_state(&v);
    if (trial >= 3) {
        /* GRUB gave up on the patched system and started the previous kernel */
        rollback(&v);
        trial = 0;
        (void)write_trial(&v, 0);
    }
    load_pending_version(&v);
    system_files = 0;
    seed_dir(&v, SYSTEM_DIR, "", 0);
    (void)fatfs_flush(&v);
    {
        char n[12];
        uint32_t x = system_files;
        int k = 0;
        char r[12];
        do { r[k++] = (char)('0' + x % 10); x /= 10; } while (x);
        for (int i = 0; i < k; i++) n[i] = r[k - 1 - i];
        n[k] = 0;
        ulog("system files loaded: ", n);
    }
    if (trial) ulog("first boot of a new patch, waiting for a healthy desktop", 0);
}

/* ---- staging and commit --------------------------------------------------- */

static int hex_eq(const uint8_t digest[32], const char *hex) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        if (hex[2 * i] != h[digest[i] >> 4] || hex[2 * i + 1] != h[digest[i] & 15]) return 0;
    }
    return hex[64] == 0;
}

static int staged_hash_ok(fatfs_t *v, const char *rel, const char *hex) {
    char path[PATH_MAX];
    fatfs_entry_t e;
    fatfs_hint_t hint;
    sha256_ctx_t ctx;
    uint8_t digest[32];
    uint8_t *buf;
    uint64_t off = 0;
    int ok = 1;

    join(path, STAGE_DIR, rel);
    if (fatfs_lookup(v, path, &e) != 0) return 0;
    buf = (uint8_t *)kmalloc(65536);
    if (!buf) return 0;
    hint.first = hint.cluster = 0;
    hint.off = 0;
    sha256_init(&ctx);
    while (off < e.size) {
        uint64_t want = e.size - off < 65536 ? e.size - off : 65536;
        int64_t got = fatfs_read_range(v, &e, off, buf, want, &hint);
        if (got <= 0) {
            ok = 0;
            break;
        }
        sha256_update(&ctx, buf, (uint32_t)got);
        off += (uint64_t)got;
    }
    kfree(buf);
    sha256_final(&ctx, digest);
    return ok && hex_eq(digest, hex);
}

/* "commit\n" + patch text: every "file REL SHA256" must be staged with that
 * hash; then the text becomes PENDING.TXT. */
static int commit(fatfs_t *v, const char *text) {
    const char *cur = text;
    char line[REL_MAX + 96];
    int files = 0;
    while (next_line(&cur, line, sizeof(line))) {
        char *rest = split_word(line);
        if (!rest) continue;
        if (seq(line, "file")) {
            char *hash = split_word(rest);
            if (!hash || !rel_ok(rest) || slen(hash) != 64) {
                ulog("commit: bad line for ", rest);
                return -1;
            }
            if (!staged_hash_ok(v, rest, hash)) {
                ulog("commit: hash mismatch for ", rest);
                return -1;
            }
            files++;
        } else if (seq(line, "remove")) {
            if (!rel_ok(rest)) return -1;
        }
    }
    (void)fatfs_mkdir(v, "/UPDATE");
    if (fatfs_write(v, PENDING_PATH, text, slen(text)) != 0) return -1;
    load_pending_version(v);
    ulog("patch staged: ", pending_version);
    (void)files;
    return 0;
}

/* "put REL OFFSET\n" + bytes */
static int put(fatfs_t *v, const char *rel, uint64_t off, const char *data, uint64_t len) {
    char path[PATH_MAX];
    if (!rel_ok(rel)) return -1;
    join(path, STAGE_DIR, rel);
    if (off == 0) {
        make_parent(v, path);
        return fatfs_write(v, path, data, len);
    }
    return fatfs_write_at(v, path, off, data, len);
}

/* ---- install at restart ---------------------------------------------------- */

void sysupdate_apply(void) {
    fatfs_t v;
    char *text, *list;
    const char *cur;
    char line[REL_MAX + 96], path[PATH_MAX], other[PATH_MAX];
    uint64_t list_cap;

    if (!supported || mount(&v) != 0 || !exists(&v, PENDING_PATH)) return;
    text = read_text(&v, PENDING_PATH);
    if (!text) return;
    list_cap = slen(text) + 64;
    list = (char *)kmalloc((size_t)list_cap);
    if (!list) {
        kfree(text);
        return;
    }
    ulog("installing patch ", pending_version);

    /* 1. backup list first, so a power cut mid-way can still be undone */
    list[0] = 0;
    cur = text;
    while (next_line(&cur, line, sizeof(line))) {
        char *rest = split_word(line);
        if (!rest) continue;
        if (seq(line, "version")) {
            scat(list, list_cap, "version ");
            scat(list, list_cap, rest);
            scat(list, list_cap, "\n");
        } else if (seq(line, "file") || seq(line, "remove")) {
            char *hash = seq(line, "file") ? split_word(rest) : 0;
            (void)hash;
            if (!rel_ok(rest)) continue;
            join(path, "/", rest);
            scat(list, list_cap, exists(&v, path) ? "had " : "new ");
            scat(list, list_cap, rest);
            scat(list, list_cap, "\n");
        }
    }
    (void)fatfs_mkdir(&v, "/UPDATE/BACKUP");
    if (fatfs_write(&v, LIST_PATH, list, slen(list)) != 0) {
        ulog("could not write the backup list, patch not installed", 0);
        kfree(list);
        kfree(text);
        return;
    }

    /* 2. move old files aside and the staged ones into place */
    cur = text;
    while (next_line(&cur, line, sizeof(line))) {
        char *rest = split_word(line);
        int is_file = seq(line, "file");
        if (!rest || !(is_file || seq(line, "remove"))) continue;
        if (is_file) (void)split_word(rest);
        if (!rel_ok(rest)) continue;
        join(path, "/", rest);
        if (exists(&v, path)) {
            join(other, BACKUP_DIR, rest);
            make_parent(&v, other);
            (void)fatfs_rename(&v, path, other);
        }
        if (is_file) {
            join(other, STAGE_DIR, rest);
            make_parent(&v, path);
            if (fatfs_rename(&v, other, path) != 0) ulog("could not install ", rest);
        }
    }
    (void)fatfs_remove(&v, PENDING_PATH);
    (void)write_trial(&v, 1);
    (void)fatfs_flush(&v);
    ulog("patch installed, restarting into it", 0);
    kfree(list);
    kfree(text);
}

/* ---- /dev/sysupdate --------------------------------------------------------- */

static void put_dec(char *out, uint64_t cap, uint64_t x) {
    char r[24];
    int k = 0;
    char s[24];
    do { r[k++] = (char)('0' + x % 10); x /= 10; } while (x);
    for (int i = 0; i < k; i++) s[i] = r[k - 1 - i];
    s[k] = 0;
    scat(out, cap, s);
}

uint64_t sysupdate_node_read(char *buf, uint64_t cap) {
    char r[512];
    r[0] = 0;
    scat(r, sizeof(r), "kernel: " ICDA_VERSION_STRING "\nsupported: ");
    scat(r, sizeof(r), supported ? "1" : "0");
    scat(r, sizeof(r), "\npending: ");
    scat(r, sizeof(r), pending_version[0] ? pending_version : "none");
    scat(r, sizeof(r), "\nstatus: ");
    scat(r, sizeof(r), status_text);
    scat(r, sizeof(r), "\ncheck: ");
    scat(r, sizeof(r), check_requested ? "1" : "0");
    scat(r, sizeof(r), "\ntrial: ");
    put_dec(r, sizeof(r), (uint64_t)trial);
    scat(r, sizeof(r), "\nbad: ");
    scat(r, sizeof(r), bad_version);
    scat(r, sizeof(r), "\nrolledback: ");
    scat(r, sizeof(r), rolled_back);
    scat(r, sizeof(r), "\nsystemfiles: ");
    put_dec(r, sizeof(r), system_files);
    scat(r, sizeof(r), "\n");
    scopy(buf, cap, r);
    return slen(buf);
}

/* Commands up to 256 bytes (status, check, confirm, discard). */
uint64_t sysupdate_node_write(const char *buf, uint64_t len) {
    return sysupdate_write_user(buf, len);
}

uint64_t sysupdate_write_user(const char *buf, uint64_t len) {
    char head[REL_MAX + 40];
    uint64_t n = 0;
    const char *body;
    uint64_t body_len;
    fatfs_t v;
    int rc = 0;

    if (!buf || !len) return (uint64_t)-1;
    while (n < len && n + 1 < sizeof(head) && buf[n] != '\n') {
        head[n] = buf[n];
        n++;
    }
    head[n] = 0;
    body = n < len ? buf + n + 1 : buf + len;
    body_len = n < len ? len - n - 1 : 0;

    if (sstarts(head, "status ")) {
        scopy(status_text, sizeof(status_text), head + 7);
        check_requested = 0;
        return len;
    }
    if (seq(head, "check")) {
        check_requested = 1;
        return len;
    }
    if (!supported) return (uint64_t)-1;
    if (mount(&v) != 0) return (uint64_t)-1;

    if (seq(head, "confirm")) {
        if (trial) {
            trial = 0;
            rc = write_trial(&v, 0);
            ulog("patched system is healthy", 0);
        }
    } else if (seq(head, "discard")) {
        (void)fatfs_remove(&v, PENDING_PATH);
        pending_version[0] = 0;
    } else if (sstarts(head, "put ")) {
        char *rel = head + 4;
        char *offs = split_word(rel);
        uint64_t off = 0;
        if (!offs) return (uint64_t)-1;
        while (*offs >= '0' && *offs <= '9') off = off * 10 + (uint64_t)(*offs++ - '0');
        rc = put(&v, rel, off, body, body_len);
    } else if (seq(head, "commit")) {
        char *text = (char *)kmalloc((size_t)body_len + 1);
        if (!text || body_len > TEXT_MAX) {
            if (text) kfree(text);
            return (uint64_t)-1;
        }
        for (uint64_t i = 0; i < body_len; i++) text[i] = body[i];
        text[body_len] = 0;
        rc = commit(&v, text);
        kfree(text);
    } else {
        rc = -1;
    }
    (void)fatfs_flush(&v);
    return rc == 0 ? len : (uint64_t)-1;
}
