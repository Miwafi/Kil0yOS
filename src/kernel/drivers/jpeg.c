/* Minimal baseline JPEG decoder for the desktop image viewer.
 *
 * Scope is deliberately narrow: the art assets and typical photos are
 * baseline SOF0/SOF1 huffman JPEGs with 1-3 components and up to 2x2
 * chroma subsampling. Anything else (progressive, arithmetic, CMYK) is
 * rejected up front rather than decoded wrongly.
 *
 * Output keeps the natural per-component planes (no upsampled RGB
 * intermediate) - the viewer downsamples to the screen anyway, so it
 * samples each plane at display coordinates and converts YCbCr->RGB on
 * the fly. This keeps peak memory at ~1 byte/pixel/component instead of
 * 3 bytes/pixel for a 1440x1920 photo.
 *
 * IDCT: separable integer DCT-III with a precomputed 12-bit cosine table
 * (int64 accumulators), +-1 lsb quality vs float at a fraction of the
 * code complexity of jidctint-style butterflies. */

#include "drivers/jpeg.h"
#include "mm/memory.h"
#include "lib/string.h"

#define JPEG_MAXCOMP 3

/* natural-order coefficients, marked as zero by the zigzag scan */
static const uint8_t jpeg_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* jpeg_idct_tab[v][x] = round(C(v)*cos((2x+1)v*pi/16)*8192), C(0)=1/sqrt(2) */
static const int16_t jpeg_idct_tab[8][8] = {
    { 5793, 5793, 5793, 5793, 5793, 5793, 5793, 5793 },
    { 8035, 6811, 4551, 1598, -1598, -4551, -6811, -8035 },
    { 7568, 3135, -3135, -7568, -7568, -3135, 3135, 7568 },
    { 6811, -1598, -8035, -4551, 4551, 8035, 1598, -6811 },
    { 5793, -5793, -5793, 5793, 5793, -5793, -5793, 5793 },
    { 4551, -8035, 1598, 6811, -6811, -1598, 8035, -4551 },
    { 3135, -7568, 7568, -3135, -3135, 7568, -7568, 3135 },
    { 1598, -4551, 6811, -8035, 8035, -6811, 4551, -1598 }
};

typedef struct {
    uint8_t bits[17];        /* bits[l] = number of codes of length l */
    uint8_t vals[256];
    int32_t mincode[17];
    int32_t maxcode[18];
    int32_t valptr[17];
} jhuff;

typedef struct {
    int id, hs, vs, tq;      /* component id, sampling, quant table */
    int dc_tbl, ac_tbl;
    int pred;                /* DC predictor */
    int bw, bh;              /* block dimensions (component space) */
    uint8_t* plane;
    int pw, ph;              /* plane dimensions */
    int plane_idx;           /* 0=Y 1=Cb 2=Cr */
} jcomp;

typedef struct {
    const uint8_t* d;
    size_t size;
    size_t pos;
    uint32_t bitbuf;
    int bitcnt;
    int marker;              /* marker seen inside the entropy stream */
    uint16_t qt[4][64];
    jhuff hdc[4], hac[4];
    jcomp comp[JPEG_MAXCOMP];
    int ncomp;
    int W, H, hmax, vmax;
    int mcux, mcuy;
    int restart_interval;
} jdec;

/* ---------- bit reader (FF00 unstuffing, marker capture) ---------- */

/* returns the 8 bits of the next byte with FF unstuffing applied,
 * -1 when a (non-RST-stuffed) marker starts */
static int jbit_byte(jdec* j) {
    if (j->pos >= j->size) return -1;
    uint8_t b = j->d[j->pos++];
    if (b != 0xFF) return b;
    /* marker or stuffed FF */
    while (b == 0xFF && j->pos < j->size) {
        uint8_t n = j->d[j->pos];
        if (n == 0x00) {           /* stuffed FF: literal 0xFF byte */
            j->pos++;
            return 0xFF;
        }
        if (n == 0xFF) { j->pos++; continue; }   /* fill byte */
        j->marker = n;             /* RSTn or a real marker: stop */
        return -1;
    }
    return -1;
}

/* next entropy bit, or -1 at marker/EOF */
static int jbit_bit(jdec* j) {
    if (j->bitcnt == 0) {
        int b = jbit_byte(j);
        if (b < 0) return -1;
        j->bitbuf = (uint32_t)b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1u);
}

/* ---------- huffman (ITU-T T.81 F.2.2.3/F.2.2.4) ---------- */

static void jhuff_derive(jhuff* h) {
    int k = 0, code = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        h->maxcode[l] = code - 1;
        k += h->bits[l];
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;   /* sentinel: never matches */
}

static int jhuff_decode(jdec* j, const jhuff* h) {
    int code = jbit_bit(j);
    if (code < 0) return -1;
    for (int l = 1; l <= 16; l++) {
        if (code <= h->maxcode[l]) {
            int idx = h->valptr[l] + code - h->mincode[l];
            if (idx < 0 || idx >= 256) return -1;
            return h->vals[idx];
        }
        code = (code << 1) | jbit_bit(j);
        if (code < 0) return -1;
    }
    return -1;
}

/* extend to signed; EXT_ERR is an impossible magnitude for n<=15 */
#define EXT_ERR (-0x40000000)
static int jextend(jdec* j, int n) {
    if (n == 0) return 0;
    int v = 0;
    for (int i = 0; i < n; i++) {
        int b = jbit_bit(j);
        if (b < 0) return EXT_ERR;
        v = (v << 1) | b;
    }
    if (v < (1 << (n - 1))) v += ((-1 << n) + 1);
    return v;
}

/* ---------- one 8x8 block: huffman -> dequant -> IDCT -> plane ---------- */

static uint8_t jclamp(int v) {
    return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

static void jidct8(const int32_t* in, uint8_t* out, int stride) {
    int32_t tmp[64];
    /* pass 1: rows of the coefficient block -> intermediate columns */
    for (int u = 0; u < 8; u++) {
        const int32_t* row = in + u * 8;
        int64_t acc[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        for (int v = 0; v < 8; v++) {
            int32_t f = row[v];
            if (f == 0) continue;
            const int16_t* t = jpeg_idct_tab[v];
            for (int x = 0; x < 8; x++) acc[x] += (int64_t)f * t[x];
        }
        for (int x = 0; x < 8; x++) tmp[u * 8 + x] = (int32_t)(acc[x] >> 13);
    }
    /* pass 2: columns -> samples; total scale 2^13*2^13 >> 13 >> 15 = /4 */
    for (int x = 0; x < 8; x++) {
        int64_t acc[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        for (int u = 0; u < 8; u++) {
            int32_t t = tmp[u * 8 + x];
            if (t == 0) continue;
            const int16_t* c = jpeg_idct_tab[u];
            for (int y = 0; y < 8; y++) acc[y] += (int64_t)t * c[y];
        }
        for (int y = 0; y < 8; y++) {
            out[y * stride] = jclamp((int)((acc[y] >> 15) + 128));
        }
    }
}

static int jblock(jdec* j, jcomp* c, int cbx, int cby) {
    int zz[64];
    for (int i = 0; i < 64; i++) zz[i] = 0;

    /* DC */
    int t = jhuff_decode(j, &j->hdc[c->dc_tbl]);
    if (t < 0 || t > 15) return -1;
    int diff = jextend(j, t);
    if (diff == EXT_ERR) return -1;
    c->pred += diff;
    zz[0] = c->pred;

    /* AC */
    for (int k = 1; k < 64;) {
        int rs = jhuff_decode(j, &j->hac[c->ac_tbl]);
        if (rs < 0) return -1;
        int r = rs >> 4, s = rs & 0x0F;
        if (s == 0) {
            if (r == 15) { k += 16; continue; }   /* ZRL */
            break;                                 /* EOB */
        }
        int v = jextend(j, s);
        if (v == EXT_ERR) return -1;
        k += r;
        if (k > 63) return -1;
        zz[jpeg_zigzag[k]] = v;
        k++;
    }

    /* dequant into natural order */
    int32_t blk[64];
    const uint16_t* q = j->qt[c->tq];
    for (int i = 0; i < 64; i++) blk[i] = zz[i] * q[i];

    /* IDCT into the component plane at (cbx*8, cby*8) */
    uint8_t* dst = c->plane + (size_t)cby * 8 * c->pw + (size_t)cbx * 8;
    jidct8(blk, dst, c->pw);
    return 0;
}

/* ---------- segments ---------- */

static int jwant(jdec* j, size_t n) {
    return (j->pos + n <= j->size) ? 0 : -1;
}

static int jparse_dqt(jdec* j) {
    if (jwant(j, 2) != 0) return -1;
    int len = (j->d[j->pos] << 8) | j->d[j->pos + 1];
    j->pos += 2;
    if (len < 2) return -1;
    len -= 2;
    while (len > 0) {
        if (jwant(j, 1) != 0) return -1;
        int pq = j->d[j->pos] >> 4, tq = j->d[j->pos] & 0x0F;
        j->pos++;
        if (tq > 3 || (pq != 0 && pq != 1)) return -1;
        int ent = pq ? 128 : 64;
        if (len < 1 + ent || jwant(j, (size_t)ent) != 0) return -1;
        for (int i = 0; i < 64; i++) {
            if (pq) {
                j->qt[tq][jpeg_zigzag[i]] =
                    (uint16_t)((j->d[j->pos] << 8) | j->d[j->pos + 1]);
                j->pos += 2;
            } else {
                j->qt[tq][jpeg_zigzag[i]] = j->d[j->pos++];
            }
        }
        len -= 1 + ent;
    }
    return 0;
}

static int jparse_dht(jdec* j) {
    if (jwant(j, 2) != 0) return -1;
    int len = (j->d[j->pos] << 8) | j->d[j->pos + 1];
    j->pos += 2;
    if (len < 2) return -1;
    len -= 2;
    while (len > 0) {
        if (len < 17 || jwant(j, 17) != 0) return -1;
        int tc = j->d[j->pos] >> 4, th = j->d[j->pos] & 0x0F;
        j->pos++;
        if (tc > 1 || th > 3) return -1;
        jhuff* h = tc ? &j->hac[th] : &j->hdc[th];
        int total = 0;
        for (int l = 1; l <= 16; l++) {
            h->bits[l] = j->d[j->pos + l - 1];
            total += h->bits[l];
        }
        j->pos += 16;
        if (total > 256 || total > len - 17) return -1;
        if (jwant(j, (size_t)total) != 0) return -1;
        for (int i = 0; i < total; i++) h->vals[i] = j->d[j->pos + i];
        j->pos += (size_t)total;
        len -= 17 + total;
        jhuff_derive(h);
    }
    return 0;
}

/* SOF0/SOF1: baseline/extended sequential huffman */
static int jparse_sof(jdec* j) {
    if (jwant(j, 2) != 0) return -1;
    int len = (j->d[j->pos] << 8) | j->d[j->pos + 1];
    j->pos += 2;
    if (len < 6 || jwant(j, (size_t)len - 2) != 0) return -1;
    if (j->d[j->pos] != 8) return -1;              /* 8-bit only */
    j->H = (j->d[j->pos + 1] << 8) | j->d[j->pos + 2];
    j->W = (j->d[j->pos + 3] << 8) | j->d[j->pos + 4];
    j->ncomp = j->d[j->pos + 5];
    j->pos += 6;
    if (j->W <= 0 || j->H <= 0 || j->W > 8192 || j->H > 8192) return -1;
    if (j->ncomp < 1 || j->ncomp > JPEG_MAXCOMP) return -1;
    if (len != 8 + j->ncomp * 3) return -1;
    j->hmax = 1;
    j->vmax = 1;
    for (int i = 0; i < j->ncomp; i++) {
        jcomp* c = &j->comp[i];
        c->id = j->d[j->pos];
        int s = j->d[j->pos + 1];
        c->hs = s >> 4;
        c->vs = s & 0x0F;
        c->tq = j->d[j->pos + 2];
        c->pred = 0;
        c->plane_idx = (j->ncomp == 3) ? i : 0;
        j->pos += 3;
        if (c->hs < 1 || c->hs > 2 || c->vs < 1 || c->vs > 2) return -1;
        if (c->tq > 3) return -1;
        if (c->hs > j->hmax) j->hmax = c->hs;
        if (c->vs > j->vmax) j->vmax = c->vs;
    }
    if (j->ncomp == 3 && (j->comp[0].hs * j->comp[0].vs <
                          j->comp[1].hs * j->comp[1].vs ||
                          j->comp[0].hs * j->comp[0].vs <
                          j->comp[2].hs * j->comp[2].vs)) {
        return -1;   /* luma must be the most-sampled component */
    }
    return 0;
}

static int jparse_sos(jdec* j) {
    if (jwant(j, 2) != 0) return -1;
    int len = (j->d[j->pos] << 8) | j->d[j->pos + 1];
    j->pos += 2;
    if (len < 4 || jwant(j, (size_t)len - 2) != 0) return -1;
    int ns = j->d[j->pos];
    if (ns != j->ncomp) return -1;                 /* single interleaved scan */
    j->pos++;
    if (len != 1 + ns * 2 + 3) return -1;
    for (int i = 0; i < ns; i++) {
        int cid = j->d[j->pos];
        int tabs = j->d[j->pos + 1];
        j->pos += 2;
        jcomp* c = NULL;
        for (int k = 0; k < j->ncomp; k++) {
            if (j->comp[k].id == cid) { c = &j->comp[k]; break; }
        }
        if (c == NULL) return -1;
        c->dc_tbl = tabs >> 4;
        c->ac_tbl = tabs & 0x0F;
        if (c->dc_tbl > 3 || c->ac_tbl > 3) return -1;
    }
    if (j->d[j->pos] != 0 || j->d[j->pos + 1] != 63 ||
        j->d[j->pos + 2] != 0) {
        return -1;   /* spectral selection must be full (Ss=0 Se=63) */
    }
    j->pos += 3;
    return 0;
}

/* ---------- planes / MCU scan ---------- */

static int jalloc_planes(jdec* j) {
    for (int i = 0; i < j->ncomp; i++) {
        jcomp* c = &j->comp[i];
        c->bw = (j->W * c->hs + j->hmax * 8 - 1) / (j->hmax * 8);
        c->bh = (j->H * c->vs + j->vmax * 8 - 1) / (j->vmax * 8);
        c->pw = c->bw * 8;
        c->ph = c->bh * 8;
        c->plane = (uint8_t*)kmalloc((size_t)c->pw * c->ph);
        if (c->plane == NULL) return -1;
        memset(c->plane, 128, (size_t)c->pw * c->ph);
    }
    return 0;
}

/* byte-align; consume one expected RSTn if present, reset predictors */
static int jsync_restart(jdec* j, int* rst_no) {
    j->bitcnt = 0;
    if (j->marker >= 0xD0 && j->marker <= 0xD7) {
        if (j->marker != 0xD0 + *rst_no) return -1;
        j->marker = 0;
    } else if (j->pos + 1 < j->size && j->d[j->pos] == 0xFF &&
               j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) {
        if (j->d[j->pos + 1] != 0xD0 + *rst_no) return -1;
        j->pos += 2;
    }
    /* a missing RST is tolerated (lenient): just reset the predictors */
    (*rst_no) = (*rst_no + 1) & 7;
    for (int i = 0; i < j->ncomp; i++) j->comp[i].pred = 0;
    return 0;
}

static int jscan(jdec* j) {
    int mcus = j->mcux * j->mcuy;
    int rst_no = 0;
    for (int m = 0; m < mcus; m++) {
        if (j->restart_interval && m > 0 &&
            (m % j->restart_interval) == 0) {
            if (jsync_restart(j, &rst_no) != 0) return -1;
        }
        if (j->marker) return -1;   /* unexpected marker mid-stream */
        for (int i = 0; i < j->ncomp; i++) {
            jcomp* c = &j->comp[i];
            int mx = m % j->mcux, my = m / j->mcux;
            for (int by = 0; by < c->vs; by++) {
                for (int bx = 0; bx < c->hs; bx++) {
                    int cbx = mx * c->hs + bx;
                    int cby = my * c->vs + by;
                    if (cbx >= c->bw || cby >= c->bh) continue;
                    if (jblock(j, c, cbx, cby) != 0) return -1;
                }
            }
        }
    }
    return 0;
}

int jpeg_decode(const uint8_t* data, size_t size, jpeg_image_t* img) {
    jdec j;
    memset(&j, 0, sizeof(j));
    memset(img, 0, sizeof(*img));
    if (data == NULL || size < 4) return -1;
    j.d = data;
    j.size = size;

    if (data[0] != 0xFF || data[1] != 0xD8) return -1;   /* SOI */
    j.pos = 2;
    int sof_seen = 0, sos_seen = 0;
    while (j.pos + 1 < size && !sos_seen) {
        if (j.d[j.pos] != 0xFF) return -1;
        uint8_t m = j.d[j.pos + 1];
        if (m == 0xFF) { j.pos++; continue; }            /* fill byte */
        j.pos += 2;
        if (m == 0xD9) break;                            /* EOI */
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) continue;  /* standalone */
        if (j.pos + 2 > size) return -1;
        int len = (j.d[j.pos] << 8) | j.d[j.pos + 1];
        if (len < 2 || j.pos + (size_t)len > size) return -1;

        if (m == 0xC0 || m == 0xC1) {
            if (sof_seen) return -1;
            if (jparse_sof(&j) != 0) return -1;
            sof_seen = 1;
        } else if (m == 0xC2 || (m >= 0xC3 && m <= 0xCF && m != 0xC4 &&
                                 m != 0xC8 && m != 0xCC)) {
            return -1;   /* progressive / arithmetic / lossless */
        } else if (m == 0xC4) {
            if (jparse_dht(&j) != 0) return -1;
        } else if (m == 0xDB) {
            if (jparse_dqt(&j) != 0) return -1;
        } else if (m == 0xDD) {
            if (jwant(&j, 4) != 0) return -1;
            j.restart_interval = (j.d[j.pos + 2] << 8) | j.d[j.pos + 3];
            j.pos += (size_t)len;
        } else if (m == 0xDA) {
            if (!sof_seen) return -1;
            if (jparse_sos(&j) != 0) return -1;
            sos_seen = 1;
        } else {
            j.pos += (size_t)len;   /* APPn / COM / anything else: skip */
        }
    }
    if (!sos_seen) return -1;

    j.mcux = (j.W + j.hmax * 8 - 1) / (j.hmax * 8);
    j.mcuy = (j.H + j.vmax * 8 - 1) / (j.vmax * 8);
    if (jalloc_planes(&j) != 0) {
        jpeg_image_free(img);   /* nothing allocated yet, keeps it symmetric */
        for (int i = 0; i < JPEG_MAXCOMP; i++) {
            if (j.comp[i].plane != NULL) kfree(j.comp[i].plane);
        }
        return -1;
    }
    int rc = jscan(&j);

    if (rc == 0) {
        img->W = j.W;
        img->H = j.H;
        img->ncomp = j.ncomp;
        for (int i = 0; i < j.ncomp; i++) {
            jcomp* c = &j.comp[i];
            img->plane[c->plane_idx] = c->plane;
            img->pw[c->plane_idx] = c->pw;
            img->ph[c->plane_idx] = c->ph;
            img->hs[c->plane_idx] = c->hs;
            img->vs[c->plane_idx] = c->vs;
            c->plane = NULL;   /* ownership moved */
        }
        return 0;
    }
    for (int i = 0; i < JPEG_MAXCOMP; i++) {
        if (j.comp[i].plane != NULL) kfree(j.comp[i].plane);
    }
    return -1;
}

void jpeg_image_free(jpeg_image_t* img) {
    for (int i = 0; i < 3; i++) {
        if (img->plane[i] != NULL) {
            kfree(img->plane[i]);
            img->plane[i] = NULL;
        }
    }
    memset(img, 0, sizeof(*img));
}

uint8_t jpeg_sample(const jpeg_image_t* img, int c, int x, int y) {
    const uint8_t* p = img->plane[c];
    if (p == NULL) return 128;
    int sx = x * img->hs[c] / 2;   /* hmax/vmax == 2 by construction */
    int sy = y * img->vs[c] / 2;
    if (sx >= img->pw[c]) sx = img->pw[c] - 1;
    if (sy >= img->ph[c]) sy = img->ph[c] - 1;
    return p[(size_t)sy * img->pw[c] + sx];
}
