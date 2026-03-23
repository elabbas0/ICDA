#include "pmm.h"
#include "multiboot2.h"
#include "drivers/display/framebuffer.h"

extern uint8_t kernel_end[];

static volatile uint64_t *bitmap       = 0;
static uint64_t  total_frames = 0;
static volatile uint64_t  used_frames  = 0;
static uint64_t  next_free    = 0; // hint so we never re-scan already-used frames

#define FRAMES_PER_WORD  64ULL
#define WORD_INDEX(f)    ((f) / FRAMES_PER_WORD)
#define BIT_INDEX(f)     ((f) % FRAMES_PER_WORD)

static inline void frame_set(uint64_t frame) {
    bitmap[WORD_INDEX(frame)] |= (1ULL << BIT_INDEX(frame));
}

static inline void frame_clear(uint64_t frame) {
    bitmap[WORD_INDEX(frame)] &= ~(1ULL << BIT_INDEX(frame));
}

static inline int frame_used(uint64_t frame) {
    return (bitmap[WORD_INDEX(frame)] >> BIT_INDEX(frame)) & 1;
}

// mark every page in [addr, addr+size) as used
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

// mark every fully contained page in [addr, addr+size) as free
static void mark_free(uint64_t addr, uint64_t size) {
    // no rounding, trust the firmware's page-aligned regions
    uint64_t first = ADDR_TO_FRAME(addr);
    uint64_t last  = ADDR_TO_FRAME(addr + size);
    for (uint64_t f = first; f < last && f < total_frames; f++) {
        if (frame_used(f)) {
            frame_clear(f);
            used_frames--;
        }
    }
    if (first < next_free) next_free = first;
}

void pmm_init(void *multiboot_info) {
    struct multiboot_info *info = (struct multiboot_info *)multiboot_info;
    uint8_t *tag_ptr = (uint8_t *)multiboot_info + 8;
    uint8_t *end_ptr = (uint8_t *)multiboot_info + info->total_size;

    // find the top of usable RAM so we know how big the bitmap needs to be
    uint64_t mem_top = 0;
    for (uint8_t *p = tag_ptr; p < end_ptr; ) {
        struct multiboot_tag *tag = (struct multiboot_tag *)p;
        if (tag->type == MULTIBOOT_TAG_TYPE_END) break;
        if (tag->type == MULTIBOOT_TAG_TYPE_MMAP) {
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

    // place the bitmap right after the kernel in memory
    total_frames = ADDR_TO_FRAME(mem_top) + 1;
    uint64_t bitmap_words = (total_frames + FRAMES_PER_WORD - 1) / FRAMES_PER_WORD;
    uint64_t bitmap_bytes = bitmap_words * 8;

    bitmap      = (uint64_t *)kernel_end;
    used_frames = total_frames;

    // start with everything marked used — safe default
    for (uint64_t i = 0; i < bitmap_words; i++)
        bitmap[i] = ~0ULL;

    // free every region the firmware says is usable
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

    // after mark_free: how many frames did we free?
    // protect regions that must never be handed out
    frame_set(0);                                               // never give out the null page
    mark_used(0x0, 0x100000);                                   // first 1MB: BIOS, VGA, ROM
    mark_used(0x100000, (uint64_t)kernel_end - 0x100000);       // the kernel itself
    mark_used((uint64_t)bitmap, bitmap_bytes);                  // the bitmap we just placed
    mark_used((uint64_t)multiboot_info, info->total_size);      // multiboot info, still needed

    // after mark_used: how many remain free?
    // find the first free word to seed next_free
    next_free = 0;
    for (uint64_t i = 0; i < bitmap_words; i++) {
        if (bitmap[i] != ~0ULL) { next_free = i * FRAMES_PER_WORD; break; }
    }

    pmm_print_stats();
}

uint64_t pmm_alloc() {
    uint64_t bitmap_words = (total_frames + FRAMES_PER_WORD - 1) / FRAMES_PER_WORD;

    for (uint64_t w = WORD_INDEX(next_free); w < bitmap_words; w++) {
        if (bitmap[w] == ~0ULL) continue; // whole word full, skip instantly

        // ctz finds the lowest 0 bit in one instruction
        uint64_t bit   = __builtin_ctzll(~bitmap[w]);
        uint64_t frame = w * FRAMES_PER_WORD + bit;
        if (frame >= total_frames) return 0;

        frame_set(frame);
        used_frames++;
        next_free = frame + 1;
        return FRAME_TO_ADDR(frame);
    }

    return 0; // out of memory
}

// allocates n physically contiguous frames — needed for page tables and DMA
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
    if (!frame_used(frame)) return; // double-free guard
    frame_clear(frame);
    used_frames--;
    if (frame < next_free) next_free = frame;
}

void pmm_free_range(uint64_t addr, uint64_t count) {
    for (uint64_t i = 0; i < count; i++)
        pmm_free(addr + i * PAGE_SIZE);
}

uint64_t pmm_free_frames()  { return total_frames - used_frames; }
uint64_t pmm_total_frames() { return total_frames; }

void pmm_print_stats() {
    uint64_t free_mb  = (pmm_free_frames()  * PAGE_SIZE) / (1024ULL * 1024ULL);
    uint64_t total_mb = (pmm_total_frames() * PAGE_SIZE) / (1024ULL * 1024ULL);

    fb_print("PMM:  ", FB_WHITE, FB_BLACK);
    fb_print_int((int)free_mb,  FB_GREEN, FB_BLACK);
    fb_print(" MB free / ", FB_WHITE, FB_BLACK);
    fb_print_int((int)total_mb, FB_WHITE, FB_BLACK);
    fb_print(" MB total\n", FB_WHITE, FB_BLACK);
}