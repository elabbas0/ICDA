#include "accounts.h"
#include "process.h"
#include "sched.h"
#include "../fs/vfs.h"
#include "../crypto/sha256.h"
#include "../memory/heap.h"
#include "../syscall/uaccess.h"
#include "../drivers/serial/serial.h"
#include "../cpu/smp.h"

/* One line per account in /etc/icda/users:
 *   name:uid:flags:has_password:rounds:salt(hex):hash(hex):home
 * The file sits in the saved image, which is on the system partition; the
 * salted, slow hash is what stands between it and someone who copies it. */
#define ACCT_MAX     16
#define ACCT_PATH    "/etc/icda/users"
/* PBKDF2 rounds: about a fifth of a second to check a password here, and as
 * slow for anyone trying guesses against a copy of the file.  Older hashes
 * are made again with this many the next time the password is given. */
#define ACCT_ROUNDS  200000
#define KEY_CHARS    20             /* a recovery key: 20 of 32 symbols, 100 bits */
#define NAME_CAP     32
#define PASS_CAP     128

typedef struct {
    char        name[NAME_CAP];
    uint32_t    uid;
    uint32_t    flags;
    int         has_password;
    uint32_t    rounds;
    uint8_t     salt[16];
    uint8_t     hash[32];
    char        home[64];
    vfs_node_t *home_node;
    vfs_node_t *cfg_node;       /* settings: the owner's /cfg, otherwise <home>/.cfg */
    /* the recovery key: sets a new password when this one is forgotten */
    int         has_key;
    uint32_t    krounds;
    uint8_t     ksalt[16];
    uint8_t     khash[32];
} account_t;

static account_t accts[ACCT_MAX];
static int nacct, loaded;
static char passwd_text[ACCT_MAX * 96 + 64];

/* ---- small helpers ------------------------------------------------------------ */

static uint64_t slen(const char *s) { uint64_t n = 0; while (s && s[n]) n++; return n; }

static int seq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

static void scopy(char *d, const char *s, uint64_t cap) {
    uint64_t i = 0;
    for (; s && s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static void put_dec(char *buf, uint64_t *o, uint64_t v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) buf[(*o)++] = t[--n];
}

static void put_str(char *buf, uint64_t *o, const char *s) {
    while (*s) buf[(*o)++] = *s++;
}

static void put_hex(char *buf, uint64_t *o, const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) {
        buf[(*o)++] = "0123456789abcdef"[b[i] >> 4];
        buf[(*o)++] = "0123456789abcdef"[b[i] & 15];
    }
}

static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static uint64_t rand64(void) {
    uint32_t a, b, c, d;
    uint64_t v;
    unsigned char ok = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (c & (1u << 30)) {                               /* RDRAND */
        for (int i = 0; i < 10 && !ok; i++) __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) return v;
    }
    {
        static uint64_t s = 0x9E3779B97F4A7C15ULL;
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        s += ((uint64_t)hi << 32 | lo) + 0x9E3779B97F4A7C15ULL;
        v = s;
        v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
        v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
        return v ^ (v >> 31);
    }
}

/* PBKDF2-HMAC-SHA256, one 32-byte block */
static void pbkdf2(const char *pw, const uint8_t salt[16], uint32_t rounds, uint8_t out[32]) {
    uint8_t k[64], pad[64], u[32], blk[20];
    uint64_t n = slen(pw);
    sha256_ctx_t in, outer, c;
    for (int i = 0; i < 64; i++) k[i] = 0;
    if (n > 64) sha256_hash((const uint8_t *)pw, (uint32_t)n, k);
    else for (uint64_t i = 0; i < n; i++) k[i] = (uint8_t)pw[i];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&in);
    sha256_update(&in, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&outer);
    sha256_update(&outer, pad, 64);
    for (int i = 0; i < 16; i++) blk[i] = salt[i];
    blk[16] = 0; blk[17] = 0; blk[18] = 0; blk[19] = 1;
    c = in;
    sha256_update(&c, blk, 20);
    sha256_final(&c, u);
    c = outer;
    sha256_update(&c, u, 32);
    sha256_final(&c, u);
    for (int i = 0; i < 32; i++) out[i] = u[i];
    /* A fifth of a second of hashing that touches nothing shared: done
     * without the kernel lock, so the other CPUs - the Wi-Fi driver above all
     * - carry on meanwhile (account calls themselves take turns, see
     * accounts_syscall). */
    int had_lock = this_cpu()->bkl_held;
    if (had_lock) bkl_exit();
    for (uint32_t r = 1; r < rounds; r++) {
        c = in;
        sha256_update(&c, u, 32);
        sha256_final(&c, u);
        c = outer;
        sha256_update(&c, u, 32);
        sha256_final(&c, u);
        for (int i = 0; i < 32; i++) out[i] ^= u[i];
    }
    if (had_lock) (void)bkl_enter();
}

static void set_password(account_t *a, const char *pw) {
    for (int i = 0; i < 16; i += 8) {
        uint64_t r = rand64();
        for (int j = 0; j < 8; j++) a->salt[i + j] = (uint8_t)(r >> (8 * j));
    }
    a->rounds = ACCT_ROUNDS;
    a->has_password = pw && pw[0];
    pbkdf2(pw ? pw : "", a->salt, a->rounds, a->hash);
}

/* A new recovery key into out ("ABCD-EFGH-..."), its hash into the account. */
static void new_key(account_t *a, char *out) {
    static const char sym[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   /* no I, O, 0, 1 */
    char plain[KEY_CHARS + 1];
    int o = 0;
    for (int i = 0; i < KEY_CHARS; i += 8) {
        uint64_t r = rand64();
        for (int j = 0; j < 8 && i + j < KEY_CHARS; j++) plain[i + j] = sym[(r >> (5 * j)) & 31];
    }
    plain[KEY_CHARS] = 0;
    for (int i = 0; i < KEY_CHARS; i++) {
        if (i && i % 4 == 0) out[o++] = '-';
        out[o++] = plain[i];
    }
    out[o] = 0;
    for (int i = 0; i < 16; i += 8) {
        uint64_t r = rand64();
        for (int j = 0; j < 8; j++) a->ksalt[i + j] = (uint8_t)(r >> (8 * j));
    }
    a->krounds = ACCT_ROUNDS;
    a->has_key = 1;
    pbkdf2(plain, a->ksalt, a->krounds, a->khash);
    for (int i = 0; i < KEY_CHARS; i++) ((volatile char *)plain)[i] = 0;
}

/* A key as typed: any case, with or without dashes and spaces. */
static int key_ok(const account_t *a, const char *typed) {
    char plain[KEY_CHARS + 1];
    uint8_t h[32], diff = 0;
    int n = 0;
    if (!a->has_key) return 0;
    for (int i = 0; typed[i]; i++) {
        char c = typed[i];
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (n >= KEY_CHARS) return 0;
        plain[n++] = c;
    }
    if (n != KEY_CHARS) return 0;
    plain[n] = 0;
    pbkdf2(plain, a->ksalt, a->krounds, h);
    for (int i = 0; i < KEY_CHARS; i++) ((volatile char *)plain)[i] = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ a->khash[i]);
    return diff == 0;
}

static int password_ok(const account_t *a, const char *pw) {
    uint8_t h[32], diff = 0;
    if (!a->has_password) return !pw || !pw[0];
    pbkdf2(pw ? pw : "", a->salt, a->rounds, h);
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(h[i] ^ a->hash[i]);
    return diff == 0;
}

/* ---- the file ------------------------------------------------------------------ */

static void make_passwd(void) {
    uint64_t o = 0;
    put_str(passwd_text, &o, "root:x:0:0:root:/:/bin/sh\n");
    for (int i = 0; i < nacct; i++) {
        put_str(passwd_text, &o, accts[i].name);
        put_str(passwd_text, &o, ":x:");
        put_dec(passwd_text, &o, accts[i].uid);
        passwd_text[o++] = ':';
        put_dec(passwd_text, &o, accts[i].uid);
        passwd_text[o++] = ':';
        put_str(passwd_text, &o, accts[i].name);
        put_str(passwd_text, &o, ":/home:/bin/sh\n");
    }
    passwd_text[o] = 0;
    (void)vfs_seed_readonly("/etc/passwd", passwd_text, o);
}

static void resolve_homes(void) {
    vfs_kernel_begin();
    for (int i = 0; i < nacct; i++) {
        char cfg[80];
        uint64_t o = 0;
        accts[i].home_node = vfs_resolve(vfs_root(), accts[i].home);
        if (accts[i].uid == ACCOUNTS_OWNER_UID) {
            put_str(cfg, &o, "/cfg");
        } else {
            put_str(cfg, &o, accts[i].home);
            put_str(cfg, &o, "/.cfg");
        }
        cfg[o] = 0;
        accts[i].cfg_node = vfs_resolve(vfs_root(), cfg);
        if (!accts[i].cfg_node && accts[i].home_node) {
            (void)vfs_mkdir(vfs_root(), cfg);
            accts[i].cfg_node = vfs_resolve(vfs_root(), cfg);
        }
    }
    vfs_kernel_end();
}

static void parse(const char *d, uint64_t size) {
    uint64_t p = 0;
    nacct = 0;
    while (p < size && nacct < ACCT_MAX) {
        account_t *a = &accts[nacct];
        char field[12][96];
        int f = 0;
        uint64_t k = 0;
        for (int i = 0; i < 12; i++) field[i][0] = 0;
        while (p < size && d[p] != '\n') {
            if (d[p] == ':' && f < 11) { field[f][k] = 0; f++; k = 0; }
            else if (k + 1 < sizeof(field[0])) field[f][k++] = d[p];
            p++;
        }
        field[f][k] = 0;
        p++;
        /* 8 fields; 12 with a recovery key */
        if ((f != 7 && f != 11) || slen(field[5]) != 32 || slen(field[6]) != 64 || !field[0][0]) continue;
        scopy(a->name, field[0], sizeof(a->name));
        a->uid = 0;
        for (const char *s = field[1]; *s >= '0' && *s <= '9'; s++) a->uid = a->uid * 10 + (uint32_t)(*s - '0');
        a->flags = (uint32_t)(field[2][0] - '0');
        a->has_password = field[3][0] == '1';
        a->rounds = 0;
        for (const char *s = field[4]; *s >= '0' && *s <= '9'; s++) a->rounds = a->rounds * 10 + (uint32_t)(*s - '0');
        for (int i = 0; i < 16; i++) a->salt[i] = (uint8_t)(hexval(field[5][2 * i]) << 4 | hexval(field[5][2 * i + 1]));
        for (int i = 0; i < 32; i++) a->hash[i] = (uint8_t)(hexval(field[6][2 * i]) << 4 | hexval(field[6][2 * i + 1]));
        scopy(a->home, field[7], sizeof(a->home));
        a->has_key = f == 11 && field[8][0] == '1';
        a->krounds = 0;
        if (f == 11) {
            for (const char *s = field[9]; *s >= '0' && *s <= '9'; s++) a->krounds = a->krounds * 10 + (uint32_t)(*s - '0');
        }
        if (a->has_key) a->has_key = slen(field[10]) == 32 && slen(field[11]) == 64 && a->krounds;
        if (a->has_key) {
            for (int i = 0; i < 16; i++) a->ksalt[i] = (uint8_t)(hexval(field[10][2 * i]) << 4 | hexval(field[10][2 * i + 1]));
            for (int i = 0; i < 32; i++) a->khash[i] = (uint8_t)(hexval(field[11][2 * i]) << 4 | hexval(field[11][2 * i + 1]));
        }
        if (a->uid < ACCOUNTS_OWNER_UID || !a->rounds || a->home[0] != '/') continue;
        nacct++;
    }
}

static void load(void) {
    const char *d;
    uint64_t size = 0;
    if (loaded) return;
    loaded = 1;
    vfs_kernel_begin();
    d = vfs_read(vfs_root(), ACCT_PATH, &size);
    if (d) parse(d, size);
    vfs_kernel_end();
    resolve_homes();
    make_passwd();
}

static int save(void) {
    char *buf = (char *)kmalloc(ACCT_MAX * 256);
    uint64_t o = 0;
    int rc;
    if (!buf) return -1;
    for (int i = 0; i < nacct; i++) {
        account_t *a = &accts[i];
        put_str(buf, &o, a->name);
        buf[o++] = ':';
        put_dec(buf, &o, a->uid);
        buf[o++] = ':';
        buf[o++] = (char)('0' + (a->flags & 3));
        buf[o++] = ':';
        buf[o++] = a->has_password ? '1' : '0';
        buf[o++] = ':';
        put_dec(buf, &o, a->rounds);
        buf[o++] = ':';
        put_hex(buf, &o, a->salt, 16);
        buf[o++] = ':';
        put_hex(buf, &o, a->hash, 32);
        buf[o++] = ':';
        put_str(buf, &o, a->home);
        if (a->has_key) {
            put_str(buf, &o, ":1:");
            put_dec(buf, &o, a->krounds);
            buf[o++] = ':';
            put_hex(buf, &o, a->ksalt, 16);
            buf[o++] = ':';
            put_hex(buf, &o, a->khash, 32);
        }
        buf[o++] = '\n';
    }
    vfs_kernel_begin();
    (void)vfs_mkdir(vfs_root(), "/etc");
    (void)vfs_mkdir(vfs_root(), "/etc/icda");
    rc = vfs_write(vfs_root(), ACCT_PATH, buf, o);
    vfs_kernel_end();
    kfree(buf);
    make_passwd();
    return rc;
}

static account_t *find(const char *name) {
    for (int i = 0; i < nacct; i++) if (seq(accts[i].name, name)) return &accts[i];
    return 0;
}

static account_t *by_uid(uint32_t uid) {
    for (int i = 0; i < nacct; i++) if (accts[i].uid == uid) return &accts[i];
    return 0;
}

static int admins(void) {
    int n = 0;
    for (int i = 0; i < nacct; i++) n += (accts[i].flags & ACCT_FLAG_ADMIN) != 0;
    return n;
}

/* ---- the VFS's questions ------------------------------------------------------- */

static vfs_cred_t cur_cred;

static const vfs_cred_t *cred_hook(void) {
    process_t *p = sched_current_process();
    if (!p || p->kind != PROCESS_USER || !p->ex_uid) return 0;
    cur_cred.uid = p->ex_uid;
    cur_cred.admin = p->ex_admin;
    cur_cred.home = p->ex_home;
    cur_cred.cfg = p->ex_cfg;
    return &cur_cred;
}

static uint32_t home_owner_hook(vfs_node_t *dir) {
    load();
    for (int i = 0; i < nacct; i++) if (accts[i].home_node == dir || accts[i].cfg_node == dir) return accts[i].uid;
    return VFS_NOBODY;
}

void accounts_init(void) {
    vfs_set_cred_hook(cred_hook);
    vfs_set_home_owner_hook(home_owner_hook);
}

int accounts_caller_is_admin(void) {
    process_t *p = sched_current_process();
    return !p || p->kind != PROCESS_USER || !p->ex_uid || p->ex_admin;
}

const char *accounts_passwd_text(void) {
    load();
    return passwd_text;
}

/* ---- calls ------------------------------------------------------------------------ */

#define E_PERM   1
#define E_NOENT  2
#define E_FAULT  14
#define E_EXIST  17
#define E_INVAL  22
#define E_ACCES  13

static int get_str(char *dst, uint64_t cap, uint64_t uptr) {
    uint64_t n;
    if (!uptr) { dst[0] = 0; return 0; }
    n = strnlen_user((const char *)uptr, cap);
    if (n >= cap || copy_from_user(dst, (const void *)uptr, n) != 0) return -1;
    dst[n] = 0;
    return 0;
}

static void wipe(char *s, uint64_t n) { for (uint64_t i = 0; i < n; i++) ((volatile char *)s)[i] = 0; }

static int valid_name(const char *n) {
    uint64_t i = 0;
    if (!n[0] || seq(n, "root") || seq(n, "users")) return 0;
    for (; n[i]; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') || i >= NAME_CAP - 1) return 0;
    }
    return 1;
}

/* Sessions take turns: what the last one left in /tmp would belong to its
 * user and stand in the way of the next one's programs. */
static void clear_tmp(void) {
    vfs_node_t *tmp;
    vfs_kernel_begin();
    (void)vfs_mkdir(vfs_root(), "/tmp");      /* users cannot make it at the top level themselves */
    tmp = vfs_resolve(vfs_root(), "/tmp");
    while (tmp && vfs_child_count(tmp) > 0) {
        char path[300];
        uint64_t o = 0;
        put_str(path, &o, "/tmp/");
        put_str(path, &o, vfs_node_name(vfs_child_at(tmp, 0)));
        path[o] = 0;
        if (vfs_remove(vfs_root(), path) != 0) break;
    }
    vfs_kernel_end();
}

/* A wrong password costs a second, so guessing from a program is slow. */
static void penalty(void) { sched_sleep(100); }

static account_t *caller(void) {
    process_t *p = sched_current_process();
    return p && p->ex_uid ? by_uid(p->ex_uid) : 0;
}

/* the caller proves who they are: their own password (or nobody signed in) */
static int caller_proven(const char *pw) {
    account_t *me = caller();
    if (!me) return 1;
    if (password_ok(me, pw)) return 1;
    penalty();
    return 0;
}

static int64_t describe(account_t *a, char *out, uint64_t *o, uint64_t cap) {
    if (*o + NAME_CAP + 24 > cap) return -1;
    put_str(out, o, a->name);
    out[(*o)++] = ' ';
    put_dec(out, o, a->uid);
    out[(*o)++] = ' ';
    if (a->flags & ACCT_FLAG_ADMIN) out[(*o)++] = 'a';
    if (a->flags & ACCT_FLAG_AUTOLOGIN) out[(*o)++] = 'l';
    if (a->has_password) out[(*o)++] = 'p';
    out[(*o)++] = '-';
    out[(*o)++] = '\n';
    return 0;
}

static void end_programs_of(uint32_t uid, process_t *except) {
    for (process_t *q = sched_first_process(); q; q = q->next_all) {
        if (q != except && q->kind == PROCESS_USER && q->ex_uid == uid &&
            q->state != PROCESS_EXITED && q->state != PROCESS_REAPED) {
            (void)sched_kill_process(q->pid, 143);
        }
    }
}

/* the recovery key to the caller (cap ACCT_KEY_CAP) */
static int give_key(uint64_t uout, const char *key) {
    return uout && copy_to_user((void *)uout, key, slen(key) + 1) ? -1 : 0;
}

/* a right password from before ACCT_ROUNDS: hashed again, as strong as new ones */
static void strengthen(account_t *a, const char *pw) {
    if (a->rounds >= ACCT_ROUNDS) return;
    set_password(a, pw);
    (void)save();
}

static int64_t accounts_call(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    process_t *p = sched_current_process();
    char name[NAME_CAP], pw[PASS_CAP], pw2[PASS_CAP], key[32];
    int64_t rc = -E_INVAL;
    if (!p) return -E_PERM;
    load();
    name[0] = pw[0] = pw2[0] = 0;
    switch (op) {
    case ACCT_LIST:
    case ACCT_WHOAMI: {
        char out[ACCT_MAX * (NAME_CAP + 24) + 1];
        uint64_t o = 0, cap = b;
        if (!a || !cap) return -E_INVAL;
        if (op == ACCT_LIST) {
            for (int i = 0; i < nacct; i++) if (describe(&accts[i], out, &o, sizeof(out)) != 0) break;
        } else if (caller()) {
            describe(caller(), out, &o, sizeof(out));
        }
        if (o >= cap) o = cap - 1;
        out[o] = 0;
        return copy_to_user((void *)a, out, o + 1) ? -E_FAULT : (int64_t)o;
    }
    case ACCT_LOGIN: {
        account_t *acct;
        if (p->ex_uid) return -E_PERM;                 /* once per session */
        if (get_str(name, sizeof(name), a) || get_str(pw, sizeof(pw), b)) return -E_FAULT;
        acct = find(name);
        if (!acct) { penalty(); return -E_ACCES; }
        if (!b ? !((acct->flags & ACCT_FLAG_AUTOLOGIN) || !acct->has_password) : !password_ok(acct, pw)) {
            wipe(pw, sizeof(pw));
            penalty();
            return -E_ACCES;
        }
        if (b) strengthen(acct, pw);
        wipe(pw, sizeof(pw));
        if (!acct->home_node) resolve_homes();
        if (!acct->home_node) {
            vfs_kernel_begin();
            (void)vfs_mkdir(vfs_root(), "/users");
            (void)vfs_mkdir(vfs_root(), acct->home);
            vfs_kernel_end();
            resolve_homes();
        }
        p->ex_uid = acct->uid;
        p->ex_admin = (acct->flags & ACCT_FLAG_ADMIN) != 0;
        p->ex_home = acct->home_node;
        p->ex_cfg = acct->cfg_node;
        if (acct->home_node) p->cwd = acct->home_node;
        clear_tmp();
        serial_write("[accounts] signed in: ");
        serial_write(acct->name);
        serial_write("\n");
        return 0;
    }
    case ACCT_VERIFY: {
        account_t *me = caller();
        if (!me) return -E_PERM;
        if (get_str(pw, sizeof(pw), a)) return -E_FAULT;
        rc = password_ok(me, pw) ? 0 : -E_ACCES;
        if (!rc) strengthen(me, pw);
        wipe(pw, sizeof(pw));
        if (rc) penalty();
        return rc;
    }
    case ACCT_CREATE: {
        account_t *n;
        uint32_t uid = ACCOUNTS_OWNER_UID;
        if (get_str(name, sizeof(name), a) || get_str(pw, sizeof(pw), b) || get_str(pw2, sizeof(pw2), d)) {
            rc = -E_FAULT;
            break;
        }
        if (!valid_name(name)) { rc = -E_INVAL; break; }
        if (find(name)) { rc = -E_EXIST; break; }
        if (nacct >= ACCT_MAX) { rc = -E_INVAL; break; }
        if (nacct) {
            account_t *me = caller();
            if (!me || !(me->flags & ACCT_FLAG_ADMIN)) { rc = -E_PERM; break; }
            if (!caller_proven(pw2)) { rc = -E_ACCES; break; }
            for (int i = 0; i < nacct; i++) if (accts[i].uid >= uid) uid = accts[i].uid + 1;
        } else if (p->ex_uid) {
            rc = -E_PERM;                               /* the first account: before anyone signs in */
            break;
        }
        n = &accts[nacct];
        for (uint64_t i = 0; i < sizeof(*n); i++) ((char *)n)[i] = 0;
        scopy(n->name, name, sizeof(n->name));
        n->uid = uid;
        /* the owner keeps the original /home: an installed system's files
         * stay where they were and an older kernel still finds them */
        if (!nacct) {
            scopy(n->home, "/home", sizeof(n->home));
            n->flags = ACCT_FLAG_ADMIN | (!pw[0] ? ACCT_FLAG_AUTOLOGIN : 0);
        } else {
            uint64_t o = 0;
            put_str(n->home, &o, "/users/");
            put_str(n->home, &o, name);
            n->home[o] = 0;
            n->flags = (uint32_t)c & ACCT_FLAG_ADMIN;
        }
        set_password(n, pw);
        new_key(n, key);
        nacct++;
        vfs_kernel_begin();
        (void)vfs_mkdir(vfs_root(), "/users");
        (void)vfs_mkdir(vfs_root(), n->home);
        vfs_kernel_end();
        resolve_homes();
        rc = save() == 0 ? (int64_t)uid : -E_INVAL;
        if (rc >= 0 && give_key(e, key)) rc = -E_FAULT;
        break;
    }
    case ACCT_DELETE: {
        account_t *t, *me = caller();
        if (get_str(name, sizeof(name), a) || get_str(pw2, sizeof(pw2), b)) { rc = -E_FAULT; break; }
        t = find(name);
        if (!t) { rc = -E_NOENT; break; }
        if (!me || !(me->flags & ACCT_FLAG_ADMIN)) { rc = -E_PERM; break; }
        if (!caller_proven(pw2)) { rc = -E_ACCES; break; }
        if (t == me || t->uid == ACCOUNTS_OWNER_UID) { rc = -E_PERM; break; }   /* not yourself, not the owner */
        end_programs_of(t->uid, 0);
        if (c) {                                        /* and the files in their home */
            vfs_kernel_begin();
            (void)vfs_remove(vfs_root(), t->home);
            vfs_kernel_end();
        }
        for (account_t *x = t; x + 1 < &accts[nacct]; x++) *x = *(x + 1);
        nacct--;
        resolve_homes();
        rc = save() == 0 ? 0 : -E_INVAL;
        break;
    }
    case ACCT_PASSWORD: {
        account_t *t, *me = caller();
        if (get_str(name, sizeof(name), a) || get_str(pw2, sizeof(pw2), b) || get_str(pw, sizeof(pw), c)) {
            rc = -E_FAULT;
            break;
        }
        t = find(name);
        if (!t) { rc = -E_NOENT; break; }
        if (!me || (t != me && !(me->flags & ACCT_FLAG_ADMIN))) { rc = -E_PERM; break; }
        if (!caller_proven(pw2)) { rc = -E_ACCES; break; }
        set_password(t, pw);
        rc = save() == 0 ? 0 : -E_INVAL;
        break;
    }
    case ACCT_SET: {
        account_t *t, *me = caller();
        uint32_t flags = (uint32_t)c & (ACCT_FLAG_ADMIN | ACCT_FLAG_AUTOLOGIN);
        if (get_str(name, sizeof(name), a) || get_str(pw2, sizeof(pw2), b)) { rc = -E_FAULT; break; }
        t = find(name);
        if (!t) { rc = -E_NOENT; break; }
        if (!me) { rc = -E_PERM; break; }
        if (!caller_proven(pw2)) { rc = -E_ACCES; break; }
        /* administrators change anything; anyone may switch their own automatic sign-in */
        if (!(me->flags & ACCT_FLAG_ADMIN) &&
            (t != me || (flags & ACCT_FLAG_ADMIN) != (t->flags & ACCT_FLAG_ADMIN))) { rc = -E_PERM; break; }
        if ((t->flags & ACCT_FLAG_ADMIN) && !(flags & ACCT_FLAG_ADMIN) && admins() <= 1) { rc = -E_PERM; break; }
        if (t->uid == ACCOUNTS_OWNER_UID) flags |= ACCT_FLAG_ADMIN;   /* the owner stays one */
        if (flags & ACCT_FLAG_AUTOLOGIN) {              /* one account signs in by itself */
            for (int i = 0; i < nacct; i++) accts[i].flags &= ~(uint32_t)ACCT_FLAG_AUTOLOGIN;
        }
        t->flags = flags;
        rc = save() == 0 ? 0 : -E_INVAL;
        break;
    }
    case ACCT_RECOVER: {                   /* name, key, new password, key out */
        account_t *t;
        char typed[48];
        if (get_str(name, sizeof(name), a) || get_str(typed, sizeof(typed), b) || get_str(pw, sizeof(pw), c)) {
            rc = -E_FAULT;
            break;
        }
        t = find(name);
        if (!t || !key_ok(t, typed)) { wipe(typed, sizeof(typed)); penalty(); rc = -E_ACCES; break; }
        wipe(typed, sizeof(typed));
        set_password(t, pw);
        new_key(t, key);                        /* a used key is spent */
        rc = save() == 0 ? 0 : -E_INVAL;
        if (!rc && give_key(d, key)) rc = -E_FAULT;
        serial_write("[accounts] password set with a recovery key: ");
        serial_write(t->name);
        serial_write("\n");
        break;
    }
    case ACCT_NEW_KEY: {                   /* name, caller's password, key out */
        account_t *t, *me = caller();
        if (get_str(name, sizeof(name), a) || get_str(pw2, sizeof(pw2), b)) { rc = -E_FAULT; break; }
        t = find(name);
        if (!t) { rc = -E_NOENT; break; }
        if (!me || (t != me && !(me->flags & ACCT_FLAG_ADMIN))) { rc = -E_PERM; break; }
        if (!caller_proven(pw2)) { rc = -E_ACCES; break; }
        new_key(t, key);
        rc = save() == 0 ? 0 : -E_INVAL;
        if (!rc && give_key(c, key)) rc = -E_FAULT;
        break;
    }
    case ACCT_LOGOUT:
        if (!p->ex_uid) return -E_PERM;
        end_programs_of(p->ex_uid, p);
        return 0;
    default:
        break;
    }
    wipe(pw, sizeof(pw));
    wipe(pw2, sizeof(pw2));
    wipe(key, sizeof(key));
    return rc;
}

/* One call at a time: hashing yields the CPU part way (see pbkdf2), and a
 * second call meanwhile could change the table under the first. */
int64_t accounts_syscall(uint64_t op, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    static int busy;
    int64_t rc;
    if (busy) return -11;                       /* EAGAIN: try again */
    busy = 1;
    rc = accounts_call(op, a, b, c, d, e);
    busy = 0;
    return rc;
}
