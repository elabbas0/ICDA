#include "pf.h"
#include "pmm.h"
#include "vmm.h"
#include "../cpu/isr.h"
#include "../drivers/console/console.h"
#include "../drivers/display/framebuffer.h"

// scheduler hook 
// the page fault handler needs the address space of the currently running process so it can map the new page into the right PML4.
// before the scheduler exists this is NULL, meaning "use the kernel AS".
static addr_space_t *current_as = NULL;

void pf_set_current_as(addr_space_t *as) {
    current_as = as;
}

static inline addr_space_t *active_as(void) {
    return current_as ? current_as : vmm_kernel_address_space();
}

// helpers 

// read CR2 — the cpu stores the faulting virtual address there
static inline uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static void print_hex(uint64_t v) {
    char buf[19];
    buf[0] = '0'; buf[1] = 'x'; buf[18] = '\0';
    for (int i = 17; i >= 2; i--) {
        int n = v & 0xF;
        buf[i] = (n < 10) ? ('0' + n) : ('a' + n - 10);
        v >>= 4;
    }
    fb_print(buf, FB_WHITE, FB_RED);
}

static void console_print_hex(uint64_t v) {
    console_write_hex64(v, CONSOLE_STYLE_ERROR);
}

//  panic helper 

static void pf_panic(struct registers *regs, uint64_t cr2) {
    console_write("\n*** PAGE FAULT ***\n", CONSOLE_STYLE_ERROR);
    console_write("  CR2 (fault addr): ", CONSOLE_STYLE_ERROR);
    console_print_hex(cr2);
    console_write("\n", CONSOLE_STYLE_ERROR);
    console_write("  RIP: ", CONSOLE_STYLE_ERROR);
    console_print_hex(regs->rip);
    console_write("\n", CONSOLE_STYLE_ERROR);
    console_write("  RSP: ", CONSOLE_STYLE_ERROR);
    console_print_hex(regs->rsp);
    console_write("\n", CONSOLE_STYLE_ERROR);
    console_write("  ERR: ", CONSOLE_STYLE_ERROR);
    console_print_hex(regs->err_code);
    console_write("\n", CONSOLE_STYLE_ERROR);

    uint64_t e = regs->err_code;
    console_write("  flags: ", CONSOLE_STYLE_ERROR);
    console_write((e & PF_PRESENT) ? "[PROT] " : "[NOT-PRESENT] ", CONSOLE_STYLE_ERROR);
    console_write((e & PF_WRITE) ? "[WRITE] " : "[READ] ", CONSOLE_STYLE_ERROR);
    console_write((e & PF_USER) ? "[USER] " : "[KERNEL] ", CONSOLE_STYLE_ERROR);
    if (e & PF_RESERVED) {
        console_write("[RSVD] ", CONSOLE_STYLE_ERROR);
    }
    if (e & PF_IFETCH) {
        console_write("[IFETCH] ", CONSOLE_STYLE_ERROR);
    }
    console_write("\n", CONSOLE_STYLE_ERROR);

    fb_print("\n*** PAGE FAULT ***\n", FB_WHITE, FB_RED);

    fb_print("  CR2 (fault addr): ", FB_WHITE, FB_RED);
    print_hex(cr2);
    fb_print("\n", FB_WHITE, FB_RED);

    fb_print("  RIP: ", FB_WHITE, FB_RED); print_hex(regs->rip); fb_print("\n", FB_WHITE, FB_RED);
    fb_print("  RSP: ", FB_WHITE, FB_RED); print_hex(regs->rsp); fb_print("\n", FB_WHITE, FB_RED);
    fb_print("  ERR: ", FB_WHITE, FB_RED); print_hex(regs->err_code); fb_print("\n", FB_WHITE, FB_RED);

    e = regs->err_code;
    fb_print("  flags: ", FB_WHITE, FB_RED);
    if (e & PF_PRESENT)  fb_print("[PROT] ",   FB_WHITE, FB_RED);
    else                 fb_print("[NOT-PRESENT] ", FB_WHITE, FB_RED);
    if (e & PF_WRITE)    fb_print("[WRITE] ",  FB_WHITE, FB_RED);
    else                 fb_print("[READ] ",   FB_WHITE, FB_RED);
    if (e & PF_USER)     fb_print("[USER] ",   FB_WHITE, FB_RED);
    else                 fb_print("[KERNEL] ", FB_WHITE, FB_RED);
    if (e & PF_RESERVED) fb_print("[RSVD] ",   FB_WHITE, FB_RED);
    if (e & PF_IFETCH)   fb_print("[IFETCH] ", FB_WHITE, FB_RED);
    fb_print("\n", FB_WHITE, FB_RED);

    __asm__ volatile("cli; hlt");
}

// Conditions that must ALL be true:
//   1. Not-present fault (P=0) — the page hasn't been mapped yet, not a
//      protection violation.
//   2. Write access (W=1) — pushing onto the stack is always a write.
//   3. The faulting address is inside the user stack region
//      [USER_STACK_LIMIT, USER_STACK_TOP).
//   4. The faulting page is at most one page below the current RSP page —
//      a correct program should only extend the stack one page at a time
//      (the ABI guarantees this for the standard call sequence).
//      We allow a small slack (4 pages) to handle red zones and alloca.
// NOTE: once we have a PCB we can also check against per-process stack limits.

#define STACK_SLACK_PAGES 4ULL

static int is_stack_growth(struct registers *regs, uint64_t cr2) {
    uint64_t e = regs->err_code;

    if (e & PF_PRESENT)  return 0;  // protection fault, not missing page
    if (!(e & PF_WRITE)) return 0;  // read fault can't be stack growth

    // must be in the user stack region
    if (cr2 < USER_STACK_LIMIT || cr2 >= USER_STACK_TOP) return 0;

    // must be within SLACK pages below rsp's current page
    uint64_t rsp_page   = regs->rsp & ~0xFFFULL;
    uint64_t fault_page = cr2       & ~0xFFFULL;
    uint64_t slack      = STACK_SLACK_PAGES * PAGE_SIZE_4K;

    if (fault_page > rsp_page) return 0;               // above rsp — weird
    if (rsp_page - fault_page > slack) return 0;        // too far below

    return 1;
}

//  main page fault handler 

static void page_fault_handler(struct registers *regs) {
    uint64_t cr2 = read_cr2();

    if (is_stack_growth(regs, cr2)) {
        // allocate a fresh physical frame
        uint64_t phys = pmm_alloc();
        if (!phys) {
            fb_print("\n*** PAGE FAULT: OOM during stack growth ***\n", FB_WHITE, FB_RED);
            __asm__ volatile("cli; hlt");
            return;
        }

        // map the page into the current address space as user read-write
        uint64_t page_va = cr2 & ~0xFFFULL;
        int ok = vmm_map_page(active_as(), page_va, phys, VMM_FLAGS_USER_RW);
        if (ok != 0) {
            pmm_free(phys);
            fb_print("\n*** PAGE FAULT: vmm_map_page failed ***\n", FB_WHITE, FB_RED);
            __asm__ volatile("cli; hlt");
            return;
        }

        // zero the new page so the process sees clean memory
        uint64_t *vaddr = (uint64_t *)page_va;
        for (int i = 0; i < (int)(PAGE_SIZE_4K / sizeof(uint64_t)); i++)
            vaddr[i] = 0;

        // return from the handler — the cpu will re-execute the faulting instruction
        return;
    }

    // not a recoverable fault — panic with full details
    pf_panic(regs, cr2);
}

// init 

void pf_init(void) {
    isr_register(14, page_fault_handler);
}
