/* MP3 streaming facade over minimp3 (include/drivers/minimp3.h).
 * The decoder state (~10 KB of float scratch) is heap-allocated because
 * kernel stacks are small; the caller keeps the file buffer alive for the
 * lifetime of the handle (bit reservoir reads run backwards into previous
 * frames). */
#include <stdint.h>
#include "lib/string.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_KERNEL
#include "drivers/minimp3.h"
#include "drivers/mp3.h"
#include "mm/memory.h"

/* The kernel is -mno-sse everywhere except this file, so the decoder is the
 * only SSE user. Two hazards follow, and both are handled by running every
 * decode with interrupts masked and the incoming FPU/SSE state parked:
 *   - a timer IRQ switching tasks mid-decode would clobber our live XMM
 *     registers (nothing else saves them),
 *   - our XMM writes would clobber the registers of whichever context was
 *     interrupted, since nothing else preserves them either. */
static uint8_t fpu_park[512] __attribute__((aligned(16)));

/* cli + park the live FPU/SSE state; returns RFLAGS so IF can be restored */
static uint64_t fpu_borrow(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) :: "memory");
    __asm__ volatile("fxsave64 (%0)" :: "r"(fpu_park) : "memory");
    return flags;
}

/* un-park and re-enable interrupts. fxrstor writes every XMM register, so
 * the clobber list keeps the compiler from holding float values across it. */
static __attribute__((noinline)) void fpu_return(uint64_t flags) {
    __asm__ volatile("fxrstor64 (%0)" :: "r"(fpu_park) : "memory",
                     "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5",
                     "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
                     "xmm12", "xmm13", "xmm14", "xmm15");
    if (flags & (1u << 9)) __asm__ volatile("sti");
}

struct mp3 {
    mp3dec_t            dec;
    mp3dec_frame_info_t info;
    const uint8_t*      data;
    size_t              size;
    size_t              pos;
    int                 channels;
    int                 rate;
    int                 bitrate;
    int                 started;
};

mp3_t* mp3_open(const uint8_t* data, size_t size) {
    if (data == NULL || size < 8) return NULL;
    mp3_t* m = (mp3_t*)kmalloc(sizeof(mp3_t));
    if (m == NULL) return NULL;
    memset(m, 0, sizeof(*m));
    m->data = data;
    m->size = size;
    mp3dec_init(&m->dec);

    /* probe: decode nothing, just locate the first frame for stream info */
    int16_t probe[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t save = 0;
    int n = 0;
    uint64_t fl = fpu_borrow();
    for (size_t off = 0; off + 4 < size; off += m->info.frame_bytes) {
        m->pos = off;
        n = mp3dec_decode_frame(&m->dec, data + off,
                                (int)(size - off), probe, &m->info);
        if (n > 0) break;
        save = off;
        if (m->info.frame_bytes <= 0) break;
    }
    fpu_return(fl);
    if (n <= 0) {
        kfree(m);
        return NULL;
    }
    m->channels = m->info.channels;
    m->rate     = m->info.hz;
    m->bitrate  = m->info.bitrate_kbps;
    m->started  = 1;
    m->pos += m->info.frame_bytes;
    (void)save;
    return m;
}

void mp3_close(mp3_t* m) {
    if (m == NULL) return;
    kfree(m);
}

/* returns number of int16 samples written to pcm (1152 * channels), or
 * 0 when the stream is exhausted/undecodable */
int mp3_decode(mp3_t* m, int16_t* pcm) {
    if (m == NULL || m->pos >= m->size) return 0;
    uint64_t fl = fpu_borrow();
    int n = mp3dec_decode_frame(&m->dec, m->data + m->pos,
                                (int)(m->size - m->pos), pcm, &m->info);
    fpu_return(fl);
    if (n <= 0) {
        m->pos = m->size;
        return 0;
    }
    m->channels = m->info.channels;
    m->rate     = m->info.hz;
    m->bitrate  = m->info.bitrate_kbps;
    m->pos     += m->info.frame_bytes;
    return n * m->channels;
}

int    mp3_channels(mp3_t* m)    { return m ? m->channels : 0; }
int    mp3_rate(mp3_t* m)        { return m ? m->rate : 0; }
int    mp3_bitrate(mp3_t* m)     { return m ? m->bitrate : 0; }
size_t mp3_total_bytes(mp3_t* m) { return m ? m->size : 0; }
size_t mp3_pos(mp3_t* m)         { return m ? m->pos : 0; }
