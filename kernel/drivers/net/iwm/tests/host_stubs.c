/*
 * Host stubs for the net80211 simulation test (tests/net80211_sim.c):
 * the few ICDA kernel functions that iwm_compat.c and net80211.c use.
 * Compiled without tests/host/rename.h, so it can use the host libc.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *kmalloc(size_t size) { return malloc(size); }
void kfree(void *ptr) { free(ptr); }

uint64_t
pmm_alloc_contiguous_below(uint64_t count, uint64_t max_addr)
{
	(void)max_addr;
	return (uint64_t)(uintptr_t)aligned_alloc(4096, count * 4096);
}

uint64_t
pmm_alloc_contiguous_aligned_below(uint64_t count, uint64_t align,
    uint64_t max_addr)
{
	(void)max_addr;
	return (uint64_t)(uintptr_t)aligned_alloc(align < 4096 ? 4096 : align,
	    count * 4096);
}

void
pmm_free_range(uint64_t addr, uint64_t count)
{
	(void)count;
	free((void *)(uintptr_t)addr);
}

typedef struct { int dummy; } addr_space_t;
static addr_space_t kas;
addr_space_t *vmm_kernel_address_space(void) { return &kas; }
uint64_t vmm_virt_to_phys(addr_space_t *as, uint64_t virt) { (void)as; return virt; }

static uint64_t ticks;
uint64_t sched_ticks(void) { return ticks++; }
void sched_sleep(uint64_t t) { ticks += t; }
void sched_yield(void) { }

int sim_quiet;
void serial_write(const char *s) { if (!sim_quiet) fputs(s, stdout); }
void console_write(const char *s, int style) { (void)s; (void)style; }

uint32_t pci_read_config32(const void *d, uint16_t off) { (void)d; (void)off; return 0; }
void pci_write_config32(const void *d, uint16_t off, uint32_t v) { (void)d; (void)off; (void)v; }

const uint8_t iwm_fw_8000c_start[16];
const uint8_t iwm_fw_8000c_end[1];
