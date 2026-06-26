#ifndef SPEAKER_H
#define SPEAKER_H

#include "lib/types.h"

void speaker_init(void);
void speaker_play(uint32_t freq_hz);
void speaker_stop(void);
void speaker_beep(uint32_t freq_hz, uint32_t duration_ms);

#endif
