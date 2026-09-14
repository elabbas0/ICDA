#ifndef ISR_H
#define ISR_H

#include <stdint.h>

// must match push order in isr.asm exactly (last pushed = first field)
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t int_no, err_code;
    // pushed automatically by cpu on interrupt:
    uint64_t rip, cs, rflags, rsp, ss;
};

// exception names (defined in isr.c)
extern const char *exception_names[32];

// cpu exception stubs (defined in isr.asm)
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

// hardware irq stubs (defined in isr.asm)
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);
// MSI single-vector stubs for vectors 64-79 (defined in isr.asm)
extern void msi64(void);  extern void msi65(void);  extern void msi66(void);
extern void msi67(void);  extern void msi68(void);  extern void msi69(void);
extern void msi70(void);  extern void msi71(void);  extern void msi72(void);
extern void msi73(void);  extern void msi74(void);  extern void msi75(void);
extern void msi76(void);  extern void msi77(void);  extern void msi78(void);
extern void msi79(void);
extern void syscall128(void);

#define MSI_VEC_BASE 64
#define MSI_VEC_COUNT 16
#define MSI_VEC_LAST (MSI_VEC_BASE + MSI_VEC_COUNT - 1)

// c handlers called from assembly stubs
void isr_handler(struct registers* regs);
void irq_handler(struct registers* regs);
void syscall_handler(struct registers* regs);

// register a custom irq handler
typedef void (*irq_handler_t)(struct registers*);
void irq_register(int irq, irq_handler_t handler);

// register a single-vector MSI handler (vectors 64-79); returns 0/-1
int irq_register_msi(int vector, irq_handler_t handler);

// register a custom exception handler (0-31)
// if a handler is registered it is called instead of the default panic
typedef void (*isr_handler_t)(struct registers*);
void isr_register(int vec, isr_handler_t handler);

#endif
