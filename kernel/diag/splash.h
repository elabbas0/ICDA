#ifndef SPLASH_H
#define SPLASH_H

#include <stdint.h>












void splash_init(void);
void splash_progress(uint32_t stage, const char *label);
void splash_finish(void);
int  splash_active(void);

#endif
