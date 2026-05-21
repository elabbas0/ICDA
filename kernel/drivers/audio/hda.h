#ifndef HDA_H
#define HDA_H

#include <stdint.h>

int hda_init(void);
int hda_available(void);
int hda_last_error(void);
int hda_stream_start_s16_stereo(uint16_t sample_rate, uint32_t buffer_len);
int hda_stream_run(void);
int hda_stream_write(uint32_t offset, const uint8_t *samples, uint32_t length);
void hda_stop_playback(void);
uint32_t hda_debug_lpi_b(void);
uint8_t hda_debug_status(void);
uint8_t hda_debug_ctl0(void);
uint8_t hda_debug_ctl2(void);

#endif
