#ifndef PERSISTFS_H
#define PERSISTFS_H

#include <stdint.h>

int persistfs_init(void);
int persistfs_sync(void);
int persistfs_present(void);
uint64_t persistfs_loaded_entries(void);

#endif
