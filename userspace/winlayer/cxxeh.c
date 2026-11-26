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
#include <pthread.h>

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
    int              try_low, try_high;   /* its try block, whose catch runs */
    int32_t          help;          /* FH3: the frame's state slot (reset when the catch ends), else -1 */
} catch_info_t;

/* the catch of this frame running (an exception coming from it): its try block */
static catch_info_t *active_catch(uint64_t frame) {
    for (int n = 0;; n++) {
        catch_info_t *c = except_catch_info(n);
        if (!c) return 0;
        if (c->frame == frame) return c;
    }
}

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
static void copy_object(void *obj, uint64_t frame, uint32_t flags, const type_info_t *want, int32_t offset,
                        const catchable_t *t, uint64_t exc_base) {
    void **dest = (void **)(uintptr_t)(frame + offset);
    if (!want || !want->mangled[0] || !offset) return;
    if (flags & TYPE_REFERENCE) {
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

/* the consolidating unwind's record: [1] the frame, [2] FH3's state slot
 * (+1; 0 none), [3] [6] the try block's states, [4] the exception, [5] the
 * catch funclet, [7] its context, [9] how many continuation addresses
 * follow (FH4: the funclet returns which), [10] [11] them */
static void *WINAPI call_catch(EXCEPTION_RECORD *crec) {
    catch_info_t *c = calloc(1, sizeof(*c));
    uint64_t frame = crec->ExceptionInformation[1];
    void *(WCALLBACK *funclet)(uint64_t, uint64_t) = (void *)crec->ExceptionInformation[5];
    void *next;
    c->rec = *(EXCEPTION_RECORD *)crec->ExceptionInformation[4];
    c->ctx = (void *)crec->ExceptionInformation[7];
    c->frame = frame;
    c->help = (int32_t)crec->ExceptionInformation[2] - 1;
    c->try_low = (int)(intptr_t)crec->ExceptionInformation[3];
    c->try_high = (int)(intptr_t)crec->ExceptionInformation[6];
    except_note_catch(c);
    if (uncaught > 0) uncaught--;
    next = funclet(0, frame);
    if (crec->ExceptionInformation[9]) {
        uintptr_t i = (uintptr_t)next;
        next = (void *)crec->ExceptionInformation[10 + (i < crec->ExceptionInformation[9] ? i : 0)];
    }
    /* the catch block ended: the object goes, the function's state is its code's again */
    destroy_object(&c->rec);
    if (crec->ExceptionInformation[2]) *(int64_t *)(uintptr_t)(frame + c->help) = -2;
    except_note_catch(0);
    free(c);
    return next;
}

/* unwinds to the catch block's frame, then calls it (call_catch) */
static void unwind_to_catch(EXCEPTION_RECORD *rec, void *ctx, uint64_t frame, DISPATCHER_CONTEXT *dc, uint64_t orig_frame,
                            int32_t help, int try_low, int try_high, void *funclet, int ncont, const uint64_t *cont) {
    EXCEPTION_RECORD crec;
    uint8_t unwound[0x4D0] __attribute__((aligned(16)));
    memset(&crec, 0, sizeof(crec));
    crec.ExceptionCode = STATUS_UNWIND_CONSOLIDATE;
    crec.ExceptionFlags = 1;
    crec.NumberParameters = 12;
    crec.ExceptionInformation[0] = (ULONG_PTR)call_catch;
    crec.ExceptionInformation[1] = orig_frame;
    crec.ExceptionInformation[2] = (ULONG_PTR)(intptr_t)(help + 1);
    crec.ExceptionInformation[3] = (ULONG_PTR)(intptr_t)try_low;
    crec.ExceptionInformation[4] = (ULONG_PTR)rec;
    crec.ExceptionInformation[5] = (ULONG_PTR)funclet;
    crec.ExceptionInformation[6] = (ULONG_PTR)(intptr_t)try_high;
    crec.ExceptionInformation[7] = (ULONG_PTR)ctx;
    crec.ExceptionInformation[9] = (ULONG_PTR)ncont;
    for (int i = 0; i < ncont && i < 2; i++) crec.ExceptionInformation[10 + i] = cont[i];
    RtlUnwindEx((void *)(uintptr_t)frame, (void *)(uintptr_t)dc->ControlPc, &crec, 0, unwound, 0);
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
    catch_info_t *running = active_catch(orig_frame);
    if (running) skip_start = running->try_low, skip_end = running->try_high;
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
            if (ti) {
                const catchable_t *t = caught_type(ti, exc_base, rva(base, cb->type_info), cb->flags);
                if (!t) continue;
                copy_object((void *)rec->ExceptionInformation[1], orig_frame, cb->flags, rva(base, cb->type_info),
                            cb->offset, t, exc_base);
            } else if (cb->type_info) {
                continue;                                  /* not C++: only catch (...) */
            }
            unwind_to_catch(rec, ctx, frame, dc, orig_frame, d->unwind_help, tb->start_level, tb->end_level,
                            rva(base, cb->handler), 0, 0);
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
    is_consolidate = rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 12 &&
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

/* ---- FH4: Visual Studio 2019 and later (__CxxFrameHandler4) ------------------------------------
 * The same information, compressed: a header byte saying which tables
 * follow, variable-length numbers (the low bits of the first byte say how
 * many bytes: 1 to 5), IPs as deltas from the function's start, an unwind
 * map whose entries say how far back (in bytes) the next state's entry is,
 * and destructors named with their object (in the frame, or through a
 * pointer there) rather than as funclets.  A catch block may list where
 * execution goes on after it; its funclet then returns which. */

enum { FI4_CATCH = 1, FI4_SEPARATED = 2, FI4_BBT = 4, FI4_UNWIND = 8, FI4_TRY = 0x10, FI4_EHS = 0x20 };

typedef struct {
    uint8_t  flags;
    int32_t  unwind_map, try_map, ip_map;     /* RVAs */
    uint32_t disp_frame;                      /* a catch funclet's: where its parent's frame is */
    uint32_t func_rva;                        /* the function (or segment) its IPs count from */
} fi4_t;

static uint32_t rd_u(const uint8_t **pp) {
    const uint8_t *p = *pp;
    uint32_t v;
    if (!(p[0] & 1)) v = p[0] >> 1, p += 1;
    else if ((p[0] & 3) == 1) v = (uint32_t)(p[0] >> 2) | (uint32_t)p[1] << 6, p += 2;
    else if ((p[0] & 7) == 3) v = (uint32_t)(p[0] >> 3) | (uint32_t)p[1] << 5 | (uint32_t)p[2] << 13, p += 3;
    else if ((p[0] & 15) == 7)
        v = (uint32_t)(p[0] >> 4) | (uint32_t)p[1] << 4 | (uint32_t)p[2] << 12 | (uint32_t)p[3] << 20, p += 4;
    else v = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 24, p += 5;
    *pp = p;
    return v;
}
static int32_t rd_i(const uint8_t **pp) {
    int32_t v;
    memcpy(&v, *pp, 4);
    *pp += 4;
    return v;
}

static int decomp_fi4(uint64_t base, int32_t rva_fi, uint32_t func_rva, fi4_t *fi) {
    const uint8_t *p = rva(base, rva_fi);
    memset(fi, 0, sizeof(*fi));
    if (!p) return -1;
    fi->func_rva = func_rva;
    fi->flags = *p++;
    if (fi->flags & FI4_BBT) rd_u(&p);
    if (fi->flags & FI4_UNWIND) fi->unwind_map = rd_i(&p);
    if (fi->flags & FI4_TRY) fi->try_map = rd_i(&p);
    if (fi->flags & FI4_SEPARATED) {
        const uint8_t *seg = rva(base, rd_i(&p));
        uint32_t n = seg ? rd_u(&seg) : 0;
        for (uint32_t i = 0; i < n; i++) {
            int32_t start = rd_i(&seg), table = rd_i(&seg);
            if ((uint32_t)start == func_rva) fi->ip_map = table;
        }
    } else {
        fi->ip_map = rd_i(&p);
    }
    if (fi->flags & FI4_CATCH) fi->disp_frame = rd_u(&p);
    return 0;
}

static int state4(const fi4_t *fi, uint64_t base, uint64_t pc) {
    const uint8_t *p = rva(base, fi->ip_map);
    uint32_t n, ip = fi->func_rva, at = (uint32_t)(pc - base);
    int state = -1;
    if (!p) return -1;
    n = rd_u(&p);
    for (uint32_t i = 0; i < n; i++) {
        ip += rd_u(&p);
        if (ip > at) break;
        state = (int)rd_u(&p) - 1;
    }
    return state;
}

typedef struct { const uint8_t *prev; uint32_t type; int32_t handler; uint32_t object; } uw4_t;

static void read_uw4(const uint8_t **pp, uw4_t *u) {
    const uint8_t *start = *pp;
    uint32_t v = rd_u(pp);
    u->type = v & 3;
    u->prev = start - (v >> 2);
    u->handler = 0, u->object = 0;
    if (u->type == 1 || u->type == 2) {            /* a destructor: of the object in the frame, or pointed at */
        u->handler = rd_i(pp);
        u->object = rd_u(pp);
    } else if (u->type == 3) {                     /* a funclet */
        u->handler = rd_i(pp);
    }
}

/* the destructors of the frame's states, from state down to last */
static void local_unwind4(uint64_t frame, uint64_t base, const fi4_t *fi, int state, int last) {
    const uint8_t *map = rva(base, fi->unwind_map), *at, *stop;
    uint32_t n;
    if (!map || state <= last) return;
    n = rd_u(&map);
    if (state < 0 || (uint32_t)state >= n) return;
    at = map;
    stop = map - 1;
    for (int i = 0; i < state; i++) {
        uw4_t u;
        if (i == last) stop = at;
        read_uw4(&at, &u);
    }
    for (int guard = 0; at > stop && guard < 100000; guard++) {
        const uint8_t *entry = at;
        uw4_t u;
        read_uw4(&entry, &u);
        if (u.handler) {
            void *h = rva(base, u.handler);
            if (u.type == 3) {
                ((void (WCALLBACK *)(uint64_t, uint64_t))h)(0, frame);
            } else {
                void *obj = (void *)(uintptr_t)(frame + u.object);
                if (u.type == 2) obj = *(void **)obj;
                ((void (WCALLBACK *)(void *, uint64_t))h)(obj, frame);
            }
        }
        if (u.prev >= at) break;                   /* no earlier state */
        at = u.prev;
    }
}

static void find_catch4(EXCEPTION_RECORD *rec, void *ctx, uint64_t frame, DISPATCHER_CONTEXT *dc, const fi4_t *fi,
                        const throw_info_t *ti, uint64_t orig_frame, int state) {
    uint64_t base = dc->ImageBase, exc_base = rec->NumberParameters >= 4 ? rec->ExceptionInformation[3] : 0;
    const uint8_t *p = rva(base, fi->try_map);
    catch_info_t *running = active_catch(orig_frame);
    uint32_t ntry;
    if (!p) return;
    ntry = rd_u(&p);
    for (uint32_t i = 0; i < ntry; i++) {
        int low = (int)rd_u(&p), high = (int)rd_u(&p);
        const uint8_t *h;
        uint32_t nh;
        rd_u(&p);                                   /* the highest state of its catch blocks */
        h = rva(base, rd_i(&p));
        if (state < low || state > high || !h) continue;
        if (running && !(fi->flags & FI4_CATCH) && low >= running->try_low && high <= running->try_high) continue;
        nh = rd_u(&h);
        for (uint32_t j = 0; j < nh; j++) {
            uint8_t hd = *h++;
            uint32_t adjectives = (hd & 1) ? rd_u(&h) : 0;
            int32_t type = (hd & 2) ? rd_i(&h) : 0;
            uint32_t catch_obj = (hd & 4) ? rd_u(&h) : 0;
            int32_t handler = rd_i(&h);
            int ncont = (hd >> 4) & 3;
            uint64_t cont[2] = { 0, 0 };
            for (int k = 0; k < ncont && k < 2; k++)
                cont[k] = (hd & 8) ? base + (uint32_t)rd_i(&h) : base + fi->func_rva + rd_u(&h);
            if (ti) {
                const catchable_t *t = caught_type(ti, exc_base, rva(base, type), adjectives);
                if (!t) continue;
                copy_object((void *)rec->ExceptionInformation[1], orig_frame, adjectives, rva(base, type),
                            (int32_t)catch_obj, t, exc_base);
            } else if (type) {
                continue;
            }
            unwind_to_catch(rec, ctx, frame, dc, orig_frame, -1, low, high, rva(base, handler), ncont, cont);
        }
    }
}

DWORD WINAPI __CxxFrameHandler4(EXCEPTION_RECORD *rec, uint64_t frame, void *ctx, DISPATCHER_CONTEXT *dc) {
    uint64_t base = dc->ImageBase, orig_frame = frame;
    fi4_t fi;
    int state, is_consolidate;
    const throw_info_t *ti = 0;
    if (!dc->FunctionEntry || decomp_fi4(base, *(const int32_t *)dc->HandlerData, dc->FunctionEntry->begin, &fi) != 0)
        return 1;
    is_consolidate = rec->ExceptionCode == STATUS_UNWIND_CONSOLIDATE && rec->NumberParameters >= 12 &&
                     rec->ExceptionInformation[0] == (ULONG_PTR)call_catch;
    if ((fi.flags & FI4_EHS) && rec->ExceptionCode != CXX_EXCEPTION && !is_consolidate &&
        !(rec->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND)))
        return 1;                                          /* C++ exceptions only */
    if (fi.flags & FI4_CATCH) orig_frame = *(uint64_t *)(uintptr_t)(frame + fi.disp_frame);
    state = state4(&fi, base, dc->ControlPc);
    if (rec->ExceptionFlags & (EH_UNWINDING | EH_EXIT_UNWIND)) {
        int last = -1;
        catch_info_t *running = (fi.flags & FI4_CATCH) ? 0 : active_catch(orig_frame);
        if ((rec->ExceptionFlags & EH_TARGET_UNWIND) && is_consolidate)
            last = (int)(intptr_t)rec->ExceptionInformation[3];
        /* a frame whose catch block runs: its try block's scopes are gone already */
        else if (running && state > running->try_low)
            state = running->try_low;
        local_unwind4(orig_frame, base, &fi, state, last);
        return 1;
    }
    if (!fi.try_map) return 1;
    if (rec->ExceptionCode == CXX_EXCEPTION) {
        if (!rec->ExceptionInformation[1] && !rec->ExceptionInformation[2]) {      /* throw; */
            EXCEPTION_RECORD *cur = cxx_current_exception(0);
            if (!cur) return 1;
            *rec = *cur;
        }
        ti = (const throw_info_t *)rec->ExceptionInformation[2];
    }
    find_catch4(rec, ctx, frame, dc, &fi, ti, orig_frame, state);
    return 1;
}

/* ---- RTTI: dynamic_cast and typeid -------------------------------------------------------------
 * An object's vtable is preceded by a pointer to its complete object
 * locator: where this vtable's part is in the whole object, its type, and
 * its class hierarchy (every base class, with how to find it from the
 * whole object).  Image-relative, from the locator's own RVA. */

typedef struct { uint32_t signature, offset, cd_offset; int32_t type, hierarchy, self; } col_t;
typedef struct { uint32_t signature, attributes, count; int32_t bases; } hierarchy_t;
typedef struct { int32_t type; uint32_t contained; this_offsets_t offsets; uint32_t attributes; int32_t hierarchy; } base_desc_t;

static const col_t *col_of(const void *obj) { return ((const col_t *const *)*(void *const *)obj)[-1]; }
static uint64_t col_base(const col_t *c) {
    wmodule_t *module_at(const void *p);
    if (c->signature == 1) return (uint64_t)(uintptr_t)c - (uint32_t)c->self;
    wmodule_t *m = module_at(c);
    return m ? (uint64_t)(uintptr_t)m->base : 0;
}

/* what an RTTI function, called from a program, is to throw (rtti_entry raises it) */
static __thread int rtti_throw;                       /* 1 std::bad_cast, 2 std::bad_typeid */

__attribute__((ms_abi, used)) void *rtti_dynamic_cast(void *obj, int32_t vfdelta, void *src, const type_info_t *dst,
                                                     int is_ref) {
    const col_t *c;
    const hierarchy_t *h;
    const int32_t *bases;
    uint64_t base;
    char *whole;
    (void)src;
    rtti_throw = 0;
    if (!obj) return 0;
    c = col_of((char *)obj + vfdelta);
    base = col_base(c);
    h = rva(base, c->hierarchy);
    bases = rva(base, h->bases);
    whole = (char *)obj + vfdelta - c->offset;
    for (uint32_t i = 0; i < h->count; i++) {
        const base_desc_t *b = rva(base, bases[i]);
        const type_info_t *t = rva(base, b->type);
        if (t == dst || (t && dst && !strcmp(t->mangled, dst->mangled))) return this_pointer(&b->offsets, whole);
    }
    if (is_ref) rtti_throw = 1;
    return 0;
}
__attribute__((ms_abi, used)) void *rtti_typeid(void *obj) {
    const col_t *c;
    rtti_throw = 0;
    if (!obj) {
        rtti_throw = 2;
        return 0;
    }
    c = col_of(obj);
    return rva(col_base(c), c->type);
}
__attribute__((ms_abi, used)) int rtti_pending(void) { return rtti_throw; }

void *WINAPI __RTCastToVoid(void *obj) { return obj ? (char *)obj - col_of(obj)->offset : 0; }

/* std::bad_cast and std::bad_typeid: an std::exception (vtable, what,
 * do-free) with its RTTI and throw info, all relative to this block */
typedef struct { const void *const *vtable; const char *what; uint8_t do_free; } std_exception_t;
static struct {
    struct { void *vt; char *name; char mangled[32]; } td[3];          /* bad_cast, bad_typeid, exception */
    throw_info_t ti[2];
    uint32_t cta[2][3];
    catchable_t ct[3];
    col_t col[2];
    hierarchy_t hier[2];
    int32_t base_rvas[2][2];
    base_desc_t bd[3];
    const void *vtables[2][3];                                          /* [0] the locator, then the vtable */
} rtti_block;
static __thread std_exception_t rtti_object;
static __thread ULONG_PTR rtti_args[4];

static const char *WINAPI std_exception_what(const std_exception_t *e) { return e->what ? e->what : "Unknown exception"; }
static void *WINAPI std_exception_dtor(std_exception_t *e, unsigned flags) {
    (void)flags;
    return e;
}

#define RB(p) ((int32_t)((char *)(p) - (char *)&rtti_block))
static void rtti_block_init(void) {
    static const char *const names[3] = { ".?AVbad_cast@std@@", ".?AVbad_typeid@std@@", ".?AVexception@std@@" };
    for (int i = 0; i < 3; i++) {
        strcpy(rtti_block.td[i].mangled, names[i]);
        rtti_block.ct[i].type_info = RB(&rtti_block.td[i]);
        rtti_block.ct[i].offsets.vbase_descr = -1;
        rtti_block.ct[i].size = sizeof(std_exception_t);
        rtti_block.bd[i].type = RB(&rtti_block.td[i]);
        rtti_block.bd[i].offsets.vbase_descr = -1;
    }
    for (int k = 0; k < 2; k++) {
        rtti_block.cta[k][0] = 2;
        rtti_block.cta[k][1] = (uint32_t)RB(&rtti_block.ct[k]);
        rtti_block.cta[k][2] = (uint32_t)RB(&rtti_block.ct[2]);
        rtti_block.ti[k].type_info_table = RB(rtti_block.cta[k]);
        rtti_block.base_rvas[k][0] = RB(&rtti_block.bd[k]);
        rtti_block.base_rvas[k][1] = RB(&rtti_block.bd[2]);
        rtti_block.hier[k].count = 2;
        rtti_block.hier[k].bases = RB(rtti_block.base_rvas[k]);
        rtti_block.col[k].signature = 1;
        rtti_block.col[k].type = RB(&rtti_block.td[k]);
        rtti_block.col[k].hierarchy = RB(&rtti_block.hier[k]);
        rtti_block.col[k].self = RB(&rtti_block.col[k]);
        rtti_block.vtables[k][0] = &rtti_block.col[k];
        rtti_block.vtables[k][1] = (const void *)std_exception_dtor;
        rtti_block.vtables[k][2] = (const void *)std_exception_what;
    }
}

__attribute__((ms_abi, used)) ULONG_PTR *rtti_throw_args(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    int k = rtti_throw == 2 ? 1 : 0;
    pthread_once(&once, rtti_block_init);
    rtti_throw = 0;
    rtti_object.vtable = &rtti_block.vtables[k][1];
    rtti_object.what = k ? "Attempted a typeid of nullptr pointer!" : "Bad dynamic_cast!";
    rtti_object.do_free = 0;
    rtti_args[0] = CXX_MAGIC_VC6;
    rtti_args[1] = (ULONG_PTR)&rtti_object;
    rtti_args[2] = (ULONG_PTR)&rtti_block.ti[k];
    rtti_args[3] = (ULONG_PTR)&rtti_block;
    uncaught++;
    return rtti_args;
}

/* the entries: the C part, then, when it says so, the exception raised
 * from the program's call (its registers as they were) */
#define RTTI_ENTRY(name, impl)                                    \
    __asm__(".text\n"                                             \
            ".globl " #name "\n" #name ":\n"                      \
            "  sub $0x38, %rsp\n"                                 \
            "  mov 0x60(%rsp), %rax\n"                            \
            "  mov %rax, 0x20(%rsp)\n"                            \
            "  call " #impl "\n"                                  \
            "  mov %rax, 0x28(%rsp)\n"                            \
            "  call rtti_pending\n"                               \
            "  test %eax, %eax\n"                                 \
            "  jnz 1f\n"                                          \
            "  mov 0x28(%rsp), %rax\n"                            \
            "  add $0x38, %rsp\n"                                 \
            "  ret\n"                                             \
            "1:\n"                                                \
            "  call rtti_throw_args\n"                            \
            "  add $0x38, %rsp\n"                                 \
            "  mov %rax, %r9\n"                                   \
            "  mov $0xE06D7363, %ecx\n"                           \
            "  mov $1, %edx\n"                                    \
            "  mov $4, %r8d\n"                                    \
            "  jmp RaiseException\n");
RTTI_ENTRY(__RTDynamicCast, rtti_dynamic_cast)
RTTI_ENTRY(__RTtypeid, rtti_typeid)
