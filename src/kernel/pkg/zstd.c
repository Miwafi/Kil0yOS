/* Minimal self-contained zstd decoder (RFC 8878 subset) for .deb
 * data.tar.zst members.
 *
 * Supports:
 *   - regular frames + skippable frames, multi-frame streams
 *   - blocks: Raw / RLE / Compressed
 *   - literals: Raw / RLE / Huffman(1 or 4 streams) / Treeless(repeat)
 *   - sequences: Predefined / RLE / FSE / Repeat tables
 *   - repeat offsets, cross-block entropy reuse
 *   - optional XXH64 content checksum
 * Not supported: dictionaries (dpkg-deb never emits them), legacy frames.
 *
 * Entropy coding follows the reference implementation exactly
 * (verified against zstd 1.5.5): 64-bit reverse bitreader, FSE decode
 * tables (state spread rule), Huffman X1 tables.
 *
 * Host test mode: compile with -DZSTD_KTEST to use malloc/printf. */

#include "pkg/zstd.h"
#include "lib/string.h"
#include "mm/memory.h"
#include "drivers/vga.h"

#ifdef ZSTD_KTEST
#include <stdio.h>
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...) do {} while (0)
#endif

/* ------------------------------------------------------------------ */
/* constants (zstd 1.5.5 reference values)                            */

#define ZSTD_MAGICNUMBER 0xFD2FB528U
#define ZSTD_MAGIC_SKIPPABLE_START 0x184D2A50U
#define ZSTD_MAGIC_SKIPPABLE_MASK  0xFFFFFFF0U

#define MAXLL 35
#define MaxML 52
#define MaxOff 31
#define LLFSELog 9
#define MLFSELog 9
#define OffFSELog 8
#define FSE_MIN_TABLELOG 5
#define FSE_TABLELOG_ABSOLUTE_MAX 15

#define ZSTD_BLOCKSIZE_MAX (1 << 17)
#define ZSTD_WINDOWLOG_ABSOLUTEMIN 10
#define ZSTD_WINDOWLOG_LIMIT_DEFAULT 27
#define LONGNBSEQ 0x7F00
#define MIN_LITERALS_FOR_4_STREAMS 6

#define HUF_TABLELOG_MAX 12

static const uint8_t LL_bits_tbl[MAXLL + 1] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16
};
static const uint32_t LL_base_tbl[MAXLL + 1] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 0x80, 0x100, 0x200,
    0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000
};
static const uint8_t ML_bits_tbl[MaxML + 1] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16
};
static const uint32_t ML_base_tbl[MaxML + 1] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 0x83, 0x103, 0x203,
    0x403, 0x803, 0x1003, 0x2003, 0x4003, 0x8003, 0x10003
};
static const uint32_t OF_base_tbl[MaxOff + 1] = {
    0, 1, 1, 5, 0xD, 0x1D, 0x3D, 0x7D, 0xFD, 0x1FD, 0x3FD, 0x7FD,
    0xFFD, 0x1FFD, 0x3FFD, 0x7FFD, 0xFFFD, 0x1FFFD, 0x3FFFD, 0x7FFFD,
    0xFFFFD, 0x1FFFFD, 0x3FFFFD, 0x7FFFFD, 0xFFFFFD, 0x1FFFFFD,
    0x3FFFFFD, 0x7FFFFFD, 0xFFFFFFD, 0x1FFFFFFD, 0x3FFFFFFD,
    0x7FFFFFFD
};
static const uint8_t OF_bits_tbl[MaxOff + 1] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
};
static const int16_t LL_defaultNorm[MAXLL + 1] = {
    4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1,
    -1, -1, -1, -1
};
static const int16_t ML_defaultNorm[MaxML + 1] = {
    1, 4, 3, 2, 2, 2, 2, 2,
    2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, -1, -1,
    -1, -1, -1, -1, -1
};
static const int16_t OF_defaultNorm[29] = {
    1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1
};
#define OF_defaultNormMax 28
/* tableLog of the PREDEFINED distributions (the *FSELog constants above
 * are only capacity limits) - reference LL/ML/OF_DEFAULTNORMLOG */
#define LL_DEFAULTNORMLOG 6
#define ML_DEFAULTNORMLOG 6
#define OF_DEFAULTNORMLOG 5

/* ------------------------------------------------------------------ */
/* small helpers                                                      */

static uint32_t z_highbit32(uint32_t v) {
    return 31 - __builtin_clz(v);
}
static uint32_t z_ctz32(uint32_t v) {
    return (uint32_t)__builtin_ctz(v);
}
static uint32_t rd16(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
static uint32_t rd24(const uint8_t* p) {
    return rd16(p) | ((uint32_t)p[2] << 16);
}
static uint32_t rd32(const uint8_t* p) {
    return rd16(p) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ------------------------------------------------------------------ */
/* reverse bitreader (64-bit)                                         */

typedef struct {
    const uint8_t* start;
    const uint8_t* ptr;
    const uint8_t* limitPtr;
    uint64_t bitContainer;
    uint32_t bitsConsumed;
} bitr_t;

typedef enum {
    BR_UNFINISHED = 0,
    BR_ENDOFBUFFER = 1,
    BR_COMPLETED = 2,
    BR_OVERFLOW = 3
} br_status;

static int br_init(bitr_t* b, const uint8_t* src, size_t size) {
    if (size == 0) { memset(b, 0, sizeof(*b)); return -1; }
    b->start = src;
    b->limitPtr = src + sizeof(b->bitContainer);
    if (size >= sizeof(b->bitContainer)) {
        b->ptr = src + size - sizeof(b->bitContainer);
        b->bitContainer = rd64(b->ptr);
        uint8_t last = src[size - 1];
        if (last == 0) { return -1; }      /* endMark not present */
        b->bitsConsumed = 8 - z_highbit32(last);
    } else {
        b->ptr = b->start;
        b->bitContainer = 0;
        /* little-endian container: byte i sits at bit 8*i */
        for (int i = (int)size - 1; i >= 0; i--)
            b->bitContainer |= (uint64_t)src[i] << (8 * i);
        uint8_t last = src[size - 1];
        if (last == 0) { return -1; }
        b->bitsConsumed = 8 - z_highbit32(last);
        b->bitsConsumed += (uint32_t)(sizeof(b->bitContainer) - size) * 8;
    }
    return 0;
}

static size_t br_look(const bitr_t* b, uint32_t nb) {
    /* reference BIT_lookBits form: safe even when bitsConsumed+nb > 64 */
    return (size_t)(((b->bitContainer << (b->bitsConsumed & 63)) >> 1) >>
                    ((63 - nb) & 63));
}
static void br_skip(bitr_t* b, uint32_t nb) {
    b->bitsConsumed += nb;
}
static size_t br_read(bitr_t* b, uint32_t nb) {
    size_t v = br_look(b, nb);
    b->bitsConsumed += nb;
    return v;
}

static br_status br_reload(bitr_t* b) {
    if (b->bitsConsumed > 64) return BR_OVERFLOW;
    if (b->ptr >= b->limitPtr) {
        b->ptr -= b->bitsConsumed >> 3;
        b->bitsConsumed &= 7;
        b->bitContainer = rd64(b->ptr);
        return BR_UNFINISHED;
    }
    if (b->ptr == b->start) {
        if (b->bitsConsumed < 64) return BR_ENDOFBUFFER;
        return BR_COMPLETED;
    }
    {
        uint32_t nbBytes = b->bitsConsumed >> 3;
        br_status res = BR_UNFINISHED;
        if (b->ptr - nbBytes < b->start) {
            nbBytes = (uint32_t)(b->ptr - b->start);
            res = BR_ENDOFBUFFER;
        }
        b->ptr -= nbBytes;
        b->bitsConsumed -= nbBytes * 8;
        b->bitContainer = rd64(b->ptr);
        return res;
    }
}
static int br_end(const bitr_t* b) {
    return (b->ptr == b->start) && (b->bitsConsumed == 64);
}

/* ------------------------------------------------------------------ */
/* FSE decode tables                                                  */

/* generic 4-byte FSE symbol table (used for Huffman weights) */
typedef struct {
    uint16_t nextState;
    uint8_t symbol;
    uint8_t nbBits;
} fse_sym_t;

/* zstd sequence symbol table, 8 bytes (must match reference layout) */
typedef struct {
    uint16_t nextState;
    uint8_t nbAdditionalBits;
    uint8_t nbBits;
    uint32_t baseValue;
} seq_sym_t;

#define FSE_MAX_SYMS 256
#define FSE_MAX_TABLE (1 << FSE_TABLELOG_ABSOLUTE_MAX)

static uint32_t fse_step(uint32_t tableSize) {
    return (tableSize >> 1) + (tableSize >> 3) + 3;
}

/* FSE_readNCount: returns bytes consumed or -1 */
static int fse_read_ncount(int16_t* norm, unsigned* maxSVPtr,
                           unsigned* tableLogPtr,
                           const uint8_t* src, size_t srcSize) {
    const uint8_t* ip = src;
    const uint8_t* iend = src + srcSize;
    int nbBits, remaining, threshold, bitCount, previous0 = 0;
    uint32_t bitStream;
    unsigned charnum = 0;
    unsigned maxSV1 = *maxSVPtr + 1;

    if (srcSize < 4) return -1;
    memset(norm, 0, maxSV1 * sizeof(norm[0]));
    bitStream = rd32(ip);
    nbBits = (int)(bitStream & 0xF) + FSE_MIN_TABLELOG;
    if (nbBits > FSE_TABLELOG_ABSOLUTE_MAX) return -1;
    bitStream >>= 4;
    bitCount = 4;
    *tableLogPtr = (unsigned)nbBits;
    remaining = (1 << nbBits) + 1;
    threshold = 1 << nbBits;
    nbBits++;

    for (;;) {
        if (previous0) {
            /* 2-bit repeat codes, 0b11 = another repeat */
            int repeats = (int)(z_ctz32(~bitStream | 0x80000000U) >> 1);
            while (repeats >= 12) {
                charnum += 3 * 12;
                if (ip <= iend - 7) {
                    ip += 3;
                } else {
                    bitCount -= (int)(8 * (iend - 7 - ip));
                    bitCount &= 31;
                    ip = iend - 4;
                }
                bitStream = rd32(ip) >> bitCount;
                repeats = (int)(z_ctz32(~bitStream | 0x80000000U) >> 1);
            }
            charnum += 3 * repeats;
            bitStream >>= 2 * repeats;
            bitCount += 2 * repeats;
            charnum += bitStream & 3;
            bitCount += 2;
            if (charnum >= maxSV1) break;
            if ((ip <= iend - 7) || (ip + (bitCount >> 3) <= iend - 4)) {
                ip += bitCount >> 3;
                bitCount &= 7;
            } else {
                bitCount -= (int)(8 * (iend - 4 - ip));
                bitCount &= 31;
                ip = iend - 4;
            }
            bitStream = rd32(ip) >> bitCount;
        }
        {
            int max = (2 * threshold - 1) - remaining;
            int count;
            if ((int)(bitStream & (uint32_t)(threshold - 1)) < max) {
                count = (int)(bitStream & (uint32_t)(threshold - 1));
                bitCount += nbBits - 1;
            } else {
                count = (int)(bitStream & (uint32_t)(2 * threshold - 1));
                if (count >= threshold) count -= max;
                bitCount += nbBits;
            }
            count--;
            remaining += (count >= 0) ? -count : count; /* -1 means "low prob" */
            norm[charnum++] = (int16_t)count;
            previous0 = (count == 0);
            if (remaining < threshold) {
                if (remaining <= 1) break;
                nbBits = (int)z_highbit32((uint32_t)remaining) + 1;
                threshold = 1 << (nbBits - 1);
            }
            if (charnum >= maxSV1) break;
            if ((ip <= iend - 7) || (ip + (bitCount >> 3) <= iend - 4)) {
                ip += bitCount >> 3;
                bitCount &= 7;
            } else {
                bitCount -= (int)(8 * (iend - 4 - ip));
                bitCount &= 31;
                ip = iend - 4;
            }
            bitStream = rd32(ip) >> bitCount;
        }
    }
    if (remaining != 1) return -1;
    if (charnum > maxSV1) return -1;
    if (bitCount > 32) return -1;
    *maxSVPtr = charnum - 1;
    ip += (bitCount + 7) >> 3;
    if (ip > iend) return -1;
    return (int)(ip - src);
}

/* build generic FSE decode table (symbol-only) */
static int fse_build_dtable(fse_sym_t* dt, const int16_t* norm,
                            unsigned maxSV, unsigned tableLog) {
    uint16_t symbolNext[FSE_MAX_SYMS];
    uint32_t tableSize = 1u << tableLog;
    uint32_t highThreshold = tableSize - 1;
    unsigned maxSV1 = maxSV + 1;

    for (unsigned s = 0; s < maxSV1; s++) {
        if (norm[s] == -1) {
            dt[highThreshold--].symbol = (uint8_t)s;
            symbolNext[s] = 1;
        } else {
            if (norm[s] < 0) return -1;
            symbolNext[s] = (uint16_t)norm[s];
        }
    }
    {
        uint32_t position = 0;
        uint32_t step = fse_step(tableSize);
        for (unsigned s = 0; s < maxSV1; s++) {
            for (int i = 0; i < norm[s]; i++) {
                dt[position].symbol = (uint8_t)s;
                position = (position + step) & (tableSize - 1);
                while (position > highThreshold)
                    position = (position + step) & (tableSize - 1);
            }
        }
        if (position != 0) return -1;
    }
    for (uint32_t u = 0; u < tableSize; u++) {
        uint32_t sym = dt[u].symbol;
        uint32_t nextState = symbolNext[sym]++;
        dt[u].nbBits = (uint8_t)(tableLog - z_highbit32(nextState));
        dt[u].nextState = (uint16_t)((nextState << dt[u].nbBits) - tableSize);
    }
    return 0;
}

/* generic FSE decompress (single stream, TWO interleaved states, exactly
 * like reference FSE_decompress_usingDTable_generic) - used for Huffman
 * weights. The generic FSE format always interleaves 2 states, so a
 * single-state decoder produces wrong weights. */
static int fse_decompress(uint8_t* out, size_t outCap, size_t* outSize,
                          const uint8_t* src, size_t srcSize,
                          fse_sym_t* dt /* workspace: 1<<11 entries */) {
    int16_t norm[FSE_MAX_SYMS];
    unsigned maxSV = 255, tableLog;
    DBG("[z] fse: enter srcSize=%zu outCap=%zu\n", srcSize, outCap);
    int hdr = fse_read_ncount(norm, &maxSV, &tableLog, src, srcSize);
    if (hdr < 0) { DBG("[z] fse: ncount failed\n"); return -1; }
    if (tableLog > 11) { DBG("[z] fse: log %u > 11\n", tableLog); return -1; }
    if (fse_build_dtable(dt, norm, maxSV, tableLog) != 0) {
        DBG("[z] fse: build failed\n"); return -1;
    }

    const uint8_t* bs = src + hdr;
    size_t bsSize = srcSize - (size_t)hdr;
    if (bsSize == 0) { DBG("[z] fse: empty bitstream\n"); return -1; }
    bitr_t b;
    if (br_init(&b, bs, bsSize) != 0) { DBG("[z] fse: br_init\n"); return -1; }
    if (br_reload(&b) == BR_OVERFLOW) { DBG("[z] fse: overflow init\n"); return -1; }

    /* init both states: state1 reads first, then state2 */
    uint32_t state1 = (uint32_t)br_read(&b, tableLog);
    br_reload(&b);
    uint32_t state2 = (uint32_t)br_read(&b, tableLog);
    br_reload(&b);

    /* decode: symbols alternate state1/state2; on the first transition
     * read that overruns the stream, one more symbol is emitted from the
     * other state and decoding stops (reference tail semantics) */
    size_t n = 0;
    for (;;) {
        if (n + 2 > outCap) { DBG("[z] fse: outCap\n"); return -1; }
        fse_sym_t* e1 = &dt[state1];
        out[n++] = e1->symbol;
        state1 = e1->nextState + (uint32_t)br_read(&b, e1->nbBits);
        if (br_reload(&b) == BR_OVERFLOW) {
            out[n++] = dt[state2].symbol;
            break;
        }
        fse_sym_t* e2 = &dt[state2];
        out[n++] = e2->symbol;
        state2 = e2->nextState + (uint32_t)br_read(&b, e2->nbBits);
        if (br_reload(&b) == BR_OVERFLOW) {
            out[n++] = dt[state1].symbol;
            break;
        }
    }
    if (n == 0) { DBG("[z] fse: n==0\n"); return -1; }
    *outSize = n;
    return (int)srcSize; /* total consumed = srcSize */
}

/* ------------------------------------------------------------------ */
/* zstd sequence FSE tables (with baseValue / additional bits).
 * Layout matches reference ZSTD_seqSymbol: dt[0] is the 8-byte header
 * (aliased {fastMode, tableLog} => tableLog lives in dt[0].baseValue),
 * actual table entries live at dt+1. */
#define SEQ_TABLE_LL (1 + (1 << LLFSELog))
#define SEQ_TABLE_OF (1 + (1 << OffFSELog))
#define SEQ_TABLE_ML (1 + (1 << MLFSELog))

static int build_seq_fse(seq_sym_t* dt, const int16_t* norm,
                         unsigned maxSV, unsigned tableLog,
                         const uint32_t* baseValue,
                         const uint8_t* nbAddBits) {
    uint16_t symbolNext[MaxML + 1];
    uint32_t tableSize = 1u << tableLog;
    uint32_t highThreshold = tableSize - 1;
    unsigned maxSV1 = maxSV + 1;
    seq_sym_t* tab = dt + 1;

    for (unsigned s = 0; s < maxSV1; s++) {
        if (norm[s] == -1) {
            tab[highThreshold--].baseValue = s;
            symbolNext[s] = 1;
        } else {
            symbolNext[s] = (uint16_t)norm[s];
        }
    }
    {
        uint32_t position = 0;
        uint32_t step = fse_step(tableSize);
        for (unsigned s = 0; s < maxSV1; s++) {
            for (int i = 0; i < norm[s]; i++) {
                tab[position].baseValue = s;
                position = (position + step) & (tableSize - 1);
                while (position > highThreshold)
                    position = (position + step) & (tableSize - 1);
            }
        }
        if (position != 0) return -1;
    }
    for (uint32_t u = 0; u < tableSize; u++) {
        uint32_t sym = tab[u].baseValue;
        uint32_t nextState = symbolNext[sym]++;
        tab[u].nbBits = (uint8_t)(tableLog - z_highbit32(nextState));
        tab[u].nextState = (uint16_t)((nextState << tab[u].nbBits) - tableSize);
        tab[u].nbAdditionalBits = nbAddBits[sym];
        tab[u].baseValue = baseValue[sym];
    }
    dt[0].baseValue = tableLog;   /* header cell */
    return 0;
}

static void build_seq_rle(seq_sym_t* dt, uint32_t baseValue, uint8_t nbAddBits) {
    seq_sym_t* tab = dt + 1;
    tab[0].nbBits = 0;
    tab[0].nextState = 0;
    tab[0].nbAdditionalBits = nbAddBits;
    tab[0].baseValue = baseValue;
    dt[0].baseValue = 0;          /* header cell: tableLog 0 */
}

static int build_seq_default(seq_sym_t* dt, unsigned maxSV, unsigned tableLog,
                             const int16_t* norm, const uint32_t* baseValue,
                             const uint8_t* nbAddBits) {
    return build_seq_fse(dt, norm, maxSV, tableLog, baseValue, nbAddBits);
}

/* ------------------------------------------------------------------ */
/* Huffman X1                                                         */

typedef struct {
    uint8_t byte;
    uint8_t nbBits;
} huf_elt_t;

typedef struct {
    /* persistent across blocks (repeat tables) */
    seq_sym_t llTable[SEQ_TABLE_LL];
    seq_sym_t ofTable[SEQ_TABLE_OF];
    seq_sym_t mlTable[SEQ_TABLE_ML];
    int fseEntropy;      /* repeat tables valid */
    int litEntropy;      /* huffman repeat valid */
    uint32_t rep[3];

    huf_elt_t hufTable[1 << HUF_TABLELOG_MAX];
    uint32_t hufLog;

    /* scratch */
    int16_t norm[FSE_MAX_SYMS];
    fse_sym_t wDT[1 << 11];
    size_t litSize;
    uint8_t litBuffer[ZSTD_BLOCKSIZE_MAX + 32];
} zstd_dctx;

static int huf_read_stats(uint8_t* hw, unsigned hwSize,
                          uint32_t* rankStats, unsigned* nbSymPtr,
                          unsigned* tableLogPtr,
                          const uint8_t* src, size_t srcSize,
                          zstd_dctx* ctx) {
    if (srcSize == 0) { DBG("[z] huf: stats srcSize 0\n"); return -1; }
    unsigned iSize = src[0];
    size_t oSize;
    DBG("[z] huf: stats hdr byte %02x srcSize %zu\n", iSize, srcSize);

    if (iSize >= 128) { /* direct weights, 4-bit packing */
        oSize = iSize - 127;
        iSize = (unsigned)((oSize + 1) / 2);
        if ((size_t)iSize + 1 > srcSize) { DBG("[z] huf: direct short\n"); return -1; }
        if (oSize >= hwSize) { DBG("[z] huf: direct oSize\n"); return -1; }
        const uint8_t* p = src + 1;
        for (size_t n = 0; n < oSize; n += 2) {
            hw[n] = p[n / 2] >> 4;
            hw[n + 1] = p[n / 2] & 15;
        }
    } else { /* FSE compressed weights */
        if ((size_t)iSize + 1 > srcSize) { DBG("[z] huf: fse hdr short\n"); return -1; }
        size_t wSize = 0;
        /* fse_decompress returns consumed bytes (>=0) on success */
        if (fse_decompress(hw, hwSize - 1, &wSize, src + 1, iSize,
                           ctx->wDT) < 0) {
            DBG("[z] huf: fse weights failed\n");
            return -1;
        }
        oSize = wSize;
        DBG("[z] huf: fse weights ok, oSize=%zu\n", oSize);
    }

    memset(rankStats, 0, (HUF_TABLELOG_MAX + 1) * sizeof(uint32_t));
    uint32_t weightTotal = 0;
    for (size_t n = 0; n < oSize; n++) {
        if (hw[n] > HUF_TABLELOG_MAX) {
            DBG("[z] huf: weight %u > 12 at n=%zu\n", hw[n], n);
            return -1;
        }
        rankStats[hw[n]]++;
        weightTotal += (1u << hw[n]) >> 1;
    }
    if (weightTotal == 0) { DBG("[z] huf: weightTotal 0\n"); return -1; }

    uint32_t tableLog = z_highbit32(weightTotal) + 1;
    if (tableLog > HUF_TABLELOG_MAX) {
        DBG("[z] huf: tableLog %u > max\n", tableLog);
        return -1;
    }
    uint32_t total = 1u << tableLog;
    uint32_t rest = total - weightTotal;
    uint32_t verif = 1u << z_highbit32(rest);
    if (verif != rest) {
        DBG("[z] huf: rest %u not pow2 (total=%u wt=%u)\n", rest, total, weightTotal);
        return -1;
    }      /* last weight must be a power of 2 */
    uint8_t lastWeight = (uint8_t)(z_highbit32(rest) + 1);
    hw[oSize] = lastWeight;
    rankStats[lastWeight]++;

    if ((rankStats[1] < 2) || (rankStats[1] & 1)) {
        DBG("[z] huf: rank1 %u\n", rankStats[1]);
        return -1;
    }

    *nbSymPtr = (unsigned)(oSize + 1);
    *tableLogPtr = tableLog;
    return (int)iSize + 1;
}

static int huf_read_dtable(zstd_dctx* ctx, const uint8_t* src, size_t srcSize) {
    uint8_t* hw = (uint8_t*)ctx->norm; /* reuse: norm is int16_t[256]=512B */
    uint32_t rankStats[HUF_TABLELOG_MAX + 1];
    unsigned nbSymbols = 0, tableLog = 0;
    int iSize = huf_read_stats(hw, 256, rankStats, &nbSymbols, &tableLog,
                               src, srcSize, ctx);
    if (iSize < 0) { DBG("[z] huf: read_stats failed\n"); return -1; }

    /* symbols ordered by weight; weight-0 symbols occupy the front of
     * symbols[] (like the reference) but never enter the table */
    uint32_t rankStart[HUF_TABLELOG_MAX + 2];
    uint8_t symbols[256];
    {
        uint32_t nextRankStart = 0;
        for (unsigned n = 0; n <= tableLog; n++) {
            rankStart[n] = nextRankStart;
            nextRankStart += rankStats[n];
        }
        for (unsigned n = 0; n < nbSymbols; n++) {
            symbols[rankStart[hw[n]]++] = (uint8_t)n;
        }
        /* fill table: weight w covers 2^(w-1) entries of nbBits=log+1-w */
        huf_elt_t* dt = ctx->hufTable;
        uint32_t symbolIdx = rankStats[0];   /* skip weight-0 symbols */
        uint32_t uStart = 0;
        for (unsigned w = 1; w <= tableLog; w++) {
            uint32_t count = rankStats[w];
            uint32_t length = (1u << w) >> 1;
            uint8_t nbBits = (uint8_t)(tableLog + 1 - w);
            for (uint32_t s = 0; s < count; s++) {
                huf_elt_t D;
                D.byte = symbols[symbolIdx + s];
                D.nbBits = nbBits;
                for (uint32_t u = 0; u < length; u++)
                    dt[uStart + u] = D;
                uStart += length;
            }
            symbolIdx += count;
        }
        if (uStart != (1u << tableLog)) {
            DBG("[z] huf: uStart %u != %u\n", uStart, 1u << tableLog);
            return -1;
        }
        ctx->hufLog = tableLog;
    }
    return iSize;
}

static uint8_t huf_decode_sym(bitr_t* b, const huf_elt_t* dt, uint32_t dtLog) {
    size_t val = br_look(b, dtLog);
    uint8_t c = dt[val].byte;
    br_skip(b, dt[val].nbBits);
    return c;
}

static void huf_decode_stream(uint8_t* p, bitr_t* b, const uint8_t* pEnd,
                              const huf_elt_t* dt, uint32_t dtLog) {
    while (p < pEnd) {
        *p++ = huf_decode_sym(b, dt, dtLog);
        br_reload(b);
    }
}

/* returns 0 on success */
static int huf_decompress(uint8_t* dst, size_t dstSize,
                          const uint8_t* cSrc, size_t cSrcSize,
                          zstd_dctx* ctx, int fourStreams) {
    const huf_elt_t* dt = ctx->hufTable;
    uint32_t dtLog = ctx->hufLog;
    if (!fourStreams) {
        bitr_t b;
        if (br_init(&b, cSrc, cSrcSize) != 0) return -1;
        uint8_t* p = dst;
        while (p < dst + dstSize) {
            *p++ = huf_decode_sym(&b, dt, dtLog);
            br_reload(&b);
        }
        if (!br_end(&b)) return -1;
        return 0;
    }
    /* 4 interleaved streams */
    if (cSrcSize < 10 || dstSize < 6) {
        DBG("[z] huf: 4stream size %zu/%zu\n", cSrcSize, dstSize);
        return -1;
    }
    {
        const uint8_t* istart = cSrc;
        size_t l1 = rd16(istart);
        size_t l2 = rd16(istart + 2);
        size_t l3 = rd16(istart + 4);
        size_t l4 = cSrcSize - (l1 + l2 + l3 + 6);
        if (l1 + l2 + l3 + 6 > cSrcSize) {
            DBG("[z] huf: stream len overrun\n");
            return -1;
        }
        const uint8_t* i1 = istart + 6;
        const uint8_t* i2 = i1 + l1;
        const uint8_t* i3 = i2 + l2;
        const uint8_t* i4 = i3 + l3;
        size_t seg = (dstSize + 3) / 4;
        uint8_t* o1 = dst;
        uint8_t* o2 = dst + seg;
        uint8_t* o3 = dst + 2 * seg;
        uint8_t* o4 = dst + 3 * seg;
        if (o4 > dst + dstSize) { DBG("[z] huf: o4 overrun\n"); return -1; }
        bitr_t b1, b2, b3, b4;
        if (br_init(&b1, i1, l1) || br_init(&b2, i2, l2) ||
            br_init(&b3, i3, l3) || br_init(&b4, i4, l4)) {
            DBG("[z] huf: stream br_init\n");
            return -1;
        }
        huf_decode_stream(o1, &b1, dst + seg, dt, dtLog);
        huf_decode_stream(o2, &b2, dst + 2 * seg, dt, dtLog);
        huf_decode_stream(o3, &b3, dst + 3 * seg, dt, dtLog);
        huf_decode_stream(o4, &b4, dst + dstSize, dt, dtLog);
        if (!br_end(&b1) || !br_end(&b2) || !br_end(&b3) || !br_end(&b4)) {
            DBG("[z] huf: stream not consumed %d%d%d%d\n",
                br_end(&b1), br_end(&b2), br_end(&b3), br_end(&b4));
            return -1;
        }
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* XXH64 (checksum, seed 0)                                           */

static const uint64_t P1 = 11400714785074694791ULL;
static const uint64_t P2 = 14029467366897019727ULL;
static const uint64_t P3 = 1609587929392839161ULL;
static const uint64_t P4 = 9650029242287828579ULL;
static const uint64_t P5 = 2870177450012600261ULL;

static uint64_t xxh_round(uint64_t acc, uint64_t input) {
    acc += input * P2;
    acc = (acc << 31) | (acc >> 33);
    acc *= P1;
    return acc;
}
static uint64_t xxh_merge(uint64_t acc, uint64_t val) {
    val = xxh_round(0, val);
    acc ^= val;
    acc = acc * P1 + P4;
    return acc;
}
static uint64_t xxh64(const uint8_t* p, size_t len) {
    const uint8_t* end = p + len;
    uint64_t h;
    if (len >= 32) {
        uint64_t a = P1 + P2, b = P2, c = 0, d = 0 - P1;
        do {
            a = xxh_round(a, rd64(p)); p += 8;
            b = xxh_round(b, rd64(p)); p += 8;
            c = xxh_round(c, rd64(p)); p += 8;
            d = xxh_round(d, rd64(p)); p += 8;
        } while (p <= end - 32);
        h = (a << 1) | (a >> 63);
        h += (b << 7) | (b >> 57);
        h += (c << 12) | (c >> 52);
        h += (d << 18) | (d >> 46);
        h = xxh_merge(h, a);
        h = xxh_merge(h, b);
        h = xxh_merge(h, c);
        h = xxh_merge(h, d);
    } else {
        h = P5;
    }
    h += (uint64_t)len;
    while (p + 8 <= end) {
        h ^= xxh_round(0, rd64(p));
        h = ((h << 27) | (h >> 37)) * P1 + P4;
        p += 8;
    }
    while (p + 4 <= end) {
        h ^= (uint64_t)rd32(p) * P1;
        h = ((h << 23) | (h >> 41)) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (*p) * P5;
        h = ((h << 11) | (h >> 53)) * P1;
        p++;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

/* ------------------------------------------------------------------ */
/* literals + sequences                                               */

typedef struct {
    size_t litLength;
    size_t matchLength;
    size_t offset;
} seq_t;

static int seq_default_tables(zstd_dctx* ctx) {
    /* predefine default tables into the persistent slots once */
    build_seq_default(ctx->llTable, MAXLL, LL_DEFAULTNORMLOG, LL_defaultNorm,
                      LL_base_tbl, LL_bits_tbl);
    build_seq_default(ctx->ofTable, OF_defaultNormMax, OF_DEFAULTNORMLOG,
                      OF_defaultNorm, OF_base_tbl, OF_bits_tbl);
    build_seq_default(ctx->mlTable, MaxML, ML_DEFAULTNORMLOG, ML_defaultNorm,
                      ML_base_tbl, ML_bits_tbl);
    return 0;
}

/* decode one literals section; returns bytes consumed or -1.
 * literals land in ctx->litBuffer, ctx->litSize set. */
static int decode_literals(zstd_dctx* ctx, const uint8_t* src, size_t srcSize,
                           int* litEntropySet) {
    if (srcSize < 2) return -1;
    unsigned type = src[0] & 3;

    if (type == 0 || type == 1) { /* Raw / RLE */
        unsigned lhl = (src[0] >> 2) & 3;
        /* size format: 00/10 -> 1 byte (5-bit size), 01 -> 2 bytes,
         * 11 -> 3 bytes (reference ZSTD_decodeLiteralsBlock) */
        size_t lhSize = (lhl == 1) ? 2 : (lhl == 3) ? 3 : 1;
        size_t litSize;
        if (lhSize == 1) litSize = src[0] >> 3;
        else if (lhSize == 2) litSize = rd16(src) >> 4;
        else litSize = rd24(src) >> 4;
        if (lhSize + (type == 1 ? 1 : 0) > srcSize) return -1;
        if (lhSize + (type == 1 ? 1 : 0) + litSize > srcSize) return -1;
        if (litSize > ZSTD_BLOCKSIZE_MAX) return -1;
        if (type == 0) {
            memcpy(ctx->litBuffer, src + lhSize, litSize);
        } else {
            memset(ctx->litBuffer, src[lhSize], litSize);
        }
        ctx->litSize = litSize;
        return (int)(lhSize + (type == 1 ? 1 : 0) + litSize);
    }

    /* Compressed (2) / Treeless (3) */
    if (type == 2 && srcSize < 5) return -1;
    {
        size_t lhSize, litSize, litCSize;
        unsigned singleStream = 0;
        unsigned lhl = (src[0] >> 2) & 3;
        uint32_t lhc = rd32(src);
        switch (lhl) {
        case 0: case 1:
            singleStream = !lhl;
            lhSize = 3;
            litSize = (lhc >> 4) & 0x3FF;
            litCSize = (lhc >> 14) & 0x3FF;
            break;
        case 2:
            lhSize = 4;
            litSize = (lhc >> 4) & 0x3FFF;
            litCSize = lhc >> 18;
            break;
        default:
            lhSize = 5;
            litSize = (lhc >> 4) & 0x3FFFF;
            litCSize = (lhc >> 22) + ((size_t)src[4] << 10);
            break;
        }
        if (type == 3 && !ctx->litEntropy) return -1;
        if (litSize > ZSTD_BLOCKSIZE_MAX) return -1;
        if (!singleStream && litSize < MIN_LITERALS_FOR_4_STREAMS) return -1;
        if (lhSize + litCSize > srcSize) return -1;
        size_t statsSize = 0;
        if (type == 2) {
            /* returns bytes consumed by the tree description; the
             * compressed literal streams follow it inside litCSize */
            int st = huf_read_dtable(ctx, src + lhSize, litCSize);
            if (st < 0) return -1;
            statsSize = (size_t)st;
        }
        if (huf_decompress(ctx->litBuffer, litSize, src + lhSize + statsSize,
                           litCSize - statsSize, ctx, !singleStream) != 0)
            return -1;
        ctx->litSize = litSize;
        ctx->litEntropy = 1;
        return (int)(lhSize + litCSize);
    }
}

/* build the three sequence tables from a sequences-section header;
 * returns bytes consumed or -1 */
static int decode_seq_headers(zstd_dctx* ctx, int* nbSeqPtr,
                              const uint8_t* src, size_t srcSize) {
    if (srcSize < 1) return -1;
    const uint8_t* ip = src;
    const uint8_t* iend = src + srcSize;
    int nbSeq = *ip++;
    if (nbSeq > 0x7F) {
        if (nbSeq == 0xFF) {
            if (ip + 2 > iend) return -1;
            nbSeq = (int)rd16(ip) + LONGNBSEQ;
            ip += 2;
        } else {
            if (ip >= iend) return -1;
            nbSeq = ((nbSeq - 0x80) << 8) + *ip++;
        }
    }
    *nbSeqPtr = nbSeq;
    if (nbSeq == 0) {
        if (ip != iend) return -1;
        return (int)(ip - src);
    }
    if (ip + 1 > iend) return -1;
    if (*ip & 3) return -1;
    unsigned LLtype = (unsigned)(*ip >> 6) & 3;
    unsigned OFtype = (unsigned)(*ip >> 4) & 3;
    unsigned MLtype = (unsigned)(*ip >> 2) & 3;
    ip++;

    struct tbldef {
        unsigned type; unsigned max; unsigned log; unsigned defLog;
        seq_sym_t* table; const uint32_t* base; const uint8_t* bits;
        const int16_t* defNorm;
    } defs[3] = {
        { LLtype, MAXLL, LLFSELog, LL_DEFAULTNORMLOG, ctx->llTable, LL_base_tbl, LL_bits_tbl, LL_defaultNorm },
        { OFtype, MaxOff, OffFSELog, OF_DEFAULTNORMLOG, ctx->ofTable, OF_base_tbl, OF_bits_tbl, OF_defaultNorm },
        { MLtype, MaxML, MLFSELog, ML_DEFAULTNORMLOG, ctx->mlTable, ML_base_tbl, ML_bits_tbl, ML_defaultNorm },
    };
    for (int i = 0; i < 3; i++) {
        struct tbldef* d = &defs[i];
        switch (d->type) {
        case 0: /* Predefined */
            if (build_seq_default(d->table, d->max, d->defLog, d->defNorm,
                                  d->base, d->bits) != 0) {
                DBG("[z] predefined table %d build failed\n", i);
                return -1;
            }
            break;
        case 1: /* RLE */
            if (ip >= iend) { DBG("[z] rle table %d eof\n", i); return -1; }
            if (*ip > d->max) { DBG("[z] rle sym %u > max %u\n", *ip, d->max); return -1; }
            build_seq_rle(d->table, d->base[*ip], d->bits[*ip]);
            ip++;
            break;
        case 2: { /* FSE */
            unsigned maxSV = d->max, tableLog = 0;
            int16_t norm[MaxML + 1];
            int hs = fse_read_ncount(norm, &maxSV, &tableLog, ip,
                                     (size_t)(iend - ip));
            if (hs < 0) { DBG("[z] fse table %d ncount failed\n", i); return -1; }
            if (tableLog > d->log) { DBG("[z] fse table %d log %u > %u\n", i, tableLog, d->log); return -1; }
            if (build_seq_fse(d->table, norm, maxSV, tableLog,
                              d->base, d->bits) != 0) {
                DBG("[z] fse table %d build failed\n", i);
                return -1;
            }
            ip += hs;
            break;
        }
        default: /* 3 = Repeat */
            if (!ctx->fseEntropy) { DBG("[z] repeat table %d w/o entropy\n", i); return -1; }
            break;
        }
    }
    ctx->fseEntropy = 1;
    return (int)(ip - src);
}

/* ------------------------------------------------------------------ */
/* frame-level state                                                  */

typedef struct {
    uint8_t* out;
    size_t outCap;
    size_t outPos;
    const uint8_t* in;
    size_t inLen;
    zstd_dctx* ctx;
} zstd_stream;

static int out_reserve(zstd_stream* s, size_t need) {
    if (s->outPos + need <= s->outCap) return 0;
    size_t ncap = s->outCap ? s->outCap : (1 << 18);
    while (ncap < s->outPos + need) ncap *= 2;
    uint8_t* nb = (uint8_t*)kmalloc(ncap);
    if (nb == NULL) return -1;
    if (s->out) {
        memcpy(nb, s->out, s->outPos);
        kfree(s->out);
    }
    s->out = nb;
    s->outCap = ncap;
    return 0;
}

static int exec_seq(zstd_stream* s, seq_t* q, const uint8_t** litPtr,
                    const uint8_t* litEnd, size_t frameStart) {
    if (q->litLength > (size_t)(litEnd - *litPtr)) return -1;
    if (out_reserve(s, q->litLength + q->matchLength) != 0) return -1;
    uint8_t* op = s->out + s->outPos;
    memcpy(op, *litPtr, q->litLength);
    *litPtr += q->litLength;
    op += q->litLength;
    if (q->matchLength > 0) {
        if (q->offset > (size_t)(op - (s->out + frameStart))) return -1;
        const uint8_t* match = op - q->offset;
        for (size_t i = 0; i < q->matchLength; i++)
            op[i] = match[i];
        op += q->matchLength;
    }
    s->outPos = (size_t)(op - s->out);
    return 0;
}

/* decode one compressed block; returns bytes consumed (never fails soft) */
static int decode_block(zstd_stream* s, const uint8_t* src, size_t srcSize,
                        size_t frameStart) {
    zstd_dctx* ctx = s->ctx;
    int litConsumed = decode_literals(ctx, src, srcSize, NULL);
    if (litConsumed < 0) { DBG("[z] literals decode failed\n"); return -1; }
    DBG("[z] literals ok consumed=%d litSize=%zu type=%u\n",
        litConsumed, ctx->litSize, src[0] & 3);
    const uint8_t* ip = src + litConsumed;
    size_t seqSize = srcSize - (size_t)litConsumed;
    int nbSeq = 0;
    int hs = decode_seq_headers(ctx, &nbSeq, ip, seqSize);
    if (hs < 0) { DBG("[z] seq headers failed\n"); return -1; }
    DBG("[z] seqhdr ok hs=%d nbSeq=%d\n", hs, nbSeq);
    ip += hs;
    seqSize -= (size_t)hs;

    const uint8_t* litPtr = ctx->litBuffer;
    const uint8_t* litEnd = ctx->litBuffer + ctx->litSize;

    if (nbSeq > 0) {
        bitr_t b;
        if (br_init(&b, ip, seqSize) != 0) return -1;
        if (br_reload(&b) == BR_OVERFLOW) return -1;
        uint32_t prevOffset[3];
        for (int i = 0; i < 3; i++) prevOffset[i] = ctx->rep[i];

        struct { seq_sym_t* table; uint32_t state; } stLL, stOF, stML;
        stLL.table = ctx->llTable + 1;
        stOF.table = ctx->ofTable + 1;
        stML.table = ctx->mlTable + 1;
        /* init order per reference: LL, OF, ML */
        stLL.state = (uint32_t)br_read(&b, ctx->llTable[0].baseValue & 0xFF);
        br_reload(&b);
        stOF.state = (uint32_t)br_read(&b, ctx->ofTable[0].baseValue & 0xFF);
        br_reload(&b);
        stML.state = (uint32_t)br_read(&b, ctx->mlTable[0].baseValue & 0xFF);
        br_reload(&b);
        DBG("[z] bitstream %zu bytes:", seqSize);
        for (size_t di = 0; di < seqSize && di < 8; di++)
            DBG(" %02x", ip[di]);
        DBG("\n");
        DBG("[z] logs ll=%u of=%u ml=%u states ll=%u of=%u ml=%u\n",
            ctx->llTable[0].baseValue & 0xFF, ctx->ofTable[0].baseValue & 0xFF,
            ctx->mlTable[0].baseValue & 0xFF, stLL.state, stOF.state, stML.state);
        DBG("[z] entry@ll: base=%u nbBits=%u next=%u | @of: base=%u bits=%u | @ml: base=%u bits=%u\n",
            stLL.table[stLL.state].baseValue, stLL.table[stLL.state].nbBits,
            stLL.table[stLL.state].nextState,
            stOF.table[stOF.state].baseValue, stOF.table[stOF.state].nbBits,
            stML.table[stML.state].baseValue, stML.table[stML.state].nbBits);

        for (int n = 0; n < nbSeq; n++) {
            seq_sym_t* ll = &stLL.table[stLL.state];
            seq_sym_t* ml = &stML.table[stML.state];
            seq_sym_t* of = &stOF.table[stOF.state];
            seq_t q;
            q.matchLength = ml->baseValue;
            q.litLength = ll->baseValue;
            uint32_t ofBase = of->baseValue;
            uint8_t llB = ll->nbAdditionalBits, mlB = ml->nbAdditionalBits;
            uint8_t ofB = of->nbAdditionalBits;
            uint16_t llN = ll->nextState, mlN = ml->nextState, ofN = of->nextState;
            uint32_t llNB = ll->nbBits, mlNB = ml->nbBits, ofNB = of->nbBits;

            size_t offset;
            if (ofB > 1) {
                offset = ofBase + br_read(&b, ofB);
                prevOffset[2] = prevOffset[1];
                prevOffset[1] = prevOffset[0];
                prevOffset[0] = (uint32_t)offset;
            } else {
                uint32_t ll0 = (q.litLength == 0);
                if (ofB == 0) {
                    offset = prevOffset[ll0];
                    prevOffset[1] = prevOffset[!ll0];
                    prevOffset[0] = (uint32_t)offset;
                } else {
                    offset = ofBase + ll0 + br_read(&b, 1);
                    size_t temp = (offset == 3) ? prevOffset[0] - 1
                                                : prevOffset[offset];
                    temp -= !temp;
                    if (offset != 1) prevOffset[2] = prevOffset[1];
                    prevOffset[1] = prevOffset[0];
                    prevOffset[0] = offset = temp;
                }
            }
            q.offset = offset;
            if (mlB > 0) q.matchLength += br_read(&b, mlB);
            /* 64-bit accumulator guard (reference: STREAM_ACCUMULATOR_MIN_64
             * - (LLFSELog+MLFSELog+OffFSELog) = 57-26 = 31): reload before
             * reading more bits so bitsConsumed never wraps the container */
            if (llB + mlB + ofB >= 31) br_reload(&b);
            if (llB > 0) q.litLength += br_read(&b, llB);

            if (n != nbSeq - 1) {
                uint32_t low;
                low = (uint32_t)br_read(&b, llNB);
                stLL.state = llN + low;
                low = (uint32_t)br_read(&b, mlNB);
                stML.state = mlN + low;
                low = (uint32_t)br_read(&b, ofNB);
                stOF.state = ofN + low;
                br_reload(&b);
            }
            DBG("[z] seq %d: ll=%zu ml=%zu off=%zu\n",
                n, q.litLength, q.matchLength, q.offset);
            if (exec_seq(s, &q, &litPtr, litEnd, frameStart) != 0) {
                DBG("[z] exec_seq failed\n");
                return -1;
            }
        }
        if (!br_end(&b)) { DBG("[z] bitstream not fully consumed\n"); return -1; }
        for (int i = 0; i < 3; i++) ctx->rep[i] = prevOffset[i];
    }

    /* last literals */
    size_t lastLL = (size_t)(litEnd - litPtr);
    if (out_reserve(s, lastLL) != 0) return -1;
    memcpy(s->out + s->outPos, litPtr, lastLL);
    s->outPos += lastLL;
    return (int)(srcSize);
}

/* ------------------------------------------------------------------ */
/* frame loop                                                         */

int zstd_decompress_heap(const uint8_t* in, size_t inLen,
                         uint8_t** out, size_t* outLen) {
    if (inLen < 4) return -1;
    zstd_dctx* ctx = (zstd_dctx*)kmalloc(sizeof(zstd_dctx));
    if (ctx == NULL) return -1;
    memset(ctx, 0, sizeof(*ctx));
    if (seq_default_tables(ctx) != 0) { kfree(ctx); return -1; }

    zstd_stream s;
    memset(&s, 0, sizeof(s));
    s.in = in;
    s.inLen = inLen;
    s.ctx = ctx;

    size_t ipos = 0;
    int rc = -1;

    while (ipos < inLen) {
        uint32_t magic = rd32(in + ipos);
        if ((magic & ZSTD_MAGIC_SKIPPABLE_MASK) == ZSTD_MAGIC_SKIPPABLE_START) {
            if (ipos + 8 > inLen) goto done;
            uint32_t skipLen = rd32(in + ipos + 4);
            ipos += 8 + skipLen;
            if (ipos > inLen) goto done;
            continue;
        }
        if (magic != ZSTD_MAGICNUMBER) goto done;
        if (inLen - ipos < 5) goto done;

        const uint8_t* fhd = in + ipos + 4;
        uint8_t fcsFlag = fhd[0] >> 6;
        unsigned singleSegment = (fhd[0] >> 5) & 1;
        unsigned checksumFlag = (fhd[0] >> 2) & 1;
        unsigned dictIDCode = fhd[0] & 3;
        unsigned reserved = (fhd[0] >> 3) & 1;
        if (reserved) goto done;

        size_t pos = 5; /* magic(4) + fhd(1) */
        uint64_t windowSize = 0;
        if (!singleSegment) {
            if (ipos + pos >= inLen) goto done;
            uint8_t wl = in[ipos + pos++];
            uint32_t wlog = (wl >> 3) + ZSTD_WINDOWLOG_ABSOLUTEMIN;
            if (wlog > ZSTD_WINDOWLOG_LIMIT_DEFAULT) goto done;
            windowSize = 1ULL << wlog;
            windowSize += (windowSize >> 3) * (wl & 7);
        }
        /* dictID */
        if (dictIDCode) {
            static const uint8_t dlen[4] = { 0, 1, 2, 4 };
            pos += dlen[dictIDCode];
        }
        /* FCS */
        uint64_t fcs = 0; int haveFcs = 0;
        switch (fcsFlag) {
        case 0: if (singleSegment) { if (ipos + pos + 1 > inLen) goto done;
                    fcs = in[ipos + pos]; pos++; haveFcs = 1; } break;
        case 1: if (ipos + pos + 2 > inLen) goto done;
                fcs = rd16(in + ipos + pos) + 256; pos += 2; haveFcs = 1; break;
        case 2: if (ipos + pos + 4 > inLen) goto done;
                fcs = rd32(in + ipos + pos); pos += 4; haveFcs = 1; break;
        default: if (ipos + pos + 8 > inLen) goto done;
                fcs = rd64(in + ipos + pos); pos += 8; haveFcs = 1; break;
        }
        if (singleSegment) windowSize = fcs;
        /* reserve output for this frame */
        if (haveFcs) {
            if (out_reserve(&s, (size_t)fcs) != 0) goto done;
        }
        size_t frameStart = s.outPos;

        /* per-frame entropy reset (reference ZSTD_decompressBegin) */
        ctx->rep[0] = 1; ctx->rep[1] = 4; ctx->rep[2] = 8;
        ctx->litEntropy = 0;
        ctx->fseEntropy = 0;

        /* blocks */
        for (;;) {
            if (ipos + pos + 3 > inLen) goto done;
            uint32_t bh = rd24(in + ipos + pos);
            pos += 3;
            unsigned last = bh & 1;
            unsigned btype = (bh >> 1) & 3;
            uint32_t bsize = bh >> 3;     /* RLE: output size; else input size */
            size_t binput = (btype == 1) ? 1 : (size_t)bsize;
            if (ipos + pos + binput > inLen) goto done;
            if (btype != 3 && bsize > ZSTD_BLOCKSIZE_MAX) goto done;
            const uint8_t* bsrc = in + ipos + pos;
        DBG("[z] block btype=%u bsize=%u last=%u\n", btype, bsize, last);
        switch (btype) {
            case 0: /* Raw */
                if (out_reserve(&s, bsize) != 0) goto done;
                if (s.outPos + bsize > s.outCap) goto done;
                memcpy(s.out + s.outPos, bsrc, bsize);
                s.outPos += bsize;
                break;
            case 1: /* RLE */
                if (out_reserve(&s, bsize) != 0) goto done;
                memset(s.out + s.outPos, bsrc[0], bsize);
                s.outPos += bsize;
                break;
            case 2: /* Compressed */
                if (decode_block(&s, bsrc, bsize, frameStart) < 0) goto done;
                break;
            default: goto done;
            }
            pos += binput;
            if (last) break;
        }
        if (checksumFlag) {
            if (ipos + pos + 4 > inLen) goto done;
            uint32_t expect = rd32(in + ipos + pos);
            uint64_t h = xxh64(s.out + frameStart, s.outPos - frameStart);
            if ((uint32_t)h != expect) goto done;
            pos += 4;
        }
        ipos += pos;
        rc = 0; /* at least one frame decoded */
    }

done:
    kfree(ctx);
    if (rc != 0) {
        if (s.out) kfree(s.out);
        return -1;
    }
    if (s.out == NULL) {
        /* empty output: hand back a 1-byte buffer so callers always see
         * an owned, non-NULL pointer */
        s.out = (uint8_t*)kmalloc(1);
        if (s.out == NULL) return -1;
    }
    *out = s.out;
    *outLen = s.outPos;
    return 0;
}
