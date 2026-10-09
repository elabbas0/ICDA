#ifndef POWER_BATTERY_H
#define POWER_BATTERY_H

#include <stdint.h>

#define POWER_SAVER       0
#define POWER_BALANCED    1
#define POWER_PERFORMANCE 2

void     power_mgmt_init(void);
void     power_tick(void);                      /* every CPU's timer interrupt */
uint64_t battery_node_read(char *buf, uint64_t cap);
uint64_t power_node_read(char *buf, uint64_t cap);
uint64_t power_node_write(const char *buf, uint64_t len);

#endif
