/* The few C++ runtime pieces OpenH264 needs, on top of malloc. */
#include <stdlib.h>

int errno;

void *_Znwm(unsigned long n) { return malloc(n ? n : 1); }          /* operator new */
void *_Znam(unsigned long n) { return malloc(n ? n : 1); }          /* operator new[] */
void  _ZdlPv(void *p) { free(p); }                                  /* operator delete */
void  _ZdaPv(void *p) { free(p); }                                  /* operator delete[] */
void  _ZdlPvm(void *p, unsigned long n) { (void)n; free(p); }       /* sized delete */
void  _ZdaPvm(void *p, unsigned long n) { (void)n; free(p); }
void  __cxa_pure_virtual(void) { abort(); }
