/* C++ exceptions as MSVC's runtime does them (x64): _CxxThrowException and
 * the frame handler for functions compiled with its "FH3" tables
 * (__CxxFrameHandler3, also named __CxxFrameHandler and 2).
 *
 * A throw raises exception 0xE06D7363 with the object, its ThrowInfo (the
 * types it can be caught as, its destructor) and the module's base.  The
 * dispatcher (except.c) asks each frame's handler; this one reads the
 * function's tables: its states (a number for each scope with something to
 * destroy), the try blocks over ranges of states, their catch blocks, and
 * the destructors ("unwind actions") of each state.  A catch block whose type
 * the object has (or catch(...)) is chosen: the object is copied (or bound,
 * for a reference) into the catch block's variable, the frames between are
 * unwound (their destructors run), the try block's own scopes are left too,
 * and the catch block (a funclet) is called from the unwind's consolidation
 * callback; it returns where execution goes on.  The object is destroyed
 * when the catch block ends.  "throw;" rethrows the exception being handled;
 * a rethrow cannot be caught by the try block whose catch rethrew it.
 *
 * Every number below is the documented layout of these tables; RVAs are
 * relative to the module's base (ImageBase for the function's tables, the
 * one passed with the exception for the thrown type's). */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct EXCEPTION_RECORD {
    DWORD                    ExceptionCode, ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    void                    *ExceptionAddress;
    DWORD                    NumberParameters, pad;
    ULONG_PTR                ExceptionInformation[15];
} EXCEPTION_RECORD;

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

#define CXX_EXCEPTION             0xE06D7363u
#define CXX_MAGIC_VC6             0x19930520u
#define CXX_MAGIC_VC8             0x19930522u
#define STATUS_UNWIND_CONSOLIDATE 0x80000029u
#define EH_UNWINDING              2
#define EH_EXIT_UNWIND            4
#define EH_TARGET_UNWIND          0x20
#define FUNC_SYNCHRONOUS          1          /* catches C++ exceptions only */

/* ---- the tables -------------------------------------------------------------------------- */

typedef struct {
    uint32_t magic, unwind_count;
    int32_t  unwind_table;
    uint32_t tryblock_count;
    int32_t  tryblock;
    uint32_t ipmap_count;
    int32_t  ipmap;
    int32_t  unwind_help;       /* frame offset of the function's 8-byte state slot */
    int32_t  expect_list;
    uint32_t flags;
} func_info_t;

typedef struct { int32_t prev, handler; } unwind_entry_t;
typedef struct { int32_t start_level, end_level, catch_level; uint32_t catchblock_count; int32_t catchblock; } tryblock_t;
typedef struct { uint32_t flags; int32_t type_info, offset, handler, frame; } catchblock_t;
typedef struct { int32_t ip, state; } ipmap_t;

/* the thrown type's */
typedef struct { uint32_t flags; int32_t destructor, custom_handler, type_info_table; } throw_info_t;
typedef struct { int32_t this_offset, vbase_descr, vbase_offset; } this_offsets_t;
typedef struct { uint32_t flags; int32_t type_info; this_offsets_t offsets; uint32_t size; int32_t copy_ctor; } catchable_t;
typedef struct { void *vtable; char *name; char mangled[1]; } type_info_t;

enum { TYPE_CONST = 1, TYPE_VOLATILE = 2, TYPE_REFERENCE = 8 };
enum { CLASS_SIMPLE = 1, CLASS_VIRTUAL_BASE = 4 };

static void *rva(uint64_t base, int32_t off) { return off ? (void *)(uintptr_t)(base + (uint32_t)off) : 0; }

/* ---- catch blocks running (their notes live with the dispatcher's) --------------------------- */

void except_note_catch(void *info);
void *except_catch_info(int nth);
void WINAPI RtlUnwindEx(void *frame, void *target, EXCEPTION_RECORD *rec, void *ret, void *ctx, void *hist);
RUNTIME_FUNCTION *WINAPI RtlLookupFunctionEntry(DWORD64 pc, DWORD64 *base, void *hist);
void WINAPI RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args);

typedef struct {
    EXCEPTION_RECORD rec;           /* the exception being handled (for "throw;") */
    void            *ctx;
    uint64_t         frame;         /* the catching function's frame */
    const func_info_t *descr;
    int              tryblock;      /* its try block, whose catch runs */
    int              rethrown;
} catch_info_t;

static __thread int uncaught;
int cxx_uncaught_exceptions(void) { return uncaught; }
/* the exception the innermost catch block handles, for std::current_exception */
void *cxx_current_exception(void **ctx) {
    catch_info_t *c = except_catch_info(0);
    if (ctx) *ctx = c ? c->ctx : 0;
    return c ? &c->rec : 0;
}

/* ---- throwing ---------------------------------------------------------------------------------- */

/* the exception's values (RaiseException copies them at once) */
static __thread ULONG_PTR throw_args[4];

__attribute__((ms_abi, used)) ULONG_PTR *cxx_throw_prepare(void *object, const throw_info_t *ti) {
    wmodule_t *module_at(const void *p);
    wmodule_t *m = ti ? module_at(ti) : 0;
    throw_args[0] = CXX_MAGIC_VC6;
    throw_args[1] = (ULONG_PTR)object;
    throw_args[2] = (ULONG_PTR)ti;
    throw_args[3] = m ? (ULONG_PTR)m->base : 0;
    if (object) uncaught++;
    return throw_args;
}

/* _CxxThrowException(object, ThrowInfo): RaiseException in its place, so
 * the exception's context is the program's call */
__asm__(".text\n"
        ".globl _CxxThrowException\n"
        "_CxxThrowException:\n"
        "  sub $0x28, %rsp\n"
        "  call cxx_throw_prepare\n"
        "  add $0x28, %rsp\n"
        "  mov %rax, %r9\n"
        "  mov $0xE06D7363, %ecx\n"
        "  mov $1, %edx\n"                   /* EXCEPTION_NONCONTINUABLE */
        "  mov $4, %r8d\n"
        "  jmp RaiseException\n");

/* ---- the tables, read ------------------------------------------------------------------------- */

static int state_from_pc(const func_info_t *d, uint64_t base, uint64_t pc) {
    const ipmap_t *m = rva(base, d->ipmap);
    int32_t ip = (int32_t)(pc - base), state = -1;
    for (uint32_t i = 0; m && i < d->ipmap_count && m[i].ip <= ip; i++) state = m[i].state;
    return state;
}

static int32_t *unwind_help(const func_info_t *d, uint64_t frame) { return (int32_t *)(uintptr_t)(frame + d->unwind_help); }

/* the destructors of the frame's states down to last */
static void local_unwind(uint64_t frame, uint64_t base, uint64_t pc, const func_info_t *d, int last, int keep_state) {
    const unwind_entry_t *u = rva(base, d->unwind_table);
    int32_t *help = unwind_help(d, frame);
    int state = (keep_state || help[0] == -2) ? state_from_pc(d, base, pc) : help[0];
    for (int n = 0; state > last && n < 100000; n++) {
        if (state < 0 || (uint32_t)state >= d->unwind_count) break;
        if (u[state].handler)
            ((void (WCALLBACK *)(uint64_t, uint64_t))rva(base, u[state].handler))(0, frame);
        state = u[state].prev;
    }
    if (!keep_state) help[0] = state;
}

static void *this_pointer(const this_offsets_t *o, void *obj) {
    if (!obj) return 0;
    if (o->vbase_descr >= 0) {
        char *p = (char *)obj + o->vbase_descr;
        obj = p + *(int32_t *)(*(char **)p + o->vbase_offset);
    }
    return (char *)obj + o->this_offset;
}

/* the type of the thrown object a catch block takes: 0 if none */
static const catchable_t *caught_type(const throw_info_t *ti, uint64_t exc_base, const type_info_t *want,
                                      uint32_t catch_flags) {
    const uint32_t *table = rva(exc_base, ti->type_info_table);
    for (uint32_t i = 0; table && i < table[0]; i++) {
        const catchable_t *t = rva(exc_base, (int32_t)table[1 + i]);
        const type_info_t *have = rva(exc_base, t->type_info);
        if (!want) return t;                                           /* catch (...) */
        if (have != want && (!have || strcmp(have->mangled, want->mangled))) continue;
        if ((ti->flags & TYPE_CONST) && !(catch_flags & TYPE_CONST)) continue;
        if ((ti->flags & TYPE_VOLATILE) && !(catch_flags & TYPE_VOLATILE)) continue;
        return t;
    }
    return 0;
}

/* the object, into the catch block's variable in the frame */
static void copy_object(void *obj, uint64_t frame, const catchblock_t *cb, const catchable_t *t, uint64_t base,
                        uint64_t exc_base) {
    void **dest = (void **)(uintptr_t)(frame + cb->offset);
    const type_info_t *want = rva(base, cb->type_info);
    if (!want || !want->mangled[0] || !cb->offset) return;
    if (cb->flags & TYPE_REFERENCE) {
        *dest = this_pointer(&t->offsets, obj);
    } else if (t->flags & CLASS_SIMPLE) {
        memmove(dest, obj, t->size);
        if (t->size == sizeof(void *)) *dest = this_pointer(&t->offsets, *dest);
    } else if (t->copy_ctor) {
        void *src = this_pointer(&t->offsets, obj);
        if (t->flags & CLASS_VIRTUAL_BASE)
            ((void (WCALLBACK *)(void *, void *, int))rva(exc_base, t->copy_ctor))(dest, src, 1);
        else
            ((void (WCALLBACK *)(void *, void *))rva(exc_base, t->copy_ctor))(dest, src);
    } else {
        memmove(dest, this_pointer(&t->offsets, obj), t->size);
    }
}

static void destroy_object(EXCEPTION_RECORD *rec) {
    const throw_info_t *ti;
    if (rec->ExceptionCode != CXX_EXCEPTION || rec->NumberParameters < 3) return;
    ti = (const throw_info_t *)rec->ExceptionInformation[2];
    if (ti && ti->destructor && rec->ExceptionInformation[1])
        ((void (WCALLBACK *)(void *))rva(rec->ExceptionInformation[3], ti->destructor))((void *)rec->ExceptionInformation[1]);
}

/* ---- calling the catch block (the consolidating unwind's callback) ----------------------------- */

static void *WINAPI call_catch(EXCEPTION_RECORD *crec) {
    catch_info_t *c = calloc(1, sizeof(*c));
    uint64_t frame = crec->ExceptionInformation[1];
    void *(WCALLBACK *funclet)(uint64_t, uint64_t) = (void *)crec->ExceptionInformation[5];
    void *next;
    c->rec = *(EXCEPTION_RECORD *)crec->ExceptionInformation[4];
    c->ctx = (void *)crec->ExceptionInformation[7];
    c->frame = frame;
    c->descr = (const func_info_t *)crec->ExceptionInformation[2];
    c->tryblock = (int)crec->ExceptionInformation[8];
    except_note_catch(c);
    if (uncaught > 0) uncaught--;
    next = funclet(0, frame);
    /* the catch block ended: the object goes, the function's state is its code's again */
    destroy_object(&c->rec);
    *(int64_t *)(uintptr_t)(frame + c->descr->unwind_help) = -2;
    except_note_catch(0);
    free(c);
    return next;
}

/* the try blocks of the frame that can catch it, and the catch that does */
static void find_catch(EXCEPTION_RECORD *rec, void *ctx, uint64_t frame, DISPATCHER_CONTEXT *dc, const func_info_t *d,
                       const throw_info_t *ti, uint64_t orig_frame, int state, const tryblock_t *in_catch) {
    uint64_t base = dc->ImageBase, exc_base = rec->NumberParameters >= 4 ? rec->ExceptionInformation[3] : 0;
    const tryblock_t *tries = rva(base, d->tryblock);
    int32_t *help = unwind_help(d, orig_frame);
    int skip_start = 0x7fffffff, skip_end = -1;
    /* a catch block of this frame running (the exception comes from it):
     * its try block, and those within it, do not catch this one */
    for (int n = 0;; n++) {
        catch_info_t *c = except_catch_info(n);
        if (!c) break;
        if (c->frame == orig_frame && c->descr == d && c->tryblock >= 0 && (uint32_t)c->tryblock < d->tryblock_count) {
            skip_start = tries[c->tryblock].start_level;
            skip_end = tries[c->tryblock].end_level;
            break;
        }
    }
    if (!in_catch) {
        if (state > help[1]) help[0] = help[1] = state;
        else if (help[1] >= 0) state = help[1];
    }
    for (uint32_t i = 0; i < d->tryblock_count; i++) {
        const tryblock_t *tb = &tries[i];
        const catchblock_t *cbs = rva(base, tb->catchblock);
        if (state < tb->start_level || state > tb->end_level) continue;
        if (tb->start_level >= skip_start && tb->end_level <= skip_end) continue;
        if (in_catch && (tb->start_level <= in_catch->end_level || tb->end_level > in_catch->catch_level)) continue;
        for (uint32_t j = 0; j < tb->catchblock_count; j++) {
            const catchblock_t *cb = &cbs[j];
            EXCEPTION_RECORD crec;
            uint8_t unwound[0x4D0] __attribute__((aligned(16)));
            if (ti) {
                const catchable_t *t = caught_type(ti, exc_base, rva(base, cb->type_info), cb->flags);
                if (!t) continue;
                copy_object((void *)rec->ExceptionInformation[1], orig_frame, cb, t, base, exc_base);
            } else if (cb->type_info) {
                continue;                                  /* not C++: only catch (...) */
            }
            memset(&crec, 0, sizeof(crec));
            crec.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
            crec.ExceptionFlags = 1;
            crec.NumberParameters = 9;
            crec.ExceptionInformation[0] = (ULONG_PTR)call_catch;
            crec.ExceptionInformation[1] = orig_frame;
            crec.ExceptionInformation[2] = (ULONG_PTR)d;
            crec.ExceptionInformation[3] = (ULONG_PTR)(intptr_t)tb->start_level;
            crec.ExceptionInformation[4] = (ULONG_PTR)rec;
            crec.ExceptionInformation[5] = (ULONG_PTR)rva(base, cb->handler);
            crec.ExceptionInformation[7] = (ULONG_PTR)ctx;
            crec.ExceptionInformation[8] = i;
            RtlUnwindEx((void *)(uintptr_t)frame, (void *)(uintptr_t)dc->ControlPc, &crec, 0, unwound, 0);
        }
    }
}

/* ---- the frame handler -------------------------------------------------------------------------- */

static DWORD frame_handler3(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc,
                            const func_info_t *d) {
    uint64_t base = dc->ImageBase, orig_frame = frame;
    int state = state_from_pc(d, base, dc->ControlPc), unwind_level = -1, is_consolidate;
    const tryblock_t *tries = rva(base, d->tryblock), *in_catch = 0;
    const throw_info_t *ti = 0;
    if (d->magic < CXX_MAGIC_VC6 || d->magic > CXX_MAGIC_VC8) return 1;
    is_consolidate = rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 9 &&
                     rec->ExceptionInformation[0] == (ULONG_PTR)call_catch;
    if (d->magic >= CXX_MAGIC_VC8 && (d->flags & FUNC_SYNCHRONOUS) && rec->ExceptionCode != CXX_EXCEPTION &&
        !is_consolidate && !(rec->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND)))
        return 1;                                          /* C++ exceptions only */
    /* a catch block's funclet: the function it belongs to is the frame its
     * try block is in; its state is the catch block's */
    {
        DWORD64 fbase = 0;
        RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(dc->ControlPc, &fbase, 0);
        void *func = fn ? (void *)(uintptr_t)(fbase + fn->begin) : 0;
        for (uint32_t i = d->tryblock_count; func && i > 0; i--) {
            const tryblock_t *tb = &tries[i - 1];
            const catchblock_t *cbs = rva(base, tb->catchblock);
            if (state <= tb->end_level || state > tb->catch_level) continue;
            for (uint32_t j = 0; j < tb->catchblock_count; j++)
                if (rva(base, cbs[j].handler) == func) {
                    unwind_level = tb->end_level;
                    orig_frame = *(uint64_t *)(uintptr_t)(frame + cbs[j].frame);
                    in_catch = tb;
                }
        }
    }
    if (rec->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND)) {
        if (in_catch)                                      /* a catch block left: its own scopes */
            local_unwind(orig_frame, base, dc->ControlPc, d, unwind_level, 1);
        else if (rec->ExceptionFlags & EH_TARGET_UNWIND)   /* the catching frame: its try block's scopes */
            local_unwind(orig_frame, base, dc->ControlPc, d,
                         is_consolidate ? (int)(intptr_t)rec->ExceptionInformation[3] : state, 0);
        else
            local_unwind(orig_frame, base, dc->ControlPc, d, -1, 0);
        return 1;
    }
    if (!d->tryblock_count) return 1;
    if (rec->ExceptionCode == CXX_EXCEPTION) {
        if (!rec->ExceptionInformation[1] && !rec->ExceptionInformation[2]) {      /* throw; */
            void *cur_ctx;
            EXCEPTION_RECORD *cur = cxx_current_exception(&cur_ctx);
            if (!cur) return 1;
            *rec = *cur;
        }
        ti = (const throw_info_t *)rec->ExceptionInformation[2];
    }
    if (in_catch) state = state_from_pc(d, base, dc->ControlPc);
    find_catch(rec, ctx, frame, dc, d, ti, orig_frame, state, in_catch);
    return 1;                                              /* ExceptionContinueSearch */
}

DWORD WINAPI __CxxFrameHandler3(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) {
    const func_info_t *d = rva(dc->ImageBase, *(const int32_t *)dc->HandlerData);
    if (!d) return 1;
    return frame_handler3(rec, frame, ctx, dc, d);
}
