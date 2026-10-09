/* updated - over-the-air patches for ICDA.
 *
 * Every few hours (and on request from Settings) it fetches the signed
 * release manifest from the `ota` branch of github.com/elabbas0/ICDA:
 *
 *   icda-ota 1
 *   version 1.6.1
 *   notes <one line>
 *   file SYSTEM/apps/wm.app <size> <sha256>
 *   file EFI/ICDA/KERNEL.BIN <size> <sha256>
 *   ...
 *   wfile linux/usr/lib/libWPEWebKit-2.0.so.1.0.7 <size> <sha256> <offset in the pack>
 *   wpack linux.pack <size> <sha256> <pieces>
 *   ...
 *   signature <ed25519 over everything above this line, hex>
 *
 * It checks the signature, compares the files with the installed manifest
 * (/etc/icda-release.txt) and downloads only the ones that changed, straight
 * into /dev/sysupdate.  The kernel checks each file's hash again and installs
 * the patch during the next restart.  Personal files are never involved. */
/*
 * "lfile" lines are WebKit, the Linux root Surfer's engine runs from (the
 * linux folder on the system partition).  Older updaters skip them, so a release with
 * WebKit still updates an old system; the updater it installs then sees the
 * WebKit files missing (the version is already current) and fetches them as
 * a hotfix.  Files over the hosting limit are published in pieces,
 * REL.part0 ... REL.part<n-1>, fetched one after the other into one file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http.h"
#include "crypto/ed25519.h"
#include "sha256.h"
#include "icda_sys.h"
#include "../version.h"

#define OTA_BASE      "https://raw.githubusercontent.com/elabbas0/ICDA/ota/"
#define DEV           "/dev/sysupdate"
#define INSTALLED     "/etc/icda-release.txt"
#define MANIFEST_REL  "SYSTEM/etc/icda-release.txt"
#define MAX_FILES     4096
#define TEXT_CAP      (1024 * 1024)
#define CHUNK         (192 * 1024)

#define TICKS_PER_S   100
#define CHECK_EVERY_S (6 * 3600)
#define FIRST_CHECK_S 90
#define CONFIRM_S     30

/* release signing key; the private half never leaves the release machine */
static const uint8_t ota_pubkey[32] = {
    0x34, 0x6d, 0xa6, 0xa5, 0x92, 0x3a, 0x32, 0xfe, 0x25, 0xc4, 0x66, 0x79, 0x54, 0xce, 0xc4, 0xdb,
    0xd2, 0x1a, 0xbd, 0xe8, 0x02, 0x13, 0x3a, 0x1b, 0x91, 0xcb, 0x6a, 0x3c, 0x11, 0xf6, 0x9e, 0x32,
};

typedef struct {
    char     rel[200];
    uint64_t size;
    char     sha[65];
    int      parts;      /* 0: one file; n: published as REL.part0 .. REL.part<n-1> */
    int      webkit;     /* an lfile or wfile line */
    int      packed;     /* a wfile line: its bytes are at pack_off in the WebKit pack */
    uint64_t pack_off;
} entry_t;

typedef struct {
    char     version[24];
    char     notes[160];
    entry_t  files[MAX_FILES];
    int      count;
    char     pack_rel[64];      /* wpack: the WebKit pack, published as REL.part0 .. */
    uint64_t pack_size;
    char     pack_sha[65];
    int      pack_parts;
} manifest_t;

static manifest_t want, have;

/* ---- /dev/sysupdate ------------------------------------------------------- */

static void dev_cmd(const char *cmd) {
    icda_write_file(DEV, cmd, strlen(cmd));
}

static void status(const char *fmt, const char *a, const char *b) {
    char line[160];
    snprintf(line, sizeof(line), "status ");
    snprintf(line + 7, sizeof(line) - 7, fmt, a ? a : "", b ? b : "");
    dev_cmd(line);
}

/* value of "key: value" in the device report */
static int dev_field(const char *key, char *out, size_t cap) {
    char r[1024];
    long n = (long)icda_read_file(DEV, r, sizeof(r) - 1);
    size_t klen = strlen(key);
    if (n <= 0) return -1;
    r[n] = 0;
    for (char *p = r; *p; ) {
        char *e = strchr(p, '\n');
        if (!e) e = p + strlen(p);
        if ((size_t)(e - p) > klen + 1 && strncmp(p, key, klen) == 0 && p[klen] == ':') {
            size_t len = (size_t)(e - p) - klen - 2;
            if (len >= cap) len = cap - 1;
            memcpy(out, p + klen + 2, len);
            out[len] = 0;
            return 0;
        }
        p = *e ? e + 1 : e;
    }
    return -1;
}

/* ---- manifests ------------------------------------------------------------ */

/* splits s at spaces in place into up to max fields; returns how many */
static int fields(char *s, char **f, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        f[n++] = s;
        while (*s && *s != ' ') s++;
    }
    while (*s == ' ') *s++ = 0;
    return *s ? -1 : n;
}

static int parse_manifest(const char *text, manifest_t *m) {
    const char *p = text;
    memset(m, 0, sizeof(*m));
    while (*p) {
        char line[512];
        size_t n = 0;
        while (p[n] && p[n] != '\n') n++;
        if (n < sizeof(line)) {
            memcpy(line, p, n);
            line[n] = 0;
            if (strncmp(line, "version ", 8) == 0) {
                snprintf(m->version, sizeof(m->version), "%s", line + 8);
            } else if (strncmp(line, "notes ", 6) == 0) {
                snprintf(m->notes, sizeof(m->notes), "%s", line + 6);
            } else if (strncmp(line, "wpack ", 6) == 0) {
                /* wpack REL SIZE SHA256 PARTS */
                char *f[4];
                if (fields(line + 6, f, 4) == 4 && strlen(f[2]) == 64 && strlen(f[0]) < sizeof(m->pack_rel) && atoi(f[3]) > 0) {
                    snprintf(m->pack_rel, sizeof(m->pack_rel), "%s", f[0]);
                    m->pack_size = strtoull(f[1], 0, 10);
                    snprintf(m->pack_sha, sizeof(m->pack_sha), "%s", f[2]);
                    m->pack_parts = atoi(f[3]);
                }
            } else if (strncmp(line, "wfile ", 6) == 0 && m->count < MAX_FILES) {
                /* wfile REL SIZE SHA256 OFFSET: a WebKit file inside the pack */
                entry_t *e = &m->files[m->count];
                char *f[4];
                if (fields(line + 6, f, 4) == 4 && strlen(f[2]) == 64 && strlen(f[0]) < sizeof(e->rel)) {
                    snprintf(e->rel, sizeof(e->rel), "%s", f[0]);
                    e->size = strtoull(f[1], 0, 10);
                    snprintf(e->sha, sizeof(e->sha), "%s", f[2]);
                    e->webkit = 1;
                    e->packed = 1;
                    e->pack_off = strtoull(f[3], 0, 10);
                    m->count++;
                }
            } else if ((strncmp(line, "file ", 5) == 0 || strncmp(line, "lfile ", 6) == 0) && m->count < MAX_FILES) {
                /* file REL SIZE SHA256, lfile REL SIZE SHA256 [parts N] */
                entry_t *e = &m->files[m->count];
                int webkit = line[0] == 'l';
                char *rel = line + (webkit ? 6 : 5), *size = strchr(rel, ' '), *sha = 0, *extra = 0;
                if (size) {
                    *size++ = 0;
                    sha = strchr(size, ' ');
                }
                if (sha) {
                    *sha++ = 0;
                    if (webkit && (extra = strchr(sha, ' ')) != 0) *extra++ = 0;
                    if (strlen(sha) == 64 && strlen(rel) < sizeof(e->rel)) {
                        snprintf(e->rel, sizeof(e->rel), "%s", rel);
                        e->size = strtoull(size, 0, 10);
                        snprintf(e->sha, sizeof(e->sha), "%s", sha);
                        e->webkit = webkit;
                        e->parts = extra && strncmp(extra, "parts ", 6) == 0 ? atoi(extra + 6) : 0;
                        m->count++;
                    }
                }
            }
        }
        p += n;
        if (*p == '\n') p++;
    }
    return m->version[0] ? 0 : -1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The signature line is last; it signs every byte before it. */
static int manifest_signed(const char *text, size_t len) {
    const char *sig = 0;
    uint8_t s[64];
    for (size_t i = 0; i + 10 < len; i++) {
        if ((i == 0 || text[i - 1] == '\n') && strncmp(text + i, "signature ", 10) == 0) sig = text + i;
    }
    if (!sig || (size_t)(text + len - sig) < 10 + 128) return 0;
    for (int i = 0; i < 64; i++) {
        int hi = hexval(sig[10 + 2 * i]), lo = hexval(sig[11 + 2 * i]);
        if (hi < 0 || lo < 0) return 0;
        s[i] = (uint8_t)(hi << 4 | lo);
    }
    return ed25519_verify(s, (const uint8_t *)text, (size_t)(sig - text), ota_pubkey);
}

static const entry_t *find(const manifest_t *m, const char *rel) {
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->files[i].rel, rel) == 0) return &m->files[i];
    return 0;
}

static void to_hex(const uint8_t d[32], char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[2 * i] = h[d[i] >> 4];
        out[2 * i + 1] = h[d[i] & 15];
    }
    out[64] = 0;
}

/* compares dotted versions; <0, 0, >0 */
static int vercmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
        if (!*a || !*b) return *a ? 1 : -1;
    }
    return 0;
}

/* ---- downloading ----------------------------------------------------------- */

static uint64_t total_bytes, done_bytes, last_report;

/* The whole update's progress, refreshed every 512 KB (it was counted per
 * file, so the thousands of small WebKit files never moved it off 0%). */
static void progress(void) {
    char pct[8], line[96];
    snprintf(pct, sizeof(pct), "%d", total_bytes ? (int)(done_bytes * 100 / total_bytes) : 0);
    snprintf(line, sizeof(line), "Downloading update %s: %s%% (%llu of %llu MB)", want.version, pct,
             (unsigned long long)(done_bytes >> 20), (unsigned long long)(total_bytes >> 20));
    status("%s%s", line, "");
}

/* Streams one file into /dev/sysupdate ("put REL OFFSET\n" + bytes). */
static int download(const entry_t *e) {
    char url[URL_CAP];
    http_req_t *r;
    sha256_ctx_t ctx;
    uint8_t digest[32];
    char hex[65];
    uint64_t off = 0;
    char *out = (char *)malloc(CHUNK + 256);
    int st;

    if (!out) return -1;
    sha256_init(&ctx);
    st = HTTP_DONE;
    for (int part = 0; part < (e->parts > 0 ? e->parts : 1) && st == HTTP_DONE; part++) {
    if (e->parts > 0) snprintf(url, sizeof(url), "%s%s.part%d", OTA_BASE, e->rel, part);
    else snprintf(url, sizeof(url), "%s%s", OTA_BASE, e->rel);
    r = http_open(url, "GET", 0);
    if (!r) {
        free(out);
        return -1;
    }
    for (;;) {
        st = http_poll(r, 200);
        if (r->headers_done && r->status != 200) {
            st = HTTP_ERROR;
            break;
        }
        while (r->body_len > 0) {
            size_t take = r->body_len < CHUNK ? r->body_len : CHUNK;
            int head = snprintf(out, 256, "put %s %llu\n", e->rel, (unsigned long long)off);
            memcpy(out + head, r->body, take);
            if ((long)icda_write_file(DEV, out, (uint64_t)head + take) < 0) {
                st = HTTP_ERROR;
                break;
            }
            sha256_update(&ctx, r->body, (uint32_t)take);
            off += take;
            done_bytes += take;
            memmove(r->body, r->body + take, r->body_len - take);
            r->body_len -= take;
        }
        if (st != HTTP_PENDING) break;
        if (done_bytes - last_report > 512 * 1024) {
            last_report = done_bytes;
            progress();
        }
    }
    http_free(r);
    }   /* next part */
    free(out);
    if (st != HTTP_DONE || off != e->size) return -1;
    if (e->size == 0) {
        char put[256];
        int head = snprintf(put, sizeof(put), "put %s 0\n", e->rel);
        icda_write_file(DEV, put, (uint64_t)head);
    }
    sha256_final(&ctx, digest);
    to_hex(digest, hex);
    return strcmp(hex, e->sha) == 0 ? 0 : -1;
}

/* Streams the WebKit pack - its files back to back at the offsets their wfile
 * lines give, published as REL.part0, REL.part1 ... - and sends the files
 * marked in need to /dev/sysupdate.  A handful of downloads instead of one
 * HTTPS connection for each of thousands of files. */
static int download_pack(const manifest_t *m, const unsigned char *need) {
    char url[URL_CAP];
    sha256_ctx_t ctx;
    uint8_t digest[32];
    char hex[65];
    uint64_t pos = 0;               /* bytes of the pack seen so far */
    int fi = 0;                     /* the wfile entry pos is in */
    char *out = (char *)malloc(CHUNK + 256);
    int st = HTTP_DONE;

    if (!out) return -1;
    sha256_init(&ctx);
    for (int i = 0; i < m->count; i++) {           /* empty files have no bytes in the pack */
        if (m->files[i].packed && need[i] && m->files[i].size == 0) {
            int head = snprintf(out, 256, "put %s 0\n", m->files[i].rel);
            icda_write_file(DEV, out, (uint64_t)head);
        }
    }
    for (int part = 0; part < m->pack_parts && st == HTTP_DONE; part++) {
        http_req_t *r;
        snprintf(url, sizeof(url), "%s%s.part%d", OTA_BASE, m->pack_rel, part);
        r = http_open(url, "GET", 0);
        if (!r) {
            free(out);
            return -1;
        }
        for (;;) {
            st = http_poll(r, 200);
            if (r->headers_done && r->status != 200) {
                st = HTTP_ERROR;
                break;
            }
            if (r->body_len > 0) {
                const char *b = (const char *)r->body;
                size_t len = r->body_len, used = 0;
                while (used < len) {
                    const entry_t *e;
                    size_t take;
                    while (fi < m->count && (!m->files[fi].packed || m->files[fi].pack_off + m->files[fi].size <= pos)) fi++;
                    if (fi >= m->count || m->files[fi].pack_off > pos) {   /* not the layout the manifest gives */
                        st = HTTP_ERROR;
                        break;
                    }
                    e = &m->files[fi];
                    take = len - used;
                    if (take > e->pack_off + e->size - pos) take = (size_t)(e->pack_off + e->size - pos);
                    if (take > CHUNK) take = CHUNK;
                    if (need[fi]) {
                        int head = snprintf(out, 256, "put %s %llu\n", e->rel, (unsigned long long)(pos - e->pack_off));
                        memcpy(out + head, b + used, take);
                        if ((long)icda_write_file(DEV, out, (uint64_t)head + take) < 0) {
                            st = HTTP_ERROR;
                            break;
                        }
                    }
                    used += take;
                    pos += take;
                }
                sha256_update(&ctx, (const uint8_t *)b, (uint32_t)used);
                done_bytes += used;
                r->body_len = 0;
            }
            if (st != HTTP_PENDING) break;
            if (done_bytes - last_report > 512 * 1024) {
                last_report = done_bytes;
                progress();
            }
        }
        http_free(r);
    }
    free(out);
    if (st != HTTP_DONE || pos != m->pack_size) return -1;
    sha256_final(&ctx, digest);
    to_hex(digest, hex);
    return strcmp(hex, m->pack_sha) == 0 ? 0 : -1;
}

/* ---- one check ------------------------------------------------------------- */

static char manifest_text[TEXT_CAP];
static unsigned char pack_need[MAX_FILES];      /* wfile entries to install */
static int packed_need;

static void check(void) {
    char pending[24], bad[24], supported[4];
    http_req_t *r;
    char final_url[URL_CAP];
    size_t mlen;
    char *installed;
    char *commit;
    size_t cap, used;
    int need = 0;
    int hotfix = 0;                 /* same version, WebKit missing */

    if (dev_field("supported", supported, sizeof(supported)) != 0 || supported[0] != '1') {
        status("Updates are not available on this installation%s%s", 0, 0);
        return;
    }
    status("Checking for updates...%s%s", 0, 0);
    r = http_get(OTA_BASE "manifest.txt", final_url, sizeof(final_url));
    if (!r || r->state != HTTP_DONE || r->status != 200 || !r->body || r->body_len >= sizeof(manifest_text)) {
        status("Could not reach the update server%s%s", r && r->error[0] ? ": " : "", r ? r->error : "");
        if (r) http_free(r);
        return;
    }
    mlen = r->body_len;
    memcpy(manifest_text, r->body, mlen);
    manifest_text[mlen] = 0;
    http_free(r);
    if (!manifest_signed(manifest_text, mlen) || parse_manifest(manifest_text, &want) != 0) {
        status("Rejected an update with a bad signature%s%s", 0, 0);
        return;
    }

    installed = (char *)malloc(TEXT_CAP);
    memset(&have, 0, sizeof(have));
    if (installed) {
        long n = (long)icda_read_file(INSTALLED, installed, TEXT_CAP - 1);
        if (n > 0) {
            installed[n] = 0;
            (void)parse_manifest(installed, &have);
        }
        free(installed);
    }
    /* installed from the ISO, no release manifest yet: this build's version
     * (an older release on the server was offered as an update before) */
    if (!have.version[0]) snprintf(have.version, sizeof(have.version), "%s", ICDA_VERSION_STRING);
    if (have.version[0] && vercmp(want.version, have.version) < 0) {
        status("ICDA %s is up to date%s", have.version, "");
        return;
    }
    if (have.version[0] && vercmp(want.version, have.version) == 0) {
        /* same version: only WebKit can be missing (installed by an older
         * updater, which skips it) - fetched as a hotfix */
        int missing = 0;
        for (int i = 0; i < want.count && !missing; i++) {
            const entry_t *old;
            if (!want.files[i].webkit) continue;
            old = find(&have, want.files[i].rel);
            if (!old || strcmp(old->sha, want.files[i].sha) != 0) missing = 1;
        }
        if (!missing) {
            status("ICDA %s is up to date%s", have.version, "");
            return;
        }
        hotfix = 1;
    }
    if (dev_field("bad", bad, sizeof(bad)) == 0 && strcmp(bad, want.version) == 0) {
        status("Update %s was undone after a failed start%s", want.version, "");
        return;
    }
    if (dev_field("pending", pending, sizeof(pending)) == 0 && strcmp(pending, want.version) == 0) {
        status("Update %s is ready: restart to install%s", want.version, "");
        return;
    }

    /* only what changed */
    total_bytes = done_bytes = last_report = 0;
    packed_need = 0;
    for (int i = 0; i < want.count; i++) {
        const entry_t *old = find(&have, want.files[i].rel);
        pack_need[i] = 0;
        if (hotfix && !want.files[i].webkit) continue;
        if (old && strcmp(old->sha, want.files[i].sha) == 0) continue;
        if (want.files[i].packed) pack_need[i] = 1, packed_need = 1;
        else total_bytes += want.files[i].size;
    }
    if (packed_need) total_bytes += want.pack_size;      /* the pack comes whole */
    cap = 512 + (size_t)want.count * 300 + (size_t)have.count * 220;
    commit = (char *)malloc(cap);
    if (!commit) return;
    used = (size_t)snprintf(commit, cap, "commit\nversion %s\n", want.version);
    for (int i = 0; i < want.count; i++) {
        const entry_t *e = &want.files[i];
        const entry_t *old = find(&have, e->rel);
        if (old && strcmp(old->sha, e->sha) == 0) continue;
        if (hotfix && !e->webkit) continue;
        if (e->packed) continue;                            /* from the pack, below */
        if (download(e) != 0) {
            status("Download of update %s failed (%s)", want.version, e->rel);
            free(commit);
            return;
        }
        used += (size_t)snprintf(commit + used, cap - used, "file %s %s\n", e->rel, e->sha);
        need++;
    }
    if (packed_need) {
        if (!want.pack_parts || download_pack(&want, pack_need) != 0) {
            status("Download of update %s failed (%s)", want.version, want.pack_rel[0] ? want.pack_rel : "WebKit");
            free(commit);
            return;
        }
        for (int i = 0; i < want.count; i++) {
            if (!pack_need[i]) continue;
            used += (size_t)snprintf(commit + used, cap - used, "file %s %s\n", want.files[i].rel, want.files[i].sha);
            need++;
        }
    }
    for (int i = 0; i < have.count && !hotfix; i++) {
        if (!find(&want, have.files[i].rel))
            used += (size_t)snprintf(commit + used, cap - used, "remove %s\n", have.files[i].rel);
    }
    /* the manifest itself becomes the installed manifest */
    {
        char put_head[256];
        uint8_t digest[32];
        char hex[65];
        char *buf = (char *)malloc(mlen + 256);
        int head = snprintf(put_head, sizeof(put_head), "put %s 0\n", MANIFEST_REL);
        if (!buf) {
            free(commit);
            return;
        }
        memcpy(buf, put_head, (size_t)head);
        memcpy(buf + head, manifest_text, mlen);
        icda_write_file(DEV, buf, (uint64_t)head + mlen);
        free(buf);
        sha256_hash((const uint8_t *)manifest_text, (uint32_t)mlen, digest);
        to_hex(digest, hex);
        used += (size_t)snprintf(commit + used, cap - used, "file %s %s\n", MANIFEST_REL, hex);
    }
    if ((long)icda_write_file(DEV, commit, used) < 0) {
        status("Update %s did not pass the final check%s", want.version, "");
    } else {
        char line[96];
        snprintf(line, sizeof(line), "Update %s is ready: restart to install", want.version);
        status("%s%s", line, "");
    }
    free(commit);
    (void)need;
}

int main(void) {
    uint64_t start = icda_ticks();
    uint64_t next_check = start + FIRST_CHECK_S * TICKS_PER_S;
    int confirmed = 0;

    status("Waiting to check for updates%s%s", 0, 0);
    for (;;) {
        uint64_t now = icda_ticks();
        char flag[4];
        if (!confirmed && now - start >= CONFIRM_S * TICKS_PER_S) {
            /* the desktop has been up for a while: a fresh patch is good */
            dev_cmd("confirm");
            confirmed = 1;
        }
        if (now >= next_check || (dev_field("check", flag, sizeof(flag)) == 0 && flag[0] == '1')) {
            check();
            next_check = icda_ticks() + (uint64_t)CHECK_EVERY_S * TICKS_PER_S;
        }
        icda_sleep(TICKS_PER_S);
    }
    return 0;
}
