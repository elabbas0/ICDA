#include "pmm.h"
#include "../cpu/multiboot2.h"
#include "../drivers/console/console.h"

// kernel_end is exported by the linker script
extern uint8_t kernel_end[];

static uint64_t *bitmap       = 0;
static uint64_t  total_frames = 0;
static uint64_t  used_frames  = 0;
static uint64_t  next_free    = 0;
static uint64_t  last_mem_top = 0;
static int       last_saw_mmap = 0;

#define FRAMES_PER_WORD  64ULL
#define WORD_INDEX(f)    ((f) / FRAMES_PER_WORD)
#define BIT_INDEX(f)     ((f) % FRAMES_PER_WORD)

static void print_hex64(uint64_t v) {
    console_write_hex64(v, CONSOLE_STYLE_INFO);
}

static void print_dec64(uint64_t v) {
    console_write_dec64(v, CONSOLE_STYLE_INFO);
}

static inline void frame_set(uint64_t frame) {
    bitmap[WORD_INDEX(frame)] |= (1ULL << BIT_INDEX(frame));
}

static inline void frame_clear(uint64_t frame) {
    bitmap[WORD_INDEX(frame)] &= ~(1ULL << BIT_INDEX(frame));
}

static inline int frame_used(uint64_t frame) {
    return (bitmap[WORD_INDEX(frame)] >> BIT_INDEX(frame)) & 1;
}

static uint64_t align_up_u64(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static void mark_used(uint64_t addr, uint64_t size) {
    uint64_t first = ADDR_TO_FRAME(addr);
    uint64_t last  = ADDR_TO_FRAME(addr + size + PAGE_SIZE - 1);

    for (uint64_t f = first; f < last && f < total_frames; f++) {
        if (!frame_used(f)) {
            frame_set(f);
            used_frames++;
        }
    }
}

static void mark_free(uint64_t addr, uint64_t size) {
    uint64_t first = ADDR_TO_FRAME(addr);
    uint64_t last  = ADDR_TO_FRAME(addr + size);

    for (uint64_t f = first; f < last && f < total_frames; f++) {
        if (frame_used(f)) {
            frame_clear(f);
            used_frames--;
        }
    }

    if (first < next_free)
        next_free = first;
}

void pmm_init(void *multiboot_info) {
    struct multiboot_info *info = (struct multiboot_info *)multiboot_info;
    uint8_t *tag_ptr = (uint8_t *)multiboot_info + 8;
    uint8_t *end_ptr = (uint8_t *)multiboot_info + info->total_size;

    uint64_t mem_top = 0;
    int saw_mmap = 0;

    // pass 1: find max usable memory
    for (uint8_t *p = tag_ptr; p < end_ptr; ) {
        struct multiboot_tag *tag = (struct multiboot_tag *)p;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;

        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            saw_mmap = 1;
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            uint8_t *ep  = (uint8_t *)mmap->entries;
            uint8_t *end = (uint8_t *)mmap + mmap->size;

            while (ep < end) {
                struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)ep;

                if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
                    uint64_t top = e->addr + e->len;
                    if (top > mem_top) mem_top = top;
                }

                ep += mmap->entry_size;
            }
        }

        uint32_t step = (tag->size + 7) & ~7;
        if (step == 0) break;
        p += step;
    }

    total_frames = ADDR_TO_FRAME(mem_top) + 1;

    last_mem_top = mem_top;
    last_saw_mmap = saw_mmap;

    if (!saw_mmap || mem_top == 0) {
        console_write("PMM debug: mmap=", CONSOLE_STYLE_WARN);
        console_write(saw_mmap ? "yes" : "no", CONSOLE_STYLE_WARN);
        console_write(" total_size=", CONSOLE_STYLE_WARN);
        print_dec64(info->total_size);
        console_write(" mem_top=", CONSOLE_STYLE_WARN);
        print_hex64(mem_top);
        console_write("\n", CONSOLE_STYLE_WARN);
    }

    uint64_t bitmap_words = (total_frames + FRAMES_PER_WORD - 1) / FRAMES_PER_WORD;
    uint64_t bitmap_bytes = bitmap_words * 8;
    uint64_t multiboot_end = (uint64_t)multiboot_info + info->total_size;
    uint64_t bitmap_base = (uint64_t)kernel_end;
    if (multiboot_end > bitmap_base)
        bitmap_base = multiboot_end;
    bitmap_base = align_up_u64(bitmap_base, 4096);

    bitmap      = (uint64_t *)bitmap_base;
    used_frames = total_frames;

    // mark everything used initially
    for (uint64_t i = 0; i < bitmap_words; i++)
        bitmap[i] = ~0ULL;

    // pass 2: free usable regions
    for (uint8_t *p = tag_ptr; p < end_ptr; ) {
        struct multiboot_tag *tag = (struct multiboot_tag *)p;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;

        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
            struct multiboot_tag_mmap *mmap = (struct multiboot_tag_mmap *)tag;
            uint8_t *ep  = (uint8_t *)mmap->entries;
            uint8_t *end = (uint8_t *)mmap + mmap->size;

            while (ep < end) {
                struct multiboot_mmap_entry *e = (struct multiboot_mmap_entry *)ep;

                if (e->type == MULTIBOOT_MEMORY_AVAILABLE)
                    mark_free(e->addr, e->len);

                ep += mmap->entry_size;
            }
        }

        uint32_t step = (tag->size + 7) & ~7;
        if (step == 0) break;
        p += step;
    }

    // protect reserved regions
    frame_set(0);
    mark_used(0x0, 0x100000);
    mark_used(0x100000, (uint64_t)kernel_end - 0x100000);
    mark_used((uint64_t)bitmap, bitmap_bytes);
    mark_used((uint64_t)multiboot_info, info->total_size);

    // find first free frame
    next_free = 0;
    for (uint64_t i = 0; i < bitmap_words; i++) {
        if (bitmap[i] != ~0ULL) {
            next_free = i * FRAMES_PER_WORD;
            break;
        }
    }
}

uint64_t pmm_alloc() {
    uint64_t bitmap_words = (total_frames + FRAMES_PER_WORD - 1) / FRAMES_PER_WORD;

    for (uint64_t w = WORD_INDEX(next_free); w < bitmap_words; w++) {
        if (bitmap[w] == ~0ULL) continue;

        uint64_t bit   = __builtin_ctzll(~bitmap[w]);
        uint64_t frame = w * FRAMES_PER_WORD + bit;
        if (frame >= total_frames) return 0;

        frame_set(frame);
        used_frames++;
        next_free = frame + 1;

        return FRAME_TO_ADDR(frame);
    }

    return 0;
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc();

    uint64_t run_start = 0, run_len = 0;

    for (uint64_t f = 0; f < total_frames; f++) {
        if (!frame_used(f)) {
            if (run_len == 0) run_start = f;

            if (++run_len == count) {
                for (uint64_t i = run_start; i < run_start + count; i++) {
                    frame_set(i);
                    used_frames++;
                }
                return FRAME_TO_ADDR(run_start);
            }
        } else {
            run_len = 0;
        }
    }

    return 0;
}

void pmm_free(uint64_t addr) {
    if (!addr) return;

    uint64_t frame = ADDR_TO_FRAME(addr);
    if (frame == 0 || frame >= total_frames) return;
    if (!frame_used(frame)) return;

    frame_clear(frame);
    used_frames--;

    if (frame < next_free)
        next_free = frame;
}

void pmm_free_range(uint64_t addr, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        pmm_free(addr + i * PAGE_SIZE);
}

uint64_t pmm_free_frames()  { return total_frames - used_frames; }
uint64_t pmm_total_frames() { return total_frames; }
uint64_t pmm_next_free_frame() { return next_free; }

void pmm_print_stats() {
    console_write("PMM stats: mmap=", CONSOLE_STYLE_WARN);
    console_write(last_saw_mmap ? "yes" : "no", CONSOLE_STYLE_WARN);
    console_write(" mem_top=", CONSOLE_STYLE_WARN);
    print_hex64(last_mem_top);
    console_write(" total=", CONSOLE_STYLE_WARN);
    print_dec64(total_frames);
    console_write(" used=", CONSOLE_STYLE_WARN);
    print_dec64(used_frames);
    console_write(" free=", CONSOLE_STYLE_WARN);
    print_dec64(pmm_free_frames());
    console_write(" next_free=", CONSOLE_STYLE_WARN);
    print_hex64(FRAME_TO_ADDR(next_free));
    console_write("\n", CONSOLE_STYLE_WARN);
}
