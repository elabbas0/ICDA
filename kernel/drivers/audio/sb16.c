#include "sb16.h"

#include "../../memory/pmm.h"
#include "../../memory/vmm.h"
#define PIT_BASE_FREQUENCY 1193182ULL
#define PIT_COMMAND_PORT   0x43
#define PIT_CHANNEL0_PORT  0x40
#define SB16_BASE          0x220
#define SB16_MIXER_ADDR    (SB16_BASE + 0x04)
#define SB16_MIXER_DATA    (SB16_BASE + 0x05)
#define SB16_DSP_RESET     (SB16_BASE + 0x06)
#define SB16_DSP_READ      (SB16_BASE + 0x0A)
#define SB16_DSP_WRITE     (SB16_BASE + 0x0C)
#define SB16_DSP_READ_STAT (SB16_BASE + 0x0E)

#define DMA8_MASK_REG      0x0A
#define DMA8_MODE_REG      0x0B
#define DMA8_CLEAR_FF      0x0C
#define DMA8_CH1_ADDR      0x02
#define DMA8_CH1_COUNT     0x03
#define DMA8_CH1_PAGE      0x83

#define SB16_DMA_CHUNK     65535U

static int sb16_present = 0;
static uint64_t sb16_dma_region_phys = 0;
static uint64_t sb16_dma_phys = 0;
static uint8_t *sb16_dma_virt = 0;

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_wait_short(void) {
    for (volatile uint32_t i = 0; i < 1000U; i++) {
        __asm__ volatile("" ::: "memory");
    }
}

static uint16_t pit_read_counter0(void) {
    uint8_t lo;
    uint8_t hi;

    outb(PIT_COMMAND_PORT, 0x00);
    lo = inb(PIT_CHANNEL0_PORT);
    hi = inb(PIT_CHANNEL0_PORT);
    return (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

static void sb16_wait_chunk(uint32_t samples, uint16_t sample_rate) {
    uint64_t counts_needed = (((uint64_t)samples * PIT_BASE_FREQUENCY) + sample_rate - 1U) / (uint64_t)sample_rate;
    uint64_t elapsed = 0;
    uint16_t prev = pit_read_counter0();

    while (elapsed < counts_needed) {
        uint16_t cur = pit_read_counter0();
        if (prev >= cur) {
            elapsed += (uint64_t)(prev - cur);
        } else {
            elapsed += (uint64_t)prev + (uint64_t)(0x10000U - cur);
        }
        prev = cur;
    }
}

static int sb16_dsp_write(uint8_t value) {
    for (uint32_t i = 0; i < 100000U; i++) {
        if ((inb(SB16_DSP_WRITE) & 0x80U) == 0) {
            outb(SB16_DSP_WRITE, value);
            return 0;
        }
    }
    return -1;
}

static int sb16_dsp_read(uint8_t *value) {
    for (uint32_t i = 0; i < 100000U; i++) {
        if (inb(SB16_DSP_READ_STAT) & 0x80U) {
            *value = inb(SB16_DSP_READ);
            return 0;
        }
    }
    return -1;
}

static int sb16_reset(void) {
    uint8_t ack = 0;

    outb(SB16_DSP_RESET, 1);
    io_wait_short();
    outb(SB16_DSP_RESET, 0);
    io_wait_short();

    if (sb16_dsp_read(&ack) != 0) {
        return -1;
    }
    return ack == 0xAA ? 0 : -1;
}

static void sb16_set_volume(uint8_t left, uint8_t right) {
    outb(SB16_MIXER_ADDR, 0x22);
    outb(SB16_MIXER_DATA, (uint8_t)((left & 0xF0U) | ((right >> 4) & 0x0FU)));
}

static void sb16_program_dma(const uint8_t *src, uint32_t length) {
    uint32_t phys = (uint32_t)(uintptr_t)VIRT_TO_PHYS((uint64_t)(uintptr_t)src);
    uint16_t addr16 = (uint16_t)(phys & 0xFFFFU);
    uint8_t page = (uint8_t)((phys >> 16) & 0xFFU);
    uint16_t count = (uint16_t)(length - 1U);

    outb(DMA8_MASK_REG, 0x05);
    outb(DMA8_CLEAR_FF, 0x00);
    outb(DMA8_MODE_REG, 0x49);
    outb(DMA8_CH1_PAGE, page);
    outb(DMA8_CH1_ADDR, (uint8_t)(addr16 & 0xFFU));
    outb(DMA8_CH1_ADDR, (uint8_t)((addr16 >> 8) & 0xFFU));
    outb(DMA8_CH1_COUNT, (uint8_t)(count & 0xFFU));
    outb(DMA8_CH1_COUNT, (uint8_t)((count >> 8) & 0xFFU));
    outb(DMA8_MASK_REG, 0x01);
}

static int sb16_start_chunk(uint32_t length, uint16_t sample_rate) {
    if (sb16_dsp_write(0xD1) != 0) return -1;
    if (sb16_dsp_write(0x41) != 0) return -1;
    if (sb16_dsp_write((uint8_t)((sample_rate >> 8) & 0xFFU)) != 0) return -1;
    if (sb16_dsp_write((uint8_t)(sample_rate & 0xFFU)) != 0) return -1;
    if (sb16_dsp_write(0xC0) != 0) return -1;
    if (sb16_dsp_write(0x00) != 0) return -1;
    if (sb16_dsp_write((uint8_t)((length - 1U) & 0xFFU)) != 0) return -1;
    if (sb16_dsp_write((uint8_t)(((length - 1U) >> 8) & 0xFFU)) != 0) return -1;
    return 0;
}

int sb16_init(void) {
    uint64_t region_phys;
    uint64_t aligned_phys;

    sb16_present = 0;
    sb16_dma_region_phys = 0;
    sb16_dma_phys = 0;
    sb16_dma_virt = 0;

    if (sb16_reset() != 0) {
        return -1;
    }

    region_phys = pmm_alloc_contiguous(32);
    if (!region_phys) {
        return -1;
    }

    aligned_phys = (region_phys + 0xFFFFULL) & ~0xFFFFULL;
    if ((aligned_phys + 0x10000ULL) > (region_phys + 32ULL * PAGE_SIZE)) {
        return -1;
    }

    sb16_dma_region_phys = region_phys;
    sb16_dma_phys = aligned_phys;
    sb16_dma_virt = (uint8_t *)PHYS_TO_VIRT(aligned_phys);

    sb16_set_volume(0xF0, 0xF0);
    sb16_present = 1;
    return 0;
}

int sb16_available(void) {
    return sb16_present;
}

int sb16_play_pcm_u8_mono(const uint8_t *samples, uint32_t length, uint16_t sample_rate) {
    uint32_t offset = 0;

    if (!sb16_present || !samples || length == 0 || sample_rate < 2000U || sample_rate > 44100U) {
        return -1;
    }

    if (sb16_reset() != 0) {
        return -1;
    }
    sb16_set_volume(0xF0, 0xF0);

    while (offset < length) {
        uint32_t remaining = length - offset;
        uint32_t chunk = remaining > SB16_DMA_CHUNK ? SB16_DMA_CHUNK : remaining;
        for (uint32_t i = 0; i < chunk; i++) {
            sb16_dma_virt[i] = samples[offset + i];
        }

        sb16_program_dma(sb16_dma_virt, chunk);
        if (sb16_start_chunk(chunk, sample_rate) != 0) {
            return -1;
        }

        sb16_wait_chunk(chunk, sample_rate);
        (void)inb(SB16_DSP_READ_STAT);
        offset += chunk;
    }

    (void)sb16_dsp_write(0xD3);
    return 0;
}
