#ifndef AUDIO_H
#define AUDIO_H

#include "lib/types.h"

/* Intel ICHx AC97 ("Sound over PCI"): NAM codec registers + NABM bus-master
 * DMA. QEMU exposes it as PCI 8086:2415 (class 04/01). Streaming model:
 * audio_open arms a ring of 8 BDL buffers; audio_write() copies stereo
 * frames into the ring ahead of the playhead; the desktop pump keeps the
 * ring filled. All ports are polled - no IRQ is used. */

int  audio_init(void);                 /* probe PCI, cold-reset codec; -1 = none */
int  audio_open(uint32_t sample_rate); /* set DAC rate, clear ring; 0 ok */
void audio_close(void);                /* stop DMA, free BDL/buffers */

/* copy up to `frames` stereo int16 frames into the ring; returns accepted
 * count (0 = playhead caught up, caller should decode more later) */
int  audio_write(const int16_t* pcm, int frames);

void audio_play(void);                 /* set DMA run bit */
void audio_pause(void);                /* clear DMA run bit */
int  audio_playing(void);

/* frames still queued ahead of the playhead (stereo frames) */
int  audio_queued(void);
/* total stereo frames handed to the ring since audio_open */
uint64_t audio_written_total(void);

#endif
