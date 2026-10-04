#include "efi.h"
#include "../cpu/multiboot2.h"
#include "../memory/vmm.h"
#include "../memory/heap.h"
#include "../drivers/serial/serial.h"

#define MB_TAG_EFI64      12
#define MB_TAG_EFI_MMAP   17
#define EFI_RT_MEMORY     (1ULL << 63)
#define EFI_VAR_NV_BS_RT  0x7U
#define EFI_MMAP_MAX      8192
#define EFI_SYSTAB_SIG    0x5453595320494249ULL

typedef uint64_t (__attribute__((ms_abi)) *efi_get_variable_fn)(const uint16_t *name, const uint8_t *guid,
                                                               uint32_t *attrs, uint64_t *size, void *data);
typedef uint64_t (__attribute__((ms_abi)) *efi_set_variable_fn)(const uint16_t *name, const uint8_t *guid,
                                                               uint32_t attrs, uint64_t size, const void *data);

static const uint8_t efi_global_guid[16] = {
    0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11, 0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C
};

static uint64_t efi_systab_phys;
static uint8_t  efi_mmap[EFI_MMAP_MAX];
static uint32_t efi_mmap_size;
static uint32_t efi_desc_size;
static efi_get_variable_fn efi_get_var;
static efi_set_variable_fn efi_set_var;

static uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void efi_init(void *multiboot_info) {
    struct multiboot_info *info = (struct multiboot_info *)multiboot_info;
    uint8_t *p, *end;
    if (!info) return;
    p = (uint8_t *)info + 8;
    end = (uint8_t *)info + info->total_size;
    while (p < end) {
        struct multiboot_tag *tag = (struct multiboot_tag *)p;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;
        if (tag->type == MB_TAG_EFI64 && tag->size >= 16) {
            efi_systab_phys = rd64(p + 8);
        } else if (tag->type == MB_TAG_EFI_MMAP && tag->size > 16) {
            uint32_t len = tag->size - 16;
            efi_desc_size = rd32(p + 8);
            if (len > EFI_MMAP_MAX) len = EFI_MMAP_MAX - (EFI_MMAP_MAX % (efi_desc_size ? efi_desc_size : 1));
            for (uint32_t i = 0; i < len; i++) efi_mmap[i] = p[16 + i];
            efi_mmap_size = len;
        }
        p += (tag->size + 7U) & ~7U;
    }
}

static int efi_resolve(void) {
    const uint8_t *st, *rt;
    if (efi_get_var && efi_set_var) return 0;
    if (!efi_systab_phys || !efi_mmap_size || efi_desc_size < 40) return -1;
    st = (const uint8_t *)PHYS_TO_VIRT(efi_systab_phys);
    if (rd64(st) != EFI_SYSTAB_SIG) return -1;
    rt = (const uint8_t *)PHYS_TO_VIRT(rd64(st + 88));
    efi_get_var = (efi_get_variable_fn)(uintptr_t)rd64(rt + 72);
    efi_set_var = (efi_set_variable_fn)(uintptr_t)rd64(rt + 88);
    return efi_get_var && efi_set_var ? 0 : -1;
}

int efi_available(void) {
    return efi_resolve() == 0;
}

#define EFI_TRACK_MAX 65536

static uint64_t *efi_mapped;
static uint32_t  efi_mapped_count;

static void identity_runtime(int map) {
    addr_space_t *as = vmm_kernel_address_space();
    if (!map) {
        for (uint32_t i = 0; i < efi_mapped_count; i++) vmm_unmap_page(as, efi_mapped[i], 0);
        efi_mapped_count = 0;
        return;
    }
    if (!efi_mapped) efi_mapped = (uint64_t *)kmalloc(sizeof(uint64_t) * EFI_TRACK_MAX);
    if (!efi_mapped) return;
    for (uint32_t off = 0; off + efi_desc_size <= efi_mmap_size; off += efi_desc_size) {
        const uint8_t *d = efi_mmap + off;
        uint32_t type = rd32(d);
        uint64_t phys = rd64(d + 8);
        uint64_t pages = rd64(d + 24);
        uint64_t attr = rd64(d + 32);
        uint64_t flags = VMM_PRESENT | VMM_WRITE;
        if (!(attr & EFI_RT_MEMORY) || pages == 0 || phys >= 0x0000800000000000ULL) continue;
        if (type == 11 || type == 12) flags |= (1ULL << 4);
        for (uint64_t pg = 0; pg < pages && efi_mapped_count < EFI_TRACK_MAX; pg++) {
            uint64_t va = phys + pg * 4096ULL;
            if (vmm_virt_to_phys(as, va)) continue;
            if (vmm_map_page(as, va, va, flags) == 0) efi_mapped[efi_mapped_count++] = va;
        }
    }
}

typedef struct {
    int      set;
    const uint16_t *name;
    uint32_t attrs;
    uint64_t *size;
    void    *data;
    uint64_t status;
} efi_call_t;

static void efi_call(efi_call_t *c) {
    uint64_t flags, cr3;
    addr_space_t *kas = vmm_kernel_address_space();
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    vmm_switch_address_space(kas);
    identity_runtime(1);
    if (c->set) {
        c->status = efi_set_var(c->name, efi_global_guid, c->attrs, *c->size, c->data);
    } else {
        uint32_t attrs = 0;
        c->status = efi_get_var(c->name, efi_global_guid, &attrs, c->size, c->data);
    }
    identity_runtime(0);
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

static void serial_hex(const char *label, uint64_t v) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 0; i < 16; i++) buf[2 + i] = hex[(v >> (60 - 4 * i)) & 0xF];
    buf[18] = 0;
    serial_write(label);
    serial_write(buf);
    serial_write("\n");
}

static void ascii_to_ucs2(const char *s, uint16_t *out, uint32_t cap) {
    uint32_t i = 0;
    for (; s[i] && i + 1 < cap; i++) out[i] = (uint16_t)(uint8_t)s[i];
    out[i] = 0;
}

static void boot_var_name(uint32_t num, uint16_t out[9]) {
    static const char hex[] = "0123456789ABCDEF";
    ascii_to_ucs2("Boot", out, 9);
    for (int i = 0; i < 4; i++) out[4 + i] = (uint16_t)hex[(num >> (12 - 4 * i)) & 0xF];
    out[8] = 0;
}

static uint64_t get_var(const uint16_t *name, void *buf, uint64_t cap, uint64_t *size_out) {
    efi_call_t c;
    uint64_t size = cap;
    c.set = 0;
    c.name = name;
    c.size = &size;
    c.data = buf;
    efi_call(&c);
    *size_out = size;
    return c.status;
}

static uint64_t set_var(const uint16_t *name, const void *data, uint64_t size) {
    efi_call_t c;
    c.set = 1;
    c.name = name;
    c.attrs = EFI_VAR_NV_BS_RT;
    c.size = &size;
    c.data = (void *)data;
    efi_call(&c);
    return c.status;
}

static int load_option_matches(const uint8_t *opt, uint64_t size, const uint16_t *label) {
    uint64_t i = 0;
    if (size < 8) return 0;
    for (; label[i]; i++) {
        if (6 + i * 2 + 1 >= size) return 0;
        if ((uint16_t)(opt[6 + i * 2] | (opt[7 + i * 2] << 8)) != label[i]) return 0;
    }
    return 6 + i * 2 + 1 < size && opt[6 + i * 2] == 0 && opt[7 + i * 2] == 0;
}

int efi_register_boot_entry(const partition_info_t *esp, const char *loader_path, const char *label) {
    uint16_t name[16], ulabel[64], upath[128];
    uint16_t order[128], new_order[129];
    uint8_t *opt;
    uint64_t order_size = 0, size = 0;
    uint32_t llen = 0, plen = 0, num = 0xFFFF, count, n = 0;
    uint16_t path_bytes;
    uint8_t *dp;
    uint64_t st;
    if (!esp || esp->kind != PARTITION_KIND_GPT || efi_resolve() != 0) return -1;
    ascii_to_ucs2(label, ulabel, 64);
    while (ulabel[llen]) llen++;
    for (uint32_t i = 0; loader_path[i] && i + 1 < 128; i++) {
        upath[i] = (uint16_t)(loader_path[i] == '/' ? '\\' : (uint8_t)loader_path[i]);
        plen = i + 1;
    }
    upath[plen] = 0;
    ascii_to_ucs2("BootOrder", name, 16);
    st = get_var(name, order, sizeof(order), &order_size);
    if (st != 0) order_size = 0;
    count = (uint32_t)(order_size / 2);

    opt = (uint8_t *)kmalloc(1024);
    if (!opt) return -1;
    for (uint32_t i = 0; i < count && num == 0xFFFF; i++) {
        boot_var_name(order[i], name);
        if (get_var(name, opt, 1024, &size) == 0 && load_option_matches(opt, size, ulabel)) num = order[i];
    }
    if (num == 0xFFFF) {
        for (uint32_t cand = 0; cand < 0xFFFF && num == 0xFFFF; cand++) {
            int used = 0;
            for (uint32_t i = 0; i < count; i++) used |= order[i] == cand;
            boot_var_name(cand, name);
            if (!used && get_var(name, opt, 1024, &size) == EFI_STATUS_NOT_FOUND) num = cand;
        }
    }
    if (num == 0xFFFF) {
        kfree(opt);
        return -2;
    }

    path_bytes = (uint16_t)(42 + 4 + (plen + 1) * 2 + 4);
    for (int i = 0; i < 1024; i++) opt[i] = 0;
    opt[0] = 1;
    opt[4] = (uint8_t)path_bytes;
    opt[5] = (uint8_t)(path_bytes >> 8);
    for (uint32_t i = 0; i <= llen; i++) {
        opt[6 + i * 2] = (uint8_t)ulabel[i];
        opt[7 + i * 2] = (uint8_t)(ulabel[i] >> 8);
    }
    dp = opt + 6 + (llen + 1) * 2;
    dp[0] = 4;
    dp[1] = 1;
    dp[2] = 42;
    {
        uint32_t partno = esp->gpt_entry_index + 1;
        for (int i = 0; i < 4; i++) dp[4 + i] = (uint8_t)(partno >> (8 * i));
        for (int i = 0; i < 8; i++) dp[8 + i] = (uint8_t)(esp->start_lba >> (8 * i));
        for (int i = 0; i < 8; i++) dp[16 + i] = (uint8_t)(esp->sector_count >> (8 * i));
        for (int i = 0; i < 16; i++) dp[24 + i] = esp->gpt_unique_guid[i];
        dp[40] = 2;
        dp[41] = 2;
    }
    dp += 42;
    dp[0] = 4;
    dp[1] = 4;
    dp[2] = (uint8_t)(4 + (plen + 1) * 2);
    dp[3] = (uint8_t)((4 + (plen + 1) * 2) >> 8);
    for (uint32_t i = 0; i <= plen; i++) {
        dp[4 + i * 2] = (uint8_t)upath[i];
        dp[5 + i * 2] = (uint8_t)(upath[i] >> 8);
    }
    dp += 4 + (plen + 1) * 2;
    dp[0] = 0x7F;
    dp[1] = 0xFF;
    dp[2] = 4;
    dp[3] = 0;
    size = (uint64_t)(dp + 4 - opt);

    boot_var_name(num, name);
    st = set_var(name, opt, size);
    kfree(opt);
    if (st != 0) {
        serial_write("efi: SetVariable Boot#### failed\n");
        return -3;
    }
    new_order[n++] = (uint16_t)num;
    for (uint32_t i = 0; i < count && n < 129; i++) {
        if (order[i] != num) new_order[n++] = order[i];
    }
    ascii_to_ucs2("BootOrder", name, 16);
    serial_hex("efi: old BootOrder bytes ", order_size);
    st = set_var(name, new_order, (uint64_t)n * 2);
    if (st != 0) {
        serial_hex("efi: SetVariable BootOrder failed ", st);
        return -4;
    }
    serial_write("efi: registered boot entry\n");
    return 0;
}
