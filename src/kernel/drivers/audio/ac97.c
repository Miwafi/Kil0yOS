/* Intel ICHx AC97 audio driver: NAM (codec, BAR0) + NABM bus master (BAR1).
 * PCM-out channel with an 8-entry buffer descriptor list; polled, no IRQ.
 * The heap lives in the identity map, so kmalloc addresses double as
 * physical bus addresses. */
#include <stdint.h>
#include "drivers/io.h"
#include "drivers/pci.h"
#include "drivers/audio/ac97.h"
#include "mm/memory.h"
#include "lib/string.h"

extern void klog(const char* s);

/* --- NAM (codec) registers --- */
#define NAM_RESET      0x00    /* write 1 = cold reset, self-clearing */
#define NAM_MASTER_VOL 0x02
#define NAM_PCM_VOL    0x18
#define NAM_EXT_ID     0x28    /* extended audio ID (bit0 = VRA support) */
#define NAM_EXT_CTRL   0x2A    /* extended audio control/status (bit0 = VRA) */
#define NAM_DAC_RATE   0x2C    /* PCM front DAC sample rate */

/* --- NABM PCM-out bus-master registers --- */
#define NABM_BDBA 0x10        /* BDL physical base, 8-byte aligned (dword) */
#define NABM_CIV  0x14        /* current index (ro, byte) */
#define NABM_LVI  0x15        /* last valid index (byte) */
#define NABM_SR   0x16        /* status (word; W1C bits below) */
#define NABM_PICB 0x18        /* position in current buffer (word, samples) */
#define NABM_CR   0x1B        /* control (byte): bit0 = run */

#define SR_DCH    0x01        /* DMA controller halted / caught up with LVI */
#define SR_CLEAR  0x1C        /* W1C: FIFO error | buffer completion | last valid */

/* The bus master's CIV/LVI are 5-bit indices into a fixed 32-entry
 * descriptor list, so the ring must be exactly 32 slots long: the hardware
 * walks civ 0..31 and the driver's LVI has to live in the same space. */
#define AUD_NBUF        32
#define AUD_BUF_FRAMES  512                  /* stereo frames per BDL entry */
#define AUD_BYTES_FRAME 4                    /* 16-bit stereo */
#define AUD_BUF_BYTES   (AUD_BUF_FRAMES * AUD_BYTES_FRAME)

typedef struct {
    uint32_t addr;                            /* buffer physical address */
    uint16_t len;                             /* 16-bit samples (not bytes) */
    uint16_t flags;                           /* bit15 BUP, bit31 IOC */
} bdl_entry_t;

static uint16_t nam, nabm;
static int dma_on;

static bdl_entry_t* bdl;
static int16_t*     bufs[AUD_NBUF];
static int          fill_buf, fill_off;      /* ring writer position */
static uint64_t     frames_written;
static int          civ_seen;                /* playhead index at last poll */
static int          in_flight;               /* slots handed to the DAC, unplayed */

static void     aud_out(uint16_t port, uint16_t v) { outw(nabm + port, v); }

int ac97_init(void) {
    nam = nabm = 0;
    dma_on = 0;
    bdl = NULL;

    pci_device_t* d = pci_find_class(0x04, 0x01);
    if (d == NULL) {
        klog("[ac97] no PCI audio device\n");
        return -1;
    }
    /* re-read BAR0 from config space: pci_init() masks the low nibble off
     * the cached copy, which would hide the I/O-space indicator bit */
    uint32_t bar0 = pci_read_dword(d->bus, d->device, d->function, 0x10);
    uint32_t bar1 = pci_read_dword(d->bus, d->device, d->function, 0x14);
    if ((bar0 & 1) == 0 || (bar1 & 1) == 0) {
        klog("[ac97] audio device has no IO BARs\n");
        return -1;
    }
    nam  = (uint16_t)(bar0 & 0xFFFC);
    nabm = (uint16_t)(bar1 & 0xFFFC);

    /* cold reset: write 1 to codec reset, wait for self-clear */
    outw(nam + NAM_RESET, 1);
    int ok = 0;
    for (int i = 0; i < 1000000; i++) {
        if ((inw(nam + NAM_RESET) & 1) == 0) { ok = 1; break; }
    }
    if (!ok) {
        klog("[ac97] codec reset timeout\n");
        return -1;
    }
    /* unmute master + PCM-out (0x0000 = 0 dB; bit15 would mute) */
    outw(nam + NAM_MASTER_VOL, 0x0000);
    outw(nam + NAM_PCM_VOL, 0x0000);
    klog("[ac97] codec ready\n");
    return 0;
}

int ac97_open(uint32_t sample_rate) {
    if (nam == 0) return -1;
    if (bdl != NULL) ac97_close();

    bdl = (bdl_entry_t*)kmalloc(sizeof(bdl_entry_t) * AUD_NBUF);
    if (bdl == NULL) return -1;
    for (int i = 0; i < AUD_NBUF; i++) {
        bufs[i] = (int16_t*)kmalloc(AUD_BUF_BYTES);
        if (bufs[i] == NULL) {
            for (int j = 0; j < i; j++) kfree(bufs[j]);
            kfree(bdl);
            bdl = NULL;
            return -1;
        }
        memset(bufs[i], 0, AUD_BUF_BYTES);   /* silence until first fill */
    }

    fill_buf = 0;
    fill_off = 0;
    frames_written = 0;
    dma_on = 0;
    civ_seen = 0;
    in_flight = 0;

    outb(nabm + NABM_CR, 0x00);               /* make sure DMA is stopped */
    aud_out(NABM_SR, SR_CLEAR);               /* drop sticky status bits */

    /* variable-rate audio: the DAC rate register is only writable once the
     * codec's VRA bit is set; without it the DAC stays at its reset rate and
     * everything plays at the wrong pitch. */
    uint16_t ext = inw(nam + NAM_EXT_ID);
    if (ext & 1) {
        outw(nam + NAM_EXT_CTRL, (uint16_t)(inw(nam + NAM_EXT_CTRL) | 1));
    }
    outw(nam + NAM_DAC_RATE, (uint16_t)sample_rate);
    uint16_t got = inw(nam + NAM_DAC_RATE);
    if (got != (uint16_t)sample_rate) {
        klog("[ac97] DAC rate rejected (ext_id=");
        char num[8];
        for (int i = 3; i >= 0; i--) { num[3 - i] = "0123456789abcdef"[(ext >> (i * 4)) & 0xF]; }
        num[4] = '\0';
        klog(num);
        klog(")\n");
    }

    /* fill BDL: identical descriptors. `len` counts 16-bit samples, so a
     * stereo frame occupies two of them. LVI is not programmed here: it is
     * advanced as slots are written, which is also what restarts the DAC
     * when it has caught up. */
    for (int i = 0; i < AUD_NBUF; i++) {
        bdl[i].addr  = (uint32_t)(uintptr_t)bufs[i];
        bdl[i].len   = AUD_BUF_FRAMES * 2;
        bdl[i].flags = 0x0000;
    }
    outd(nabm + NABM_BDBA, (uint32_t)(uintptr_t)bdl);
    outb(nabm + NABM_LVI, 0);
    return 0;
}

void ac97_close(void) {
    if (bdl == NULL) return;
    outb(nabm + NABM_CR, 0x00);               /* stop DMA */
    dma_on = 0;
    in_flight = 0;
    for (int i = 0; i < AUD_NBUF; i++) {
        if (bufs[i] != NULL) kfree(bufs[i]);
        bufs[i] = NULL;
    }
    kfree(bdl);
    bdl = NULL;
}

/* The controller halts (SR.DCH) once it has played everything it was told
 * about through LVI, so a halt means the whole ring is ours again. While it
 * runs, CIV steps forward every time it finishes a slot. */
static void dma_reclaim(void) {
    if (!dma_on) return;
    if (inw(nabm + NABM_SR) & SR_DCH) {
        in_flight = 0;
        civ_seen = inb(nabm + NABM_CIV);
        return;
    }
    int civ = inb(nabm + NABM_CIV);
    int done = civ - civ_seen;
    if (done < 0) done += AUD_NBUF;
    if (done > in_flight) done = in_flight;
    in_flight -= done;
    civ_seen = civ;
}

/* stereo frame = 4 bytes (L,R int16). Accepts as many frames as free ring
 * slots can hold; each completed slot is published by advancing LVI. */
int ac97_write(const int16_t* pcm, int frames) {
    if (bdl == NULL) return 0;
    dma_reclaim();

    int written = 0;
    while (frames > 0) {
        int claimed = in_flight + (fill_off != 0 ? 1 : 0);
        if (claimed >= AUD_NBUF) break;       /* ring exhausted */

        int space = AUD_BUF_FRAMES - fill_off;
        int n = frames < space ? frames : space;
        memcpy(bufs[fill_buf] + fill_off * 2, pcm + written * 2,
               (size_t)n * AUD_BYTES_FRAME);
        fill_off += n;
        written += n;
        frames -= n;
        if (fill_off == AUD_BUF_FRAMES) {
            fill_off = 0;
            /* hand the slot over; if the DAC had caught up this restarts it */
            outb(nabm + NABM_LVI, (uint8_t)fill_buf);
            fill_buf = (fill_buf + 1) % AUD_NBUF;
            in_flight++;
        }
    }
    frames_written += (uint64_t)written;
    return written;
}

void ac97_play(void) {
    if (bdl == NULL || dma_on) return;
    dma_on = 1;
    outb(nabm + NABM_CR, 0x01);               /* run */
}

void ac97_pause(void) {
    if (bdl == NULL || !dma_on) return;
    dma_on = 0;
    outb(nabm + NABM_CR, 0x00);
}

int ac97_playing(void) { return bdl != NULL && dma_on; }

/* stereo frames still queued ahead of the playhead */
int ac97_queued(void) {
    if (bdl == NULL) return 0;
    if (!dma_on) return in_flight * AUD_BUF_FRAMES;
    if (inw(nabm + NABM_SR) & SR_DCH) return 0;   /* caught up with LVI */
    int picb = inw(nabm + NABM_PICB);             /* samples left in the slot */
    int played = AUD_BUF_FRAMES - picb / 2;
    if (played < 0) played = 0;
    if (played > AUD_BUF_FRAMES) played = AUD_BUF_FRAMES;
    int queued = in_flight * AUD_BUF_FRAMES - played;
    return queued > 0 ? queued : 0;
}

uint64_t ac97_written_total(void) { return frames_written; }
