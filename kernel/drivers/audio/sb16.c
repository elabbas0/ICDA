#include "sb16.h"

#include "../../memory/vmm.h"
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

#define SB16_DMA_MAX_CHUNK 32768U
#define SB16_DMA_LOW_LIMIT 0x01000000ULL

static int sb16_present = 0;
static int sb16_error = 0;
static uint64_t sb16_dma_phys = 0;
static uint8_t *sb16_dma_virt = 0;
static uint32_t sb16_stream_len = 0;
static int sb16_stream_active = 0;
static uint8_t sb16_dma_static[SB16_DMA_MAX_CHUNK]
    __attribute__((aligned(65536), section(".dma_low")));

static int sb16_dsp_write(uint8_t value);

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

static void sb16_program_dma(const uint8_t *src, uint32_t length, uint8_t mode) {
    uint32_t phys = (uint32_t)sb16_dma_phys;
    uint16_t addr16 = (uint16_t)(phys & 0xFFFFU);
    uint8_t page = (uint8_t)((phys >> 16) & 0xFFU);
    uint16_t count = (uint16_t)(length - 1U);

    (void)src;

    outb(DMA8_MASK_REG, 0x05);
    outb(DMA8_CLEAR_FF, 0x00);
    outb(DMA8_MODE_REG, mode);
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

static int sb16_start_auto_init(uint32_t length, uint16_t sample_rate) {
    if (sb16_dsp_write(0xD1) != 0) return -1;
    if (sb16_dsp_write(0x41) != 0) return -1;
    if (sb16_dsp_write((uint8_t)((sample_rate >> 8) & 0xFFU)) != 0) return -1;
    if (sb16_dsp_write((uint8_t)(sample_rate & 0xFFU)) != 0) return -1;
    if (sb16_dsp_write(0xC6) != 0) return -1;
    if (sb16_dsp_write(0x00) != 0) return -1;
    if (sb16_dsp_write((uint8_t)((length - 1U) & 0xFFU)) != 0) return -1;
    if (sb16_dsp_write((uint8_t)(((length - 1U) >> 8) & 0xFFU)) != 0) return -1;
    return 0;
}

int sb16_init(void) {
    uint64_t phys;

    sb16_present = 0;
    sb16_error = 0;
    sb16_dma_phys = 0;
    sb16_dma_virt = 0;

    if (sb16_reset() != 0) {
        sb16_error = 1;
        return -1;
    }

    phys = (uint64_t)(uintptr_t)sb16_dma_static;
    if ((phys + SB16_DMA_MAX_CHUNK) > SB16_DMA_LOW_LIMIT) {
        sb16_error = 2;
        return -1;
    }

    sb16_dma_phys = phys;
    sb16_dma_virt = sb16_dma_static;

    sb16_set_volume(0xF0, 0xF0);
    sb16_present = 1;
    return 0;
}

int sb16_available(void) {
    return sb16_present;
}

int sb16_last_error(void) {
    return sb16_error;
}

void sb16_stop_playback(void) {
    if (!sb16_present) {
        return;
    }
    sb16_stream_active = 0;
    sb16_stream_len = 0;
    (void)sb16_dsp_write(0xD0);
    (void)sb16_dsp_write(0xDA);
    (void)sb16_dsp_write(0xD3);
}

int sb16_play_pcm_u8_mono_interruptible(const uint8_t *samples, uint32_t length, uint16_t sample_rate,
                                        volatile uint32_t *stop_flag, volatile uint32_t *played_out) {
    if (!sb16_present || !samples || length == 0 || sample_rate < 2000U || sample_rate > 44100U) {
        return -1;
    }

    if (length > SB16_DMA_MAX_CHUNK) {
        return -1;
    }
    if (stop_flag && *stop_flag) {
        if (played_out) {
            *played_out = 0;
        }
        return 0;
    }

    sb16_stop_playback();
    if (sb16_reset() != 0) {
        return -1;
    }
    sb16_set_volume(0xF0, 0xF0);

    for (uint32_t i = 0; i < length; i++) {
        sb16_dma_virt[i] = samples[i];
    }

    sb16_program_dma(sb16_dma_virt, length, 0x49);
    if (sb16_start_chunk(length, sample_rate) != 0) {
        return -1;
    }

    if (played_out) {
        *played_out = length;
    }
    return 0;
}

int sb16_play_pcm_u8_mono(const uint8_t *samples, uint32_t length, uint16_t sample_rate) {
    return sb16_play_pcm_u8_mono_interruptible(samples, length, sample_rate, 0, 0);
}

int sb16_stream_start_u8_mono(uint16_t sample_rate, uint32_t buffer_len) {
    if (!sb16_present || buffer_len == 0 || buffer_len > SB16_DMA_MAX_CHUNK || sample_rate < 2000U || sample_rate > 44100U) {
        return -1;
    }
    if (((sb16_dma_phys & 0xFFFFU) + buffer_len) > 0x10000U) {
        return -1;
    }

    sb16_stop_playback();
    if (sb16_reset() != 0) {
        return -1;
    }
    sb16_set_volume(0xF0, 0xF0);
    for (uint32_t i = 0; i < buffer_len; i++) {
        sb16_dma_virt[i] = 0x80U;
    }
    sb16_program_dma(sb16_dma_virt, buffer_len, 0x59);
    if (sb16_start_auto_init(buffer_len, sample_rate) != 0) {
        return -1;
    }
    sb16_stream_len = buffer_len;
    sb16_stream_active = 1;
    return 0;
}

int sb16_stream_write(uint32_t offset, const uint8_t *samples, uint32_t length) {
    if (!sb16_stream_active || !samples || length == 0 || offset >= sb16_stream_len || length > sb16_stream_len) {
        return -1;
    }
    for (uint32_t i = 0; i < length; i++) {
        sb16_dma_virt[(offset + i) % sb16_stream_len] = samples[i];
    }
    return 0;
}

void sb16_stream_silence(uint32_t offset, uint32_t length) {
    if (!sb16_stream_active || length == 0 || offset >= sb16_stream_len || length > sb16_stream_len) {
        return;
    }
    for (uint32_t i = 0; i < length; i++) {
        sb16_dma_virt[(offset + i) % sb16_stream_len] = 0x80U;
    }
}
