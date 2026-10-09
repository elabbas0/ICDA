#ifndef DEV_NOTIFY_H
#define DEV_NOTIFY_H

#include <stdint.h>

uint64_t notify_node_read(char *buf, uint64_t cap);
uint64_t notify_node_write(const char *buf, uint64_t len);

#endif
