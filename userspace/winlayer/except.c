/* Exceptions, as x64 Windows dispatches them.
 *
 * An exception (RaiseException, a C++ throw, or a fault in a program's code)
 * goes first to the vectored handlers, then up the program's frames: each
 * function's unwind data (unwind.c) says how to find its caller and may name
 * a language handler (__C_specific_handler for __try/__except, a C++ frame
 * handler, libgcc's for MinGW's C++), which is asked whether it handles the
 * exception.  One that does unwinds (RtlUnwindEx): the frames between are
 * left, their handlers running their cleanups (__finally, destructors), and
 * execution goes on at the target with the registers that frame had
 * (RtlRestoreContext).  An exception nobody handles goes to the program's
 * unhandled exception filter, then is reported as before.
 *
 * Frames of the layer's own (C, without Windows unwind data) are crossed
 * where a program called into the layer through the barrier (barrier.c),
 * which knows the caller's registers: an exception a window procedure throws
 * reaches the program's handler above DispatchMessage.
 *
 * Whose context unwinding starts from: during a dispatch, the exception's
 * own (the frames below, the dispatcher's and the handler's, are dropped);
 * during an unwind, the frame whose handler is running (a handler that
 * unwinds again, as libgcc's does to run a cleanup, starts there); otherwise
 * the caller's.  That is what Windows' collided-unwind handling achieves. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include <unistd.h>

/* ---- the records ---------------------------------------------------------------------------- */

typedef struct EXCEPTION_RECORD {
    DWORD                    ExceptionCode, ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    void                    *ExceptionAddress;
    DWORD                    NumberParameters, pad;
    ULONG_PTR                ExceptionInformation[15];
} EXCEPTION_RECORD;
_Static_assert(sizeof(EXCEPTION_RECORD) == 152, "EXCEPTION_RECORD");

typedef struct { uint32_t begin, end, unwind; } RUNTIME_FUNCTION;

typedef struct {
    uint64_t          ControlPc, ImageBase;
    RUNTIME_FUNCTION *FunctionEntry;
    uint64_t          EstablisherFrame, TargetIp;
    void             *ContextRecord;
    void             *LanguageHandler;
    void             *HandlerData;
    void             *HistoryTable;
    DWORD             ScopeIndex, Fill0;
} DISPATCHER_CONTEXT;

typedef struct { EXCEPTION_RECORD *rec; void *ctx; } EXCEPTION_POINTERS;

#define CTX_SIZE      0x4D0
#define CTX_FLAGS(c)  (*(DWORD *)((uint8_t *)(c) + 0x30))
#define CTX_MXCSR(c)  (*(DWORD *)((uint8_t *)(c) + 0x34))
#define CTX_EFLAGS(c) (*(DWORD *)((uint8_t *)(c) + 0x44))
#define CTX_REG(c, n) (((uint64_t *)((uint8_t *)(c) + 0x78))[n])
#define CTX_RAX(c)    CTX_REG(c, 0)
#define CTX_RSP(c)    CTX_REG(c, 4)
#define CTX_RIP(c)    (*(uint64_t *)((uint8_t *)(c) + 0xF8))

enum {
    EH_NONCONTINUABLE = 1, EH_UNWINDING = 2, EH_EXIT_UNWIND = 4, EH_STACK_INVALID = 8, EH_NESTED_CALL = 0x10,
    EH_TARGET_UNWIND = 0x20, EH_COLLIDED_UNWIND = 0x40,
};
enum { DISP_CONTINUE_EXECUTION = 0, DISP_CONTINUE_SEARCH = 1, DISP_NESTED = 2, DISP_COLLIDED = 3 };
#define STATUS_UNWIND              0xC0000027u
#define STATUS_LONGJUMP            0x80000026u
#define STATUS_UNWIND_CONSOLIDATE  0x80000029u
#define STATUS_INVALID_DISPOSITION 0xC0000026u
#define STATUS_NONCONTINUABLE      0xC0000025u

typedef DWORD (WCALLBACK *lang_handler_t)(EXCEPTION_RECORD *, uint64_t, void *, DISPATCHER_CONTEXT *);
void WINAPI RtlUnwindEx(void *frame, void *target, EXCEPTION_RECORD *rec, void *ret, void *ctx, void *hist);

RUNTIME_FUNCTION *WINAPI RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *base, void *hist);
void *WINAPI RtlVirtualUnwind(DWORD type, DWORD64 base, DWORD64 pc, RUNTIME_FUNCTION *fn, void *ctx, void **data,
                              DWORD64 *establisher, void *ptrs);
int barrier_caller(uint64_t rsp, void *ctx);
wmodule_t *module_at(const void *p);
void unwind_report(void *ctx);

/* ---- what the dispatcher is doing, per thread ---------------------------------------------------
 * While it calls a handler (or a catch block, for MSVC's C++), a note says
 * which frame that is for and where the dispatcher's own (C) frames are, so
 * that an exception raised in the handler, or an unwind it starts, finds the
 * program's frames past the dispatcher's, as Windows' collided and nested
 * unwinds do. */
enum { ACT_DISPATCH = 1, ACT_UNWIND, ACT_CONSOLIDATE };
typedef struct {
    int       kind;
    void     *frame_ctx;      /* the frame's own context (for a catch block: where it goes back to) */
    void     *origin;         /* a dispatch's exception context */
    uintptr_t sp;             /* the dispatcher's frames are below this */
    void     *user;           /* a catch block's, for the C++ runtime (cxxeh.c) */
} act_t;
#define MAX_ACTS 32
static __thread act_t acts[MAX_ACTS];
static __thread int nacts;

static void act_push(int kind, void *frame_ctx, void *origin, uintptr_t sp) {
    if (nacts < MAX_ACTS) acts[nacts] = (act_t){ kind, frame_ctx, origin, sp, 0 };
    nacts++;
}

static void act_pop(void) { if (nacts > 0) nacts--; }
static act_t *act_top(void) { return nacts > 0 && nacts <= MAX_ACTS ? &acts[nacts - 1] : 0; }
/* going on at a frame whose stack is rsp: notes of frames left behind go */
static void act_drop_below(uint64_t rsp) {
    if (nacts > MAX_ACTS) nacts = MAX_ACTS;
    while (nacts > 0 && acts[nacts - 1].sp < rsp) nacts--;
}

/* the C++ runtime's note on the catch block now called (from its
 * consolidation callback), and the notes of those running, innermost first */
void except_note_catch(void *info) {
    act_t *t = act_top();
    if (t && t->kind == ACT_CONSOLIDATE) t->user = info;
}
void *except_catch_info(int nth) {
    for (int i = (nacts > MAX_ACTS ? MAX_ACTS : nacts) - 1; i >= 0; i--)
        if (acts[i].kind == ACT_CONSOLIDATE && acts[i].user && nth-- == 0) return acts[i].user;
    return 0;
}

/* the stack the thread has (its TEB's NT_TIB), to tell a frame from garbage */
static int frame_ok(uint64_t frame) {
    const uint64_t *tib = teb_current();
    uint64_t base = tib ? tib[1] : 0, limit = tib ? tib[2] : 0;
    if (!frame || (frame & 7)) return 0;
    if (!base || !limit || limit >= base) return 1;           /* unknown bounds */
    return frame >= limit && frame <= base;
}

/* a frame's caller: by the function's unwind data; through a call into the
 * layer, by the barrier's note; a leaf in Windows code, by its return
 * address.  0 when there is nowhere to go. */
static int virtual_unwind(DWORD type, DISPATCHER_CONTEXT *dc, void *ctx) {
    uint64_t pc = CTX_RIP(ctx);
    dc->ImageBase = 0;
    dc->ScopeIndex = 0;
    dc->ControlPc = pc;
    dc->LanguageHandler = 0;
    dc->HandlerData = 0;
    if (!pc || !CTX_RSP(ctx)) return 0;
    dc->FunctionEntry = RtlLookupFunctionEntry(pc, &dc->ImageBase, 0);
    if (dc->FunctionEntry) {
        dc->LanguageHandler = RtlVirtualUnwind(type, dc->ImageBase, pc, dc->FunctionEntry, ctx, &dc->HandlerData,
                                               &dc->EstablisherFrame, 0);
        return CTX_RIP(ctx) != 0;
    }
    dc->EstablisherFrame = CTX_RSP(ctx);
    if (module_at((void *)(uintptr_t)pc)) {                    /* a leaf function of a module */
        CTX_RIP(ctx) = *(uint64_t *)(uintptr_t)CTX_RSP(ctx);
        CTX_RSP(ctx) += 8;
        return 1;
    }
    {
        Dl_info di;
        if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname) {
            /* the layer's own code: the dispatcher's (to the frame it works
             * for), or a call into the layer (to its Windows caller) */
            uint64_t rsp = CTX_RSP(ctx);
            uint8_t via_barrier[CTX_SIZE] __attribute__((aligned(16)));
            int have_barrier;
            act_t *a = 0;
            for (int i = (nacts > MAX_ACTS ? MAX_ACTS : nacts) - 1; i >= 0; i--)
                if (acts[i].sp >= rsp) {
                    a = &acts[i];
                    break;
                }
            memcpy(via_barrier, ctx, CTX_SIZE);
            have_barrier = barrier_caller(rsp, via_barrier);
            if (a && (!have_barrier || a->sp <= CTX_RSP(via_barrier))) {
                memcpy(ctx, a->frame_ctx, CTX_SIZE);
                return 1;
            }
            if (have_barrier) {
                memcpy(ctx, via_barrier, CTX_SIZE);
                return 1;
            }
        }
    }
    return 0;                                                  /* code without unwind data (a JIT's): the end */
}

/* ---- vectored handlers -------------------------------------------------------------------------- */

typedef struct veh {
    struct veh *next;
    void       *fn;
    int         cont;        /* a continue handler (called last) */
} veh_t;
static veh_t *vehs;
static pthread_mutex_t veh_lock = PTHREAD_MUTEX_INITIALIZER;

static void *veh_add(ULONG first, void *fn, int cont) {
    veh_t *v = calloc(1, sizeof(*v));
    if (!v) return 0;
    v->fn = fn;
    v->cont = cont;
    pthread_mutex_lock(&veh_lock);
    if (first || !vehs) {
        v->next = vehs;
        vehs = v;
    } else {
        veh_t *t = vehs;
        while (t->next) t = t->next;
        t->next = v;
    }
    pthread_mutex_unlock(&veh_lock);
    return v;
}
static ULONG veh_remove(void *h) {
    int found = 0;
    pthread_mutex_lock(&veh_lock);
    for (veh_t **p = &vehs; *p; p = &(*p)->next)
        if (*p == h) {
            *p = ((veh_t *)h)->next;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&veh_lock);
    if (found) free(h);
    return found;
}
void *WINAPI AddVectoredExceptionHandler(ULONG first, void *fn) { return veh_add(first, fn, 0); }
ULONG WINAPI RemoveVectoredExceptionHandler(void *h) { return veh_remove(h); }
void *WINAPI AddVectoredContinueHandler(ULONG first, void *fn) { return veh_add(first, fn, 1); }
ULONG WINAPI RemoveVectoredContinueHandler(void *h) { return veh_remove(h); }

/* -1 EXCEPTION_CONTINUE_EXECUTION from one, else 0 */
static int call_vectored(EXCEPTION_RECORD *rec, void *ctx, int cont) {
    EXCEPTION_POINTERS ep = { rec, ctx };
    veh_t *list[64];
    int n = 0;
    pthread_mutex_lock(&veh_lock);
    for (veh_t *v = vehs; v && n < 64; v = v->next)
        if (v->cont == cont) list[n++] = v;
    pthread_mutex_unlock(&veh_lock);
    for (int i = 0; i < n; i++)
        if (((LONG (WCALLBACK *)(EXCEPTION_POINTERS *))list[i]->fn)(&ep) == -1) return -1;
    return 0;
}

/* ---- dispatching: who handles it -------------------------------------------------------------- */

/* 1: a handler wants execution to go on at ctx (which it may have changed) */
static int dispatch(EXCEPTION_RECORD *rec, void *orig) {
    uint8_t context[CTX_SIZE] __attribute__((aligned(16))), frame_ctx[CTX_SIZE] __attribute__((aligned(16)));
    DISPATCHER_CONTEXT dc;
    int result = 0;
    if (call_vectored(rec, orig, 0) == -1) return 1;
    memcpy(context, orig, CTX_SIZE);
    memset(&dc, 0, sizeof(dc));
    dc.ContextRecord = context;
    for (int frames = 0; frames < 10000; frames++) {
        memcpy(frame_ctx, context, CTX_SIZE);
        if (!virtual_unwind(1 /* UNW_FLAG_EHANDLER */, &dc, context)) break;
    unwind_done:
        if (!frame_ok(dc.EstablisherFrame)) {
            rec->ExceptionFlags |= EH_STACK_INVALID;
            break;
        }
        if (dc.LanguageHandler) {
            DWORD r;
            act_push(ACT_DISPATCH, frame_ctx, orig, (uintptr_t)__builtin_frame_address(0));
            r = ((lang_handler_t)dc.LanguageHandler)(rec, dc.EstablisherFrame, orig, &dc);
            act_pop();
            rec->ExceptionFlags &= EH_NONCONTINUABLE;
            if (r == DISP_CONTINUE_EXECUTION) {
                if (rec->ExceptionFlags & EH_NONCONTINUABLE) {
                    wlog("exception 0x%08X: a handler continued a non-continuable exception", rec->ExceptionCode);
                    break;
                }
                result = 1;
                break;
            }
            if (r == DISP_NESTED) rec->ExceptionFlags |= EH_NESTED_CALL;
            else if (r == DISP_COLLIDED) {
                uint64_t frame;
                memcpy(context, dc.ContextRecord, CTX_SIZE);
                dc.ContextRecord = context;
                RtlVirtualUnwind(0, dc.ImageBase, dc.ControlPc, dc.FunctionEntry, context, 0, &frame, 0);
                goto unwind_done;
            } else if (r != DISP_CONTINUE_SEARCH) {
                wlog("exception 0x%08X: a handler answered %u", rec->ExceptionCode, r);
                break;
            }
        }
    }
    if (!result && call_vectored(rec, orig, 1) == -1) result = 1;
    return result;
}

/* ---- going on: the registers of a context --------------------------------------------------------- */

/* every register from the CONTEXT (rcx), the stack and rip last */
__asm__(".text\n"
        ".globl restore_context\n"
        "restore_context:\n"
        "  movdqu 0x1a0(%rcx), %xmm0\n"
        "  movdqu 0x1b0(%rcx), %xmm1\n"
        "  movdqu 0x1c0(%rcx), %xmm2\n"
        "  movdqu 0x1d0(%rcx), %xmm3\n"
        "  movdqu 0x1e0(%rcx), %xmm4\n"
        "  movdqu 0x1f0(%rcx), %xmm5\n"
        "  movdqu 0x200(%rcx), %xmm6\n"
        "  movdqu 0x210(%rcx), %xmm7\n"
        "  movdqu 0x220(%rcx), %xmm8\n"
        "  movdqu 0x230(%rcx), %xmm9\n"
        "  movdqu 0x240(%rcx), %xmm10\n"
        "  movdqu 0x250(%rcx), %xmm11\n"
        "  movdqu 0x260(%rcx), %xmm12\n"
        "  movdqu 0x270(%rcx), %xmm13\n"
        "  movdqu 0x280(%rcx), %xmm14\n"
        "  movdqu 0x290(%rcx), %xmm15\n"
        "  mov 0x34(%rcx), %eax\n"
        "  test %eax, %eax\n"
        "  jz 1f\n"
        "  ldmxcsr 0x34(%rcx)\n"
        "1:\n"
        /* the values go below the target stack, then are popped */
        "  mov 0x98(%rcx), %rax\n"
        "  lea -0x88(%rax), %rax\n"
        "  mov 0x90(%rcx), %rdx\n  mov %rdx, 0x00(%rax)\n"
        "  mov 0xa0(%rcx), %rdx\n  mov %rdx, 0x08(%rax)\n"
        "  mov 0xa8(%rcx), %rdx\n  mov %rdx, 0x10(%rax)\n"
        "  mov 0xb0(%rcx), %rdx\n  mov %rdx, 0x18(%rax)\n"
        "  mov 0xb8(%rcx), %rdx\n  mov %rdx, 0x20(%rax)\n"
        "  mov 0xc0(%rcx), %rdx\n  mov %rdx, 0x28(%rax)\n"
        "  mov 0xc8(%rcx), %rdx\n  mov %rdx, 0x30(%rax)\n"
        "  mov 0xd0(%rcx), %rdx\n  mov %rdx, 0x38(%rax)\n"
        "  mov 0xd8(%rcx), %rdx\n  mov %rdx, 0x40(%rax)\n"
        "  mov 0xe0(%rcx), %rdx\n  mov %rdx, 0x48(%rax)\n"
        "  mov 0xe8(%rcx), %rdx\n  mov %rdx, 0x50(%rax)\n"
        "  mov 0xf0(%rcx), %rdx\n  mov %rdx, 0x58(%rax)\n"
        "  mov 0x88(%rcx), %rdx\n  mov %rdx, 0x60(%rax)\n"
        "  mov 0x80(%rcx), %rdx\n  mov %rdx, 0x68(%rax)\n"
        "  mov 0x78(%rcx), %rdx\n  mov %rdx, 0x70(%rax)\n"
        "  mov 0x44(%rcx), %edx\n"
        "  or $0x202, %edx\n"                  /* reserved bit 1, interrupts on */
        "  and $0xfffffeff, %edx\n"            /* not the trap flag */
        "  mov %rdx, 0x78(%rax)\n"
        "  mov 0xf8(%rcx), %rdx\n  mov %rdx, 0x80(%rax)\n"
        "  mov %rax, %rsp\n"
        "  pop %rbx\n  pop %rbp\n  pop %rsi\n  pop %rdi\n"
        "  pop %r8\n  pop %r9\n  pop %r10\n  pop %r11\n"
        "  pop %r12\n  pop %r13\n  pop %r14\n  pop %r15\n"
        "  pop %rdx\n  pop %rcx\n  pop %rax\n"
        "  popfq\n"
        "  ret\n");
__attribute__((ms_abi, noreturn)) void restore_context(void *ctx);

typedef struct {
    uint64_t Frame, Rbx, Rsp, Rbp, Rsi, Rdi, R12, R13, R14, R15, Rip;
    DWORD    MxCsr;
    uint16_t FpCsr, Spare;
    uint8_t  Xmm[10][16];
} JUMP_BUFFER;

/* a consolidating unwind (MSVC's catch): the callback, on the target's stack
 * side, says where to go on */
typedef void *(WCALLBACK *consolidate_t)(EXCEPTION_RECORD *);

void WINAPI RtlRestoreContext(void *ctx, EXCEPTION_RECORD *rec) {
    if (rec && rec->ExceptionCode == STATUS_LONGJUMP && rec->NumberParameters >= 1) {
        JUMP_BUFFER *j = (JUMP_BUFFER *)rec->ExceptionInformation[0];
        CTX_REG(ctx, 3) = j->Rbx, CTX_REG(ctx, 4) = j->Rsp, CTX_REG(ctx, 5) = j->Rbp, CTX_REG(ctx, 6) = j->Rsi;
        CTX_REG(ctx, 7) = j->Rdi, CTX_REG(ctx, 12) = j->R12, CTX_REG(ctx, 13) = j->R13, CTX_REG(ctx, 14) = j->R14;
        CTX_REG(ctx, 15) = j->R15, CTX_RIP(ctx) = j->Rip;
        memcpy((uint8_t *)ctx + 0x200, j->Xmm, sizeof(j->Xmm));
        CTX_MXCSR(ctx) = j->MxCsr;
    } else if (rec && rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 1) {
        consolidate_t cb = (consolidate_t)rec->ExceptionInformation[0];
        void *next;
        act_drop_below(CTX_RSP(ctx));
        act_push(ACT_CONSOLIDATE, ctx, 0, (uintptr_t)__builtin_frame_address(0));
        next = cb(rec);
        act_pop();
        CTX_RIP(ctx) = (uint64_t)(uintptr_t)next;
    }
    act_drop_below(CTX_RSP(ctx));
    restore_context(ctx);
}

/* ---- unwinding --------------------------------------------------------------------------------------- */

static DWORD call_unwind_handler(EXCEPTION_RECORD *rec, DISPATCHER_CONTEXT *dc) {
    DWORD r;
    act_push(ACT_UNWIND, dc->ContextRecord, 0, (uintptr_t)__builtin_frame_address(0));
    r = ((lang_handler_t)dc->LanguageHandler)(rec, dc->EstablisherFrame, dc->ContextRecord, dc);
    act_pop();
    if (r == DISP_CONTINUE_SEARCH || r == DISP_COLLIDED) return r;
    wlog("unwinding: a handler answered %u", r);
    return DISP_CONTINUE_SEARCH;
}

/* from the RtlUnwindEx entry: caller is the context of its caller */
__attribute__((ms_abi, used, noreturn)) void unwind_impl(void *end_frame, void *target_ip, EXCEPTION_RECORD *rec,
                                                        void *retval, void *orig_ctx, void *table, void *caller) {
    uint8_t context[CTX_SIZE] __attribute__((aligned(16))), next[CTX_SIZE] __attribute__((aligned(16)));
    EXCEPTION_RECORD local;
    DISPATCHER_CONTEXT dc;
    /* where unwinding starts: a dispatch's exception, the frame an unwind
     * was at, or (a catch block's or no handler's call) the caller */
    act_t *top = act_top();
    void *start = !top ? caller : top->kind == ACT_DISPATCH ? top->origin : top->kind == ACT_UNWIND ? top->frame_ctx : caller;
    memcpy(context, start, CTX_SIZE);
    memcpy(next, context, CTX_SIZE);
    if (!rec) {
        memset(&local, 0, sizeof(local));
        local.ExceptionCode = STATUS_UNWIND;
        local.ExceptionAddress = (void *)(uintptr_t)CTX_RIP(context);
        rec = &local;
    }
    rec->ExceptionFlags |= EH_UNWINDING | (end_frame ? 0 : EH_EXIT_UNWIND);
    memset(&dc, 0, sizeof(dc));
    dc.EstablisherFrame = CTX_RSP(context);
    dc.TargetIp = (uint64_t)(uintptr_t)target_ip;
    dc.ContextRecord = context;
    dc.HistoryTable = table;
    for (int frames = 0; frames < 10000; frames++) {
        if (!virtual_unwind(2 /* UNW_FLAG_UHANDLER */, &dc, next)) {
            if (end_frame) wfatal("Windows program", "Unwinding found no frame %p to go back to.", end_frame);
            break;
        }
    unwind_done:
        if (!frame_ok(dc.EstablisherFrame)) {
            rec->ExceptionFlags |= EH_STACK_INVALID;
            break;
        }
        if (dc.LanguageHandler) {
            if (end_frame && dc.EstablisherFrame > (uint64_t)(uintptr_t)end_frame)
                wfatal("Windows program", "Unwinding went past its target frame %p.", end_frame);
            if (dc.EstablisherFrame == (uint64_t)(uintptr_t)end_frame) rec->ExceptionFlags |= EH_TARGET_UNWIND;
            if (call_unwind_handler(rec, &dc) == DISP_COLLIDED) {
                uint64_t frame;
                memcpy(next, dc.ContextRecord, CTX_SIZE);
                memcpy(context, next, CTX_SIZE);
                dc.ContextRecord = context;
                RtlVirtualUnwind(0, dc.ImageBase, dc.ControlPc, dc.FunctionEntry, next, 0, &frame, 0);
                rec->ExceptionFlags |= EH_COLLIDED_UNWIND;
                goto unwind_done;
            }
            rec->ExceptionFlags &= ~EH_COLLIDED_UNWIND;
        }
        if (dc.EstablisherFrame == (uint64_t)(uintptr_t)end_frame) break;
        memcpy(context, next, CTX_SIZE);
    }
    if (orig_ctx) memcpy(orig_ctx, context, CTX_SIZE);
    if (rec->ExceptionCode != STATUS_UNWIND_CONSOLIDATE) CTX_RIP(context) = (uint64_t)(uintptr_t)target_ip;
    CTX_RAX(context) = (uint64_t)(uintptr_t)retval;
    RtlRestoreContext(context, rec);
    __builtin_unreachable();
}

/* the caller's whole context at a call, into a CONTEXT at %rax: the
 * integer registers, the ones Windows' convention keeps in xmm, mxcsr and
 * the flags; rsp and rip as at the call's return (from the frame size) */
#define CAPTURE(frame)                                   \
    "  mov %rcx, 0x80(%rax)\n"                           \
    "  mov %rdx, 0x88(%rax)\n"                           \
    "  mov %rbx, 0x90(%rax)\n"                           \
    "  lea " #frame "+8(%rsp), %r10\n"                   \
    "  mov %r10, 0x98(%rax)\n"                           \
    "  mov %rbp, 0xa0(%rax)\n"                           \
    "  mov %rsi, 0xa8(%rax)\n"                           \
    "  mov %rdi, 0xb0(%rax)\n"                           \
    "  mov %r8, 0xb8(%rax)\n"                            \
    "  mov %r9, 0xc0(%rax)\n"                            \
    "  mov %r12, 0xd8(%rax)\n"                           \
    "  mov %r13, 0xe0(%rax)\n"                           \
    "  mov %r14, 0xe8(%rax)\n"                           \
    "  mov %r15, 0xf0(%rax)\n"                           \
    "  mov " #frame "(%rsp), %r10\n"                     \
    "  mov %r10, 0xf8(%rax)\n"                           \
    "  movl $0x10000b, 0x30(%rax)\n"                     \
    "  stmxcsr 0x34(%rax)\n"                             \
    "  pushfq\n"                                         \
    "  pop %r10\n"                                       \
    "  mov %r10d, 0x44(%rax)\n"                          \
    "  movdqu %xmm6, 0x200(%rax)\n"                      \
    "  movdqu %xmm7, 0x210(%rax)\n"                      \
    "  movdqu %xmm8, 0x220(%rax)\n"                      \
    "  movdqu %xmm9, 0x230(%rax)\n"                      \
    "  movdqu %xmm10, 0x240(%rax)\n"                     \
    "  movdqu %xmm11, 0x250(%rax)\n"                     \
    "  movdqu %xmm12, 0x260(%rax)\n"                     \
    "  movdqu %xmm13, 0x270(%rax)\n"                     \
    "  movdqu %xmm14, 0x280(%rax)\n"                     \
    "  movdqu %xmm15, 0x290(%rax)\n"

/* RtlUnwindEx(frame, ip, rec, retval, orig_ctx, table): the caller's context
 * (0x528-byte frame: the CONTEXT at 0x40, stack arguments from 0x20) */
__asm__(".text\n"
        ".globl RtlUnwindEx\n"
        "RtlUnwindEx:\n"
        "  sub $0x528, %rsp\n"
        "  lea 0x40(%rsp), %rax\n"
        CAPTURE(0x528)
        "  mov 0x550(%rsp), %r10\n  mov %r10, 0x20(%rsp)\n"       /* orig_ctx (5th) */
        "  mov 0x558(%rsp), %r10\n  mov %r10, 0x28(%rsp)\n"       /* table (6th) */
        "  mov %rax, 0x30(%rsp)\n"
        "  call unwind_impl\n"
        "  ud2\n"
        ".globl RtlUnwind\n"
        "RtlUnwind:\n"                      /* (frame, ip, rec, retval) */
        "  sub $0x528, %rsp\n"
        "  lea 0x40(%rsp), %rax\n"
        CAPTURE(0x528)
        "  movq $0, 0x20(%rsp)\n"
        "  movq $0, 0x28(%rsp)\n"
        "  mov %rax, 0x30(%rsp)\n"
        "  call unwind_impl\n"
        "  ud2\n");

/* ---- raising -------------------------------------------------------------------------------------- */

static void *unhandled_filter;

void *WINAPI SetUnhandledExceptionFilter(void *fn) { return __atomic_exchange_n(&unhandled_filter, fn, __ATOMIC_ACQ_REL); }

LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS *ep) {
    (void)ep;
    return 1;     /* EXCEPTION_EXECUTE_HANDLER: the program ends */
}

/* nobody handled it: the program's filter, then the report */
static void unhandled(EXCEPTION_RECORD *rec, void *ctx) {
    void *filter = __atomic_load_n(&unhandled_filter, __ATOMIC_ACQUIRE);
    if (filter) {
        EXCEPTION_POINTERS ep = { rec, ctx };
        LONG r = ((LONG (WCALLBACK *)(EXCEPTION_POINTERS *))filter)(&ep);
        if (r == -1) restore_context(ctx);                   /* EXCEPTION_CONTINUE_EXECUTION */
        if (r == 1) _exit((int)rec->ExceptionCode);          /* EXCEPTION_EXECUTE_HANDLER */
    }
    wlog("exception 0x%08X not handled", rec->ExceptionCode);
    for (DWORD i = 0; i < rec->NumberParameters && i < 8; i++)
        wlog("  value %u: 0x%llx", i, (unsigned long long)rec->ExceptionInformation[i]);
    unwind_report(ctx);
    if (rec->ExceptionCode == 0xE06D7363) wfatal("Windows program", "The program threw a C++ exception it does not catch.");
    if (rec->ExceptionCode == 0xC0000005)
        wfatal("Windows program", "The program stopped: a memory access fault at %p (address %p).", rec->ExceptionAddress,
               (void *)rec->ExceptionInformation[1]);
    wfatal("Windows program", "The program stopped with exception 0x%08X.", rec->ExceptionCode);
}

/* RaiseException and RtlRaiseException: rec dispatched from the caller's
 * context; a handler that continues it returns to the caller */
__attribute__((ms_abi, used)) void raise_rec_impl(EXCEPTION_RECORD *rec, void *ctx) {
    if (!rec->ExceptionAddress) rec->ExceptionAddress = (void *)(uintptr_t)CTX_RIP(ctx);
    if (dispatch(rec, ctx)) restore_context(ctx);
    if (rec->ExceptionCode == 0x406D1388) return;          /* naming a thread, for a debugger: no one listens */
    unhandled(rec, ctx);
}

__attribute__((ms_abi, used)) void raise_impl(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args, void *ctx) {
    EXCEPTION_RECORD rec;
    memset(&rec, 0, sizeof(rec));
    rec.ExceptionCode = code;
    rec.ExceptionFlags = flags & EH_NONCONTINUABLE;
    rec.ExceptionAddress = (void *)(uintptr_t)CTX_RIP(ctx);
    rec.NumberParameters = args ? (n > 15 ? 15 : n) : 0;
    for (DWORD i = 0; i < rec.NumberParameters; i++) rec.ExceptionInformation[i] = args[i];
    raise_rec_impl(&rec, ctx);
}

__asm__(".text\n"
        ".globl RaiseException\n"
        "RaiseException:\n"                 /* (code, flags, count, values): 0x518-byte frame, CONTEXT at 0x30 */
        "  sub $0x518, %rsp\n"
        "  lea 0x30(%rsp), %rax\n"
        CAPTURE(0x518)
        "  mov %rax, 0x20(%rsp)\n"
        "  call raise_impl\n"
        "  add $0x518, %rsp\n"
        "  ret\n"
        ".globl RtlRaiseException\n"
        "RtlRaiseException:\n"              /* (rec) */
        "  sub $0x518, %rsp\n"
        "  lea 0x30(%rsp), %rax\n"
        CAPTURE(0x518)
        "  mov %rax, %rdx\n"
        "  call raise_rec_impl\n"
        "  add $0x518, %rsp\n"
        "  ret\n");

/* ---- faults in a program's code --------------------------------------------------------------------- */

/* on the program's stack, out of the signal handler */
__attribute__((ms_abi, used, noreturn)) void fault_dispatch(EXCEPTION_RECORD *rec, void *ctx) {
    if (dispatch(rec, ctx)) restore_context(ctx);
    unhandled(rec, ctx);
    _exit(1);
}
extern char fault_entry[];
/* rcx = rec, rdx = ctx, rsp aligned below them by the signal handler */
__asm__(".text\n"
        ".globl fault_entry\n"
        "fault_entry:\n"
        "  sub $0x28, %rsp\n"
        "  call fault_dispatch\n"
        "  ud2\n");

/* from the fault handler: a fault in a program's code (a module's, or code a
 * program made) becomes an exception it may handle.  1 when it was taken:
 * the handler returns into fault_entry on the program's stack. */
int except_from_signal(int sig, siginfo_t *si, void *ucontext) {
    ucontext_t *uc = ucontext;
    greg_t *g = uc->uc_mcontext.gregs;
    uint64_t rip = (uint64_t)g[REG_RIP], rsp = (uint64_t)g[REG_RSP], sp;
    EXCEPTION_RECORD *rec;
    uint8_t *ctx;
    Dl_info di;
    static const int order[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                   REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
    if (dladdr((void *)(uintptr_t)rip, &di) && di.dli_fname) return 0;    /* the layer's own code */
    if (getenv("ICDA_WIN_NO_SEH")) return 0;
    /* the stack overflowed: no room to dispatch on it */
    if (sig == SIGSEGV && (uint64_t)(uintptr_t)si->si_addr < rsp && rsp - (uint64_t)(uintptr_t)si->si_addr < 65536) {
        const uint64_t *tib = teb_current();
        if (tib && tib[2] && rsp < tib[2] + 0x3000) return 0;
    }
    sp = (rsp - 0x200 - CTX_SIZE - sizeof(EXCEPTION_RECORD)) & ~(uint64_t)15;
    ctx = (uint8_t *)(uintptr_t)sp;
    rec = (EXCEPTION_RECORD *)(ctx + CTX_SIZE);
    memset(ctx, 0, CTX_SIZE);
    memset(rec, 0, sizeof(*rec));
    for (int i = 0; i < 16; i++) CTX_REG(ctx, i) = (uint64_t)g[order[i]];
    CTX_RIP(ctx) = rip;
    CTX_EFLAGS(ctx) = (DWORD)g[REG_EFL];
    CTX_FLAGS(ctx) = 0x10001f;                                    /* CONTEXT_ALL */
    if (uc->uc_mcontext.fpregs) {
        memcpy(ctx + 0x100, uc->uc_mcontext.fpregs, 512);         /* FltSave: the FXSAVE layout */
        CTX_MXCSR(ctx) = uc->uc_mcontext.fpregs->mxcsr;
    }
    rec->ExceptionAddress = (void *)(uintptr_t)rip;
    switch (sig) {
    case SIGSEGV:
    case SIGBUS:
        rec->ExceptionCode = 0xC0000005;                          /* EXCEPTION_ACCESS_VIOLATION */
        rec->NumberParameters = 2;
        rec->ExceptionInformation[0] = (g[REG_ERR] & 16) ? 8 : (g[REG_ERR] & 2) ? 1 : 0;   /* exec, write, read */
        rec->ExceptionInformation[1] = (ULONG_PTR)si->si_addr;
        break;
    case SIGILL:
        rec->ExceptionCode = 0xC000001D;                          /* EXCEPTION_ILLEGAL_INSTRUCTION */
        break;
    case SIGFPE:
        rec->ExceptionCode = si->si_code == FPE_INTDIV ? 0xC0000094 : si->si_code == FPE_INTOVF ? 0xC0000095
                           : si->si_code == FPE_FLTDIV ? 0xC000008E : si->si_code == FPE_FLTOVF ? 0xC0000091
                           : si->si_code == FPE_FLTUND ? 0xC0000093 : si->si_code == FPE_FLTRES ? 0xC000008F
                           : si->si_code == FPE_FLTINV ? 0xC0000090 : 0xC0000094;
        break;
    case SIGTRAP:
        rec->ExceptionCode = 0x80000003;                          /* EXCEPTION_BREAKPOINT: at the int3 */
        CTX_RIP(ctx) = rip - 1;
        rec->ExceptionAddress = (void *)(uintptr_t)(rip - 1);
        rec->NumberParameters = 1;
        break;
    default:
        return 0;
    }
    g[REG_RSP] = (greg_t)(sp - 8);                                /* as if called: aligned for fault_entry */
    g[REG_RIP] = (greg_t)(uintptr_t)fault_entry;
    g[REG_RCX] = (greg_t)(uintptr_t)rec;
    g[REG_RDX] = (greg_t)(uintptr_t)ctx;
    return 1;
}

/* ---- __try / __except / __finally ------------------------------------------------------------------- */

typedef struct {
    DWORD Count;
    struct { DWORD BeginAddress, EndAddress, HandlerAddress, JumpTarget; } ScopeRecord[];
} SCOPE_TABLE;

DWORD WINAPI __C_specific_handler(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) {
    SCOPE_TABLE *t = dc->HandlerData;
    uint64_t base = dc->ImageBase, pc = dc->ControlPc;
    if (rec->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND)) {
        for (DWORD i = dc->ScopeIndex; i < t->Count; i++) {
            if (pc < base + t->ScopeRecord[i].BeginAddress || pc >= base + t->ScopeRecord[i].EndAddress) continue;
            if (t->ScopeRecord[i].JumpTarget) continue;            /* an __except: nothing to run */
            if ((rec->ExceptionFlags & EH_TARGET_UNWIND) && dc->TargetIp >= base + t->ScopeRecord[i].BeginAddress &&
                dc->TargetIp < base + t->ScopeRecord[i].EndAddress)
                break;
            dc->ScopeIndex = i + 1;
            ((void (WCALLBACK *)(BOOLEAN, uint64_t))(uintptr_t)(base + t->ScopeRecord[i].HandlerAddress))(TRUE, frame);
        }
        return DISP_CONTINUE_SEARCH;
    }
    for (DWORD i = dc->ScopeIndex; i < t->Count; i++) {
        if (pc < base + t->ScopeRecord[i].BeginAddress || pc >= base + t->ScopeRecord[i].EndAddress) continue;
        if (!t->ScopeRecord[i].JumpTarget) continue;              /* a __finally */
        if (t->ScopeRecord[i].HandlerAddress != 1) {              /* a filter, not EXCEPTION_EXECUTE_HANDLER */
            EXCEPTION_POINTERS ep = { rec, ctx };
            LONG r = ((LONG (WCALLBACK *)(EXCEPTION_POINTERS *, uint64_t))(uintptr_t)(base + t->ScopeRecord[i].HandlerAddress))(
                &ep, frame);
            if (r == 0) continue;                                  /* EXCEPTION_CONTINUE_SEARCH */
            if (r < 0) return DISP_CONTINUE_EXECUTION;
        }
        RtlUnwindEx((void *)(uintptr_t)frame, (void *)(uintptr_t)(base + t->ScopeRecord[i].JumpTarget), rec,
                    (void *)(uintptr_t)rec->ExceptionCode, dc->ContextRecord, dc->HistoryTable);
    }
    return DISP_CONTINUE_SEARCH;
}
