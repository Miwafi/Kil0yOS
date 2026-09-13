#ifndef JPEG_H
#define JPEG_H

#include "lib/types.h"

/* Minimal baseline-JPEG (SOF0/SOF1 huffman) decoder.
 *
 * Supported: 8-bit samples, 1..3 components, sampling factors up to 2x2
 * (grayscale / 4:4:4 / 4:2:2 / 4:2:0), restart intervals (DRI/RSTn), 8- and
 * 16-bit quant tables. Rejected: progressive (SOF2), arithmetic coding,
 * 4-component (CMYK/Adobe) streams.
 *
 * On success returns 0 and fills img: one 8-bit plane per component
 * (kmalloc'd, component resolution - e.g. Cb/Cr are W/2 x H/2 for 4:2:0),
 * plus the full display resolution and per-component sampling factors.
 * The caller renders via YCbCr->RGB and frees with jpeg_image_free().
 * Returns -1 on unsupported or corrupt input (img zeroed, nothing to free). */
typedef struct {
    int W, H;                /* display resolution */
    int ncomp;               /* 1 = grayscale (plane[1]/[2] == NULL) */
    uint8_t* plane[3];
    int pw[3], ph[3];        /* plane dimensions */
    int hs[3], vs[3];        /* sampling factors */
} jpeg_image_t;

int  jpeg_decode(const uint8_t* data, size_t size, jpeg_image_t* img);
void jpeg_image_free(jpeg_image_t* img);

/* sample plane[c] at display coords (x,y): replicate-upsampled */
uint8_t jpeg_sample(const jpeg_image_t* img, int c, int x, int y);

#endif /* JPEG_H */
