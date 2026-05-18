#ifndef SB16_H
#define SB16_H

#include <stdint.h>

int sb16_init(void);
int sb16_available(void);
int sb16_play_pcm_u8_mono(const uint8_t *samples, uint32_t length, uint16_t sample_rate);

#endif
