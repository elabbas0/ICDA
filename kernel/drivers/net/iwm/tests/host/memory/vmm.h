/* Host stub: identity "HHDM". */
#include <stdint.h>
#define PHYSICAL_BASE 0ULL
#define KERNEL_VMA 0xFFFFFFFFFFFFFFFFULL
#define PHYS_TO_VIRT(p) ((void *)(uintptr_t)(p))
#define VIRT_TO_PHYS(v) ((uint64_t)(uintptr_t)(v))
typedef struct { int dummy; } addr_space_t;
addr_space_t *vmm_kernel_address_space(void);
uint64_t vmm_virt_to_phys(addr_space_t *as, uint64_t virt);
