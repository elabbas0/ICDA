#ifndef ICDA_COMPAT_SETJMP_H
#define ICDA_COMPAT_SETJMP_H
/* setjmp / longjmp for libvpx's error recovery (media/setjmp.asm) */
typedef unsigned long jmp_buf[8];       /* rbx rbp r12 r13 r14 r15 rsp rip */
int  icda_setjmp(jmp_buf env) __attribute__((returns_twice));
void icda_longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define setjmp(env) icda_setjmp(env)
#define longjmp(env, v) icda_longjmp(env, v)
#endif
