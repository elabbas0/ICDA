/* x64 stack unwinding, as Windows does it: each function's unwind
 * information (a module's .pdata and .xdata, or tables a program adds for
 * code it makes, as V8 does for its JIT) says how its prologue moved the
 * stack and saved registers, so a frame's caller is found by undoing that.
 *
 * Programs use it for stack traces (RtlCaptureStackBackTrace), and icda-win
 * for its reports: an exception a program raises is reported with where it
 * came from, frame by frame. */
#define _GNU_SOURCE
#include "win.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

wmodule_t *module_at(const void *p);

/* CONTEXT (x64): the integer registers from Rax at 0x78 (Rax Rcx Rdx Rbx
 * Rsp Rbp Rsi Rdi R8 ... R15, in that order), Rip at 0xF8, Xmm0 at 0x1A0 */
#define CTX_REG(c, n) (((uint64_t *)((uint8_t *)(c) + 0x78))[n])
#define CTX_RSP(c)    CTX_REG(c, 4)
#define CTX_RIP(c)    (*(uint64_t *)((uint8_t *)(c) + 0xF8))
#define CTX_XMM(c, n) ((uint8_t *)(c) + 0x1A0 + 16 * (n))

typedef struct {
    uint32_t begin, end, unwind;
} RUNTIME_FUNCTION;

/* ---- function tables added at run time ------------------------------------------------- */

typedef RUNTIME_FUNCTION *(WCALLBACK *table_callback_t)(uint64_t pc, void *ctx);

typedef struct dyntable {
    struct dyntable  *next;
    RUNTIME_FUNCTION *fns;
    uint32_t          count, max;
    uint64_t          base, start, end;     /* the code range it covers */
    table_callback_t  cb;
    void             *ctx;
} dyntable_t;

static dyntable_t *tables;
static pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER;

static void table_add(dyntable_t *t) {
    pthread_mutex_lock(&tlock);
    t->next = tables;
    tables = t;
    pthread_mutex_unlock(&tlock);
}

BOOLEAN WINAPI RtlAddFunctionTable(RUNTIME_FUNCTION *fns, DWORD count, DWORD64 base) {
    dyntable_t *t = calloc(1, sizeof(*t));
    t->fns = fns, t->count = t->max = count, t->base = base;
    t->start = base + (count ? fns[0].begin : 0);
    t->end = base + (count ? fns[count - 1].end : 0);
    table_add(t);
    return TRUE;
}

BOOLEAN WINAPI RtlInstallFunctionTableCallback(DWORD64 id, DWORD64 base, DWORD len, table_callback_t cb, void *ctx,
                                               const WCHAR *dll) {
    dyntable_t *t = calloc(1, sizeof(*t));
    (void)dll;
    t->fns = (RUNTIME_FUNCTION *)(uintptr_t)(id | 3);     /* the id: how it is deleted */
    t->base = base, t->start = base, t->end = base + len, t->cb = cb, t->ctx = ctx;
    table_add(t);
    return TRUE;
}

DWORD WINAPI RtlAddGrowableFunctionTable(void **handle, RUNTIME_FUNCTION *fns, DWORD count, DWORD max, ULONG_PTR start,
                                         ULONG_PTR end) {
    dyntable_t *t = calloc(1, sizeof(*t));
    t->fns = fns, t->count = count, t->max = max, t->base = start, t->start = start, t->end = end;
    table_add(t);
    *handle = t;
    return 0;
}

void WINAPI RtlGrowFunctionTable(void *handle, DWORD count) {
    dyntable_t *t = handle;
    if (t && count <= t->max) t->count = count;
}

static void table_remove(dyntable_t *match, RUNTIME_FUNCTION *fns) {
    pthread_mutex_lock(&tlock);
    for (dyntable_t **pp = &tables; *pp; pp = &(*pp)->next) {
        if (*pp == match || (fns && (*pp)->fns == fns)) {
            dyntable_t *t = *pp;
            *pp = t->next;
            free(t);
            break;
        }
    }
    pthread_mutex_unlock(&tlock);
}

void WINAPI RtlDeleteGrowableFunctionTable(void *handle) { table_remove(handle, 0); }
BOOLEAN WINAPI RtlDeleteFunctionTable(RUNTIME_FUNCTION *fns) {
    table_remove(0, fns);
    return TRUE;
}

/* ---- finding a function's entry ------------------------------------------------------------- */

static RUNTIME_FUNCTION *search(RUNTIME_FUNCTION *fns, uint32_t n, uint32_t rva) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rva < fns[mid].begin) hi = mid;
        else if (rva >= fns[mid].end) lo = mid + 1;
        else return &fns[mid];
    }
    return 0;
}

RUNTIME_FUNCTION *WINAPI RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *base, void *hist) {
    wmodule_t *m = module_at((void *)(uintptr_t)pc);
    (void)hist;
    if (base) *base = 0;
    if (m && !m->builtin) {
        const uint8_t *nt = m->base + *(const uint32_t *)(m->base + 0x3C);
        /* the data directories of a PE32+ header start 0x88 into it; the exception one is the fourth */
        const uint32_t *dir = (const uint32_t *)(nt + 0x88 + 3 * 8);
        if (base) *base = (DWORD64)(uintptr_t)m->base;
        if (dir[0] && dir[1])
            return search((RUNTIME_FUNCTION *)(m->base + dir[0]), dir[1] / sizeof(RUNTIME_FUNCTION),
                          (uint32_t)(pc - (uintptr_t)m->base));
        return 0;
    }
    {
        RUNTIME_FUNCTION *found = 0;
        pthread_mutex_lock(&tlock);
        for (dyntable_t *t = tables; t; t = t->next) {
            if (pc < t->start || pc >= t->end) continue;
            if (base) *base = t->base;
            found = t->cb ? t->cb(pc, t->ctx) : search(t->fns, t->count, (uint32_t)(pc - t->base));
            break;
        }
        pthread_mutex_unlock(&tlock);
        return found;
    }
}

/* ---- undoing a frame ------------------------------------------------------------------------- */

typedef struct {
    uint8_t ver_flags, prolog, ncodes, frame;
    uint16_t codes[];
} unwind_info_t;

/* the caller's registers, from a frame's; returns the language handler (0 here) */
void *WINAPI RtlVirtualUnwind(DWORD type, DWORD64 base, DWORD64 pc, RUNTIME_FUNCTION *fn, void *ctx, void **data,
                              DWORD64 *establisher, void *ptrs) {
    uint64_t frame;
    (void)type, (void)ptrs;
    if (data) *data = 0;
    if (!fn) {                                    /* a leaf: only its return address on the stack */
        CTX_RIP(ctx) = *(uint64_t *)(uintptr_t)CTX_RSP(ctx);
        CTX_RSP(ctx) += 8;
        if (establisher) *establisher = CTX_RSP(ctx);
        return 0;
    }
    for (int depth = 0; depth < 32; depth++) {
        unwind_info_t *ui = (unwind_info_t *)(uintptr_t)(base + (fn->unwind & ~1u));
        uint32_t offset = (uint32_t)(pc - base - fn->begin);
        int fr = ui->frame & 15, i;
        /* the frame base: from the frame register when the prologue set it */
        frame = CTX_RSP(ctx);
        if (fr) {
            for (i = 0; i < ui->ncodes; i++) {
                uint8_t at = (uint8_t)ui->codes[i], op = (uint8_t)(ui->codes[i] >> 8) & 15;
                if (op == 3 && (depth > 0 || offset >= at)) {
                    frame = CTX_REG(ctx, fr) - (uint64_t)(ui->frame >> 4) * 16;
                    break;
                }
                if (op == 1) i += ((ui->codes[i] >> 12) & 15) ? 2 : 1;
                else if (op == 4 || op == 8) i += 1;
                else if (op == 5 || op == 9) i += 2;
            }
        }
        if (establisher && depth == 0) *establisher = frame;
        for (i = 0; i < ui->ncodes; i++) {
            uint16_t c = ui->codes[i];
            uint8_t at = (uint8_t)c, op = (uint8_t)(c >> 8) & 15, info = (uint8_t)(c >> 12) & 15;
            int done = depth > 0 || offset >= at;          /* in a chained entry, the whole prologue ran */
            switch (op) {
            case 0:     /* UWOP_PUSH_NONVOL */
                if (done) {
                    CTX_REG(ctx, info) = *(uint64_t *)(uintptr_t)CTX_RSP(ctx);
                    CTX_RSP(ctx) += 8;
                }
                break;
            case 1:     /* UWOP_ALLOC_LARGE */
                if (info == 0) {
                    if (done) CTX_RSP(ctx) += (uint64_t)ui->codes[i + 1] * 8;
                    i += 1;
                } else {
                    if (done) CTX_RSP(ctx) += (uint64_t)ui->codes[i + 1] | (uint64_t)ui->codes[i + 2] << 16;
                    i += 2;
                }
                break;
            case 2:     /* UWOP_ALLOC_SMALL */
                if (done) CTX_RSP(ctx) += (uint64_t)info * 8 + 8;
                break;
            case 3:     /* UWOP_SET_FPREG */
                if (done) CTX_RSP(ctx) = frame;
                break;
            case 4:     /* UWOP_SAVE_NONVOL */
                if (done) CTX_REG(ctx, info) = *(uint64_t *)(uintptr_t)(frame + (uint64_t)ui->codes[i + 1] * 8);
                i += 1;
                break;
            case 5:     /* UWOP_SAVE_NONVOL_FAR */
                if (done)
                    CTX_REG(ctx, info) =
                        *(uint64_t *)(uintptr_t)(frame + ((uint64_t)ui->codes[i + 1] | (uint64_t)ui->codes[i + 2] << 16));
                i += 2;
                break;
            case 6:     /* UWOP_EPILOG (version 2): its slot only */
                break;
            case 8:     /* UWOP_SAVE_XMM128 */
                if (done) memcpy(CTX_XMM(ctx, info), (void *)(uintptr_t)(frame + (uint64_t)ui->codes[i + 1] * 16), 16);
                i += 1;
                break;
            case 9:     /* UWOP_SAVE_XMM128_FAR */
                if (done)
                    memcpy(CTX_XMM(ctx, info),
                           (void *)(uintptr_t)(frame + ((uint64_t)ui->codes[i + 1] | (uint64_t)ui->codes[i + 2] << 16)), 16);
                i += 2;
                break;
            case 10:    /* UWOP_PUSH_MACHFRAME: an interrupt's frame */
                if (done) {
                    uint64_t sp = CTX_RSP(ctx) + (info ? 8 : 0);
                    CTX_RIP(ctx) = *(uint64_t *)(uintptr_t)sp;
                    CTX_RSP(ctx) = *(uint64_t *)(uintptr_t)(sp + 24);
                    return 0;
                }
                break;
            default:
                break;
            }
        }
        if (!((ui->ver_flags >> 3) & 4)) break;           /* UNW_FLAG_CHAININFO: the entry it continues */
        fn = (RUNTIME_FUNCTION *)&ui->codes[(ui->ncodes + 1) & ~1];
    }
    CTX_RIP(ctx) = *(uint64_t *)(uintptr_t)CTX_RSP(ctx);
    CTX_RSP(ctx) += 8;
    return 0;
}

/* the return addresses from a context up, while they are in Windows code */
int unwind_walk(void *ctx, void **pcs, int max) {
    int n = 0;
    while (n < max) {
        uint64_t pc = CTX_RIP(ctx), base = 0;
        RUNTIME_FUNCTION *fn;
        if (!pc || !CTX_RSP(ctx)) break;
        fn = RtlLookupFunctionEntry(pc, &base, 0);
        if (!fn && !module_at((void *)(uintptr_t)pc)) {
            /* code a program made (JIT) without a table, or ours: stop */
            int known = 0;
            pthread_mutex_lock(&tlock);
            for (dyntable_t *t = tables; t; t = t->next) known |= pc >= t->start && pc < t->end;
            pthread_mutex_unlock(&tlock);
            if (!known) break;
        }
        pcs[n++] = (void *)(uintptr_t)pc;
        RtlVirtualUnwind(0, base, pc, fn, ctx, 0, 0, 0);
        if (CTX_RIP(ctx) == pc && n > 1) break;
    }
    return n;
}

/* RtlCaptureContext: the caller's registers, as at the call's return */
__asm__(".text\n"
        ".globl RtlCaptureContext\n"
        "RtlCaptureContext:\n"            /* ms_abi: rcx = the CONTEXT */
        "  mov %rax, 0x78(%rcx)\n"
        "  mov %rcx, 0x80(%rcx)\n"
        "  mov %rdx, 0x88(%rcx)\n"
        "  mov %rbx, 0x90(%rcx)\n"
        "  lea 8(%rsp), %rax\n"
        "  mov %rax, 0x98(%rcx)\n"
        "  mov %rbp, 0xa0(%rcx)\n"
        "  mov %rsi, 0xa8(%rcx)\n"
        "  mov %rdi, 0xb0(%rcx)\n"
        "  mov %r8, 0xb8(%rcx)\n"
        "  mov %r9, 0xc0(%rcx)\n"
        "  mov %r10, 0xc8(%rcx)\n"
        "  mov %r11, 0xd0(%rcx)\n"
        "  mov %r12, 0xd8(%rcx)\n"
        "  mov %r13, 0xe0(%rcx)\n"
        "  mov %r14, 0xe8(%rcx)\n"
        "  mov %r15, 0xf0(%rcx)\n"
        "  mov (%rsp), %rax\n"
        "  mov %rax, 0xf8(%rcx)\n"
        "  movl $0x10000b, 0x30(%rcx)\n"   /* CONTEXT_FULL */
        "  movdqu %xmm6, 0x200(%rcx)\n"
        "  movdqu %xmm7, 0x210(%rcx)\n"
        "  movdqu %xmm8, 0x220(%rcx)\n"
        "  movdqu %xmm9, 0x230(%rcx)\n"
        "  movdqu %xmm10, 0x240(%rcx)\n"
        "  movdqu %xmm11, 0x250(%rcx)\n"
        "  movdqu %xmm12, 0x260(%rcx)\n"
        "  movdqu %xmm13, 0x270(%rcx)\n"
        "  movdqu %xmm14, 0x280(%rcx)\n"
        "  movdqu %xmm15, 0x290(%rcx)\n"
        "  mov 0x78(%rcx), %rax\n"
        "  ret\n");

void RtlCaptureContext(void *ctx) __attribute__((ms_abi));

/* skip: frames left out above the caller's own */
USHORT WINAPI RtlCaptureStackBackTrace(ULONG skip, ULONG n, void **frames, ULONG *hash) {
    uint8_t ctx[0x4D0] __attribute__((aligned(16)));
    void *pcs[128];
    int got;
    ULONG o = 0, h = 0;
    memset(ctx, 0, sizeof(ctx));
    RtlCaptureContext(ctx);
    /* this function's frame is ours (not Windows code): start at its caller */
    CTX_RIP(ctx) = (uint64_t)(uintptr_t)__builtin_return_address(0);
    CTX_RSP(ctx) = (uint64_t)(uintptr_t)__builtin_frame_address(0) + 16;
    got = unwind_walk(ctx, pcs, 128);
    for (int i = (int)skip; i < got && o < n; i++) {
        frames[o++] = pcs[i];
        h += (ULONG)(uintptr_t)pcs[i];
    }
    if (hash) *hash = h;
    return (USHORT)o;
}

/* for icda-win's reports: the frames from a raise, with their modules */
void unwind_report(void *ctx) {
    void *pcs[48];
    int n = unwind_walk(ctx, pcs, 48);
    for (int i = 0; i < n; i++) {
        wmodule_t *m = module_at(pcs[i]);
        if (m) wlog("  frame %2d: %s+0x%lx", i, m->name, (unsigned long)((uintptr_t)pcs[i] - (uintptr_t)m->base));
        else wlog("  frame %2d: %p", i, pcs[i]);
    }
}

#define X(f) WEXP(f)
const wexport_t unwind_exports[] = {
    X(RtlAddFunctionTable), X(RtlInstallFunctionTableCallback), X(RtlAddGrowableFunctionTable), X(RtlGrowFunctionTable),
    X(RtlDeleteGrowableFunctionTable), X(RtlDeleteFunctionTable), X(RtlLookupFunctionEntry), X(RtlVirtualUnwind),
    WEXPN("RtlCaptureContext", RtlCaptureContext), X(RtlCaptureStackBackTrace),
};
const int unwind_nexports = sizeof(unwind_exports) / sizeof(unwind_exports[0]);
