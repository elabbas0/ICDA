#include "user.h"
#include "elf.h"

#include "sched.h"
#include "../cpu/gdt.h"
#include "../drivers/console/console.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../memory/pf.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

extern uint8_t user_demo_start[];
extern uint8_t user_demo_end[];

static uint64_t user_exit_code = 0;

#define USER_ARG_MAX         32
#define USER_ARG_BYTES_MAX   2048
#define USER_SHEBANG_MAX     256

#define ICX_MAGIC    0x31584349U
#define ICX_VERSION  1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint64_t entry_offset;
    uint64_t reserved;
} __attribute__((packed)) icx_header_t;

static void copy_bytes(char *dst, const char *src, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }
}

static void zero_bytes(char *dst, uint64_t size) {
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = 0;
    }
}

static uint64_t cstr_len(const char *s) {
    uint64_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void copy_cstr(char *dst, const char *src, uint64_t cap) {
    uint64_t i = 0;
    if (!dst || cap == 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int cstr_eq(const char *a, const char *b) {
    uint64_t i = 0;
    while (a && b && a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a && b && a[i] == 0 && b[i] == 0;
}

static int cstr_has_slash(const char *s) {
    for (uint64_t i = 0; s && s[i]; i++) {
        if (s[i] == '/') return 1;
    }
    return 0;
}

static int basename_eq(const char *path, const char *name) {
    uint64_t i = cstr_len(path);
    while (i > 0 && path[i - 1] != '/') i--;
    return cstr_eq(path + i, name);
}

static int split_words(const char *text, char *storage, uint64_t storage_cap,
                       char **argv_out, uint64_t argv_cap, uint64_t *argc_out) {
    uint64_t argc = 0;
    uint64_t out = 0;
    uint64_t i = 0;

    if (argc_out) *argc_out = 0;
    if (!text || !*text) return 0;

    while (text[i]) {
        char quote = 0;
        uint64_t start;

        while (text[i] == ' ' || text[i] == '\t') i++;
        if (!text[i]) break;
        if (argc >= argv_cap) return -1;
        if (out >= storage_cap) return -1;

        argv_out[argc++] = storage + out;
        start = out;

        if (text[i] == '"' || text[i] == '\'') {
            quote = text[i++];
        }

        while (text[i]) {
            if (quote) {
                if (text[i] == quote) {
                    i++;
                    break;
                }
            } else if (text[i] == ' ' || text[i] == '\t') {
                break;
            }

            if (text[i] == '\\' && text[i + 1]) {
                i++;
            }
            if (out + 1 >= storage_cap) return -1;
            storage[out++] = text[i++];
        }

        if (out + 1 >= storage_cap) return -1;
        storage[out++] = 0;

        while (text[i] == ' ' || text[i] == '\t') i++;
        if (!quote && out == start + 1) {
            argc--;
            out = start;
        }
    }

    if (argc_out) *argc_out = argc;
    return 0;
}

static int write_stack_u64(char *stack_page, uint64_t base, uint64_t *sp_io, uint64_t value) {
    if (!stack_page || !sp_io || *sp_io < base + sizeof(uint64_t)) return -1;
    *sp_io -= sizeof(uint64_t);
    *(uint64_t *)(stack_page + (*sp_io - base)) = value;
    return 0;
}

static int user_build_initial_stack(process_t *proc, uint64_t *user_rsp_out,
                                    const char *argv0, uint64_t extra_argc, char *const extra_argv[]) {
    uint64_t total_argc;
    uint64_t stack_base = USER_STACK_TOP - PAGE_SIZE_4K;
    uint64_t sp = USER_STACK_TOP;
    uint64_t phys;
    char *stack_page;
    uint64_t arg_ptrs[USER_ARG_MAX + 1];

    if (!proc || !proc->addr_space || !user_rsp_out || !argv0) return -1;
    total_argc = 1 + extra_argc;
    if (total_argc > USER_ARG_MAX) return -1;

    phys = vmm_virt_to_phys(proc->addr_space, stack_base);
    if (!phys) return -1;
    stack_page = (char *)PHYS_TO_VIRT(phys);

    for (uint64_t i = total_argc; i > 0; i--) {
        const char *src = (i == 1) ? argv0 : extra_argv[i - 2];
        uint64_t len = cstr_len(src) + 1;
        if (sp < stack_base + len) return -1;
        sp -= len;
        copy_bytes(stack_page + (sp - stack_base), src, len);
        arg_ptrs[i - 1] = sp;
    }

    sp &= ~0xFULL;

    if (proc->linux_personality) {
        if (write_stack_u64(stack_page, stack_base, &sp, 0) != 0) return -1; /* AT_NULL value */
        if (write_stack_u64(stack_page, stack_base, &sp, 0) != 0) return -1; /* AT_NULL key */
        if (write_stack_u64(stack_page, stack_base, &sp, PAGE_SIZE_4K) != 0) return -1; /* AT_PAGESZ value */
        if (write_stack_u64(stack_page, stack_base, &sp, 6) != 0) return -1; /* AT_PAGESZ key */
    } else {
        if (write_stack_u64(stack_page, stack_base, &sp, 0) != 0) return -1;
        if (write_stack_u64(stack_page, stack_base, &sp, 0) != 0) return -1;
    }
    if (write_stack_u64(stack_page, stack_base, &sp, 0) != 0) return -1; /* envp NULL */
    if (write_stack_u64(stack_page, stack_base, &sp, 0) != 0) return -1; /* argv NULL */
    for (uint64_t i = total_argc; i > 0; i--) {
        if (write_stack_u64(stack_page, stack_base, &sp, arg_ptrs[i - 1]) != 0) return -1;
    }
    if (write_stack_u64(stack_page, stack_base, &sp, total_argc) != 0) return -1;

    *user_rsp_out = sp;
    return 0;
}

int user_prepare_address_space(process_t *proc) {
    uint64_t stack_page;

    if (!proc) {
        return -1;
    }

    if (!proc->addr_space || proc->addr_space == vmm_kernel_address_space()) {
        proc->addr_space = vmm_create_address_space();
        if (!proc->addr_space) {
            return -1;
        }
    }

    /* Map USER_STACK_INITIAL_PAGES of stack up front.  Apps carry multi-KB
     * stack buffers (a 4KiB directory listing is common), so a single
     * mapped page lets the kernel's buffer copy fault on an unmapped page
     * mid-syscall.  Extra pages are mapped on demand by the page fault
     * handler (see is_stack_growth in pf.c). */
    for (int i = 0; i < USER_STACK_INITIAL_PAGES; i++) {
        stack_page = pmm_alloc();
        if (!stack_page) {
            return -1;
        }

        {
            uint64_t *stack = (uint64_t *)PHYS_TO_VIRT(stack_page);
            for (int j = 0; j < (int)(PAGE_SIZE_4K / sizeof(uint64_t)); j++) {
                stack[j] = 0;
            }
        }

        if (vmm_map_page(proc->addr_space,
                         USER_STACK_TOP - (uint64_t)PAGE_SIZE_4K * (i + 1),
                         stack_page, VMM_FLAGS_USER_RW) != 0) {
            pmm_free(stack_page);
            return -1;
        }
    }

    return 0;
}

int user_map_blob(process_t *proc, const void *blob, uint64_t size, uint64_t virt_base) {
    uint64_t pages;
    uint64_t copied = 0;

    if (!proc || !proc->addr_space || !blob || size == 0) {
        return -1;
    }

    pages = (size + PAGE_SIZE_4K - 1) / PAGE_SIZE_4K;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc();
        uint64_t chunk = size - copied;
        char *dst;

        if (!phys) {
            return -1;
        }
        if (chunk > PAGE_SIZE_4K) {
            chunk = PAGE_SIZE_4K;
        }
        // ICX blobs need writable pages for .data/.bss sections
        if (vmm_map_page(proc->addr_space, virt_base + i * PAGE_SIZE_4K, phys, VMM_FLAGS_USER_RW) != 0) {
            pmm_free(phys);
            return -1;
        }

        dst = (char *)PHYS_TO_VIRT(phys);
        zero_bytes(dst, PAGE_SIZE_4K);
        copy_bytes(dst, (const char *)blob + copied, chunk);
        copied += chunk;
    }
    return 0;
}

static int user_map_segment(process_t *proc, const char *image, uint64_t image_size,
                            uint64_t virt, uint64_t file_off, uint64_t file_size,
                            uint64_t mem_size, uint64_t seg_flags) {
    uint64_t page_base;
    uint64_t seg_end;
    uint64_t map_flags;

    if (!proc || !proc->addr_space || !image) {
        return -1;
    }
    if (file_size > mem_size) {
        return -1;
    }
    if (file_off > image_size || file_size > image_size - file_off) {
        return -1;
    }
    if (mem_size == 0) {
        return 0;
    }

    page_base = virt & ~(PAGE_SIZE_4K - 1);
    seg_end = (virt + mem_size + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1);
    map_flags = (seg_flags & PF_W) ? VMM_FLAGS_USER_RW : VMM_FLAGS_USER_RO;

    for (uint64_t page = page_base; page < seg_end; page += PAGE_SIZE_4K) {
        uint64_t phys = pmm_alloc();
        char *dst;
        uint64_t page_data_start;
        uint64_t page_data_end;

        if (!phys) {
            return -1;
        }
        if (vmm_map_page(proc->addr_space, page, phys, map_flags) != 0) {
            pmm_free(phys);
            return -1;
        }

        dst = (char *)PHYS_TO_VIRT(phys);
        zero_bytes(dst, PAGE_SIZE_4K);

        page_data_start = page > virt ? page : virt;
        page_data_end = (page + PAGE_SIZE_4K) < (virt + file_size) ? (page + PAGE_SIZE_4K) : (virt + file_size);

        if (page_data_end > page_data_start) {
            uint64_t copy_size = page_data_end - page_data_start;
            uint64_t dst_off = page_data_start - page;
            uint64_t src_off = file_off + (page_data_start - virt);
            copy_bytes(dst + dst_off, image + src_off, copy_size);
        }
    }

    return 0;
}

void user_request_exit_to_kernel(uint64_t code) {
    thread_t *thread = sched_current_thread();
    process_t *proc = sched_current_process();
    if (proc) {
        fd_proc_exit(proc);
        proc->state = PROCESS_EXITED;
        proc->exit_code = code;
    }
    if (thread) {
        thread->state = THREAD_ZOMBIE;
        thread->user_return_pending = 1;
        thread->block_reason = THREAD_BLOCK_NONE;
        thread->wake_tick = 0;
    }
    if (proc && proc->parent && proc->parent->main_thread &&
        proc->parent->main_thread->state == THREAD_BLOCKED) {
        proc->parent->main_thread->state = THREAD_READY;
        proc->parent->main_thread->block_reason = THREAD_BLOCK_NONE;
        proc->parent->main_thread->wake_tick = 0;
        if (proc->parent->state == PROCESS_BLOCKED) {
            proc->parent->state = PROCESS_READY;
        }
    }
    user_exit_code = code;
}

uint64_t user_last_exit_code(void) {
    return user_exit_code;
}

static int user_load_elf(process_t *user_proc, const void *image, uint64_t image_size,
                          uint64_t *entry_rip_out, uint64_t *interp_vaddr,
                          char *interp_path, uint64_t interp_cap) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    const elf64_phdr_t *ph;
    uint64_t load_bias = 0;
    int has_load = 0;

    if (image_size < sizeof(elf64_ehdr_t)) return -1;
    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) return -1;
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) return -1;
    if (eh->e_ident[6] != EV_CURRENT ||
        (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ||
        eh->e_machine != EM_X86_64) return -1;
    if (eh->e_phentsize != sizeof(elf64_phdr_t) || eh->e_phnum == 0) return -1;
    if (eh->e_phoff > image_size) return -1;
    if ((uint64_t)eh->e_phnum > (image_size - eh->e_phoff) / sizeof(elf64_phdr_t)) return -1;

    if (interp_path) interp_path[0] = 0;
    if (interp_vaddr) *interp_vaddr = 0;

    ph = (const elf64_phdr_t *)((const char *)image + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_INTERP && interp_path && interp_cap > 0) {
            if (ph[i].p_offset + ph[i].p_filesz <= image_size) {
                uint64_t copy = ph[i].p_filesz < interp_cap - 1 ? ph[i].p_filesz : interp_cap - 1;
                copy_bytes(interp_path, (const char *)image + ph[i].p_offset, copy);
                interp_path[copy] = 0;
            }
        }
    }

    if (eh->e_type == ET_DYN) {
        uint64_t min_vaddr = 0xFFFFFFFFFFFFFFFFULL;
        uint64_t max_vaddr = 0;
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD) continue;
            if (ph[i].p_vaddr < min_vaddr) min_vaddr = ph[i].p_vaddr;
            if (ph[i].p_vaddr + ph[i].p_memsz > max_vaddr) max_vaddr = ph[i].p_vaddr + ph[i].p_memsz;
        }
        /* For ET_DYN, always apply the load bias so the binary maps at
         * USER_TEXT_BASE regardless of its link-time p_vaddr. */
        load_bias = USER_TEXT_BASE - min_vaddr;
        if (max_vaddr > 0 && (load_bias + max_vaddr) >= USER_STACK_LIMIT) {
            return -1;
        }
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        uint64_t vaddr = ph[i].p_vaddr + load_bias;
        if (vaddr + ph[i].p_memsz < vaddr) return -1;
        if (eh->e_type == ET_EXEC) {
            if (vaddr >= USER_STACK_LIMIT) return -1;
        }
        if (user_map_segment(user_proc, (const char *)image, image_size,
                             vaddr, ph[i].p_offset, ph[i].p_filesz, ph[i].p_memsz, ph[i].p_flags) != 0) {
            return -1;
        }
        has_load = 1;
    }

    if (!has_load) return -1;

    /* Apply relocations for ET_DYN (PIE) binaries. The user-space .app files
     * link as ET_EXEC and need none of this, but PIE ELFs do: -mcmodel=large
     * emits absolute data pointers that only resolve once the load bias is
     * added. Handle both DT_REL (implicit addend) and DT_RELA (explicit). */
    if (eh->e_type == ET_DYN) {
        uint64_t rel_off = 0;
        uint64_t rel_sz = 0;
        uint64_t rel_ent = 0;
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_DYNAMIC) continue;
            const elf64_dyn_t *dyn = (const elf64_dyn_t *)((const char *)image + ph[i].p_offset);
            uint64_t dyn_size = ph[i].p_filesz;
            for (uint64_t j = 0; j < dyn_size / sizeof(elf64_dyn_t); j++) {
                if (dyn[j].d_tag == DT_REL) rel_off = dyn[j].d_val;
                else if (dyn[j].d_tag == DT_RELSZ) rel_sz = dyn[j].d_val;
                else if (dyn[j].d_tag == DT_RELENT) rel_ent = dyn[j].d_val;
            }
            break;
        }

        /* Helper: kernel virtual pointer for a relocated user virtual address. */
        if (rel_off && rel_sz && rel_ent) {
            if (rel_ent == sizeof(elf64_rel_t)) {
                uint64_t count = rel_sz / rel_ent;
                const elf64_rel_t *rels = (const elf64_rel_t *)((const char *)image + rel_off);
                for (uint64_t i = 0; i < count; i++) {
                    uint64_t virt = rels[i].r_offset + load_bias;
                    uint64_t phys = vmm_virt_to_phys(user_proc->addr_space, virt);
                    if (!phys) continue;
                    uint64_t *where = (uint64_t *)PHYS_TO_VIRT(phys);
                    if (ELF64_R_TYPE(rels[i].r_info) == R_X86_64_RELATIVE) {
                        *where += load_bias;
                    }
                }
            } else if (rel_ent == sizeof(elf64_rela_t)) {
                uint64_t count = rel_sz / rel_ent;
                const elf64_rela_t *rels = (const elf64_rela_t *)((const char *)image + rel_off);
                for (uint64_t i = 0; i < count; i++) {
                    uint64_t virt = rels[i].r_offset + load_bias;
                    uint64_t phys = vmm_virt_to_phys(user_proc->addr_space, virt);
                    if (!phys) continue;
                    uint64_t *where = (uint64_t *)PHYS_TO_VIRT(phys);
                    if (ELF64_R_TYPE(rels[i].r_info) == R_X86_64_RELATIVE) {
                        *where = (uint64_t)rels[i].r_addend + load_bias;
                    }
                }
            }
        }
    }

    *entry_rip_out = eh->e_entry + load_bias;
    return 0;
}

static int user_load_icx(process_t *user_proc, const void *image, uint64_t image_size, uint64_t *entry_rip_out) {
    const icx_header_t *hdr = (const icx_header_t *)image;
    if (image_size < sizeof(icx_header_t)) return -1;
    if (hdr->magic != ICX_MAGIC || hdr->version != ICX_VERSION) return -1;
    if (hdr->header_size < sizeof(icx_header_t) || hdr->header_size > image_size) return -1;
    if (hdr->entry_offset < hdr->header_size || hdr->entry_offset >= image_size) return -1;
    if (user_map_blob(user_proc, image, image_size, USER_TEXT_BASE) != 0) return -1;
    *entry_rip_out = USER_TEXT_BASE + hdr->entry_offset;
    return 0;
}

static int user_load_image(process_t *user_proc, const void *image, uint64_t image_size, uint64_t *entry_rip_out) {
    if (!user_proc || !image || !entry_rip_out) return -1;
    if (user_prepare_address_space(user_proc) != 0) {
        return -1;
    }

    if (image_size >= sizeof(icx_header_t)) {
        const icx_header_t *hdr = (const icx_header_t *)image;
        if (hdr->magic == ICX_MAGIC && hdr->version == ICX_VERSION) {
            return user_load_icx(user_proc, image, image_size, entry_rip_out);
        }
    }

    if (image_size >= sizeof(elf64_ehdr_t)) {
        const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
        if (eh->e_ident[0] == ELF_MAGIC0 && eh->e_ident[1] == ELF_MAGIC1 &&
            eh->e_ident[2] == ELF_MAGIC2 && eh->e_ident[3] == ELF_MAGIC3) {
            if (user_load_elf(user_proc, image, image_size, entry_rip_out,
                              NULL, NULL, 0) != 0) {
                return -1;
            }
            return 0;
        }
    }

    console_write("user_load_image: unknown format\n", CONSOLE_STYLE_ERROR);
    return -1;
}

static int resolve_interpreter_path(const char *token0, const char *token1,
                                    char *out_path, uint64_t out_cap) {
    if (!token0 || !*token0 || !out_path || out_cap == 0) return -1;

    if ((basename_eq(token0, "env") || cstr_eq(token0, "/usr/bin/env")) && token1 && *token1) {
        if (cstr_eq(token1, "bash") || cstr_eq(token1, "sh")) {
            copy_cstr(out_path, "/apps/shell.app", out_cap);
            return 0;
        }
        if (cstr_has_slash(token1)) {
            copy_cstr(out_path, token1, out_cap);
            return 0;
        }
        copy_cstr(out_path, "/bin/", out_cap);
        copy_cstr(out_path + cstr_len(out_path), token1, out_cap - cstr_len(out_path));
        return 0;
    }

    if (cstr_eq(token0, "bash") || cstr_eq(token0, "sh") ||
        cstr_eq(token0, "/bin/bash") || cstr_eq(token0, "/bin/sh") ||
        cstr_eq(token0, "/usr/bin/bash") || cstr_eq(token0, "/usr/bin/sh")) {
        copy_cstr(out_path, "/apps/shell.app", out_cap);
        return 0;
    }

    copy_cstr(out_path, token0, out_cap);
    return 0;
}

static int build_shebang_argv(const char *script_path, const char *argline,
                              char *interp_path, uint64_t interp_cap,
                              char *arg_storage, uint64_t arg_storage_cap,
                              char **argv_out, uint64_t *argc_out) {
    char shebang[USER_SHEBANG_MAX];
    char shebang_storage[USER_SHEBANG_MAX];
    char parse_input[USER_SHEBANG_MAX];
    char *tokens[4];
    uint64_t token_count = 0;
    uint64_t i = 2;
    uint64_t line_len = 0;
    uint64_t argc = 0;
    uint64_t extra_argc = 0;
    char *extra_argv[USER_ARG_MAX];

    if (argc_out) *argc_out = 0;
    if (!script_path || !interp_path || !arg_storage || !argv_out || !argc_out) return -1;

    zero_bytes(shebang, sizeof(shebang));
    {
        process_t *current_proc = sched_current_process();
        uint64_t size = 0;
        const char *data = vfs_read(current_proc ? current_proc->cwd : vfs_root(), script_path, &size);
        long read;
        if (!data || size <= 3) return -1;
        read = (long)(size < sizeof(shebang) - 1 ? size : (sizeof(shebang) - 1));
        copy_bytes(shebang, data, (uint64_t)read);
        shebang[read] = 0;
        if (shebang[0] != '#' || shebang[1] != '!') return -1;
        while (i < (uint64_t)read && (shebang[i] == ' ' || shebang[i] == '\t')) i++;
        while (i + line_len < (uint64_t)read && shebang[i + line_len] &&
               shebang[i + line_len] != '\n' && shebang[i + line_len] != '\r') {
            line_len++;
        }
        if (line_len == 0 || line_len >= sizeof(shebang_storage)) return -1;
        copy_bytes(shebang_storage, shebang + i, line_len);
        shebang_storage[line_len] = 0;
    }

    copy_cstr(parse_input, shebang_storage, sizeof(parse_input));
    if (split_words(parse_input, shebang_storage, sizeof(shebang_storage), tokens, 4, &token_count) != 0 || token_count == 0) {
        return -1;
    }
    if (resolve_interpreter_path(tokens[0], token_count > 1 ? tokens[1] : 0, interp_path, interp_cap) != 0) {
        return -1;
    }

    argv_out[argc++] = (char *)script_path;
    if (argline && *argline) {
        if (split_words(argline, arg_storage, arg_storage_cap, extra_argv, USER_ARG_MAX - 1, &extra_argc) != 0) {
            return -1;
        }
        for (uint64_t n = 0; n < extra_argc; n++) {
            argv_out[argc++] = extra_argv[n];
        }
    }
    *argc_out = argc;
    return 0;
}

static int user_spawn_pathv_depth(const char *path, uint64_t extra_argc, char *const extra_argv[],
                                  uint64_t *pid_out, uint64_t depth) {
    process_t *user_proc;
    process_t *current_proc = sched_current_process();
    const char *image;
    uint64_t image_size = 0;
    uint64_t entry_rip = 0;
    uint64_t user_rsp = 0;
    thread_t *thread;
    char interp_path[USER_SHEBANG_MAX];
    char shebang_arg_storage[USER_ARG_BYTES_MAX];
    char *shebang_argv[USER_ARG_MAX];
    uint64_t shebang_argc = 0;

    if (!path || !*path || depth > 4) {
        return -1;
    }

    image = vfs_read(current_proc ? current_proc->cwd : vfs_root(), path, &image_size);
    if (!image || image_size == 0) {
        return -1;
    }

    if (image_size >= 2 && image[0] == '#' && image[1] == '!') {
        if (build_shebang_argv(path, 0, interp_path, sizeof(interp_path),
                               shebang_arg_storage, sizeof(shebang_arg_storage),
                               shebang_argv, &shebang_argc) != 0) {
            return -1;
        }
        if (shebang_argc + extra_argc > USER_ARG_MAX) {
            return -1;
        }
        for (uint64_t i = 0; i < extra_argc; i++) {
            shebang_argv[shebang_argc++] = extra_argv[i];
        }
        return user_spawn_pathv_depth(interp_path, shebang_argc, shebang_argv, pid_out, depth + 1);
    }

    user_proc = proc_create_empty(PROCESS_USER);
    if (!user_proc) {
        return -1;
    }

    /* External identity (B3: before READY, so no gate ever observes a
     * half-set identity). Children inherit; the PID1 image is re-rooted
     * to uid 0 / session leader with a fresh monotonic token. */
    if (current_proc) {
        user_proc->ex_uid = current_proc->ex_uid;
        user_proc->ex_token = current_proc->ex_token;
        user_proc->ex_session_leader = 0;
    }
    if (cstr_eq(path, "/sbin/init.app")) {
        user_proc->ex_uid = 0;
        user_proc->ex_session_leader = 1;
        user_proc->ex_token = sched_ticks() ? (uint64_t)sched_ticks() : 1;
    }

    user_proc->state = PROCESS_READY;
    if (path[0] == '/' && path[1] == 'b' && path[2] == 'i' && path[3] == 'n' && path[4] == '/') {
        user_proc->linux_personality = 1;
    }
    if (user_load_image(user_proc, image, image_size, &entry_rip) != 0) {
        return -1;
    }
    if (user_build_initial_stack(user_proc, &user_rsp, path, extra_argc, extra_argv) != 0) {
        return -1;
    }

    thread = proc_create_user_thread(user_proc, entry_rip, user_rsp, user_thread_start);
    if (!thread) {
        return -1;
    }
    /* Task-manager label: the basename of the spawned image. */
    {
        const char *base = path;
        const char *p = path;
        while (*p) {
            if (*p == '/') base = p + 1;
            p++;
        }
        {
            uint64_t i = 0;
            while (base[i] && i < sizeof(user_proc->name) - 1) {
                user_proc->name[i] = base[i];
                i++;
            }
            user_proc->name[i] = 0;
        }
    }
    if (pid_out) {
        *pid_out = user_proc->pid;
    }
    return 0;
}

int user_run_path(const char *path) {
    uint64_t pid;
    if (user_spawn_path(path, &pid) != 0) {
        return -1;
    }
    return user_wait_pid(pid, &user_exit_code);
}

__attribute__((noreturn)) void user_thread_finish(void) {
    process_t *proc = sched_current_process();
    thread_t *thread = sched_current_thread();

    if (thread) {
        tss_set_rsp0(thread->kernel_stack_top);
    }

    vmm_switch_address_space(vmm_kernel_address_space());
    pf_set_current_as(vmm_kernel_address_space());

    user_exit_code = proc ? proc->exit_code : (uint64_t)-1;
    sched_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void user_thread_start(void) {
    thread_t *thread = sched_current_thread();
    process_t *proc = sched_current_process();

    if (!thread || !proc || !thread->user_rip || !thread->user_rsp) {
        if (thread) {
            thread->state = THREAD_ZOMBIE;
        }
        if (proc) {
            proc->state = PROCESS_EXITED;
            proc->exit_code = (uint64_t)-1;
        }
        sched_yield();
        for (;;) {
            __asm__ volatile("hlt");
        }
    }

    proc->state = PROCESS_RUNNING;
    thread->user_return_pending = 0;
    vmm_switch_address_space(proc->addr_space);
    pf_set_current_as(proc->addr_space);
    tss_set_rsp0(thread->user_entry_stack_top ? thread->user_entry_stack_top : thread->kernel_stack_top);
    user_enter(thread->user_rip, thread->user_rsp);
    user_thread_finish();
}

int user_spawn_path(const char *path, uint64_t *pid_out) {
    return user_spawn_pathv_depth(path, 0, 0, pid_out, 0);
}

int user_spawn_path_args(const char *path, const char *argline, uint64_t *pid_out) {
    char arg_storage[USER_ARG_BYTES_MAX];
    char *argv[USER_ARG_MAX];
    uint64_t argc = 0;

    if (!path || !*path) {
        return -1;
    }
    if (argline && *argline) {
        if (split_words(argline, arg_storage, sizeof(arg_storage), argv, USER_ARG_MAX, &argc) != 0) {
            return -1;
        }
    }
    return user_spawn_pathv_depth(path, argc, argv, pid_out, 0);
}

int user_run_path_args(const char *path, const char *argline) {
    uint64_t pid;
    if (user_spawn_path_args(path, argline, &pid) != 0) {
        return -1;
    }
    return user_wait_pid(pid, &user_exit_code);
}

int user_wait_pid(uint64_t pid, uint64_t *exit_code_out) {
    process_t *current = sched_current_process();
    process_t *child = sched_find_process(pid);

    if (!current || !child || child->parent != current) {
        return -1;
    }
    if (child->state == PROCESS_REAPED) {
        return -1;
    }

    while (child->state != PROCESS_EXITED) {
        thread_t *thread = sched_current_thread();
        if (!thread) {
            return -1;
        }
        thread->block_reason = THREAD_BLOCK_WAIT_CHILD;
        thread->wake_tick = 0;
        thread->state = THREAD_BLOCKED;
        current->state = PROCESS_BLOCKED;
        sched_yield();
        thread->block_reason = THREAD_BLOCK_NONE;
        if (child->state == PROCESS_REAPED) {
            return -1;
        }
    }

    if (exit_code_out) {
        *exit_code_out = child->exit_code;
    }
    child->state = PROCESS_REAPED;
    return 0;
}

int user_run_demo(void) {
    process_t *user_proc;
    uint64_t blob_size;
    uint64_t entry_rip = 0;
    thread_t *thread;

    user_proc = proc_create_empty(PROCESS_USER);
    if (!user_proc) {
        return -1;
    }
    blob_size = (uint64_t)(user_demo_end - user_demo_start);
    if (user_load_image(user_proc, user_demo_start, blob_size, &entry_rip) != 0) {
        return -1;
    }
    thread = proc_create_user_thread(user_proc, entry_rip, USER_STACK_TOP - 16, user_thread_start);
    if (!thread) {
        return -1;
    }
    return user_wait_pid(user_proc->pid, &user_exit_code);
}
