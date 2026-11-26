/* Exceptions as Windows dispatches them: a vectored handler that handles a
 * fault in the program's code and goes on, a divide by zero, an exception
 * raised and continued, and the unhandled exception filter.
 * Prints one line a check; a line starting FAIL says what was wrong. */
#include <windows.h>
#include <stdio.h>

static int fails;
static void check(const char *what, int ok) {
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

static volatile LONG seen_code;
static volatile ULONG_PTR seen_addr, seen_kind;

/* handles an access violation by skipping the faulting 3-byte instruction
 * (mov (%rax),%eax is 2 bytes; the test's load is known below) */
static LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        seen_code = ep->ExceptionRecord->ExceptionCode;
        seen_kind = ep->ExceptionRecord->ExceptionInformation[0];
        seen_addr = ep->ExceptionRecord->ExceptionInformation[1];
        ep->ContextRecord->Rip += 2;             /* past "mov (%rcx), %eax" */
        ep->ContextRecord->Rax = 1234;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        seen_code = ep->ExceptionRecord->ExceptionCode;
        ep->ContextRecord->Rip += 2;             /* past "div %ecx" (f7 f1) */
        ep->ContextRecord->Rax = 77;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (ep->ExceptionRecord->ExceptionCode == 0xE0001234) {
        seen_code = ep->ExceptionRecord->ExceptionCode;
        seen_addr = ep->ExceptionRecord->ExceptionInformation[0];
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI last_chance(EXCEPTION_POINTERS *ep) {
    printf("ok unhandled filter called for 0x%lx\n", (unsigned long)ep->ExceptionRecord->ExceptionCode);
    printf("done, %d failed\n", fails);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;            /* the program ends, its exit code the exception's */
}

int main(void) {
    void *h = AddVectoredExceptionHandler(1, on_fault);
    int v = 0;
    ULONG_PTR arg = 99;
    check("AddVectoredExceptionHandler", h != NULL);
    /* a read of address 0x10 in the program's own code */
    __asm__ volatile("mov $0x10, %%rcx\n\tmov (%%rcx), %%eax" : "=a"(v) : : "rcx", "memory");
    check("access violation handled, the program goes on", seen_code == (LONG)EXCEPTION_ACCESS_VIOLATION && v == 1234);
    check("  with what was read and where", seen_kind == 0 && seen_addr == 0x10);
    seen_code = 0;
    __asm__ volatile("xor %%edx, %%edx\n\tmov $5, %%eax\n\txor %%ecx, %%ecx\n\tdiv %%ecx" : "=a"(v) : : "rcx", "rdx");
    check("divide by zero handled", seen_code == (LONG)EXCEPTION_INT_DIVIDE_BY_ZERO && v == 77);
    seen_code = 0;
    RaiseException(0xE0001234, 0, 1, &arg);
    check("RaiseException continued", seen_code == (LONG)0xE0001234 && seen_addr == 99);
    check("RemoveVectoredExceptionHandler", RemoveVectoredExceptionHandler(h) == 1);
    SetUnhandledExceptionFilter(last_chance);
    RaiseException(0xE0005678, 0, 0, NULL);      /* nobody handles it: the filter, then the end */
    printf("FAIL the program went on after an unhandled exception\n");
    return 1;
}
