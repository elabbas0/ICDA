#include "user.h"
#include "elf.h"

#include "sched.h"
#include "../cpu/gdt.h"
#include "../drivers/console/console.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../memory/pf.h"
#include "../memory/pmm.h"
#include "../memory/vmm.h"

extern uint8_t user_demo_start[];
extern uint8_t user_demo_end[];

static uint64_t user_exit_code = 0;

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

    stack_page = pmm_alloc();
    if (!stack_page) {
        return -1;
    }

    {
        uint64_t *stack = (uint64_t *)PHYS_TO_VIRT(stack_page);
        for (int i = 0; i < (int)(PAGE_SIZE_4K / sizeof(uint64_t)); i++) {
            stack[i] = 0;
        }
    }

    if (vmm_map_page(proc->addr_space, USER_STACK_TOP - PAGE_SIZE_4K, stack_page, VMM_FLAGS_USER_RW) != 0) {
        pmm_free(stack_page);
        return -1;
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
        if (vmm_map_page(proc->addr_space, virt_base + i * PAGE_SIZE_4K, phys, VMM_FLAGS_USER_RO) != 0) {
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

static int user_load_image(process_t *user_proc, const void *image, uint64_t image_size, uint64_t *entry_rip_out) {
    uint64_t entry_rip;

    if (!user_proc || !image || !entry_rip_out) {
        return -1;
    }

    if (user_prepare_address_space(user_proc) != 0) {
        return -1;
    }

    if (image_size >= sizeof(icx_header_t)) {
        const icx_header_t *hdr = (const icx_header_t *)image;

        if (hdr->magic == ICX_MAGIC && hdr->version == ICX_VERSION) {
            if (hdr->header_size < sizeof(icx_header_t) || hdr->header_size > image_size) {
                return -1;
            }
            if (hdr->entry_offset < hdr->header_size || hdr->entry_offset >= image_size) {
                return -1;
            }
            if (user_map_blob(user_proc, image, image_size, USER_TEXT_BASE) != 0) {
                return -1;
            }
            entry_rip = USER_TEXT_BASE + hdr->entry_offset;
            *entry_rip_out = entry_rip;
            return 0;
        }
    }

    if (image_size >= sizeof(elf64_ehdr_t)) {
        const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
        const elf64_phdr_t *ph;

        if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
            eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
            return -1;
        }
        if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
            return -1;
        }
        if (eh->e_ident[6] != EV_CURRENT || eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64) {
            return -1;
        }
        if (eh->e_phentsize != sizeof(elf64_phdr_t) || eh->e_phnum == 0) {
            return -1;
        }
        if (eh->e_phoff > image_size) {
            return -1;
        }
        if ((uint64_t)eh->e_phnum > (image_size - eh->e_phoff) / sizeof(elf64_phdr_t)) {
            return -1;
        }

        ph = (const elf64_phdr_t *)((const char *)image + eh->e_phoff);
        for (uint16_t i = 0; i < eh->e_phnum; i++) {
            if (ph[i].p_type != PT_LOAD) {
                continue;
            }
            if (ph[i].p_vaddr < USER_TEXT_BASE || ph[i].p_vaddr >= USER_STACK_LIMIT) {
                return -1;
            }
            if (ph[i].p_memsz == 0) {
                continue;
            }
            if (ph[i].p_vaddr + ph[i].p_memsz < ph[i].p_vaddr) {
                return -1;
            }
            if (ph[i].p_vaddr + ph[i].p_memsz >= USER_STACK_LIMIT) {
                return -1;
            }
            if (user_map_segment(user_proc, (const char *)image, image_size, ph[i].p_vaddr,
                                 ph[i].p_offset, ph[i].p_filesz, ph[i].p_memsz, ph[i].p_flags) != 0) {
                return -1;
            }
        }

        entry_rip = eh->e_entry;
        if (entry_rip < USER_TEXT_BASE || entry_rip >= USER_STACK_LIMIT) {
            return -1;
        }
    } else {
        return -1;
    }

    *entry_rip_out = entry_rip;
    return 0;
}

int user_run_path(const char *path) {
    uint64_t pid;
    if (user_spawn_path(path, &pid) != 0) {
        return -1;
    }
    return user_wait_pid(pid, &user_exit_code);
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
    user_exit_code = proc->exit_code;
    sched_yield();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

int user_spawn_path(const char *path, uint64_t *pid_out) {
    process_t *user_proc;
    process_t *current_proc = sched_current_process();
    const char *image;
    uint64_t image_size = 0;
    uint64_t entry_rip = 0;
    thread_t *thread;

    if (!path || !*path) {
        return -1;
    }

    image = vfs_read(current_proc ? current_proc->cwd : vfs_root(), path, &image_size);
    if (!image || image_size == 0) {
        return -1;
    }

    user_proc = proc_create_empty(PROCESS_USER);
    if (!user_proc) {
        return -1;
    }

    user_proc->state = PROCESS_READY;
    if (user_load_image(user_proc, image, image_size, &entry_rip) != 0) {
        return -1;
    }
    thread = proc_create_user_thread(user_proc, entry_rip, USER_STACK_TOP - 16, user_thread_start);
    if (!thread) {
        return -1;
    }
    if (pid_out) {
        *pid_out = user_proc->pid;
    }
    return 0;
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
