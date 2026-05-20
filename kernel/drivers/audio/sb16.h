#ifndef SB16_H
#define SB16_H

#include <stdint.h>

int sb16_init(void);
int sb16_available(void);
int sb16_last_error(void);
int sb16_play_pcm_u8_mono(const uint8_t *samples, uint32_t length, uint16_t sample_rate);
int sb16_play_pcm_u8_mono_interruptible(const uint8_t *samples, uint32_t length, uint16_t sample_rate,
                                        volatile uint32_t *stop_flag, volatile uint32_t *played_out);
int sb16_stream_start_u8_mono(uint16_t sample_rate, uint32_t buffer_len);
int sb16_stream_write(uint32_t offset, const uint8_t *samples, uint32_t length);
void sb16_stream_silence(uint32_t offset, uint32_t length);
void sb16_stop_playback(void);

#endif
