/* Bad pointers handed to the Windows API fail the call, as on Windows.
 *
 * Windows checks what programs pass it (its kernel probes every pointer, and
 * much of user32 and gdi32 tries before it touches one): a NULL or bad
 * pointer makes the call fail, with ERROR_NOACCESS or ERROR_INVALID_PARAMETER,
 * where the layer's C code would fault and stop the program.  Rather than a
 * check in each of its thousands of functions, the layer's functions are
 * entered through a barrier: a thunk that notes the caller's state and the
 * call's arguments, and lets the function return through it.  A memory fault
 * in the layer's own code (icda-win or the libraries it uses, not a
 * program's), at NULL or within an argument the program passed, then ends the
 * call as a failure: the function's own (FALSE, NULL, an error code ...,
 * from apis.c), with ERROR_NOACCESS.  Other faults are the layer's own bugs
 * and stop the program as before.
 *
 * The thunk enters the function with the stack as the caller left it (its
 * stack arguments where they were) and its return address replaced with the
 * barrier's, which pops the note and returns to the caller.  Not entered so:
 * functions that look at their return address or the stack (the unwinder,
 * RaiseException, context capture), functions called very often (locks,
 * TLS, the last error, heaps), and the C runtimes and OpenGL (whose callers
 * expect them to crash on bad pointers, or want every cycle).
 * ICDA_WIN_BARRIER=0 turns it off. */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#include <dlfcn.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>

typedef struct barrier_info {
    void                *fn;          /* first: the thunk jumps through it */
    uint64_t             fail;        /* what the function returns when it fails */
    uint32_t             lasterr;     /* the last error it sets then */
    int                  reported;
    struct barrier_info *next;        /* by function, in the cache */
    void                *thunk;
    char                 name[80];
} barrier_info_t;

#define NARGS 12
typedef struct {
    barrier_info_t *info;
    uint64_t        entry_rsp;        /* where the return address was */
    uint64_t        ret;
    uint64_t        rbx, rbp, rdi, rsi, r12, r13, r14, r15;
    uint8_t         xmm[10][16];      /* xmm6-15, kept by the callee in Windows' convention */
    uint64_t        args[NARGS];
} barrier_rec_t;

_Static_assert(offsetof(barrier_rec_t, rbx) == 24 && offsetof(barrier_rec_t, r15) == 80 &&
                   offsetof(barrier_rec_t, xmm) == 88 && offsetof(barrier_rec_t, args) == 248,
               "the asm below knows these offsets");

#define DEPTH 96
static __thread barrier_rec_t recs[DEPTH];
static __thread int depth;

/* called from barrier_entry (in Windows' convention, so it keeps rbx, rbp,
 * rdi, rsi, r12-r15 and xmm6-15 as the program had them): a note for this
 * call, or 0 (too deep: the call goes without one) */
__attribute__((ms_abi, used)) barrier_rec_t *barrier_push(barrier_info_t *info, uint64_t entry_rsp, uint64_t *regargs) {
    barrier_rec_t *r;
    const uint64_t *stack = (const uint64_t *)entry_rsp;
    /* notes of calls left without returning (a longjmp, an exception) are deeper than this one */
    while (depth > 0 && recs[depth - 1].entry_rsp <= entry_rsp) depth--;
    if (depth >= DEPTH) return 0;
    r = &recs[depth++];
    r->info = info;
    r->entry_rsp = entry_rsp;
    r->ret = stack[0];
    for (int i = 0; i < 4; i++) r->args[i] = regargs[i];
    for (int i = 4; i < NARGS; i++) r->args[i] = stack[1 + i];    /* past the return address and 4 home slots */
    return r;
}

/* from barrier_return: the caller's return address (rsp: where the function returned to) */
__attribute__((ms_abi, used)) uint64_t barrier_pop(uint64_t rsp) {
    while (depth > 0 && recs[depth - 1].entry_rsp + 8 < rsp) depth--;      /* stale notes */
    if (depth > 0 && recs[depth - 1].entry_rsp + 8 == rsp) return recs[--depth].ret;
    wfatal("Windows program", "The Windows layer lost track of a call it made (barrier).");
}

/* r11 = the function's info; [rsp] = the caller's return address */
__asm__(".text\n"
        ".globl barrier_entry\n"
        "barrier_entry:\n"
        "  sub $0xa8, %rsp\n"
        "  mov %rcx, 0x20(%rsp)\n"
        "  mov %rdx, 0x28(%rsp)\n"
        "  mov %r8, 0x30(%rsp)\n"
        "  mov %r9, 0x38(%rsp)\n"
        "  movdqu %xmm0, 0x40(%rsp)\n"
        "  movdqu %xmm1, 0x50(%rsp)\n"
        "  movdqu %xmm2, 0x60(%rsp)\n"
        "  movdqu %xmm3, 0x70(%rsp)\n"
        "  mov %r11, 0x80(%rsp)\n"
        "  mov %r11, %rcx\n"
        "  lea 0xa8(%rsp), %rdx\n"
        "  lea 0x20(%rsp), %r8\n"
        "  call barrier_push\n"
        "  test %rax, %rax\n"
        "  jz 1f\n"
        "  mov %rbx, 24(%rax)\n"
        "  mov %rbp, 32(%rax)\n"
        "  mov %rdi, 40(%rax)\n"
        "  mov %rsi, 48(%rax)\n"
        "  mov %r12, 56(%rax)\n"
        "  mov %r13, 64(%rax)\n"
        "  mov %r14, 72(%rax)\n"
        "  mov %r15, 80(%rax)\n"
        "  movdqu %xmm6, 88(%rax)\n"
        "  movdqu %xmm7, 104(%rax)\n"
        "  movdqu %xmm8, 120(%rax)\n"
        "  movdqu %xmm9, 136(%rax)\n"
        "  movdqu %xmm10, 152(%rax)\n"
        "  movdqu %xmm11, 168(%rax)\n"
        "  movdqu %xmm12, 184(%rax)\n"
        "  movdqu %xmm13, 200(%rax)\n"
        "  movdqu %xmm14, 216(%rax)\n"
        "  movdqu %xmm15, 232(%rax)\n"
        "  lea barrier_return(%rip), %r10\n"
        "  mov %r10, 0xa8(%rsp)\n"
        "1:\n"
        "  mov 0x80(%rsp), %r11\n"
        "  mov 0x20(%rsp), %rcx\n"
        "  mov 0x28(%rsp), %rdx\n"
        "  mov 0x30(%rsp), %r8\n"
        "  mov 0x38(%rsp), %r9\n"
        "  movdqu 0x40(%rsp), %xmm0\n"
        "  movdqu 0x50(%rsp), %xmm1\n"
        "  movdqu 0x60(%rsp), %xmm2\n"
        "  movdqu 0x70(%rsp), %xmm3\n"
        "  add $0xa8, %rsp\n"
        "  jmp *(%r11)\n"
        "\n"
        "barrier_return:\n"
        "  sub $0x40, %rsp\n"
        "  mov %rax, 0x20(%rsp)\n"
        "  movdqu %xmm0, 0x30(%rsp)\n"
        "  lea 0x40(%rsp), %rcx\n"
        "  call barrier_pop\n"
        "  mov %rax, %r11\n"
        "  mov 0x20(%rsp), %rax\n"
        "  movdqu 0x30(%rsp), %xmm0\n"
        "  add $0x40, %rsp\n"
        "  jmp *%r11\n");
extern char barrier_entry[];

/* ---- which functions --------------------------------------------------------------------- */

static int enabled = -1;

static int barrier_dll(const char *dll) {
    static const char *const dlls[] = {
        "kernel32.dll", "ntdll.dll", "advapi32.dll", "user32.dll", "gdi32.dll", "shell32.dll", "ole32.dll",
        "oleaut32.dll", "version.dll", "ws2_32.dll", "winmm.dll", "setupapi.dll", "imm32.dll", "bcrypt.dll",
        "cabinet.dll", "misc.dll", 0
    };
    for (int i = 0; dlls[i]; i++)
        if (!strcmp(dll, dlls[i])) return 1;
    return 0;
}

static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }

static int barrier_function(const char *n) {
    static const char *const hot[] = {
        "GetLastError", "SetLastError", "RtlGetLastWin32Error", "RtlSetLastWin32Error", "GetCurrentThreadId",
        "GetCurrentProcessId", "GetCurrentThread", "GetCurrentProcess", "GetProcessHeap", "GetTickCount",
        "GetTickCount64", "QueryPerformanceCounter", "QueryPerformanceFrequency", "RtlQueryPerformanceCounter",
        "GetSystemTimeAsFileTime", "GetSystemTimePreciseAsFileTime", "EncodePointer", "DecodePointer",
        "RtlEncodePointer", "RtlDecodePointer", "IsProcessorFeaturePresent", "SwitchToThread", "IsDebuggerPresent",
        "RaiseException", "RtlRaiseException", "RtlPcToFileHeader", "RtlRestoreContext", "NtContinue",
        "ExitProcess", "ExitThread", "FreeLibraryAndExitThread", "RtlExitUserThread", "RtlExitUserProcess",
        "WaitOnAddress", "WakeByAddressSingle", "WakeByAddressAll", "GetModuleHandleExW", "GetModuleHandleExA", 0
    };
    static const char *const prefixes[] = {
        "Tls", "Fls", "Interlocked", "RtlInterlocked", "_Interlocked", "Heap", "RtlAllocateHeap", "RtlFreeHeap",
        "RtlReAllocateHeap", "RtlSizeHeap", "Local", "Global", 0
    };
    static const char *const within[] = { "CriticalSection", "SRWLock", "ConditionVariable", "InitOnce", "Unwind",
                                          "Capture", "FunctionTable", "LookupFunctionEntry", "VirtualUnwind", 0 };
    for (int i = 0; hot[i]; i++)
        if (!strcmp(n, hot[i])) return 0;
    for (int i = 0; prefixes[i]; i++)
        if (starts(n, prefixes[i])) return 0;
    for (int i = 0; within[i]; i++)
        if (strstr(n, within[i])) return 0;
    return 1;
}

/* ---- thunks ----------------------------------------------------------------------------------- */

#define BUCKETS 1024
static barrier_info_t *cache[BUCKETS];
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *code;
static int code_used;

/* what fails, by what the function returns (apis.c), and the last error then */
static void failure_of(barrier_info_t *b, const char *dll, const char *name) {
    int k = api_kind(name);
    int known;
    b->fail = api_failure(name, &known);
    b->lasterr = 998;                                   /* ERROR_NOACCESS */
    switch (k) {
    case 'h': b->fail = 0x80004003u; b->lasterr = 0; break;          /* E_POINTER */
    case 'n': b->fail = 0xC0000005u; b->lasterr = 0; break;          /* STATUS_ACCESS_VIOLATION */
    case 'e': b->fail = 998; break;
    case 'm': b->fail = 11; break;                                  /* MMSYSERR_INVALPARAM */
    case 'i': b->fail = 0; break;
    default: break;
    }
    if (!k && !strcmp(dll, "ntdll.dll") && (starts(name, "Nt") || starts(name, "Zw"))) b->fail = 0xC0000005u;
}

/* the function as programs get it: through a thunk, when it is one the barrier covers */
void *barrier_wrap(const char *dll, const char *name, int ordinal, void *fn) {
    barrier_info_t *b;
    unsigned h;
    if (!fn) return 0;
    if (enabled < 0) {
        const char *e = getenv("ICDA_WIN_BARRIER");
        enabled = !(e && *e == '0');
    }
    if (!enabled || !dll || !barrier_dll(dll)) return fn;
    if (!name) name = api_ordinal_name(dll, ordinal);
    if (!name || !barrier_function(name)) return fn;
    h = (unsigned)(((uintptr_t)fn >> 4) % BUCKETS);
    pthread_mutex_lock(&cache_lock);
    for (b = cache[h]; b; b = b->next)
        if (b->fn == fn) {
            pthread_mutex_unlock(&cache_lock);
            return b->thunk;
        }
    if (!code || code_used + 32 > 65536) {
        code = mmap(0, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code == MAP_FAILED) {
            code = 0;
            pthread_mutex_unlock(&cache_lock);
            return fn;
        }
        code_used = 0;
    }
    b = calloc(1, sizeof(*b));
    if (!b) {
        pthread_mutex_unlock(&cache_lock);
        return fn;
    }
    b->fn = fn;
    snprintf(b->name, sizeof(b->name), "%s", name);
    failure_of(b, dll, name);
    {
        uint8_t *c = code + code_used;
        void *e = barrier_entry;
        code_used += 32;
        c[0] = 0x49, c[1] = 0xBB;                 /* movabs r11, b */
        memcpy(c + 2, &b, 8);
        c[10] = 0x49, c[11] = 0xBA;               /* movabs r10, barrier_entry */
        memcpy(c + 12, &e, 8);
        c[20] = 0x41, c[21] = 0xFF, c[22] = 0xE2; /* jmp r10 */
        b->thunk = c;
    }
    b->next = cache[h];
    cache[h] = b;
    pthread_mutex_unlock(&cache_lock);
    return b->thunk;
}

/* ---- a fault ----------------------------------------------------------------------------------- */

/* the address is NULL's page, or within something the program passed */
static int bad_argument(const barrier_rec_t *r, uintptr_t a) {
    if (a < 0x10000) return 1;
    for (int i = 0; i < NARGS; i++) {
        uintptr_t p = (uintptr_t)r->args[i];
        if (p >= 0x10000 && a >= p && a - p < 0x100000) return 1;
    }
    return 0;
}

/* from the fault handler: 1 when the fault ended a call as its failure
 * (the context is then the caller's, after the call) */
int barrier_recover(void *ucontext, uintptr_t addr) {
    ucontext_t *uc = ucontext;
    greg_t *g = uc->uc_mcontext.gregs;
    uintptr_t rip = (uintptr_t)g[REG_RIP], rsp = (uintptr_t)g[REG_RSP];
    barrier_rec_t *r;
    Dl_info di;
    if (depth <= 0) return 0;
    /* the layer's own code (an ELF object), not a program's */
    if (!dladdr((void *)rip, &di) || !di.dli_fname) return 0;
    while (depth > 0 && recs[depth - 1].entry_rsp < rsp) depth--;    /* calls already left */
    if (depth <= 0) return 0;
    r = &recs[depth - 1];
    if (!bad_argument(r, addr)) return 0;
    depth--;
    g[REG_RBX] = (greg_t)r->rbx, g[REG_RBP] = (greg_t)r->rbp, g[REG_RDI] = (greg_t)r->rdi, g[REG_RSI] = (greg_t)r->rsi;
    g[REG_R12] = (greg_t)r->r12, g[REG_R13] = (greg_t)r->r13, g[REG_R14] = (greg_t)r->r14, g[REG_R15] = (greg_t)r->r15;
    g[REG_RSP] = (greg_t)(r->entry_rsp + 8);
    g[REG_RIP] = (greg_t)r->ret;
    g[REG_RAX] = (greg_t)r->info->fail;
    if (uc->uc_mcontext.fpregs)
        for (int i = 0; i < 10; i++) memcpy(&uc->uc_mcontext.fpregs->_xmm[6 + i], r->xmm[i], 16);
    if (r->info->lasterr) wset_last_error(r->info->lasterr);
    if (!r->info->reported++ || wtrace)
        wlog("%s: a bad pointer (%p) was passed; the call fails", r->info->name, (void *)addr);
    return 1;
}
