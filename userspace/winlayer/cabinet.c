/* cabinet.dll: reading .cab archives (the FDI interface), for installers.
 *
 * libmspack does the decompressing (MSZIP, LZX, Quantum); the program's
 * own callbacks get each file as FDI gives it: the program is told of a
 * file (fdintCOPY_FILE) and opens where it goes, its contents arrive
 * through the program's write callback, and the program is told it is
 * done (fdintCLOSE_FILE_INFO). */
#define _GNU_SOURCE
#include "win.h"
#include <mspack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int path_find(const char *wpath, char *out, size_t cap);

typedef void *(WCALLBACK *PFNALLOC)(ULONG);
typedef void (WCALLBACK *PFNFREE)(void *);
typedef INT_PTR (WCALLBACK *PFNOPEN)(char *, int, int);
typedef UINT (WCALLBACK *PFNREAD)(INT_PTR, void *, UINT);
typedef UINT (WCALLBACK *PFNWRITE)(INT_PTR, void *, UINT);
typedef int (WCALLBACK *PFNCLOSE)(INT_PTR);
typedef LONG (WCALLBACK *PFNSEEK)(INT_PTR, LONG, int);

typedef struct {
    LONG    cb;
    char   *psz1, *psz2, *psz3;
    void   *pv;
    INT_PTR hf;
    USHORT  date, time, attribs, setID, iCabinet, iFolder;
    int     fdie;
} FDINOTIFICATION;

typedef INT_PTR (WCALLBACK *PFNFDINOTIFY)(int, FDINOTIFICATION *);

typedef struct {
    int  erfOper, erfType;
    BOOL fError;
} ERF;

typedef struct {
    LONG   cbCabinet;
    USHORT cFolders, cFiles, setID, iCabinet;
    BOOL   fReserve, hasprev, hasnext;
} FDICABINETINFO;

enum { fdintCABINET_INFO = 0, fdintPARTIAL_FILE, fdintCOPY_FILE, fdintCLOSE_FILE_INFO, fdintNEXT_CABINET, fdintENUMERATE };
enum { FDIERROR_NONE = 0, FDIERROR_CABINET_NOT_FOUND, FDIERROR_NOT_A_CABINET, FDIERROR_UNKNOWN_CABINET_VERSION,
       FDIERROR_CORRUPT_CABINET, FDIERROR_ALLOC_FAIL, FDIERROR_BAD_COMPR_TYPE, FDIERROR_MDI_FAIL, FDIERROR_TARGET_FILE,
       FDIERROR_RESERVE_MISMATCH, FDIERROR_WRONG_CABINET, FDIERROR_USER_ABORT };

typedef struct {
    int      magic;
    PFNALLOC alloc;
    PFNFREE  free;
    PFNOPEN  open;
    PFNREAD  read;
    PFNWRITE write;
    PFNCLOSE close;
    PFNSEEK  seek;
    ERF     *erf;
} fdi_t;

#define FDI_MAGIC 0x46444921

static void fail(fdi_t *f, int e) {
    if (f && f->erf) {
        f->erf->erfOper = e;
        f->erf->erfType = 0;
        f->erf->fError = TRUE;
    }
}

static void *WINAPI FDICreate(PFNALLOC a, PFNFREE fr, PFNOPEN o, PFNREAD r, PFNWRITE w, PFNCLOSE c, PFNSEEK s, int cpu, ERF *erf) {
    fdi_t *f = calloc(1, sizeof(*f));
    (void)cpu;
    f->magic = FDI_MAGIC;
    f->alloc = a, f->free = fr, f->open = o, f->read = r, f->write = w, f->close = c, f->seek = s;
    f->erf = erf;
    if (erf) memset(erf, 0, sizeof(*erf));
    return f;
}

static BOOL WINAPI FDIDestroy(void *h) {
    fdi_t *f = h;
    if (!f || f->magic != FDI_MAGIC) return FALSE;
    f->magic = 0;
    free(f);
    return TRUE;
}

/* the header, read through the program's own handle */
static BOOL WINAPI FDIIsCabinet(void *h, INT_PTR hf, FDICABINETINFO *ci) {
    fdi_t *f = h;
    uint8_t hdr[36];
    if (!f || f->magic != FDI_MAGIC) return FALSE;
    if (f->read(hf, hdr, sizeof(hdr)) != sizeof(hdr) || memcmp(hdr, "MSCF", 4)) {
        fail(f, FDIERROR_NOT_A_CABINET);
        return FALSE;
    }
    if (ci) {
        uint16_t flags = *(uint16_t *)(hdr + 30);
        memset(ci, 0, sizeof(*ci));
        ci->cbCabinet = (LONG) * (uint32_t *)(hdr + 8);
        ci->cFolders = *(uint16_t *)(hdr + 26);
        ci->cFiles = *(uint16_t *)(hdr + 28);
        ci->setID = *(uint16_t *)(hdr + 32);
        ci->iCabinet = *(uint16_t *)(hdr + 34);
        ci->hasprev = (flags & 1) != 0;
        ci->hasnext = (flags & 2) != 0;
        ci->fReserve = (flags & 4) != 0;
    }
    return TRUE;
}

static BOOL WINAPI FDICopy(void *h, char *cab_name, char *cab_path, int flags, PFNFDINOTIFY notify, void *decrypt, void *user) {
    fdi_t *f = h;
    char wpath[1100], upath[1100], tmp[64];
    struct mscab_decompressor *cabd;
    struct mscabd_cabinet *cab;
    FDINOTIFICATION n;
    BOOL ok = TRUE;
    (void)flags, (void)decrypt;
    if (!f || f->magic != FDI_MAGIC) return FALSE;
    snprintf(wpath, sizeof(wpath), "%s%s", cab_path ? cab_path : "", cab_name ? cab_name : "");
    if (path_find(wpath, upath, sizeof(upath)) != 0) {
        fail(f, FDIERROR_CABINET_NOT_FOUND);
        return FALSE;
    }
    cabd = mspack_create_cab_decompressor(0);
    if (!cabd) {
        fail(f, FDIERROR_ALLOC_FAIL);
        return FALSE;
    }
    cab = cabd->open(cabd, upath);
    if (!cab) {
        wlog("cabinet %s: not readable (%d)", wpath, cabd->last_error(cabd));
        fail(f, cabd->last_error(cabd) == MSPACK_ERR_SIGNATURE ? FDIERROR_NOT_A_CABINET : FDIERROR_CORRUPT_CABINET);
        mspack_destroy_cab_decompressor(cabd);
        return FALSE;
    }
    memset(&n, 0, sizeof(n));
    n.psz1 = "";
    n.psz2 = "";
    n.psz3 = cab_path ? cab_path : "";
    n.setID = (USHORT)cab->set_id;
    n.iCabinet = (USHORT)cab->set_index;
    n.pv = user;
    if (notify(fdintCABINET_INFO, &n) == -1) {
        fail(f, FDIERROR_USER_ABORT);
        ok = FALSE;
    }
    snprintf(tmp, sizeof(tmp), "/tmp/icda-cab-%d", getpid());
    for (struct mscabd_file *file = cab->files; file && ok; file = file->next) {
        INT_PTR hf;
        memset(&n, 0, sizeof(n));
        n.cb = (LONG)file->length;
        n.psz1 = file->filename;
        n.pv = user;
        n.date = (USHORT)(((file->date_y - 1980) << 9) | (file->date_m << 5) | file->date_d);
        n.time = (USHORT)((file->time_h << 11) | (file->time_m << 5) | (file->time_s / 2));
        n.attribs = (USHORT)(file->attribs & 0x7F);
        hf = notify(fdintCOPY_FILE, &n);
        if (hf == -1) {
            fail(f, FDIERROR_USER_ABORT);
            ok = FALSE;
            break;
        }
        if (hf == 0) continue;                  /* the program skips this one */
        if (cabd->extract(cabd, file, tmp) != MSPACK_ERR_OK) {
            wlog("cabinet %s: %s could not be expanded (%d)", wpath, file->filename, cabd->last_error(cabd));
            fail(f, FDIERROR_CORRUPT_CABINET);
            f->close(hf);
            ok = FALSE;
            break;
        }
        {
            int fd = open(tmp, O_RDONLY | O_CLOEXEC);
            char buf[65536];
            ssize_t r;
            while (fd >= 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
                if (f->write(hf, buf, (UINT)r) != (UINT)r) {
                    fail(f, FDIERROR_TARGET_FILE);
                    ok = FALSE;
                    break;
                }
            }
            if (fd >= 0) close(fd);
            unlink(tmp);
        }
        if (!ok) {
            f->close(hf);
            break;
        }
        memset(&n, 0, sizeof(n));
        n.hf = hf;
        n.psz1 = file->filename;
        n.pv = user;
        n.date = (USHORT)(((file->date_y - 1980) << 9) | (file->date_m << 5) | file->date_d);
        n.time = (USHORT)((file->time_h << 11) | (file->time_m << 5) | (file->time_s / 2));
        n.attribs = (USHORT)(file->attribs & 0x7F);
        n.cb = (file->attribs & MSCAB_ATTRIB_EXEC) ? 1 : 0;
        if (!notify(fdintCLOSE_FILE_INFO, &n)) {
            fail(f, FDIERROR_TARGET_FILE);
            ok = FALSE;
        }
        if (wtrace) wlog("cabinet %s: %s expanded (%u bytes)", wpath, file->filename, file->length);
    }
    cabd->close(cabd, cab);
    mspack_destroy_cab_decompressor(cabd);
    return ok;
}

static BOOL WINAPI FDITruncateCabinet(void *h, char *name, USHORT folder) {
    (void)h, (void)name, (void)folder;
    return FALSE;
}

static const wexport_t cab_exports[] = {
    WEXPO("FDICreate", FDICreate, 20), WEXPO("FDICopy", FDICopy, 22), WEXPO("FDIIsCabinet", FDIIsCabinet, 21),
    WEXPO("FDIDestroy", FDIDestroy, 23), WEXPO("FDITruncateCabinet", FDITruncateCabinet, 24),
};

const wdll_t dll_cabinet = { "cabinet.dll", cab_exports, sizeof(cab_exports) / sizeof(cab_exports[0]) };
