#ifndef BOOTSTAGE_H
#define BOOTSTAGE_H

#include <stdint.h>

void bootstage_set(uint32_t stage, const char *label);
uint32_t bootstage_current(void);
const char *bootstage_label(void);

#endif
