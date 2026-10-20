/*
 * Multiboot 1 -> Multiboot 2 boot shim for the in-browser demo.
 *
 * Under QEMU compiled to WebAssembly, GRUB spends over a minute loading the
 * kernel from the emulated CD.  QEMU's own -kernel loader copies files in
 * through fw_cfg DMA almost instantly, but it only speaks Multiboot 1.  So
 * QEMU loads this shim with kernel.bin as its module, and the shim does what
 * GRUB did: sets the 800x600x24 Bochs VBE mode, copies the ELF64 segments to
 * their physical addresses, builds the Multiboot 2 tags ICDA reads (command
 * line, memory map, framebuffer, ACPI RSDP) and jumps to the entry point.
 */

#include <stdint.h>

#define MB1_MAGIC         0x1BADB002
#define MB1_FLAGS         0x00000003 /* page-align modules, memory info */
#define MB1_BOOT_MAGIC    0x2BADB002
#define MB2_BOOT_MAGIC    0x36D76289

#define FB_WIDTH   800
#define FB_HEIGHT  600
#define FB_BPP     24

#define MBI2_ADDR  0x60000u /* low memory; ICDA marks 0-1 MB used */

static const char cmdline[] = "icda.live=1";

__attribute__((section(".multiboot"), used))
static const uint32_t mb1_header[3] = {
    MB1_MAGIC, MB1_FLAGS, (uint32_t)-(MB1_MAGIC + MB1_FLAGS),
};

struct mb1_info {
    uint32_t flags, mem_lower, mem_upper, boot_device, cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
};

struct mb1_module {
    uint32_t start, end, string, reserved;
};

struct mb1_mmap {
    uint32_t size;
    uint64_t addr, len;
    uint32_t type;
} __attribute__((packed));

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_puts(const char *s)
{
    while (*s) {
        outb(0x3F8, (uint8_t)*s++);
    }
}

static __attribute__((noreturn)) void fail(const char *why)
{
    serial_puts("[shim] ");
    serial_puts(why);
    serial_puts("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static void copy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
}

static void zero(void *dst, uint32_t n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = 0;
    }
}

static uint32_t pci_read(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t off)
{
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}

/* Linear framebuffer of the Bochs/QEMU std VGA (1234:1111), from BAR0 */
static uint32_t find_bochs_fb(void)
{
    for (uint32_t dev = 0; dev < 32; dev++) {
        if (pci_read(0, dev, 0, 0) == 0x11111234) {
            return pci_read(0, dev, 0, 0x10) & ~0xFu;
        }
    }
    return 0;
}

static void bochs_set_mode(void)
{
    static const uint16_t regs[][2] = {
        { 4, 0 },            /* disable */
        { 1, FB_WIDTH },
        { 2, FB_HEIGHT },
        { 3, FB_BPP },
        { 4, 0x41 },         /* enable, linear framebuffer */
    };
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        outw(0x1CE, regs[i][0]);
        outw(0x1CF, regs[i][1]);
    }
}

static const uint8_t *find_rsdp(void)
{
    for (uint32_t p = 0xE0000; p < 0x100000; p += 16) {
        const uint8_t *s = (const uint8_t *)p;
        if (s[0] == 'R' && s[1] == 'S' && s[2] == 'D' && s[3] == ' ' &&
            s[4] == 'P' && s[5] == 'T' && s[6] == 'R' && s[7] == ' ') {
            uint8_t sum = 0;
            for (int i = 0; i < 20; i++) {
                sum += s[i];
            }
            if (sum == 0) {
                return s;
            }
        }
    }
    return 0;
}

static uint8_t *tag_begin(uint8_t *p, uint32_t type)
{
    ((uint32_t *)p)[0] = type;
    return p;
}

static uint8_t *tag_end(uint8_t *tag, uint32_t size)
{
    ((uint32_t *)tag)[1] = size;
    return tag + ((size + 7) & ~7u);
}

void shim_main(uint32_t magic, struct mb1_info *mbi)
{
    if (magic != MB1_BOOT_MAGIC) {
        fail("not started by a Multiboot 1 loader");
    }
    if (!(mbi->flags & (1 << 3)) || mbi->mods_count < 1) {
        fail("kernel.bin module missing");
    }
    if (!(mbi->flags & (1 << 6))) {
        fail("no memory map");
    }

    const struct mb1_module *mod = (const struct mb1_module *)mbi->mods_addr;
    const uint8_t *elf = (const uint8_t *)mod->start;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)elf;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[4] != 2) {
        fail("module is not an ELF64 file");
    }

    uint32_t fb = find_bochs_fb();
    if (!fb) {
        fail("no std VGA device (-vga std)");
    }
    bochs_set_mode();

    /* Build the Multiboot 2 information before the kernel copy can clobber
     * QEMU's Multiboot 1 structures. */
    uint8_t *out = (uint8_t *)MBI2_ADDR;
    uint8_t *p = out + 8;
    uint8_t *t;

    t = tag_begin(p, 1);
    copy(t + 8, cmdline, sizeof(cmdline));
    p = tag_end(t, 8 + sizeof(cmdline));

    t = tag_begin(p, 6);
    ((uint32_t *)t)[2] = 24; /* entry size */
    ((uint32_t *)t)[3] = 0;  /* entry version */
    uint8_t *e = t + 16;
    for (uint32_t off = 0; off < mbi->mmap_length; ) {
        const struct mb1_mmap *m = (const struct mb1_mmap *)(mbi->mmap_addr + off);
        *(uint64_t *)(e + 0) = m->addr;
        *(uint64_t *)(e + 8) = m->len;
        *(uint32_t *)(e + 16) = m->type;
        *(uint32_t *)(e + 20) = 0;
        e += 24;
        off += m->size + 4;
    }
    p = tag_end(t, (uint32_t)(e - t));

    t = tag_begin(p, 8);
    *(uint64_t *)(t + 8) = fb;
    *(uint32_t *)(t + 16) = FB_WIDTH * (FB_BPP / 8);
    *(uint32_t *)(t + 20) = FB_WIDTH;
    *(uint32_t *)(t + 24) = FB_HEIGHT;
    t[28] = FB_BPP;
    t[29] = 1; /* direct RGB */
    t[30] = t[31] = 0;
    t[32] = 16; t[33] = 8; /* red position, size */
    t[34] = 8;  t[35] = 8; /* green */
    t[36] = 0;  t[37] = 8; /* blue */
    p = tag_end(t, 38);

    const uint8_t *rsdp = find_rsdp();
    if (rsdp) {
        t = tag_begin(p, 14);
        copy(t + 8, rsdp, 20);
        p = tag_end(t, 28);
    }

    t = tag_begin(p, 0);
    p = tag_end(t, 8);
    ((uint32_t *)out)[0] = (uint32_t)(p - out);
    ((uint32_t *)out)[1] = 0;

    const uint8_t *ph = elf + (uint32_t)eh->phoff;
    for (uint32_t i = 0; i < eh->phnum; i++, ph += eh->phentsize) {
        const struct elf64_phdr *seg = (const struct elf64_phdr *)ph;
        if (seg->type != 1) { /* PT_LOAD */
            continue;
        }
        uint8_t *dst = (uint8_t *)(uint32_t)seg->paddr;
        copy(dst, elf + (uint32_t)seg->offset, (uint32_t)seg->filesz);
        zero(dst + (uint32_t)seg->filesz, (uint32_t)(seg->memsz - seg->filesz));
    }

    serial_puts("[shim] starting ICDA\n");
    __asm__ volatile("jmp *%2"
                     :
                     : "a"(MB2_BOOT_MAGIC), "b"(out), "r"((uint32_t)eh->entry)
                     : "memory");
    __builtin_unreachable();
}
