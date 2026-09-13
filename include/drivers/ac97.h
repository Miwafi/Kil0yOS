#ifndef AC97_H
#define AC97_H

/* Intel ICH AC'97 bus-master backend. The public entry points live in
 * drivers/audio.h; drivers/audio.c picks between this and HDA. */
#include "lib/types.h"

int  ac97_init(void);                            /* probe PCI + cold reset; -1 = none */
int  ac97_open(uint32_t sample_rate);            /* set DAC rate, arm the ring */
void ac97_close(void);                           /* stop DMA, free ring */
int  ac97_write(const int16_t* pcm, int frames); /* stereo frames accepted */
void ac97_play(void);
void ac97_pause(void);
int  ac97_playing(void);
int  ac97_queued(void);                          /* stereo frames ahead of the DAC */
uint64_t ac97_written_total(void);

#endif
