/* The PE loader: Windows programs and DLLs (PE32+, x86-64).
 *
 * A module is mapped at its preferred base when that is free, else anywhere
 * and relocated.  Its imports come from builtin DLLs (dll_*.c) for the
 * system's DLLs, and from DLL files next to the program (or in the C:
 * drive's System32) for the rest.  A function a builtin lacks becomes a
 * stub that logs its name and returns 0, so a program loads and fails only
 * if it really needs the function; GetProcAddress, though, says NULL, so
 * programs that look for optional functions fall back as on older Windows.
 *
 * Static TLS (__declspec(thread), and the TLS callbacks mingw uses) gets a
 * block per module per thread through TEB.ThreadLocalStoragePointer. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define DIR_EXPORT    0
#define DIR_IMPORT    1
#define DIR_RESOURCE  2
#define DIR_BASERELOC 5
#define DIR_TLS       9

typedef struct {
    uint32_t sig;
    uint16_t machine, nsections;
    uint32_t timestamp, symtab, nsyms;
    uint16_t opt_size, characteristics;
    /* optional header (PE32+) */
    uint16_t magic;
    uint8_t  lmajor, lminor;
    uint32_t code_size, idata_size, udata_size, entry, code_base;
    uint64_t image_base;
    uint32_t sect_align, file_align;
    uint16_t os_major, os_minor, img_major, img_minor, sub_major, sub_minor;
    uint32_t win32_ver, image_size, headers_size, checksum;
    uint16_t subsystem, dll_chars;
    uint64_t stack_reserve, stack_commit, heap_reserve, heap_commit;
    uint32_t loader_flags, ndirs;
    struct { uint32_t rva, size; } dir[16];
} __attribute__((packed)) nt_headers_t;

typedef struct {
    char     name[8];
    uint32_t vsize, va, raw_size, raw_ptr, reloc_ptr, line_ptr;
    uint16_t nrelocs, nlines;
    uint32_t chars;
} __attribute__((packed)) section_t;

typedef struct {
    uint32_t orig_thunk, timestamp, forwarder, name, first_thunk;
} __attribute__((packed)) import_desc_t;

typedef struct {
    uint32_t chars, timestamp;
    uint16_t major, minor;
    uint32_t name, base, nfuncs, nnames, funcs, names, ordinals;
} __attribute__((packed)) export_dir_t;

typedef struct {
    uint64_t start, end, index_addr, callbacks;
    uint32_t zero_fill, chars;
} __attribute__((packed)) tls_dir_t;

typedef BOOL (WCALLBACK *dllmain_t)(HINSTANCE, DWORD, void *);
typedef void (WCALLBACK *tls_cb_t)(void *, DWORD, void *);

static wmodule_t *modules;            /* most recent first */
static wmodule_t *main_mod;
static pthread_mutex_t ldlock;        /* recursive: loading a module loads its imports */

__attribute__((constructor)) static void ldlock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&ldlock, &a);
}
static wmodule_t *init_order[256];    /* DLLs in the order to start them */
static int ninit;

/* TLS: modules with static TLS, by index.  The program's own is index 0,
 * as on Windows: compilers read an .exe's TLS block straight from slot 0
 * (gs:[0x58] -> [0]) without looking at _tls_index. */
static wmodule_t *tls_mods[64];
static int ntls = 1;

wmodule_t *ld_modules(void) { return modules; }
wmodule_t *ld_main(void) { return main_mod; }

static nt_headers_t *nt_of(uint8_t *base) {
    return (nt_headers_t *)(base + *(uint32_t *)(base + 0x3C));
}

wmodule_t *ld_by_handle(HMODULE h) {
    if (!h) return main_mod;
    for (wmodule_t *m = modules; m; m = m->next)
        if ((uint8_t *)h == m->base) return m;
    return 0;
}

static void base_name(const char *name, char *out, size_t cap) {
    const char *b = name;
    size_t n;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/') b = p + 1;
    snprintf(out, cap, "%s", b);
    a_lower(out);
    n = strlen(out);
    if (!strchr(out, '.') && n + 4 < cap) strcat(out, ".dll");
}

wmodule_t *ld_by_name(const char *name) {
    char b[64];
    base_name(name, b, sizeof(b));
    for (wmodule_t *m = modules; m; m = m->next)
        if (!strcmp(m->name, b)) return m;
    /* api sets and aliases are found under the DLL that has them */
    {
        const wdll_t *d = builtin_find(b);
        if (d)
            for (wmodule_t *m = modules; m; m = m->next)
                if (m->builtin == d) return m;
    }
    return 0;
}

/* ---- stubs for what a builtin lacks ------------------------------------------------- */

typedef struct {
    char name[96];
    int  calls;
} stub_info_t;

WINAPI void *stub_hit(stub_info_t *s) {
    if (s->calls++ < 3 || wtrace >= 2) wlog("unimplemented: %s", s->name);
    return 0;
}

/* r10 = the stub's info; called the Windows way, returns 0 */
__asm__(".text\n"
        ".globl stub_entry\n"
        "stub_entry:\n"
        "  sub $40, %rsp\n"
        "  mov %r10, %rcx\n"
        "  call stub_hit\n"
        "  add $40, %rsp\n"
        "  xor %eax, %eax\n"
        "  ret\n");
extern char stub_entry[];

static uint8_t *stub_page;
static int stub_used;

static void *make_stub(const char *dll, const char *name) {
    stub_info_t *s;
    uint8_t *c;
    if (!stub_page || stub_used + 32 > 65536) {
        stub_page = mmap(0, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stub_page == MAP_FAILED) return 0;
        stub_used = 0;
    }
    s = calloc(1, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s!%s", dll, name);
    c = stub_page + stub_used;
    stub_used += 32;
    c[0] = 0x49, c[1] = 0xBA;                  /* movabs r10, s */
    memcpy(c + 2, &s, 8);
    c[10] = 0x48, c[11] = 0xB8;                /* movabs rax, stub_entry */
    {
        void *e = stub_entry;
        memcpy(c + 12, &e, 8);
    }
    c[20] = 0xFF, c[21] = 0xE0;                /* jmp rax */
    return c;
}

/* ---- builtin modules ------------------------------------------------------------------
 * A builtin has a small fake image, so that its handle points at "MZ" and a
 * PE header as programs that look expect. */

static wmodule_t *builtin_module(const wdll_t *d, const char *as_name) {
    wmodule_t *m = calloc(1, sizeof(*m));
    uint8_t *img = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    nt_headers_t *nt;
    if (!m || img == MAP_FAILED) return 0;
    img[0] = 'M', img[1] = 'Z';
    *(uint32_t *)(img + 0x3C) = 0x40;
    nt = (nt_headers_t *)(img + 0x40);
    nt->sig = 0x4550;
    nt->machine = 0x8664;
    nt->opt_size = 240;
    nt->characteristics = 0x2022;
    nt->magic = 0x20B;
    nt->image_size = 4096;
    nt->headers_size = 4096;
    nt->image_base = (uint64_t)(uintptr_t)img;
    nt->ndirs = 16;
    m->base = img;
    m->size = 4096;
    m->builtin = d;
    m->refs = 1;
    m->tls_index = -1;
    m->inited = 1;
    snprintf(m->name, sizeof(m->name), "%s", as_name);
    m->next = modules;
    modules = m;
    return m;
}

/* ---- exports ---------------------------------------------------------------------------- */

static void *export_lookup(wmodule_t *m, const char *name, int ordinal, int depth);

static void *forward(const char *fwd, int depth) {
    char dll[128], fn[128];
    const char *dot = strrchr(fwd, '.');
    wmodule_t *m;
    int err = 0;
    if (!dot || depth > 4 || (size_t)(dot - fwd) >= sizeof(dll)) return 0;
    memcpy(dll, fwd, (size_t)(dot - fwd));
    dll[dot - fwd] = 0;
    snprintf(fn, sizeof(fn), "%s", dot + 1);
    m = ld_load(dll, 0, &err);
    if (!m) return 0;
    if (fn[0] == '#') return export_lookup(m, 0, atoi(fn + 1), depth + 1);
    return export_lookup(m, fn, 0, depth + 1);
}

static void *export_lookup(wmodule_t *m, const char *name, int ordinal, int depth) {
    nt_headers_t *nt;
    export_dir_t *ed;
    uint32_t *funcs, *names;
    uint16_t *ords;
    uint32_t rva = 0;
    if (m->builtin) return builtin_proc(m->builtin, name, ordinal);
    nt = nt_of(m->base);
    if (!nt->dir[DIR_EXPORT].rva) return 0;
    ed = (export_dir_t *)(m->base + nt->dir[DIR_EXPORT].rva);
    funcs = (uint32_t *)(m->base + ed->funcs);
    names = (uint32_t *)(m->base + ed->names);
    ords = (uint16_t *)(m->base + ed->ordinals);
    if (name) {
        /* the names are sorted: binary search */
        int lo = 0, hi = (int)ed->nnames - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2, c = strcmp(name, (const char *)(m->base + names[mid]));
            if (!c) {
                rva = funcs[ords[mid]];
                break;
            }
            if (c < 0) hi = mid - 1;
            else lo = mid + 1;
        }
    } else if (ordinal >= (int)ed->base && (uint32_t)(ordinal - (int)ed->base) < ed->nfuncs) {
        rva = funcs[ordinal - (int)ed->base];
    }
    if (!rva) return 0;
    if (rva >= nt->dir[DIR_EXPORT].rva && rva < nt->dir[DIR_EXPORT].rva + nt->dir[DIR_EXPORT].size)
        return forward((const char *)(m->base + rva), depth);
    return m->base + rva;
}

void *ld_proc(wmodule_t *m, const char *name, int ordinal) {
    if (!m) return 0;
    return export_lookup(m, name, ordinal, 0);
}

/* ---- loading a PE file --------------------------------------------------------------------- */

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* where a DLL file is: next to whoever asks, the program's folder, the current
 * folder, then C:\Windows\System32 */
static int find_dll(const char *name, const char *referrer_dir, char *out, size_t cap) {
    char dirs[4][1024];
    int nd = 0;
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, ':')) {
        if (path_to_unix(name, out, cap) == 0 && file_exists(out)) return 0;
        return -1;
    }
    if (referrer_dir && *referrer_dir) snprintf(dirs[nd++], sizeof(dirs[0]), "%s", referrer_dir);
    if (main_mod) {
        char *s;
        snprintf(dirs[nd], sizeof(dirs[0]), "%s", main_mod->path);
        s = strrchr(dirs[nd], '/');
        if (s) *s = 0, nd++;
    }
    snprintf(dirs[nd++], sizeof(dirs[0]), ".");
    snprintf(dirs[nd++], sizeof(dirs[0]), "%s/Windows/System32", path_prefix());
    for (int i = 0; i < nd; i++) {
        snprintf(out, cap, "%s/%s", dirs[i], name);
        if (file_exists(out)) return 0;
        /* FAT folders are case-blind, Linux ones not: try lower case */
        {
            char low[128];
            snprintf(low, sizeof(low), "%s", name);
            a_lower(low);
            snprintf(out, cap, "%s/%s", dirs[i], low);
            if (file_exists(out)) return 0;
        }
    }
    return -1;
}

static uint8_t *read_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    uint8_t *buf;
    size_t got = 0;
    if (fd < 0) return 0;
    if (fstat(fd, &st) != 0 || st.st_size < 0x200) {
        close(fd);
        return 0;
    }
    buf = malloc((size_t)st.st_size);
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size) {
        free(buf);
        return 0;
    }
    *size = got;
    return buf;
}

static int relocate(uint8_t *base, nt_headers_t *nt, int64_t delta) {
    uint8_t *p, *end;
    if (!delta) return 0;
    if (!nt->dir[DIR_BASERELOC].rva || (nt->characteristics & 1)) return -1;   /* RELOCS_STRIPPED */
    p = base + nt->dir[DIR_BASERELOC].rva;
    end = p + nt->dir[DIR_BASERELOC].size;
    while (p + 8 <= end) {
        uint32_t page = *(uint32_t *)p, bsize = *(uint32_t *)(p + 4);
        uint16_t *e = (uint16_t *)(p + 8);
        if (bsize < 8) break;
        for (uint32_t i = 0; i < (bsize - 8) / 2; i++) {
            int type = e[i] >> 12, off = e[i] & 0xFFF;
            uint8_t *at = base + page + off;
            if (type == 10) *(uint64_t *)at += (uint64_t)delta;
            else if (type == 3) *(uint32_t *)at += (uint32_t)delta;
            else if (type == 2) *(uint16_t *)at += (uint16_t)(delta >> 16);
            else if (type == 1) *(uint16_t *)at += (uint16_t)delta;
        }
        p += bsize;
    }
    return 0;
}

static int load_deps(wmodule_t *m);
static int check_mode;      /* ICDA_WIN_CHECK: list what is missing, run nothing */

static wmodule_t *load_pe(const char *path, const char *as_name, int *err, int is_main) {
    size_t fsize = 0;
    uint8_t *file = read_file(path, &fsize), *base;
    nt_headers_t *nt;
    section_t *sec;
    wmodule_t *m;
    if (!file) {
        *err = ERROR_FILE_NOT_FOUND;
        return 0;
    }
    if (file[0] != 'M' || file[1] != 'Z' || *(uint32_t *)(file + 0x3C) + sizeof(nt_headers_t) > fsize) {
        free(file);
        *err = 193;   /* ERROR_BAD_EXE_FORMAT */
        return 0;
    }
    nt = (nt_headers_t *)(file + *(uint32_t *)(file + 0x3C));
    if (nt->sig != 0x4550 || nt->machine != 0x8664 || nt->magic != 0x20B) {
        free(file);
        *err = 193;
        return 0;
    }
    /* the preferred base, else anywhere (then relocated) */
    base = mmap((void *)(uintptr_t)nt->image_base, nt->image_size, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED || base != (uint8_t *)(uintptr_t)nt->image_base) {
        if (base != MAP_FAILED) munmap(base, nt->image_size);
        base = mmap(0, nt->image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            free(file);
            *err = ERROR_NOT_ENOUGH_MEMORY;
            return 0;
        }
    }
    memcpy(base, file, nt->headers_size < fsize ? nt->headers_size : fsize);
    sec = (section_t *)((uint8_t *)&nt->magic + nt->opt_size);
    for (int i = 0; i < nt->nsections; i++) {
        uint32_t n = sec[i].raw_size;
        if (sec[i].va >= nt->image_size) continue;
        if (n > sec[i].vsize && sec[i].vsize) n = sec[i].vsize;
        if (sec[i].va + n > nt->image_size) n = nt->image_size - sec[i].va;
        if (sec[i].raw_ptr && n && sec[i].raw_ptr + n <= fsize) memcpy(base + sec[i].va, file + sec[i].raw_ptr, n);
    }
    free(file);
    nt = nt_of(base);
    if (relocate(base, nt, (int64_t)((uintptr_t)base - nt->image_base)) != 0) {
        munmap(base, nt->image_size);
        *err = 193;
        wlog("%s cannot be moved from its base (no relocations)", path);
        return 0;
    }
    m = calloc(1, sizeof(*m));
    m->base = base;
    m->size = nt->image_size;
    m->refs = 1;
    m->tls_index = -1;
    m->entry = nt->entry ? base + nt->entry : 0;
    snprintf(m->path, sizeof(m->path), "%s", path);
    snprintf(m->name, sizeof(m->name), "%s", as_name);
    m->next = modules;
    modules = m;
    if (wtrace) wlog("loaded %s at %p", m->name, (void *)base);
    if (load_deps(m) != 0) {
        *err = ERROR_MOD_NOT_FOUND;
        return 0;
    }
    /* the sections' own protections, now that imports are written (only
     * for page-aligned sections) */
    if (nt->sect_align >= 4096) {
        section_t *s = (section_t *)((uint8_t *)&nt->magic + nt->opt_size);
        for (int i = 0; i < nt->nsections; i++) {
            int prot = 0;
            uint32_t len = (s[i].vsize > s[i].raw_size ? s[i].vsize : s[i].raw_size);
            len = (len + 4095) & ~4095U;
            if (!len) continue;
            if (s[i].chars & 0x40000000) prot |= PROT_READ;
            if (s[i].chars & 0x80000000) prot |= PROT_WRITE | PROT_READ;
            if (s[i].chars & 0x20000000) prot |= PROT_EXEC | PROT_READ;
            if (prot) mprotect(base + s[i].va, len, prot);
        }
    }
    /* static TLS */
    if (nt->dir[DIR_TLS].rva && (is_main || ntls < 64)) {
        tls_dir_t *td = (tls_dir_t *)(base + nt->dir[DIR_TLS].rva);
        m->tls_index = is_main ? 0 : ntls++;
        tls_mods[m->tls_index] = m;
        if (td->index_addr) *(uint32_t *)(uintptr_t)td->index_addr = (uint32_t)m->tls_index;
    }
    if (ninit < 256) init_order[ninit++] = m;   /* after its dependencies, which loaded first */
    return m;
}

static int load_deps(wmodule_t *m) {
    nt_headers_t *nt = nt_of(m->base);
    import_desc_t *d;
    char dir[512], *s;
    snprintf(dir, sizeof(dir), "%s", m->path);
    s = strrchr(dir, '/');
    if (s) *s = 0;
    if (!nt->dir[DIR_IMPORT].rva) return 0;
    for (d = (import_desc_t *)(m->base + nt->dir[DIR_IMPORT].rva); d->name; d++) {
        const char *dname = (const char *)(m->base + d->name);
        uint64_t *iat = (uint64_t *)(m->base + d->first_thunk);
        uint64_t *names = (uint64_t *)(m->base + (d->orig_thunk ? d->orig_thunk : d->first_thunk));
        int err = 0;
        wmodule_t *dep = ld_load(dname, dir, &err);
        if (!dep) {
            wnotice("This program cannot start", "");
            wfatal("Windows program", "%s needs %s, which was not found next to it.", m->name, dname);
        }
        for (int i = 0; names[i]; i++) {
            void *fn;
            char label[96];
            if (names[i] >> 63) {
                int ord = (int)(names[i] & 0xFFFF);
                fn = ld_proc(dep, 0, ord);
                snprintf(label, sizeof(label), "#%d", ord);
            } else {
                const char *fname = (const char *)(m->base + (uint32_t)names[i] + 2);
                fn = ld_proc(dep, fname, 0);
                snprintf(label, sizeof(label), "%s", fname);
            }
            if (!fn) {
                if (!dep->builtin || check_mode) wlog("%s: %s has no %s", m->name, dname, label);
                fn = make_stub(dep->name, label);
            }
            iat[i] = (uint64_t)(uintptr_t)fn;
        }
    }
    return 0;
}

/* DLLs every Windows program can count on: builtin even when not
 * implemented, so programs that only import from them still load */
static const char *const system_dlls[] = {
    "kernelbase.dll", "dsound.dll", "dinput.dll", "dinput8.dll", "xinput1_3.dll", "xinput1_4.dll",
    "xinput9_1_0.dll", "dbghelp.dll", "comctl32.dll", "comdlg32.dll", "uxtheme.dll", "dwmapi.dll", "hid.dll",
    "cfgmgr32.dll", "powrprof.dll", "psapi.dll", "iphlpapi.dll", "crypt32.dll", "bcrypt.dll", "secur32.dll",
    "wininet.dll", "winhttp.dll", "normaliz.dll", "wldap32.dll", "shlwapi.dll", "mswsock.dll", "wsock32.dll",
    "d3d9.dll", "d3d11.dll", "dxgi.dll", "d3dcompiler_47.dll", "glu32.dll", "msimg32.dll", "winspool.drv",
    "userenv.dll", "wtsapi32.dll", "rpcrt4.dll", "msvcp140.dll", "ntdll.dll", "avrt.dll", "mmdevapi.dll",
    "propsys.dll", "shcore.dll", "dnsapi.dll", "credui.dll", "netapi32.dll", "urlmon.dll", "ncrypt.dll",
    "wintrust.dll", "msi.dll", "mpr.dll", "oleacc.dll", "sspicli.dll", "wevtapi.dll", "taskschd.dll", "dwrite.dll",
    "d2d1.dll", "dcomp.dll", "uiautomationcore.dll", "ntdsapi.dll", "rstrtmgr.dll", "winsta.dll", "dhcpcsvc.dll",
    "fwpuclnt.dll", "wlanapi.dll", "msimg32.dll", "usp10.dll", "imagehlp.dll", "dxva2.dll", "mf.dll", "mfplat.dll",
    "mfreadwrite.dll", "d3d12.dll", "xinput1_2.dll", "xinput1_1.dll", "winusb.dll", "bluetoothapis.dll", 0
};

static int is_system_dll(const char *n) {
    for (int i = 0; system_dlls[i]; i++)
        if (!strcmp(n, system_dlls[i])) return 1;
    return 0;
}

wmodule_t *ld_load(const char *name, const char *referrer_dir, int *err) {
    char b[64], path[1024];
    wmodule_t *m;
    const wdll_t *d;
    *err = 0;
    base_name(name, b, sizeof(b));
    pthread_mutex_lock(&ldlock);
    m = ld_by_name(b);
    if (m) {
        m->refs++;
        pthread_mutex_unlock(&ldlock);
        return m;
    }
    /* the system's DLLs are always builtin; others are looked for on disk
     * first, so a program's own DLLs win */
    d = builtin_find(b);
    if (!d || is_system_dll(b) == 0) {
        if (!d && find_dll(name, referrer_dir, path, sizeof(path)) == 0) {
            m = load_pe(path, b, err, 0);
            pthread_mutex_unlock(&ldlock);
            return m;
        }
    }
    if (!d && is_system_dll(b)) d = &dll_misc;
    if (d) {
        m = builtin_module(d, b);
        pthread_mutex_unlock(&ldlock);
        return m;
    }
    pthread_mutex_unlock(&ldlock);
    *err = ERROR_MOD_NOT_FOUND;
    return 0;
}

/* ---- TLS ---------------------------------------------------------------------------------- */

void teb_set_tls_pointer(void **blocks);
void **thread_tls_blocks(wthread_t *t);
void thread_set_tls_blocks(wthread_t *t, void **b);

static void *tls_block(wmodule_t *m) {
    tls_dir_t *td = (tls_dir_t *)(m->base + nt_of(m->base)->dir[DIR_TLS].rva);
    size_t data = (size_t)(td->end - td->start), total = data + td->zero_fill;
    uint8_t *b = calloc(1, total + 16);
    if (b && data) memcpy(b, (void *)(uintptr_t)td->start, data);
    return b;
}

void tls_thread_blocks(wthread_t *t) {
    void **blocks = calloc(64, sizeof(void *));
    for (int i = 0; i < ntls; i++) blocks[i] = tls_mods[i] ? tls_block(tls_mods[i]) : 0;
    thread_set_tls_blocks(t, blocks);
    teb_set_tls_pointer(blocks);
}

static void tls_callbacks(wmodule_t *m, DWORD reason) {
    nt_headers_t *nt = nt_of(m->base);
    tls_dir_t *td;
    if (!nt->dir[DIR_TLS].rva) return;
    td = (tls_dir_t *)(m->base + nt->dir[DIR_TLS].rva);
    if (!td->callbacks) return;
    for (tls_cb_t *cb = (tls_cb_t *)(uintptr_t)td->callbacks; *cb; cb++) (*cb)(m->base, reason, 0);
}

/* ---- starting DLLs ----------------------------------------------------------------------- */

static int init_module(wmodule_t *m) {
    nt_headers_t *nt;
    if (m->inited || m->builtin) return 0;
    m->inited = 1;
    nt = nt_of(m->base);
    tls_callbacks(m, 1);
    if (m != main_mod && m->entry && (nt->characteristics & 0x2000)) {      /* IMAGE_FILE_DLL */
        if (wtrace) wlog("DllMain %s", m->name);
        if (!((dllmain_t)m->entry)((HINSTANCE)m->base, 1, 0)) {
            wlog("%s did not start (DllMain said no)", m->name);
            return -1;
        }
    }
    return 0;
}

int ld_init_all(void) {
    int rc = 0;
    /* the TLS blocks of the calling thread, for modules loaded since */
    {
        wthread_t *t = thread_current();
        void **b = thread_tls_blocks(t);
        if (!b) tls_thread_blocks(t);
        else
            for (int i = 0; i < ntls; i++)
                if (!b[i] && tls_mods[i]) b[i] = tls_block(tls_mods[i]);
    }
    for (int i = 0; i < ninit; i++)
        if (init_order[i] != main_mod && init_module(init_order[i]) != 0) rc = -1;
    return rc;
}

void ld_thread_attach(int attach) {
    for (int i = 0; i < ninit; i++) {
        wmodule_t *m = init_order[i];
        nt_headers_t *nt;
        if (m->builtin || !m->inited) continue;
        nt = nt_of(m->base);
        tls_callbacks(m, attach ? 2 : 3);
        if (m != main_mod && m->entry && (nt->characteristics & 0x2000) && !(nt->dll_chars & 0))
            ((dllmain_t)m->entry)((HINSTANCE)m->base, attach ? 2 : 3, 0);
    }
}

/* ---- resources ------------------------------------------------------------------------------ */

/* (naturally aligned: no packing needed) */
typedef struct {
    uint32_t chars, timestamp;
    uint16_t major, minor, nnamed, nids;
} res_dir_t;

typedef struct {
    uint32_t name, offset;
} res_entry_t;

static int res_match(uint8_t *rbase, res_entry_t *e, const WCHAR *want) {
    if ((uintptr_t)want < 0x10000) return !(e->name & 0x80000000) && (e->name & 0xFFFF) == (uintptr_t)want;
    if (!(e->name & 0x80000000)) {
        /* "#123" */
        if (want[0] == '#') {
            int v = 0;
            for (const WCHAR *p = want + 1; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
            return (int)(e->name & 0xFFFF) == v;
        }
        return 0;
    }
    {
        uint16_t *s = (uint16_t *)(rbase + (e->name & 0x7FFFFFFF));
        uint16_t n = s[0];
        size_t i;
        for (i = 0; i < n && want[i]; i++) {
            WCHAR a = s[1 + i], b = want[i];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) return 0;
        }
        return i == n && !want[i];
    }
}

static res_dir_t *res_find(uint8_t *rbase, res_dir_t *dir, const WCHAR *want, int any) {
    res_entry_t *e = (res_entry_t *)(dir + 1);
    for (int i = 0; i < dir->nnamed + dir->nids; i++) {
        if (any || res_match(rbase, &e[i], want)) {
            if (e[i].offset & 0x80000000) return (res_dir_t *)(rbase + (e[i].offset & 0x7FFFFFFF));
            return (res_dir_t *)(rbase + e[i].offset);     /* a data entry */
        }
    }
    return 0;
}

void *ld_resource(wmodule_t *m, const WCHAR *type, const WCHAR *name, DWORD *size) {
    nt_headers_t *nt;
    uint8_t *rbase;
    res_dir_t *d;
    uint32_t *data;
    if (!m || m->builtin) return 0;
    nt = nt_of(m->base);
    if (!nt->dir[DIR_RESOURCE].rva) return 0;
    rbase = m->base + nt->dir[DIR_RESOURCE].rva;
    d = res_find(rbase, (res_dir_t *)rbase, type, 0);
    if (!d) return 0;
    d = res_find(rbase, d, name, 0);
    if (!d) return 0;
    data = (uint32_t *)(void *)res_find(rbase, d, 0, 1);       /* any language */
    if (!data) return 0;
    if (size) *size = data[1];
    return m->base + data[0];
}

/* the names (or the types, with type 0) a module has resources of: cb gets
 * each, an id as a small number or a string; it returns 0 to stop */
int ld_enum_resources(wmodule_t *m, const WCHAR *type, int (*cb)(void *ctx, const WCHAR *name), void *ctx) {
    nt_headers_t *nt;
    uint8_t *rbase;
    res_dir_t *d;
    res_entry_t *e;
    int n = 0;
    if (!m || m->builtin) return 0;
    nt = nt_of(m->base);
    if (!nt->dir[DIR_RESOURCE].rva) return 0;
    rbase = m->base + nt->dir[DIR_RESOURCE].rva;
    d = (res_dir_t *)rbase;
    if (type) {
        d = res_find(rbase, d, type, 0);
        if (!d) return 0;
    }
    e = (res_entry_t *)(d + 1);
    for (int i = 0; i < d->nnamed + d->nids; i++) {
        WCHAR name[256];
        const WCHAR *arg;
        if (e[i].name & 0x80000000) {
            uint16_t *s = (uint16_t *)(rbase + (e[i].name & 0x7FFFFFFF));
            int len = s[0] < 255 ? s[0] : 255;
            memcpy(name, s + 1, (size_t)len * 2);
            name[len] = 0;
            arg = name;
        } else {
            arg = (const WCHAR *)(uintptr_t)(e[i].name & 0xFFFF);
        }
        n++;
        if (!cb(ctx, arg)) break;
    }
    return n;
}

/* a resource of a PE file on disk, without loading it (version.dll's
 * GetFileVersionInfo): a malloc'ed copy, 0 if none */
void *pe_file_resource(const char *upath, const WCHAR *type, const WCHAR *name, DWORD *size) {
    size_t fsize = 0;
    uint8_t *file = read_file(upath, &fsize), *img = 0, *res;
    nt_headers_t *nt;
    section_t *sec;
    void *out = 0;
    if (!file) return 0;
    if (file[0] != 'M' || file[1] != 'Z' || *(uint32_t *)(file + 0x3C) + sizeof(nt_headers_t) > fsize) goto done;
    nt = (nt_headers_t *)(file + *(uint32_t *)(file + 0x3C));
    if (nt->sig != 0x4550 || nt->magic != 0x20B || !nt->dir[DIR_RESOURCE].rva) goto done;
    /* only the sections the resources are in, laid out as in memory */
    img = calloc(1, nt->image_size);
    if (!img) goto done;
    sec = (section_t *)((uint8_t *)&nt->magic + nt->opt_size);
    for (int i = 0; i < nt->nsections; i++) {
        uint32_t n = sec[i].raw_size;
        if (sec[i].va >= nt->image_size) continue;
        if (sec[i].va + n > nt->image_size) n = nt->image_size - sec[i].va;
        if (sec[i].raw_ptr && n && sec[i].raw_ptr + n <= fsize) memcpy(img + sec[i].va, file + sec[i].raw_ptr, n);
    }
    res = img + nt->dir[DIR_RESOURCE].rva;
    {
        res_dir_t *d = res_find(res, (res_dir_t *)res, type, 0);
        uint32_t *data;
        if (!d || !(d = res_find(res, d, name, 0))) goto done;
        data = (uint32_t *)(void *)res_find(res, d, 0, 1);
        if (!data || data[0] + data[1] > nt->image_size) goto done;
        out = malloc(data[1] ? data[1] : 1);
        memcpy(out, img + data[0], data[1]);
        if (size) *size = data[1];
    }
done:
    free(img);
    free(file);
    return out;
}

/* ---- the program ------------------------------------------------------------------------------ */

typedef DWORD (WCALLBACK *exe_entry_t)(void *peb);

int ld_run_exe(const char *unix_path, char *const *argv) {
    int err = 0;
    char b[64];
    (void)argv;
    check_mode = getenv("ICDA_WIN_CHECK") != 0;
    base_name(unix_path, b, sizeof(b));
    pthread_mutex_lock(&ldlock);
    /* the program's own image first, so its DLLs are found next to it */
    {
        size_t fsize = 0;
        uint8_t *file = read_file(unix_path, &fsize);
        nt_headers_t *nt;
        if (!file) wfatal("Windows program", "%s could not be read.", unix_path);
        if (file[0] != 'M' || file[1] != 'Z') wfatal("Windows program", "%s is not a Windows program.", unix_path);
        nt = (nt_headers_t *)(file + *(uint32_t *)(file + 0x3C));
        if (nt->machine == 0x14c)
            wfatal("32-bit Windows program", "%s is a 32-bit Windows program. ICDA runs 64-bit Windows programs only.", b);
        if (nt->machine != 0x8664) wfatal("Windows program", "%s is not for x86-64 PCs.", b);
        free(file);
    }
    pthread_mutex_unlock(&ldlock);
    main_mod = calloc(1, sizeof(*main_mod));          /* placeholder so find_dll knows the folder */
    snprintf(main_mod->path, sizeof(main_mod->path), "%s", unix_path);
    {
        wmodule_t *placeholder = main_mod;
        pthread_mutex_lock(&ldlock);
        main_mod = load_pe(unix_path, b, &err, 1);
        pthread_mutex_unlock(&ldlock);
        free(placeholder);
    }
    if (!main_mod) wfatal("Windows program", "%s could not be loaded (error %d).", b, err);
    if (check_mode) {
        wlog("checked %s: the lines above are the functions ICDA lacks", b);
        exit(0);
    }
    *(void **)((uint8_t *)peb_get() + 0x10) = main_mod->base;       /* PEB.ImageBaseAddress */
    if (ld_init_all() != 0) wfatal("Windows program", "A DLL that %s needs did not start.", b);
    tls_callbacks(main_mod, 1);
    main_mod->inited = 1;
    if (wtrace) wlog("starting %s at %p", b, main_mod->entry);
    {
        DWORD rc = ((exe_entry_t)main_mod->entry)(peb_get());
        return (int)rc;
    }
}
