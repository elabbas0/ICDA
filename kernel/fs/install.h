#ifndef INSTALL_H
#define INSTALL_H

#include <stdint.h>

int system_install_run(uint64_t *files_installed, uint64_t *bytes_installed);
int system_install_device(uint32_t device_index, uint64_t *files_installed, uint64_t *bytes_installed);
int system_install_present(void);

#endif
