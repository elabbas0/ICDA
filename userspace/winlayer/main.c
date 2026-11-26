/* icda-win: runs a Windows program.
 *
 *   icda-win program.exe [arguments...]
 *
 * ICDA starts it for any Windows program (the kernel hands a PE file to
 * /usr/bin/icda-win, as it hands a script to its interpreter).  The
 * program runs on a thread with a Windows-sized stack, with its TEB at GS;
 * its current folder is its own folder, as when Explorer starts one.
 *
 * ICDA_WIN_TRACE=1 says what is loaded and called of note; 2 also every
 * unimplemented call.  ICDA_WIN_PREFIX is the C: drive's folder
 * (~/.windows/c by default). */
#define _GNU_SOURCE
#include "win.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <ucontext.h>
#include <limits.h>
#include <sys/mman.h>
#include <dlfcn.h>

char   *g_cmdline_a;
WCHAR  *g_cmdline_w;
int     g_argc;
char  **g_argv_a;
WCHAR **g_argv_w;
char    g_exe_win[MAX_PATH * 2];
char    g_exe_unix[1024];

int path_find(const char *wpath, char *out, size_t cap);

/* Windows quoting: what CommandLineToArgvW gives back */
static void quote_arg(const char *a, char *out, size_t cap, size_t *o) {
    int need = !*a || strpbrk(a, " \t\"") != 0;
    if (!need) {
        size_t n = strlen(a);
        if (*o + n + 1 < cap) memcpy(out + *o, a, n), *o += n;
        return;
    }
    if (*o + 1 < cap) out[(*o)++] = '"';
    for (const char *p = a;; p++) {
        size_t bs = 0;
        while (*p == '\\') p++, bs++;
        if (!*p) {
            for (size_t i = 0; i < bs * 2 && *o + 1 < cap; i++) out[(*o)++] = '\\';
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1 && *o + 1 < cap; i++) out[(*o)++] = '\\';
        } else {
            for (size_t i = 0; i < bs && *o + 1 < cap; i++) out[(*o)++] = '\\';
        }
        if (*o + 1 < cap) out[(*o)++] = *p;
    }
    if (*o + 1 < cap) out[(*o)++] = '"';
}

/* ---- crashes: say where, in which module ------------------------------------------------- */

static const char *crash_title = "Windows program";

/* where an address is: a Windows module and offset, or a Unix library and symbol */
static void where_is(uintptr_t a, char *out, size_t cap) {
    Dl_info di;
    for (wmodule_t *m = ld_modules(); m; m = m->next) {
        if (a >= (uintptr_t)m->base && a < (uintptr_t)m->base + m->size) {
            snprintf(out, cap, "%s+0x%lx", m->name, (unsigned long)(a - (uintptr_t)m->base));
            return;
        }
    }
    if (dladdr((void *)a, &di) && di.dli_fname) {
        const char *b = strrchr(di.dli_fname, '/');
        if (di.dli_sname)
            snprintf(out, cap, "%s:%s+0x%lx", b ? b + 1 : di.dli_fname, di.dli_sname, (unsigned long)(a - (uintptr_t)di.dli_saddr));
        else
            snprintf(out, cap, "%s+0x%lx", b ? b + 1 : di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase));
        return;
    }
    snprintf(out, cap, "%p", (void *)a);
}

static int is_code(uintptr_t a) {
    Dl_info di;
    for (wmodule_t *m = ld_modules(); m; m = m->next)
        if (a >= (uintptr_t)m->base && a < (uintptr_t)m->base + m->size) return 1;
    return a > 0x10000 && dladdr((void *)a, &di) && di.dli_fname != 0;
}

/* return addresses on a stack, as module+offset (for a crash, or an exception raised) */
void wlog_stack(const uintptr_t *sp, int most) {
    int shown = 0;
    for (int i = 0; i < 512 && shown < most && sp; i++) {
        uintptr_t v = sp[i];
        if (is_code(v)) {
            char w[200];
            where_is(v, w, sizeof(w));
            wlog("  stack+%03x: %s", i * 8, w);
            shown++;
        }
    }
}

static int module_at_pc(uintptr_t a) {
    for (wmodule_t *m = ld_modules(); m; m = m->next)
        if (!m->builtin && a >= (uintptr_t)m->base && a < (uintptr_t)m->base + m->size) return 1;
    return 0;
}

static void crash(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    /* a bad pointer a program passed to the layer: that call fails */
    if ((sig == SIGSEGV || sig == SIGBUS) && barrier_recover(ctx, (uintptr_t)si->si_addr)) return;
    /* a fault in a program's code: an exception it may handle */
    {
        int except_from_signal(int sig, siginfo_t *si, void *ucontext);
        if (except_from_signal(sig, si, ctx)) return;
    }
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    uintptr_t *sp = (uintptr_t *)uc->uc_mcontext.gregs[REG_RSP];
    char where[200], msg[700];
    where_is(rip, where, sizeof(where));
    snprintf(msg, sizeof(msg), "%s stopped: %s in %s (address %p).",
             strrchr(g_exe_win, '\\') ? strrchr(g_exe_win, '\\') + 1 : g_exe_win,
             sig == SIGSEGV ? "a memory access fault" : sig == SIGILL ? "an unknown instruction" :
             sig == SIGFPE ? "an arithmetic fault" : sig == SIGTRAP ? "a breakpoint (a check in the program failed)" :
             "a bus error",
             where, si->si_addr);
    /* in Windows code: its callers, frame by frame, from the modules' unwind data */
    if (module_at_pc(rip)) {
        void unwind_report(void *ctx);
        static uint8_t wctx[0x4D0] __attribute__((aligned(16)));
        static const int order[16] = { REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                       REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15 };
        memset(wctx, 0, sizeof(wctx));
        for (int i = 0; i < 16; i++) ((uint64_t *)(wctx + 0x78))[i] = (uint64_t)uc->uc_mcontext.gregs[order[i]];
        *(uint64_t *)(wctx + 0xF8) = (uint64_t)rip;
        wlog("crash: rip %s, rsp %p; the frames:", where, (void *)sp);
        unwind_report(wctx);
    } else
    /* what called it: return addresses on the stack */
    {
        int shown = 0;
        wlog("crash: rip %s, rsp %p", where, (void *)sp);
        for (int i = 0; i < 512 && shown < 12 && sp; i++) {
            uintptr_t v = sp[i];
            if (is_code(v)) {
                char w[200];
                where_is(v, w, sizeof(w));
                wlog("  stack+%03x: %s", i * 8, w);
                shown++;
            }
        }
    }
    wnotice(crash_title, msg);
    _exit(128 + sig);
}

void crash_handler_thread(void) {
    /* an alternate stack, so a stack overflow is reported too */
    stack_t ss;
    ss.ss_sp = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ss.ss_size = 65536;
    ss.ss_flags = 0;
    if (ss.ss_sp != MAP_FAILED) sigaltstack(&ss, 0);
}

/* stopped from outside (timeout, kill): what was printed is kept */
static void stop_signal(int sig) {
    fflush(0);
    _exit(128 + sig);
}

static void crash_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGFPE, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stop_signal);
    signal(SIGINT, stop_signal);
    signal(SIGHUP, stop_signal);
    /* a program's console output shows as it is written, also in a log file */
    setvbuf(stdout, 0, _IOLBF, 0);
}

/* ---- compiled shaders, kept ------------------------------------------------------------- */

/* Mesa (llvmpipe) compiles each shader the first time a program draws with
 * it, and a game stutters while it does; what it compiled is kept on disk
 * for the next run.  Mesa's own place is under the account's home folder in
 * the password file, which a Linux root need not have: here it is beside the
 * C: drive's folder (~/.windows/cache/mesa), unless the user chose one. */
static void shader_cache(void) {
    char dir[1100], *s;
    if (getenv("MESA_SHADER_CACHE_DIR") || getenv("XDG_CACHE_HOME")) return;
    snprintf(dir, sizeof(dir), "%s", path_prefix());
    s = strrchr(dir, '/');
    if (!s || s == dir) return;
    snprintf(s, sizeof(dir) - (size_t)(s - dir), "/cache/mesa");
    setenv("MESA_SHADER_CACHE_DIR", dir, 0);
    if (wtrace) wlog("compiled shaders kept in %s", dir);
}

/* ---- the program's thread ----------------------------------------------------------------- */

void ExitProcess(UINT code) __attribute__((ms_abi));

static void *program_thread(void *arg) {
    pthread_attr_t a;
    void *addr = 0;
    size_t size = 0;
    int rc;
    (void)arg;
    if (pthread_getattr_np(pthread_self(), &a) == 0) {
        pthread_attr_getstack(&a, &addr, &size);
        pthread_attr_destroy(&a);
    }
    thread_main_init((uint8_t *)addr + size, addr);
    crash_handler_thread();
    rc = ld_run_exe(g_exe_unix, g_argv_a);
    ExitProcess((UINT)rc);
    return 0;
}

int main(int argc, char **argv) {
    const char *t = getenv("ICDA_WIN_TRACE");
    char found[1024], cmd[8192];
    size_t o = 0;
    pthread_t th;
    pthread_attr_t a;
    wtrace = t ? atoi(t) : 0;
    if (argc < 2) {
        fprintf(stderr, "usage: icda-win program.exe [arguments...]\n");
        return 2;
    }
    path_init();
    shader_cache();
    /* the program: a Unix path, or a Windows one ("C:\Games\x.exe") */
    if (!realpath(argv[1], found)) {
        char tmp[1024];
        if (path_find(argv[1], tmp, sizeof(tmp)) != 0 || !realpath(tmp, found))
            wfatal("Windows program", "%s was not found.", argv[1]);
    }
    snprintf(g_exe_unix, sizeof(g_exe_unix), "%s", found);
    path_to_win(g_exe_unix, g_exe_win, sizeof(g_exe_win));
    {
        const char *b = strrchr(g_exe_unix, '/');
        crash_title = b ? b + 1 : g_exe_unix;
    }
    /* Explorer starts a program in its own folder */
    if (!getenv("ICDA_WIN_KEEP_CWD")) {
        char dir[1024], *s;
        snprintf(dir, sizeof(dir), "%s", g_exe_unix);
        s = strrchr(dir, '/');
        if (s) {
            *s = 0;
            if (chdir(dir[0] ? dir : "/") != 0) wlog("cannot enter %s", dir);
        }
    }
    /* the command line and its arguments, the Windows way.  A program a
     * Windows program started gets the exact line it was given. */
    {
        const char *given = getenv("ICDA_WIN_CMDLINE");
        if (given && *given) {
            g_cmdline_a = strdup(given);
            unsetenv("ICDA_WIN_CMDLINE");
            g_argc = split_cmdline(g_cmdline_a, &g_argv_a);
        } else {
            g_argc = argc - 1;
            g_argv_a = calloc((size_t)g_argc + 1, sizeof(char *));
            g_argv_a[0] = g_exe_win;
            for (int i = 2; i < argc; i++) g_argv_a[i - 1] = argv[i];
            for (int i = 0; i < g_argc; i++) {
                if (i) cmd[o++] = ' ';
                quote_arg(g_argv_a[i], cmd, sizeof(cmd), &o);
            }
            cmd[o] = 0;
            g_cmdline_a = strdup(cmd);
        }
        g_argv_w = calloc((size_t)g_argc + 1, sizeof(WCHAR *));
        for (int i = 0; i < g_argc; i++) g_argv_w[i] = utf8_to_w_dup(g_argv_a[i]);
        g_cmdline_w = utf8_to_w_dup(g_cmdline_a);
    }
    if (wtrace) wlog("running %s as %s: %s", g_exe_unix, g_exe_win, g_cmdline_a);
    crash_handlers();
    /* 16 MB of stack: more than any program asks of Windows */
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&th, &a, program_thread, 0) != 0) wfatal("Windows program", "No thread could be started.");
    pthread_join(th, 0);
    return 0;
}
