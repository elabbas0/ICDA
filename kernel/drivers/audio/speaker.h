#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

void speaker_init(void);
void speaker_stop(void);
void speaker_play(uint32_t frequency_hz);
void speaker_play_for(uint32_t frequency_hz, uint64_t ticks);

#endif
