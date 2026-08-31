/* Phase 4.1: gzip/DEFLATE decoder for .deb data.tar.* members.
 *
 * Single-level 15-bit Huffman lookup tables (fill cost is bounded by
 * 2^15 entries per table, decode is one refill+lookup per symbol), an
 * LSB-first bit accumulator, and puff-style block handling. All state
 * is local - the kernel runs this from the shell command context only.
 */

#include "pkg/inflate.h"
#include "lib/string.h"

#define INFLATE_MAX_BITS 15
#define HTAB_SIZE (1 << INFLATE_MAX_BITS)

/* Table entry: bit 0 = valid, bits 1..4 = code length, bits 5..15 = symbol */
#define HENT(sym, len) ((uint16_t)(1 | ((len) << 1) | ((sym) << 5)))
#define HENT_VALID(e)  ((e) & 1)
#define HENT_LEN(e)    (((e) >> 1) & 0xF)
#define HENT_SYM(e)    ((e) >> 5)

typedef struct {
    uint16_t count[INFLATE_MAX_BITS + 1]; /* codes per length (count[0]=0) */
    uint16_t sym[320];                    /* symbols sorted by (len, sym) */
    uint16_t table[HTAB_SIZE];            /* direct 15-bit lookup */
} huff_t;

typedef struct {
    const uint8_t* in;
    size_t         in_len;
    size_t         in_pos;
    uint64_t       bitbuf;
    int            bitcnt;
    uint8_t*       out;
    size_t         out_cap;
    size_t         out_len;
    /* streaming mode: when set, out points at the shared sliding window
     * and fullness is relieved by flushing to the sink (see out_flush) */
    int (*sink)(void* ctx, const uint8_t* data, size_t n);
    void*          sink_ctx;
    size_t         flushed;   /* bytes already handed to the sink */
} inf_state_t;

/* Sliding-window parameters. DEFLATE back-references reach at most
 * 32 KB back, so keeping the last INF_KEEP bytes as history after a
 * flush keeps every `from` index valid. The window must hold
 * INF_KEEP (history) + room for one match run (<= 258 B). */
#define INF_KEEP  32768
#define INF_BUFSZ 65536

/* Flush all but the last INF_KEEP bytes to the sink and slide the
 * history to the front. Legacy (sink == NULL) callers never reach this. */
static int out_flush(inf_state_t* s) {
    size_t keep = s->out_len < INF_KEEP ? s->out_len : INF_KEEP;
    size_t flush_n = s->out_len - keep;
    if (flush_n) {
        if (s->sink(s->sink_ctx, s->out, flush_n) != 0) return -1;
        for (size_t i = 0; i < keep; i++) s->out[i] = s->out[flush_n + i];
        s->flushed += flush_n;
    }
    s->out_len = keep;
    return 0;
}

static const uint16_t len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t len_ext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dist_ext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* Refill the bit accumulator (keeps up to 56 bits, LSB-first stream). */
static void refill(inf_state_t* s) {
    while (s->bitcnt <= 56 && s->in_pos < s->in_len) {
        s->bitbuf |= (uint64_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
}

static int get_bits(inf_state_t* s, int n, uint32_t* val) {
    if (n > 0) refill(s);
    if (s->bitcnt < n) return -1;
    *val = (uint32_t)(s->bitbuf & ((1ULL << n) - 1));
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return 0;
}

/* Build the direct 15-bit lookup table from count[]/sym[] (RFC 1951 3.2.2).
 * sym[] must hold the symbols in canonical order (length, then symbol). */
static int huff_build(huff_t* h) {
    uint16_t next_code[INFLATE_MAX_BITS + 1];
    uint16_t code = 0;
    uint32_t total = 0;

    for (int len = 1; len <= INFLATE_MAX_BITS; len++) {
        code = (uint16_t)((code + h->count[len - 1]) << 1);
        next_code[len] = code;
        total += h->count[len];
    }
    if (total == 0 || total > 320) return -1;
    /* Over-subscription check: Kraft inequality must hold. */
    {
        uint32_t left = 1;
        for (int len = 1; len <= INFLATE_MAX_BITS; len++) {
            left <<= 1;
            if (h->count[len] > left) return -1;
            left -= h->count[len];
        }
    }

    memset(h->table, 0, sizeof(h->table));
    uint16_t sym_idx = 0;
    for (int len = 1; len <= INFLATE_MAX_BITS; len++) {
        uint32_t stride = 1u << len;
        for (int i = 0; i < h->count[len]; i++) {
            uint16_t c = next_code[len]++;
            uint16_t sym = h->sym[sym_idx++];
            /* DEFLATE packs Huffman codes first-bit-first: the table
             * index is the bit-reversed code in the low `len` bits. */
            uint32_t rev = 0;
            for (int b = 0; b < len; b++)
                if (c & (1 << b)) rev |= 1u << (len - 1 - b);
            uint16_t ent = HENT(sym, len);
            for (uint32_t j = rev; j < HTAB_SIZE; j += stride) h->table[j] = ent;
        }
    }
    return 0;
}

static int huff_decode(inf_state_t* s, const huff_t* h, int* sym) {
    refill(s);
    if (s->bitcnt < 1) return -1;
    uint16_t e = h->table[s->bitbuf & (HTAB_SIZE - 1)];
    if (!HENT_VALID(e)) return -1;
    if (s->bitcnt < HENT_LEN(e)) return -1;
    s->bitbuf >>= HENT_LEN(e);
    s->bitcnt -= HENT_LEN(e);
    *sym = HENT_SYM(e);
    return 0;
}

/* Fixed-code tables (RFC 1951 3.2.6), built once. */
static int fixed_ready;
static huff_t fixed_lit, fixed_dist;

static void build_fixed(void) {
    if (fixed_ready) return;
    memset(&fixed_lit, 0, sizeof(fixed_lit));
    memset(&fixed_dist, 0, sizeof(fixed_dist));

    /* lengths: 0-143:8, 144-255:9, 256-279:7, 280-287:8 */
    for (int i = 0; i < 144; i++)   fixed_lit.count[8]++;
    for (int i = 144; i < 256; i++) fixed_lit.count[9]++;
    for (int i = 256; i < 280; i++) fixed_lit.count[7]++;
    for (int i = 280; i < 288; i++) fixed_lit.count[8]++;
    {
        int idx = 0;
        for (int len = 1; len <= INFLATE_MAX_BITS; len++)
            for (int sym = 0; sym < 288; sym++) {
                int l = (sym < 144) ? 8 : (sym < 256) ? 9 : (sym < 280) ? 7 : 8;
                if (l == len) fixed_lit.sym[idx++] = (uint16_t)sym;
            }
    }

    for (int i = 0; i < 30; i++) fixed_dist.count[5]++;
    for (int i = 0; i < 30; i++) fixed_dist.sym[i] = (uint16_t)i;

    fixed_ready = (huff_build(&fixed_lit) == 0 && huff_build(&fixed_dist) == 0);
}

static int inflate_stored(inf_state_t* s) {
    /* discard bits up to the next byte boundary */
    int drop = s->bitcnt & 7;
    s->bitbuf >>= drop;
    s->bitcnt -= drop;

    uint32_t len, nlen;
    if (get_bits(s, 16, &len) != 0) return -1;
    if (get_bits(s, 16, &nlen) != 0) return -1;
    if ((len ^ 0xFFFF) != nlen) return -1;
    if (!s->sink && s->out_len + len > s->out_cap) return -1;
    for (uint32_t i = 0; i < len; i++) {
        if (s->out_len >= s->out_cap) {
            if (!s->sink || out_flush(s) != 0) return -1;
        }
        uint32_t b;
        if (get_bits(s, 8, &b) != 0) return -1;
        s->out[s->out_len++] = (uint8_t)b;
    }
    return 0;
}

static int inflate_codes(inf_state_t* s, const huff_t* lit, const huff_t* dist) {
    for (;;) {
        int sym;
        if (huff_decode(s, lit, &sym) != 0) return -1;
        if (sym < 256) {
            if (s->out_len >= s->out_cap) {
                if (!s->sink || out_flush(s) != 0) return -1;
            }
            s->out[s->out_len++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            uint32_t extra = 0;
            if (len_ext[sym] && get_bits(s, len_ext[sym], &extra) != 0) return -1;
            uint32_t length = len_base[sym] + extra;

            int dsym;
            if (huff_decode(s, dist, &dsym) != 0) return -1;
            if (dsym >= 30) return -1;
            extra = 0;
            if (dist_ext[dsym] && get_bits(s, dist_ext[dsym], &extra) != 0) return -1;
            uint32_t distance = dist_base[dsym] + extra;

            if (distance > s->out_len) return -1;
            /* distance <= 32768 = INF_KEEP, so the flush inside the copy
             * loop always leaves the referenced history intact. */
            size_t from = s->out_len - distance;
            for (uint32_t i = 0; i < length; i++) {
                if (s->out_len >= s->out_cap) {
                    if (!s->sink || out_flush(s) != 0) return -1;
                    from = s->out_len - distance;
                }
                s->out[s->out_len] = s->out[from];
                s->out_len++;
                from++;
            }
        }
    }
}

/* Dynamic-table huffman trees live at file scope: one huff_t is ~66KB
 * (the 15-bit direct table dominates), far beyond the 16KB kernel stack. */
static huff_t dyn_lit, dyn_dist, dyn_clc;

/* Read one code-length alphabet entry (16/17/18 repeats handled by caller). */
static int inflate_dynamic_tables(inf_state_t* s, huff_t* lit, huff_t* dist) {
    static const uint8_t clc_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    uint32_t v;
    if (get_bits(s, 5, &v) != 0) return -1;
    int hlit = (int)v + 257;
    if (get_bits(s, 5, &v) != 0) return -1;
    int hdist = (int)v + 1;
    if (get_bits(s, 4, &v) != 0) return -1;
    int hclen = (int)v + 4;
    if (hlit > 286 || hdist > 30) return -1;

    uint8_t lens[320];
    memset(lens, 0, sizeof(lens));

    uint8_t clc_len[19];
    memset(clc_len, 0, sizeof(clc_len));
    huff_t* clc = &dyn_clc;
    memset(clc, 0, sizeof(*clc));
    for (int i = 0; i < hclen; i++) {
        if (get_bits(s, 3, &v) != 0) return -1;
        clc_len[clc_order[i]] = (uint8_t)v;
        if (v) clc->count[v]++;
    }
    {
        int idx = 0;
        for (int len = 1; len <= INFLATE_MAX_BITS; len++)
            for (int sym = 0; sym < 19; sym++)
                if (clc_len[sym] == len) clc->sym[idx++] = (uint16_t)sym;
    }
    if (huff_build(clc) != 0) return -1;

    int n = hlit + hdist;
    int cur = 0;
    int last = -1;
    while (cur < n) {
        int sym;
        if (huff_decode(s, clc, &sym) != 0) return -1;
        if (sym < 16) {
            lens[cur++] = (uint8_t)sym;
            last = sym;
        } else if (sym == 16) {
            if (last < 0) return -1;
            if (get_bits(s, 2, &v) != 0) return -1;
            int rep = 3 + (int)v;
            while (rep-- && cur < n) lens[cur++] = (uint8_t)last;
        } else if (sym == 17) {
            if (get_bits(s, 3, &v) != 0) return -1;
            int rep = 3 + (int)v;
            while (rep-- && cur < n) lens[cur++] = 0;
        } else {
            if (get_bits(s, 7, &v) != 0) return -1;
            int rep = 11 + (int)v;
            while (rep-- && cur < n) lens[cur++] = 0;
        }
    }
    if (lens[256] == 0) return -1; /* no end-of-block code */

    memset(lit, 0, sizeof(*lit));
    for (int i = 0; i < hlit; i++)
        if (lens[i]) lit->count[lens[i]]++;
    {
        int idx = 0;
        for (int len = 1; len <= INFLATE_MAX_BITS; len++)
            for (int sym = 0; sym < hlit; sym++)
                if (lens[sym] == len) lit->sym[idx++] = (uint16_t)sym;
    }
    if (huff_build(lit) != 0) return -1;

    memset(dist, 0, sizeof(*dist));
    for (int i = 0; i < hdist; i++)
        if (lens[hlit + i]) dist->count[lens[hlit + i]]++;
    {
        int idx = 0;
        for (int len = 1; len <= INFLATE_MAX_BITS; len++)
            for (int sym = 0; sym < hdist; sym++)
                if (lens[hlit + sym] == len) dist->sym[idx++] = (uint16_t)sym;
    }
    if (huff_build(dist) != 0) return -1;
    return 0;
}

static int inflate_raw(inf_state_t* s) {
    int last;
    do {
        uint32_t v;
        if (get_bits(s, 1, &v) != 0) return -1;
        last = (int)v;
        if (get_bits(s, 2, &v) != 0) return -1;
        int type = (int)v;
        if (type == 0) {
            if (inflate_stored(s) != 0) return -1;
        } else if (type == 1) {
            build_fixed();
            if (!fixed_ready) return -1;
            if (inflate_codes(s, &fixed_lit, &fixed_dist) != 0) return -1;
        } else if (type == 2) {
            if (inflate_dynamic_tables(s, &dyn_lit, &dyn_dist) != 0) return -1;
            if (inflate_codes(s, &dyn_lit, &dyn_dist) != 0) return -1;
        } else {
            return -1;
        }
    } while (!last);
    return 0;
}

/* CRC-32 (RFC 1952), bitwise (called a few times per stream). */
static uint32_t crc32_update(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/* Shared sliding window (single-threaded shell context only). */
static uint8_t inf_window[INF_BUFSZ];

/* CRC-covering wrapper: counts bytes passing to the user sink so the
 * per-member trailer check works without holding the whole output. */
typedef struct {
    gzip_sink_fn user;
    void*        uctx;
    uint32_t     crc;
} crc_sink_t;

static int crc_sink(void* ctx, const uint8_t* d, size_t n) {
    crc_sink_t* c = ctx;
    c->crc = crc32_update(c->crc, d, n);
    return c->user(c->uctx, d, n);
}

int gzip_inflate_cb(const uint8_t* in, size_t in_len,
                    gzip_sink_fn sink, void* ctx, size_t* out_len) {
    size_t pos = 0;
    size_t total = 0;
    crc_sink_t cs;
    cs.user = sink;
    cs.uctx = ctx;

    while (in_len - pos >= 18) { /* 10 hdr + 8 trailer minimum */
        const uint8_t* p = in + pos;
        if (p[0] != 0x1F || p[1] != 0x8B || p[2] != 8) return -1;
        uint8_t flg = p[3];
        size_t off = 10;
        if (flg & 0x04) { /* FEXTRA */
            if (pos + off + 2 > in_len) return -1;
            uint16_t xlen = (uint16_t)(p[off] | (p[off + 1] << 8));
            off += 2 + xlen;
        }
        if (flg & 0x08) { /* FNAME */
            while (pos + off < in_len && p[off] != 0) off++;
            off++;
        }
        if (flg & 0x10) { /* FCOMMENT */
            while (pos + off < in_len && p[off] != 0) off++;
            off++;
        }
        if (flg & 0x02) off += 2; /* FHCRC */
        if (pos + off > in_len) return -1;

        inf_state_t s;
        memset(&s, 0, sizeof(s));
        s.in = in + pos + off;
        s.in_len = in_len - pos - off;
        s.out = inf_window;
        s.out_cap = INF_BUFSZ;
        s.sink = crc_sink;
        s.sink_ctx = &cs;
        cs.crc = 0;
        if (inflate_raw(&s) != 0) return -1;

        /* trailer: CRC32 (ISIZE checked by the caller via ISIZE field).
         * refill() may have pulled trailer bytes into the bit buffer, so
         * the stream position is a BIT offset: in_pos*8 - bitcnt. The
         * trailer starts at the next byte boundary after the final block. */
        size_t consumed_bits = s.in_pos * 8 - (size_t)s.bitcnt;
        size_t trailer_pos = (consumed_bits + 7) / 8;
        if (s.in_len < trailer_pos + 8) return -1;
        const uint8_t* t = s.in + trailer_pos;
        uint32_t want_crc = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                            ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
        /* deliver the tail still sitting in the window (this also
         * completes the CRC, which only counts bytes passing the sink) */
        if (s.out_len > 0 && crc_sink(&cs, s.out, s.out_len) != 0) return -1;
        if (cs.crc != want_crc) return -1;

        total += s.flushed + s.out_len;
        pos += off + trailer_pos + 8;

        /* concatenated members: continue; stray trailing zero bytes
         * (padding) are tolerated */
        while (pos < in_len && in[pos] == 0) pos++;
        if (pos >= in_len) break;
        if (in_len - pos < 18) return -1;
    }
    if (total == 0) return -1;
    *out_len = total;
    return 0;
}

/* One-shot buffer sink used by the legacy gzip_inflate wrapper. */
typedef struct {
    uint8_t* out;
    size_t   cap;
    size_t   len;
} buf_sink_t;

static int buf_sink(void* ctx, const uint8_t* d, size_t n) {
    buf_sink_t* b = ctx;
    if (b->len + n > b->cap) return -1;
    for (size_t i = 0; i < n; i++) b->out[b->len + i] = d[i];
    b->len += n;
    return 0;
}

int gzip_inflate(const uint8_t* in, size_t in_len,
                 uint8_t* out, size_t out_cap, size_t* out_len) {
    buf_sink_t b;
    b.out = out;
    b.cap = out_cap;
    b.len = 0;
    size_t total = 0;
    if (gzip_inflate_cb(in, in_len, buf_sink, &b, &total) != 0) return -1;
    *out_len = total;
    return 0;
}

long long gzip_payload_size(const uint8_t* in, size_t in_len) {
    if (in_len < 18 || in[0] != 0x1F || in[1] != 0x8B) return -1;
    /* Read the ISIZE trailer at its TRUE position (last 4 bytes). Do NOT
     * scan backwards over zero bytes here: ISIZE high bytes are zero for
     * any payload < 64 KB, so a "padding" scan eats the real trailer and
     * returns garbage (broke every small .deb). Mirror padding AFTER the
     * trailer is a non-issue for callers: gzip_inflate[_cb] locates the
     * trailer bit-exactly after the final DEFLATE block and only skips
     * padding between members. */
    const uint8_t* isize = in + in_len - 4;
    return (long long)((uint32_t)isize[0] | ((uint32_t)isize[1] << 8) |
                       ((uint32_t)isize[2] << 16) | ((uint32_t)isize[3] << 24));
}
