#ifndef HDA_H
#define HDA_H

/* Intel High Definition Audio (class 0x0403) controller + codec backend.
 * Covers the Realtek ALC family (vendor 0x10EC) and any other HDA codec with
 * a discoverable DAC -> pin path. The public entry points live in
 * drivers/audio/audio.h; drivers/audio.c picks between this and AC'97. */
#include "lib/types.h"

int  hda_init(void);                             /* probe PCI, reset, enumerate codec */
const char* hda_codec_name(void);                /* "Realtek ALC..." or "" if unknown */
const char* hda_last_error(void);                /* short reason when init failed */
int  hda_open(uint32_t sample_rate);             /* program converter + stream ring */
void hda_close(void);                            /* stop the stream, free the ring */
int  hda_write(const int16_t* pcm, int frames);  /* stereo frames accepted */
void hda_play(void);
void hda_pause(void);
int  hda_playing(void);
int  hda_queued(void);                           /* stereo frames ahead of the DMA */
uint64_t hda_written_total(void);

#endif
