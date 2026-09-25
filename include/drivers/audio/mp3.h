#ifndef MP3_H
#define MP3_H

#include "lib/types.h"

/* Thin streaming wrapper around the vendored minimp3 decoder
 * (include/drivers/audio/minimp3.h, MIT - see file header). The whole file is
 * expected to stay in memory; every mp3_decode() call yields one MPEG
 * audio frame (1152 samples per channel for Layer III, interleaved L/R).
 *
 *   mp3_t* m = mp3_open(data, size);
 *   if (m) {
 *       audio_open(mp3_rate(m));
 *       int n;
 *       while ((n = mp3_decode(m, pcm)) > 0)
 *           audio_write(pcm, n / mp3_channels(m));
 *       mp3_close(m);
 *   } */

typedef struct mp3 mp3_t;

mp3_t* mp3_open(const uint8_t* data, size_t size);   /* NULL = no audio */
void   mp3_close(mp3_t* m);

int    mp3_decode(mp3_t* m, int16_t* pcm);           /* samples; 0 = EOF */
int    mp3_channels(mp3_t* m);
int    mp3_rate(mp3_t* m);                           /* sample rate Hz */
int    mp3_bitrate(mp3_t* m);                        /* kbps */
size_t mp3_total_bytes(mp3_t* m);                    /* file size */
size_t mp3_pos(mp3_t* m);                            /* current read offset */

#endif
