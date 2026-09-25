/* Intel High Definition Audio controller + codec driver (PCI class 0x0403).
 *
 * Covers the Realtek ALC family (vendor 0x10EC) and any other HDA codec with
 * a discoverable DAC -> pin path: the controller is reset, CORB/RIRB command
 * rings are brought up, the codec's audio function group is walked to find an
 * output pin whose connection graph reaches a DAC, that path is powered to D0
 * and unmuted (plus EAPD on pins that need it - mandatory on most Realteks),
 * and a single output stream descriptor plays a 32-slot cyclic BDL ring.
 *
 * Register access and every DMA buffer (BAR0, CORB/RIRB rings, position
 * buffer, BDL, sample ring) are aliased into one uncached window so the CPU
 * and the DMA engines always agree on memory contents; verbs go through
 * CORB/RIRB with the immediate command interface (ICW/IRR/ICS) as fallback.
 * Polled: no interrupts. */
#include <stdint.h>
#ifdef HDA_HOST_TEST
/* tools/hda_host_test.c drives the codec layer against a canned topology:
 * no MMIO, no controller, no PCI - the test supplies klog/ksprintf/kmalloc,
 * hda_cmd() and the stub declarations below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void klog(const char* s);
void ksprintf(char* buf, size_t size, const char* fmt, ...);
#define kmalloc(sz) malloc(sz)
#define kfree(p) free(p)
static void pit_delay_ms(uint32_t ms) { (void)ms; }
#else
#include "drivers/io.h"
#include "drivers/pci.h"
#include "drivers/audio/hda.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "timer/pit.h"
#include "fs/fs.h"

extern void klog(const char* s);
#endif

/* Every [hda] line is teed into a buffer; when the probe fails the whole log
 * is written to /home/user/hda.log. On a headless real machine the klog pane
 * only shows the tail of the scrollback (the Files panel occludes the rest),
 * so this is how the full probe log comes back off the box. */
static char   hlog_buf[4096];
static size_t hlog_n;
static void hlog(const char* s) {
    klog(s);
#ifndef HDA_HOST_TEST
    size_t l = strlen(s);
    if (hlog_n + l + 1 < sizeof(hlog_buf)) {
        memcpy(hlog_buf + hlog_n, s, l);
        hlog_n += l;
        hlog_buf[hlog_n] = '\0';
    }
#endif
}

/* ---------------- controller registers (BAR0) ---------------- */
#define HDA_GCAP      0x00    /* u16: ISS/OSS/BSS, 64OK */
#define HDA_GCTL      0x08    /* u32 */
#define   GCTL_CRST   0x00000001
#define HDA_STATESTS  0x0e    /* u16: codec presence */
#define HDA_INTCTL    0x20
#define HDA_INTSTS    0x24
#define HDA_WAKEEN    0x0c

#define HDA_CORBLBASE 0x40    /* u32 */
#define HDA_CORBUBASE 0x44
#define HDA_CORBWP    0x48    /* u16 */
#define HDA_CORBRP    0x4a    /* u16, bit15 = reset */
#define   CORBRP_RST  0x8000
#define HDA_CORBCTL   0x4c    /* u8, bit1 = run */
#define   CORBCTL_RUN 0x02
#define HDA_CORBSTS   0x4d    /* u8: bit0 = CORB memory error (W1C) */
#define HDA_CORBSIZE  0x4e    /* u8, bits1:0: 0=2 1=16 2=256 entries */

#define HDA_ICW       0x60    /* u32: immediate command write */
#define HDA_IRR       0x64    /* u32: immediate response read */
#define HDA_ICIS      0x68    /* u16: bit0 ICB busy, bit1 IRV result valid */
#define HDA_DPLBASE   0x70    /* u32: DMA position buffer low, bit0 = enable */
#define HDA_DPUBASE   0x74    /* u32: DMA position buffer high */

#define HDA_RIRBLBASE 0x50
#define HDA_RIRBUBASE 0x54
#define HDA_RIRBWP    0x58    /* u16, bit15 = reset */
#define   RIRBWP_RST  0x8000
#define HDA_RINTCNT   0x5a
#define HDA_RIRBCTL   0x5c    /* u8: bit0 irq, bit1 dma */
#define   RIRBCTL_DMA 0x02
#define   RIRBCTL_IRQ 0x01
#define HDA_RIRBSTS   0x5d    /* u8: bit0 response int, bit2 overrun (W1C) */
#define HDA_RIRBSIZE  0x5e

/* stream descriptor block: SD0 at 0x80, one per 0x20 bytes */
#define HDA_SD_BASE   0x80
#define HDA_SD_STRIDE 0x20

/* Uncached DMA window. Everything the controller's DMA engines touch (the
 * BAR0 registers, the command rings, the position buffer, the BDL and the
 * sample ring) is aliased into one VMM_CD window. The kernel identity map is
 * write-back, and a cached view of DMA memory disagrees with the device on
 * real hardware (QEMU has no caches, so it never notices): verbs would read
 * back as zero and streams would play stale bytes. power.c keeps its ACPI
 * window at 0xFFFFFFFFC0000000 - a different 1 GiB region - so no overlap. */
#define HDA_MMIO_VIRT 0xFFFFFFFFC1000000ULL
#define HDA_WIN_MMIO  0x0000    /* BAR0 alias, up to 3 pages */
#define HDA_WIN_CORB  0x3000
#define HDA_WIN_RIRB  0x4000    /* 2 pages */
#define HDA_WIN_POSB  0x6000
#define HDA_WIN_BDL   0x7000
#define HDA_WIN_RING  0x8000    /* 16 pages = 64 KiB */
#define HDA_WIN_TOTAL 0x18000   /* 96 KiB = 24 pages */

#define SD_CTL        0x00    /* u32: bit0 SRST, bit1 RUN, 23:20 tag */
#define   SD_CTL_SRST 0x01
#define   SD_CTL_RUN  0x02
#define SD_STS        0x03    /* u8: W1C bits 4:2 */
#define SD_LPIB       0x04    /* u32: byte position in the cyclic buffer */
#define SD_CBL        0x08    /* u32: cyclic buffer length */
#define SD_LVI        0x0c    /* u16: last valid BDL index */
#define SD_FMT        0x12    /* u16: stream format */
#define SD_BDLPL      0x18    /* u32: BDL base low */
#define SD_BDLPU      0x1c

/* ---------------- codec verbs ---------------- */
#define V_PARAMETERS      0x0f00
#define V_GET_CONN_LIST   0x0f02
#define V_GET_CONFIG_DEF  0x0f1c
#define V_SET_STREAM_FMT  0x0002
#define V_SET_AMP_GAIN    0x0003
#define V_SET_CONNECT_SEL 0x0701
#define V_SET_POWER_STATE 0x0705
#define V_SET_STREAM_ID   0x0706
#define V_SET_PIN_CTL     0x0707
#define V_SET_EAPD        0x070c
#define V_SET_FUNC_RESET  0x07ff
/* Realtek coefficient registers live behind the vendor widget (nid 0x20):
 * SET_COEF_INDEX picks the register, PROC_COEF reads/writes it. The coef
 * write has a 16-bit payload and therefore goes out in the long (4-bit)
 * verb form. */
#define V_SET_COEF_INDEX  0x0500
#define V_SET_PROC_COEF   0x0004
#define V_GET_PROC_COEF   0x0c00
/* GPIO registers live on the AFG; the data register is 0xF17 and the
 * direction register 0xF16 (0xF15 is the MASK, not data - swapping those
 * made every gpio log line report the wrong register). */
#define V_GET_GPIO_MASK   0x0f15
#define V_GET_GPIO_DIR    0x0f16
#define V_GET_GPIO_DATA   0x0f17
#define V_SET_GPIO_MASK   0x0715
#define V_SET_GPIO_DIR    0x0716
#define V_SET_GPIO_DATA   0x0717
/* read-back verbs for the analog path verification log */
#define V_GET_POWER_STATE 0x0f05
#define V_GET_AMP_GAIN    0x000b    /* long form: payload picks out/in, L/R, idx */
#define V_GET_PIN_CTL     0x0f07
#define V_GET_EAPD        0x0f0c

#define PAR_VENDOR_ID     0x00
#define PAR_REV_ID        0x02
#define PAR_NODE_COUNT    0x04
#define PAR_FUNC_TYPE     0x05
#define PAR_WIDGET_CAP    0x09
#define PAR_CONNLIST_LEN  0x0e
#define PAR_AMP_OUT_CAP   0x12
#define PAR_AMP_IN_CAP    0x0d
#define PAR_PIN_CAP       0x0c
#define PAR_GPIO_CAP      0x11

#define WID_AUD_OUT   0x0
#define WID_AUD_IN    0x1
#define WID_AUD_MIX   0x2
#define WID_AUD_SEL   0x3
#define WID_PIN       0x4

#define GRP_AUDIO_FUNC 0x01

#define WCAP_OUT_AMP   (1u << 2)
#define WCAP_POWER     (1u << 10)
#define WCAP_TYPE(v)   (((v) >> 20) & 0x0f)
#define PINCAP_OUT     (1u << 4)
#define PINCAP_EAPD    (1u << 16)
#define PIN_CTL_OUT_EN (1u << 6)

#define AMPCAP_MUTE    (1u << 31)
#define AMPCAP_OFFSET  (0x7f << 0)
#define AMPCAP_STEPS   (0x7f << 8)
#define AMP_SET_OUTPUT (1u << 15)
#define AMP_SET_INPUT  (1u << 14)
#define AMP_SET_LEFT   (1u << 13)
#define AMP_SET_RIGHT  (1u << 12)
#define AMP_SET_IDX(v) (((v) & 0xf) << 8)
#define AMP_MUTE       (1u << 7)

#define DEFCFG_PORT_CONN(v) (((v) >> 30) & 0x3)     /* 0 jack, 1 no connection */
#define DEFCFG_DEVICE(v)    (((v) >> 20) & 0x0f)    /* 0 line out, 1 speaker */

/* ---------------- ring geometry ----------------
 * The cyclic buffer must be a multiple of 128 bytes per BDL entry; 32 slots
 * of 512 stereo frames is 64 KiB and ~371 ms of playback. */
#define HDA_NBUF      32
#define HDA_FRAMES    512
#define HDA_SLOT      (HDA_FRAMES * 4)
#define HDA_RING      (HDA_NBUF * HDA_SLOT)
#define HDA_CORB_LEN  256
#define HDA_RIRB_LEN  256

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} __attribute__((packed)) hda_bdle_t;

/* ---------------- state ---------------- */
static volatile uint8_t* mmio;
#ifndef HDA_HOST_TEST
/* physical bases of the DMA buffers inside the uncached window (lazy, kept
 * for the lifetime of the kernel - every controller retry reuses them) */
static uint64_t corb_phys, rirb_phys, posb_phys, bdl_phys, ring_phys;
static uint32_t* corb;
static uint64_t* rirb;
static int       dma_ready;
static int       use_immediate;      /* CORB DMA dead -> immediate verbs */
static uint16_t corb_wp;
static uint16_t rirb_last;
/* register snapshot of the last verb timeout, folded into hda_err */
static char verb_snap[40] = "rp=?";
#endif
static int      hda_ok;                 /* controller + codec path ready */

static uint8_t  cad;                    /* codec address */
static uint16_t vendor, device;
static uint8_t  afg;                    /* audio function group nid */
static uint8_t  dac, pin, sel;          /* output path */
static int      sel_idx;                /* selector connection index */
static int      pin_eapd;
static char     codec_name[32];

static int      sd_index;               /* stream descriptor used */
static uint8_t  stream_tag;
static uint8_t* ring;                   /* 128-byte aligned cyclic buffer */
static hda_bdle_t* bdl;
#ifdef HDA_HOST_TEST
static int16_t* ring_raw;               /* allocation base */
static hda_bdle_t* bdl_raw;
#endif
static uint32_t wpos;                   /* writer byte offset in the ring */
static int      lpib_logged;            /* one-shot DMA health check in hda_write */
static int      running;                /* our run-bit shadow */
static int      was_paused;
static uint64_t frames_written;
static uint16_t cur_fmt;                /* stream format of the open stream */

/* ---------------- MMIO ---------------- */
static uint8_t  r8 (uint32_t off) { return *(volatile uint8_t* )(uintptr_t)((uintptr_t)mmio + off); }
static uint16_t r16(uint32_t off) { return *(volatile uint16_t*)(uintptr_t)((uintptr_t)mmio + off); }
static uint32_t r32(uint32_t off) { return *(volatile uint32_t*)(uintptr_t)((uintptr_t)mmio + off); }
static void w8 (uint32_t off, uint8_t v)  { *(volatile uint8_t* )(uintptr_t)((uintptr_t)mmio + off) = v; }
static void w16(uint32_t off, uint16_t v) { *(volatile uint16_t*)(uintptr_t)((uintptr_t)mmio + off) = v; }
static void w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(uintptr_t)((uintptr_t)mmio + off) = v; }
static uint32_t sd_reg(int sd) { return HDA_SD_BASE + (uint32_t)sd * HDA_SD_STRIDE; }

#ifdef HDA_HOST_TEST
/* the host test has no VMM window, so it keeps plain heap allocations */
static void* alloc_aligned(size_t size, size_t align, void** raw) {
    uint8_t* p = (uint8_t*)kmalloc(size + align);
    if (p == NULL) return NULL;
    *raw = p;
    return (void*)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
}
#else
/* map a physical range into the uncached window */
static void hda_map_win(uint64_t win_off, uint64_t phys, size_t bytes) {
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        vmm_map_page(HDA_MMIO_VIRT + win_off + off, phys + off,
                     VMM_PRESENT | VMM_WRITABLE | VMM_CD);
    }
}

/* One-time allocation + uncached mapping of every DMA buffer. The rings and
 * the sample ring must be RAM the controller AND the CPU agree on, so they
 * live in the VMM_CD window like the BAR itself; their PHYSICAL addresses
 * are what the controller is programmed with. */
static int hda_dma_bufs(void) {
    if (dma_ready) return 0;
    corb_phys = pmm_alloc_pages(1);
    rirb_phys = pmm_alloc_pages(2);
    posb_phys = pmm_alloc_pages(1);
    bdl_phys  = pmm_alloc_pages(1);
    ring_phys = pmm_alloc_pages(HDA_RING / PAGE_SIZE);
    if (corb_phys == 0 || rirb_phys == 0 || posb_phys == 0 ||
        bdl_phys == 0 || ring_phys == 0) {
        return -1;
    }
    hda_map_win(HDA_WIN_CORB, corb_phys, PAGE_SIZE);
    hda_map_win(HDA_WIN_RIRB, rirb_phys, 2 * PAGE_SIZE);
    hda_map_win(HDA_WIN_POSB, posb_phys, PAGE_SIZE);
    hda_map_win(HDA_WIN_BDL,  bdl_phys,  PAGE_SIZE);
    hda_map_win(HDA_WIN_RING, ring_phys, HDA_RING);
    corb = (uint32_t*)(uintptr_t)(HDA_MMIO_VIRT + HDA_WIN_CORB);
    rirb = (uint64_t*)(uintptr_t)(HDA_MMIO_VIRT + HDA_WIN_RIRB);
    ring = (uint8_t*)(uintptr_t)(HDA_MMIO_VIRT + HDA_WIN_RING);
    bdl  = (hda_bdle_t*)(uintptr_t)(HDA_MMIO_VIRT + HDA_WIN_BDL);
    memset(corb, 0, HDA_CORB_LEN * 4);
    memset(rirb, 0, HDA_RIRB_LEN * 8);
    memset(ring, 0, HDA_RING);
    memset(bdl, 0, sizeof(hda_bdle_t) * HDA_NBUF);
    dma_ready = 1;
    return 0;
}
#endif

/* ---------------- CORB/RIRB command interface ---------------- */
#ifndef HDA_HOST_TEST
/* Immediate command interface (ICW/IRR/ICS at 0x60/0x64/0x68): one verb per
 * round trip, no DMA involved. Optional per the spec but implemented by every
 * real chipset - the escape hatch when the CORB DMA engine stays dead. */
static int hda_cmd_imm(uint32_t cmd, uint32_t* resp) {
    for (int i = 0; i < 2000 && (r16(HDA_ICIS) & 0x1); i++) { }
    if (r16(HDA_ICIS) & 0x1) return -1;
    w32(HDA_ICW, cmd);
    for (int i = 0; i < 200000; i++) {
        uint16_t st = r16(HDA_ICIS);
        if (!(st & 0x1)) {
            if (st & 0x2) {
                uint32_t r = r32(HDA_IRR);
                w16(HDA_ICIS, 0x2);            /* IRV: write 1 to clear */
                if (resp != NULL) *resp = r;
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

static int hda_cmd(uint32_t cmd, uint32_t* resp) {
    if (use_immediate) return hda_cmd_imm(cmd, resp);

    w8(HDA_CORBSTS, 0x01);      /* W1C: a stale memory error stalls CORB DMA */
    corb_wp = (uint16_t)((corb_wp + 1) % HDA_CORB_LEN);
    corb[corb_wp] = cmd;
    w16(HDA_CORBWP, corb_wp);

    for (int i = 0; i < 200000; i++) {
        uint16_t wp = (uint16_t)(r16(HDA_RIRBWP) & 0xff);
        if (wp != rirb_last) {
            rirb_last = wp;
            uint64_t entry = rirb[wp];
            /* ack: clears the response-int status, which is also what lets the
             * controller hand the next CORB entry over */
            w8(HDA_RIRBSTS, 0x05);
            if (resp != NULL) *resp = (uint32_t)entry;
            return 0;
        }
    }
    hlog("[hda] command timeout (codec not answering)\n");
    {
        char b[160];
        ksprintf(b, sizeof(b),
                 "[hda] corbrp=%x corbwp=%x rirbwp=%x corbsts=%x rirbctl=%x gctl=%x cmd=%x\n",
                 (unsigned)r16(HDA_CORBRP), (unsigned)r16(HDA_CORBWP),
                 (unsigned)r16(HDA_RIRBWP), (unsigned)r8(HDA_CORBSTS),
                 (unsigned)r8(HDA_RIRBCTL), (unsigned)r32(HDA_GCTL), (unsigned)cmd);
        hlog(b);
        /* short snapshot for the Files error dialog (the only text a real
         * machine's user can photograph without scrolling the klog):
         * rp advancing = the controller DMA-fetched the command, rb advancing
         * = the codec answered, cs bit0 = CORB DMA memory error */
        ksprintf(verb_snap, sizeof(verb_snap), "rp=%x wp=%x rb=%x cs=%x",
                 (unsigned)r16(HDA_CORBRP), (unsigned)r16(HDA_CORBWP),
                 (unsigned)(r16(HDA_RIRBWP) & 0xff), (unsigned)r8(HDA_CORBSTS));
    }
    /* CORB DMA dead on this board? Try the same verb without any DMA. */
    if (hda_cmd_imm(cmd, resp) == 0) {
        use_immediate = 1;
        hlog("[hda] immediate command interface answers, using it for verbs\n");
        return 0;
    }
    return -1;
}
#else
static int hda_cmd(uint32_t cmd, uint32_t* resp);   /* supplied by the test */
#endif

/* verbs with a 16-bit payload use the long form (4-bit verb id): 2 set
 * converter format, 3 set amp, 4/5 coef, a/b/c/d the matching getters */
static int verb_long(uint32_t verb) {
    return (verb & 0xf00) == 0 && verb != 0;
}

static uint32_t mk_cmd(uint8_t c, uint8_t nid, uint32_t v, uint32_t payload) {
    if (verb_long(v)) return ((uint32_t)c << 28) | ((uint32_t)nid << 20) | ((v & 0xf) << 16) | (payload & 0xffff);
    return ((uint32_t)c << 28) | ((uint32_t)nid << 20) | ((v & 0xfff) << 8) | (payload & 0xff);
}

static int hda_verb(uint8_t c, uint8_t nid, uint32_t v, uint32_t payload, uint32_t* resp) {
    return hda_cmd(mk_cmd(c, nid, v, payload), resp);
}

static uint32_t hda_param(uint8_t c, uint8_t nid, uint32_t pid) {
    uint32_t v = 0;
    hda_verb(c, nid, V_PARAMETERS, pid, &v);
    return v;
}

/* ---------------- Realtek coefficient registers ---------------- */
#define ALC_VENDOR_NID 0x20

static int alc_coef_read(uint8_t c, uint8_t idx, uint32_t* val) {
    uint32_t v = 0;
    if (hda_verb(c, ALC_VENDOR_NID, V_SET_COEF_INDEX, idx, NULL) != 0) return -1;
    if (hda_verb(c, ALC_VENDOR_NID, V_GET_PROC_COEF, 0, &v) != 0) return -1;
    *val = v & 0xffff;
    return 0;
}

static int alc_coef_write(uint8_t c, uint8_t idx, uint16_t val) {
    if (hda_verb(c, ALC_VENDOR_NID, V_SET_COEF_INDEX, idx, NULL) != 0) return -1;
    return hda_verb(c, ALC_VENDOR_NID, V_SET_PROC_COEF, val, NULL);
}

/* ---------------- widget bookkeeping ---------------- */
typedef struct {
    uint8_t nid;
    uint8_t type;
    int     in_idx;        /* connection index we descend through */
} path_node_t;

static path_node_t path[8];
static int path_len;

/* connection list: bit7 of the length parameter selects 16-bit entries */
static int hda_conn_len(uint8_t c, uint8_t nid, int* longform) {
    uint32_t v = hda_param(c, nid, PAR_CONNLIST_LEN);
    *longform = (int)((v >> 7) & 1);
    return (int)(v & 0x7f);
}

static uint8_t hda_conn_get(uint8_t c, uint8_t nid, int idx, int longform) {
    uint32_t v = 0;
    hda_verb(c, nid, V_GET_CONN_LIST, (uint32_t)idx, &v);
    if (longform) return (uint8_t)((v >> ((idx & 1) * 16)) & 0xffff);
    return (uint8_t)((v >> ((idx & 3) * 8)) & 0xff);
}

static int hda_widget_type(uint8_t c, uint8_t nid) {
    return (int)WCAP_TYPE(hda_param(c, nid, PAR_WIDGET_CAP));
}

/* depth-first from a pin (or mixer) down to the first output converter */
static int hda_find_dac(uint8_t c, uint8_t nid, int depth) {
    if (depth >= 6 || path_len >= (int)(sizeof(path) / sizeof(path[0]))) return 0;

    int longform = 0;
    int len = hda_conn_len(c, nid, &longform);
    for (int i = 0; i < len; i++) {
        uint8_t child = hda_conn_get(c, nid, i, longform);
        if (child == 0) continue;
        int type = hda_widget_type(c, child);

        if (type == WID_AUD_OUT) {
            path[path_len].nid = child;
            path[path_len].type = WID_AUD_OUT;
            path[path_len].in_idx = i;
            path_len++;
            return 1;
        }
        if (type == WID_AUD_MIX || type == WID_AUD_SEL) {
            int mark = path_len;
            path[path_len].nid = child;
            path[path_len].type = (uint8_t)type;
            path[path_len].in_idx = i;
            path_len++;
            if (hda_find_dac(c, child, depth + 1)) return 1;
            path_len = mark;
        }
    }
    return 0;
}

/* ---------------- codec setup ---------------- */
static void hda_set_amp(uint8_t c, uint8_t nid, int in_amp, int idx, uint32_t caps) {
    /* 0 dB sits at the offset step (see HDA 1.0a 7.3.4.10). A widget reports
     * amp capabilities even when it has no gain steps at all - the ALC662's
     * line-out pin is "mute only" (AMPCAP_MUTE set, AMPCAP_STEPS == 0). After a
     * codec reset such an amp powers up MUTED, so the mute bit still has to be
     * cleared (with gain = offset = 0) or the jack stays silent while every
     * other verb reads back as correct. Only widgets with no amp caps at all
     * (caps == 0) are skipped. */
    if (caps == 0) return;
    uint32_t gain = (caps & AMPCAP_OFFSET) & 0x7f;
    uint32_t payload = AMP_SET_LEFT | AMP_SET_RIGHT | AMP_SET_IDX(idx) | (gain & 0x7f);
    payload |= in_amp ? AMP_SET_INPUT : AMP_SET_OUTPUT;
    hda_verb(c, nid, V_SET_AMP_GAIN, payload, NULL);
}

static void hda_widget_power(uint8_t c, uint8_t nid) {
    uint32_t caps = hda_param(c, nid, PAR_WIDGET_CAP);
    if (caps & WCAP_POWER) hda_verb(c, nid, V_SET_POWER_STATE, 0 /* D0 */, NULL);
}

static const char* hex4(uint16_t v, char* buf) {
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) buf[i] = d[(v >> ((3 - i) * 4)) & 0xf];
    buf[4] = '\0';
    return buf;
}

/* Realtek ALC model names for the parts the HDA device ids spell out.
 * ALC662-VD / ALC662-VD3 share device id 0x0662 with the plain ALC662 and are
 * told apart only by the revision id, which the probe logs next to the name.
 * Anything not listed falls back to ALC<device id>. */
typedef struct { uint16_t id; const char* name; } alc_model_t;
static const alc_model_t alc_models[] = {
    { 0x0662, "ALC662" },   /* also ALC662-VD / ALC662-VD3 */
    { 0x0663, "ALC663" }, { 0x0665, "ALC665" }, { 0x0668, "ALC668" },
    { 0x0670, "ALC670" }, { 0x0671, "ALC671" }, { 0x0861, "ALC861" },
    { 0x0862, "ALC861-VD" }, { 0x0880, "ALC880" }, { 0x0882, "ALC882" },
    { 0x0883, "ALC883" }, { 0x0885, "ALC885" }, { 0x0886, "ALC886" },
    { 0x0887, "ALC887" }, { 0x0888, "ALC888" }, { 0x0889, "ALC889" },
    { 0x0892, "ALC892" }, { 0x0898, "ALC898" }, { 0x0900, "ALC1150" },
    { 0x1220, "ALC1220" }, { 0x0262, "ALC262" }, { 0x0268, "ALC268" },
    { 0x0269, "ALC269" }, { 0x0272, "ALC272" }, { 0x0282, "ALC282" },
    { 0x0256, "ALC256" }, { 0x0295, "ALC295" },
};

static const char* alc_model_name(uint16_t id) {
    for (int i = 0; i < (int)(sizeof(alc_models) / sizeof(alc_models[0])); i++) {
        if (alc_models[i].id == id) return alc_models[i].name;
    }
    return NULL;
}

static const char* hda_pin_dev_name(int dev) {
    switch (dev) {
        case 0x0: return "line-out";
        case 0x1: return "speaker";
        case 0x2: return "hp-out";
        case 0x3: return "cd";
        case 0x4: return "spdif-out";
        case 0x5: return "dig-out";
        case 0x8: return "line-in";
        case 0xa: return "mic";
        default:  return "other";
    }
}

/* an output pin found during the walk, with the DAC path behind it */
typedef struct {
    uint8_t     pin;
    int         dev;                /* default config device type */
    int         assoc;              /* default association (bits 7:4) */
    int         score;
    int         path_len;
    path_node_t path[8];
} hda_out_t;

static hda_out_t outs[8];
static int       out_count;
static uint8_t   dacs[8];           /* converters that carry the open stream */
static int       dac_count;
static int       primary_assoc;
/* How picky the output-pin search is; hda_try_controller loosens it when a
 * codec yields nothing: 2 = analog pin with a physical connection, 1 = analog
 * pin regardless of connectivity, 0 = accept any output pin (HDMI included).
 * Firmware pin configuration is unreliable in the field, so the last two
 * levels exist to still find a jack a board forgot to describe. */
static int       probe_policy = 2;
static int       saw_out_pin;       /* a pin advertised output, path or not */
static uint16_t  seen_vendor, seen_device;

static void hda_add_dac(uint8_t nid) {
    for (int i = 0; i < dac_count; i++) {
        if (dacs[i] == nid) return;
    }
    if (dac_count < (int)(sizeof(dacs))) dacs[dac_count++] = nid;
}

/* power + unmute a pin's whole DAC path: mixers get their selected input amp
 * unmuted and selectors their connect point set. The input index of a MIX/SEL
 * node is the index of its CHILD in the mixer's own connection list - that is
 * the next node's in_idx, not this node's (which is the node's index in its
 * PARENT's list and means nothing to the mixer's input amps). */
static void hda_setup_path(uint8_t c, const path_node_t* p, int len) {
    for (int k = 0; k < len; k++) {
        hda_widget_power(c, p[k].nid);
        hda_set_amp(c, p[k].nid, 0, 0, hda_param(c, p[k].nid, PAR_AMP_OUT_CAP));
        if (p[k].type == WID_AUD_MIX && k + 1 < len) {
            hda_set_amp(c, p[k].nid, 1, p[k + 1].in_idx,
                        hda_param(c, p[k].nid, PAR_AMP_IN_CAP));
        }
        if (p[k].type == WID_AUD_SEL && k + 1 < len) {
            hda_verb(c, p[k].nid, V_SET_CONNECT_SEL, (uint32_t)p[k + 1].in_idx, NULL);
        }
    }
}

static void hda_path_str(char* buf, size_t n, const hda_out_t* o) {
    size_t p = 0;
    buf[0] = '\0';
    for (int k = 0; k < o->path_len; k++) {
        char nb[16];
        ksprintf(nb, sizeof(nb), "%s0x%02x", k ? " <- " : "", o->path[k].nid);
        for (char* s = nb; *s != '\0' && p + 1 < n; s++) buf[p++] = *s;
    }
    buf[p] = '\0';
}

/* Walk one codec: find its AFG, every output pin that reaches a DAC, unmute
 * the primary path and every sibling pin in the same default association. */

/* Audio function group lookup. Codecs differ in how (and whether) they report
 * their subordinate nodes, so try the parameter both ways and finally every
 * plausible nid - nid 1 is the AFG on essentially every codec in the field. */
static int hda_find_afg(uint8_t c) {
    uint32_t nc = hda_param(c, 0, PAR_NODE_COUNT);
    /* HDA 1.0a: subordinate node count is [15:0] = START nid, [31:16] = total
     * count. QEMU's codec happens to have start == count, which hid an older
     * reversed read of this field - on a real ALC the wrong order walks a
     * couple of high nids (beep/SPDIF) and misses every jack pin. */
    int ranges[3][2] = {
        { (int)(nc & 0xffff), (int)(nc >> 16) },
        { (int)(nc & 0xff), (int)((nc >> 8) & 0xff) },
        { 1, 15 },
    };
    for (int r = 0; r < 3; r++) {
        int first = ranges[r][0], count = ranges[r][1];
        if (count <= 0 || count > 64) continue;
        for (int i = 0; i < count; i++) {
            uint8_t nid = (uint8_t)(first + i);
            if (nid == 0) continue;
            if ((hda_param(c, nid, PAR_FUNC_TYPE) & 0xff) == GRP_AUDIO_FUNC) return nid;
        }
    }
    return 0;
}

/* Collect the output pins of a node range (see the probe_policy comment). */
static void hda_collect_outs(uint8_t c, int first, int count) {
    if (count <= 0 || count > 64) return;
    for (int i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(first + i);
        uint32_t wcaps = hda_param(c, nid, PAR_WIDGET_CAP);
        if (WCAP_TYPE(wcaps) != WID_PIN) continue;
        uint32_t pcaps = hda_param(c, nid, PAR_PIN_CAP);
        if (!(pcaps & PINCAP_OUT)) continue;
        saw_out_pin = 1;

        uint32_t cfg = 0;
        hda_verb(c, nid, V_GET_CONFIG_DEF, 0, &cfg);
        int dev = (int)DEFCFG_DEVICE(cfg);
        int conn = (int)DEFCFG_PORT_CONN(cfg);
        int ok = (probe_policy <= 1 || conn != 0x1) &&      /* 1 = no connection */
                 (probe_policy == 0 || dev <= 0x2);         /* analog outputs */
        path_len = 0;
        if (ok) ok = hda_find_dac(c, nid, 0);               /* converter behind it */
        if (!ok) {
            if (probe_policy == 0) {
                /* last attempt: leave a trace of why every pin was rejected */
                char b[160];
                int lf = 0;
                int clen = hda_conn_len(c, nid, &lf);
                uint32_t c0 = 0, c1 = 0;
                if (clen > 0) (void)hda_conn_get(c, nid, 0, lf);
                hda_verb(c, nid, V_GET_CONN_LIST, 0, &c0);
                hda_verb(c, nid, V_GET_CONN_LIST, 1, &c1);
                ksprintf(b, sizeof(b),
                         "[hda] skip pin 0x%02x dev=%x conn=%d caps=%x clen=%d conns=%x,%x\n",
                         (int)nid, (unsigned)dev, conn, (unsigned)pcaps, clen,
                         (unsigned)c0, (unsigned)c1);
                hlog(b);
            }
            continue;
        }
        if (out_count >= (int)(sizeof(outs))) return;

        hda_out_t* o = &outs[out_count];
        o->pin = nid;
        o->dev = dev;
        o->assoc = (int)((cfg >> 4) & 0xf);
        o->path_len = path_len;
        for (int k = 0; k < path_len; k++) o->path[k] = path[k];
        /* front line-out wins, then speaker, then headphone; the primary
         * association (1) is the one the board wires first */
        o->score = (o->dev == 0x0) ? 4 : (o->dev == 0x1) ? 3 : (o->dev == 0x2) ? 2 : 1;
        if (o->assoc == 1) o->score += 1;
        out_count++;
        path_len = 0;
    }
}

static int hda_probe_codec(uint8_t c) {
    /* The root node must answer Get Parameter(Vendor ID) - if even that times
     * out the verb path itself is dead, and grinding through the other codec
     * addresses (and probe policies) only burns a timeout each. -2 = link
     * dead, stops the whole probe for this controller. */
    uint32_t ven = 0;
    if (hda_verb(c, 0, V_PARAMETERS, PAR_VENDOR_ID, &ven) != 0) return -2;
    vendor = (uint16_t)(ven >> 16);
    device = (uint16_t)(ven & 0xffff);
    /* remember the most interesting codec seen, for the failure message:
     * first one that answers at all, upgraded to a Realtek if there is one
     * (an ALC is what the user cares about, a GPU HDMI codec is not) */
    if (vendor != 0 && (seen_vendor == 0 || (vendor == 0x10ec && seen_vendor != 0x10ec))) {
        seen_vendor = vendor;
        seen_device = device;
    }
    uint32_t rev = hda_param(c, 0, PAR_REV_ID);

    afg = (uint8_t)hda_find_afg(c);
    if (afg == 0) return -1;

    hda_verb(c, afg, V_SET_FUNC_RESET, 0, NULL);
    pit_delay_ms(2);
    hda_widget_power(c, afg);            /* reset lands in D3 on power-capped AFGs */

    /* Linux alc_fill_eapd_coef(): on the ALC662 rev3 the EAPD output is gated
     * by coefficient register 0x04 bit 10 - while it is set the chip ignores
     * SET_EAPD verbs entirely, so the line-out amplifier never powers up
     * (silent green jack on real boards; QEMU's codec has no such gate, which
     * is why acceptance audio passed there). Clearing the bit hands EAPD to
     * verb control; the sibling 66x parts want coef 0x0d bit 14 SET instead.
     * Runs after FUNC_RESET because the reset restores power-on defaults. */
    uint32_t coef0 = 0, coef4 = 0;
    if (vendor == 0x10ec && device == 0x0662 &&
        alc_coef_read(c, 0x00, &coef0) == 0 && ((coef0 >> 4) & 0xf) == 0x3 &&
        alc_coef_read(c, 0x04, &coef4) == 0) {
        uint32_t clr = coef4 & ~(1u << 10);
        if (alc_coef_write(c, 0x04, (uint16_t)clr) == 0) {
            char b[80];
            ksprintf(b, sizeof(b),
                     "[hda] ALC662 rev3: coef4 %04x->%04x, EAPD under verb control\n",
                     (unsigned)(coef4 & 0xffff), (unsigned)(clr & 0xffff));
            hlog(b);
            coef4 = clr;
        }
    } else if (vendor == 0x10ec &&
               (device == 0x0272 || device == 0x0273 || device == 0x0663 ||
                device == 0x0665 || device == 0x0670 || device == 0x0671 ||
                device == 0x0672)) {
        uint32_t v = 0;
        if (alc_coef_read(c, 0x0d, &v) == 0) {
            alc_coef_write(c, 0x0d, (uint16_t)(v | (1u << 14)));
        }
    }

    /* Widget ranges: the AFG's node count ([15:0] start, [31:16] total), read
     * the other way round, and finally every plausible widget nid. The first
     * range that yields an output pin wins, so a codec that reports its range
     * oddly still works. */
    uint32_t nc = hda_param(c, afg, PAR_NODE_COUNT);
    int ranges[3][2] = {
        { (int)(nc & 0xffff), (int)(nc >> 16) },
        { (int)(nc & 0xff), (int)((nc >> 8) & 0xff) },
        { 2, 0x3e },
    };
    out_count = 0;
    for (int r = 0; r < 3 && out_count == 0; r++) {
        hda_collect_outs(c, ranges[r][0], ranges[r][1]);
    }
    if (out_count == 0) return -1;

    int best = 0;
    for (int i = 1; i < out_count; i++) {
        if (outs[i].score > outs[best].score) best = i;
    }
    pin = outs[best].pin;
    primary_assoc = outs[best].assoc;
    dac = outs[best].path[outs[best].path_len - 1].nid;
    pin_eapd = (hda_param(c, pin, PAR_PIN_CAP) & PINCAP_EAPD) ? 1 : 0;
    sel = 0; sel_idx = 0;
    for (int k = 0; k < outs[best].path_len - 1; k++) {
        if (outs[best].path[k].type == WID_AUD_SEL) {
            sel = outs[best].path[k].nid;
            sel_idx = outs[best].path[k + 1].in_idx;   /* child's index, see hda_setup_path */
        }
    }

    /* unmute the primary path and open its jack. The PIN ITSELF needs power
     * too - hda_setup_path only covers the mixer/DAC nodes behind it, and a
     * pin left in D3 by firmware stays silent no matter what else is set. */
    hda_setup_path(c, outs[best].path, outs[best].path_len);
    hda_widget_power(c, pin);
    hda_verb(c, pin, V_SET_PIN_CTL, PIN_CTL_OUT_EN, NULL);
    hda_set_amp(c, pin, 0, 0, hda_param(c, pin, PAR_AMP_OUT_CAP));
    /* EAPD powers the external amplifier on most Realtek ALC parts (the
     * ALC662 included) - without it the jack stays silent */
    if (pin_eapd) hda_verb(c, pin, V_SET_EAPD, 0x02, NULL);

    /* EAPD goes to EVERY output pin regardless of association - boards share
     * the jack amplifier across ports and Linux's alc_auto_setup_eapd does
     * the same. Path setup, pin power and the stream stay inside the primary
     * association: one stream feeds only the DACs assigned to it. */
    dac_count = 0;
    hda_add_dac(dac);
    for (int i = 0; i < out_count; i++) {
        if (i == best) continue;
        if (hda_param(c, outs[i].pin, PAR_PIN_CAP) & PINCAP_EAPD) {
            hda_verb(c, outs[i].pin, V_SET_EAPD, 0x02, NULL);
        }
        if (outs[i].assoc != primary_assoc) continue;
        hda_setup_path(c, outs[i].path, outs[i].path_len);
        hda_widget_power(c, outs[i].pin);
        hda_verb(c, outs[i].pin, V_SET_PIN_CTL, PIN_CTL_OUT_EN, NULL);
        hda_set_amp(c, outs[i].pin, 0, 0, hda_param(c, outs[i].pin, PAR_AMP_OUT_CAP));
        hda_add_dac(outs[i].path[outs[i].path_len - 1].nid);
    }

    /* GPIO fallback. The controller link reset AND the function-group reset
     * above both wipe the codec's GPIO registers to power-on defaults, and on
     * boards that wire a jack/speaker amplifier (or the analog-supply switch)
     * to a codec GPIO line that leaves the amplifier unpowered no matter how
     * right every verb reads back. Drive every GPIO line the codec exposes
     * high - the usual active-high amp enable; the before/after values go to
     * the log so a board that needs the opposite polarity is easy to spot. */
    {
        uint32_t gcap = hda_param(c, afg, PAR_GPIO_CAP) & 0xff;
        uint32_t gdir = 0, gdat = 0, all;
        if (gcap > 8) gcap = 8;
        all = gcap ? ((1u << gcap) - 1) : 0;
        if (all != 0) {
            hda_verb(c, afg, V_GET_GPIO_DIR, 0, &gdir);
            hda_verb(c, afg, V_GET_GPIO_DATA, 0, &gdat);
            gdir &= 0xff;
            gdat &= 0xff;
            if (gdir != all || gdat != all) {
                hda_verb(c, afg, V_SET_GPIO_MASK, all, NULL);
                hda_verb(c, afg, V_SET_GPIO_DIR, all, NULL);
                hda_verb(c, afg, V_SET_GPIO_DATA, all, NULL);
                char b[96];
                ksprintf(b, sizeof(b),
                         "[hda] gpio %u line(s): dir %x->%x data %x->%x (amp enable)\n",
                         (unsigned)gcap, (unsigned)gdir, (unsigned)all,
                         (unsigned)gdat, (unsigned)all);
                hlog(b);
            }
        }
    }

    /* ---- report what was found: enough to triage a board from the log ---- */
    if (vendor == 0x10ec) {
        const char* n = alc_model_name(device);
        uint32_t r = (rev >> 8) & 0xff;
        if (n != NULL && r >= 1 && r <= 9) {
            ksprintf(codec_name, sizeof(codec_name), "Realtek %s rev%u", n, (unsigned)r);
        } else if (n != NULL) {
            ksprintf(codec_name, sizeof(codec_name), "Realtek %s", n);
        } else {
            char b[8];
            ksprintf(codec_name, sizeof(codec_name), "Realtek ALC%s", hex4(device, b));
        }
    } else {
        char b[8];
        ksprintf(codec_name, sizeof(codec_name), "codec %x:%s", vendor, hex4(device, b));
    }
    {
        char buf[112];
        ksprintf(buf, sizeof(buf), "[hda] %s rev=%x cad=%d afg=%d (%d out pins)\n",
                 codec_name, (unsigned)rev, (int)c, (int)afg, out_count);
        hlog(buf);
    }
    {
        /* full triage data for the primary pin - logged FIRST, before the out
         * list, because the Files preview shows only the first lines of
         * hda.log and on a headless real machine these two rows are the whole
         * amp story: eapd/outamp/pwr = capability bits, coef0/coef4 = the
         * EAPD-gate registers (rev3 unlocks silence), cfg bits 15:12 give the
         * jack color (3=blue 4=green 9=pink), gpio = AFG GPIO state (a GPIO-
         * controlled amp is the next thing to look at when everything else
         * reads fine) */
        uint32_t pcaps = hda_param(c, pin, PAR_PIN_CAP);
        uint32_t oamp = hda_param(c, pin, PAR_AMP_OUT_CAP);
        uint32_t cfg = 0, gdir = 0, gdat = 0;
        hda_verb(c, pin, V_GET_CONFIG_DEF, 0, &cfg);
        hda_verb(c, afg, V_GET_GPIO_DIR, 0, &gdir);
        hda_verb(c, afg, V_GET_GPIO_DATA, 0, &gdat);
        char buf[112];
        ksprintf(buf, sizeof(buf),
                 "[hda] primary pin 0x%02x: eapd=%d outamp=%d(mute=%d) pwr=%d wcaps=%x pcaps=%x oamp=%x\n",
                 (int)pin, pin_eapd, (int)((oamp & AMPCAP_STEPS) ? 1 : 0),
                 (int)((oamp & AMPCAP_MUTE) ? 1 : 0),
                 (int)((hda_param(c, pin, PAR_WIDGET_CAP) & WCAP_POWER) ? 1 : 0),
                 (unsigned)hda_param(c, pin, PAR_WIDGET_CAP), (unsigned)pcaps,
                 (unsigned)oamp);
        hlog(buf);
        ksprintf(buf, sizeof(buf),
                 "[hda] pin 0x%02x cfg=%08x coef0=%04x coef4=%04x gpio cap=%x dir=%x dat=%x\n",
                 (int)pin, (unsigned)cfg, (unsigned)(coef0 & 0xffff),
                 (unsigned)(coef4 & 0xffff), (unsigned)hda_param(c, afg, PAR_GPIO_CAP),
                 (unsigned)(gdir & 0xffff), (unsigned)(gdat & 0xffff));
        hlog(buf);
    }
    {
        /* verb read-back: prove every write of the analog path actually took.
         * A pin left out-disabled, an amp that ignored its gain verb or an
         * EAPD that failed to latch show up here instead of hiding behind a
         * silent green jack with an otherwise perfect probe log. amp fields:
         * bit7 = mute, bits 6:0 = gain. */
        uint32_t pctl = 0, eapd = 0, pwr = 0, damp = 0, miamp = 0, pamp = 0;
        uint8_t mixer = 0;
        int midx = 0;
        for (int k = 0; k + 1 < outs[best].path_len; k++) {
            if (outs[best].path[k].type == WID_AUD_MIX) {
                mixer = outs[best].path[k].nid;
                midx = outs[best].path[k + 1].in_idx;
            }
        }
        hda_verb(c, pin, V_GET_PIN_CTL, 0, &pctl);
        hda_verb(c, pin, V_GET_EAPD, 0, &eapd);
        hda_verb(c, pin, V_GET_POWER_STATE, 0, &pwr);
        hda_verb(c, dac, V_GET_AMP_GAIN, AMP_SET_OUTPUT | AMP_SET_LEFT, &damp);
        hda_verb(c, pin, V_GET_AMP_GAIN, AMP_SET_OUTPUT | AMP_SET_LEFT, &pamp);
        if (mixer != 0) {
            hda_verb(c, mixer, V_GET_AMP_GAIN,
                     AMP_SET_INPUT | AMP_SET_LEFT | AMP_SET_IDX(midx), &miamp);
        }
        char buf[112];
        ksprintf(buf, sizeof(buf),
                 "[hda] verify: pinctl=%02x eapd=%02x pwr=%02x pinamp=%02x dac %02x amp=%02x mix %02x in%d amp=%02x\n",
                 (unsigned)(pctl & 0xff), (unsigned)(eapd & 0xff), (unsigned)(pwr & 0xff),
                 (unsigned)(pamp & 0xff),
                 (unsigned)dac, (unsigned)(damp & 0xff), (unsigned)mixer, midx,
                 (unsigned)(miamp & 0xff));
        hlog(buf);
    }
    for (int i = 0; i < out_count; i++) {
        char pathstr[64], buf[144];
        hda_path_str(pathstr, sizeof(pathstr), &outs[i]);
        ksprintf(buf, sizeof(buf), "[hda] out 0x%02x assoc%u %s <- %s%s\n",
                 outs[i].pin, (unsigned)outs[i].assoc, hda_pin_dev_name(outs[i].dev),
                 pathstr, (i == best) ? "  [primary]" : "");
        hlog(buf);
    }
    {
        char buf[96];
        int n = 0;
        for (int i = 0; i < dac_count && n < (int)sizeof(buf) - 8; i++) {
            char nb[8];
            ksprintf(nb, sizeof(nb), "%s0x%02x", i ? " " : "", dacs[i]);
            for (char* s = nb; *s != '\0' && n < (int)sizeof(buf) - 2; s++) buf[n++] = *s;
        }
        buf[n] = '\0';
        char line[112];
        ksprintf(line, sizeof(line), "[hda] stream dacs:%s\n", buf);
        hlog(line);
    }
    return 0;
}

/* ---------------- controller bring-up ---------------- */
#ifndef HDA_HOST_TEST
/* short reason the probe failed, shown by the Files panel */
static char hda_err[44] = "HDA: no 0403 controller";

const char* hda_last_error(void) { return hda_err; }

static void hda_fail(const char* msg) {
    strncpy(hda_err, msg, sizeof(hda_err) - 1);
    hda_err[sizeof(hda_err) - 1] = '\0';
}

/* Alias a controller's BAR into the uncached window. Probing a second
 * controller overwrites these page-table entries, so a restore after a failed
 * second probe must call this again for the first controller's BAR. */
static uint64_t win_bar;                /* physical base currently aliased */
static void hda_alias_bar(uint64_t base) {
    uint64_t base_page = base & ~0xFFFULL;
    uint64_t off0 = (uint64_t)base - base_page;
    for (uint64_t off = 0; off < 0x2000 + off0; off += PAGE_SIZE) {
        vmm_map_page(HDA_MMIO_VIRT + off, base_page + off,
                     VMM_PRESENT | VMM_WRITABLE | VMM_CD);
    }
    mmio = (volatile uint8_t*)(uintptr_t)(HDA_MMIO_VIRT + off0);
    win_bar = base;
}

/* spill the whole probe log to disk - the klog pane only shows the tail */
static void hlog_dump(void) {
    fs_entry_t* f = fs_resolve_path("/home/user/hda.log");
    if (f == NULL) f = fs_create_file("/home/user/hda.log");
    if (f != NULL && fs_write_file(f, (const uint8_t*)hlog_buf, hlog_n) >= 0) {
        hlog("[hda] probe log saved to /home/user/hda.log\n");
    }
}

static int hda_try_controller(pci_device_t* d);

/* One controller came up but with a non-analog codec (GPU HDMI): its full
 * software state is parked here so later analog-only probes can be rolled
 * back if nothing better is found. The BAR alias needs a re-map on restore
 * because a later hda_try_controller overwrote the window's page-table
 * entries with its own BAR. */
typedef struct {
    volatile uint8_t* mmio;
    uint64_t          bar;
    uint16_t          vendor, device;
    uint8_t           afg, dac, pin, sel;
    int               sel_idx, pin_eapd;
    int               primary_assoc;
    int               sd_index;
    uint8_t           stream_tag;
    int               dac_count;
    uint8_t           dacs[8];
    char              codec_name[32];
} hda_stash_t;

static void hda_stash(hda_stash_t* s) {
    s->mmio = mmio;          s->bar = win_bar;
    s->vendor = vendor;      s->device = device;
    s->afg = afg;            s->dac = dac;
    s->pin = pin;            s->sel = sel;
    s->sel_idx = sel_idx;    s->pin_eapd = pin_eapd;
    s->primary_assoc = primary_assoc;
    s->sd_index = sd_index;  s->stream_tag = stream_tag;
    s->dac_count = dac_count;
    for (int i = 0; i < dac_count && i < (int)(sizeof(s->dacs)); i++) s->dacs[i] = dacs[i];
    int ci = 0;
    for (; ci < (int)sizeof(s->codec_name) - 1 && codec_name[ci] != '\0'; ci++)
        s->codec_name[ci] = codec_name[ci];
    s->codec_name[ci] = '\0';
}

static void hda_restore(const hda_stash_t* s) {
    hda_alias_bar(s->bar);
    mmio = s->mmio;
    vendor = s->vendor;      device = s->device;
    afg = s->afg;            dac = s->dac;
    pin = s->pin;            sel = s->sel;
    sel_idx = s->sel_idx;    pin_eapd = s->pin_eapd;
    primary_assoc = s->primary_assoc;
    sd_index = s->sd_index;  stream_tag = s->stream_tag;
    dac_count = s->dac_count;
    for (int i = 0; i < (int)sizeof(dacs); i++) dacs[i] = (i < s->dac_count) ? s->dacs[i] : 0;
    for (int i = 0; i < (int)sizeof(codec_name); i++) codec_name[i] = s->codec_name[i];
}

int hda_init(void) {
    if (hda_ok) return 0;
    hlog_n = 0;                      /* fresh log per probe round */

    /* A board with a discrete GPU has a second class-0x0403 controller: the
     * graphics card's HD Audio (HDMI output only). Try every controller and
     * keep the first ANALOG codec (the kind wired to the 3.5 mm jacks); a
     * controller that only produced an HDMI codec is parked and only used if
     * no analog one exists, so the chipset codec wins over the graphics one. */
    int ctrls = 0;
    int have_stash = 0;
    hda_stash_t st;
    for (pci_device_t* d = pci_get_device_list(); d != NULL; d = d->next) {
        if (d->class_code != 0x04 || d->subclass_code != 0x03) continue;
        ctrls++;
        if (hda_try_controller(d) != 0) continue;
        if (vendor == 0x10ec) {                  /* analog: drives the jacks */
            have_stash = 0;
            hlog_dump();
            return 0;
        }
        if (!have_stash) { hda_stash(&st); have_stash = 1; }
        hlog("[hda] non-analog codec, keeping it only as a fallback\n");
    }
    if (have_stash) {
        hda_restore(&st);
        hda_ok = 1;
        hlog("[hda] no analog codec anywhere, using the HDMI one\n");
        hlog_dump();
        return 0;
    }
    if (ctrls == 0) {
        ksprintf(hda_err, sizeof(hda_err), "HDA: no 0403 controller");
        hlog("[hda] no class 0x0403 controller in the PCI list\n");
    } else {
        char b[80];
        ksprintf(b, sizeof(b), "[hda] %d controller(s) probed, none usable: %s\n",
                 ctrls, hda_err);
        hlog(b);
    }
    hlog_dump();
    return -1;
}

static int hda_try_controller(pci_device_t* d) {
    char buf[112];

    uint32_t bar0 = pci_read_dword(d->bus, d->device, d->function, 0x10);
    uint32_t bar1 = pci_read_dword(d->bus, d->device, d->function, 0x14);
    if (bar0 & 1) {
        hda_fail("HDA: BAR0 is I/O space");
        hlog("[hda] BAR0 is I/O space\n");
        return -1;
    }
    uint64_t base = bar0 & ~0xfu;
    if ((bar0 & 0x6) == 0x4) base |= (uint64_t)(bar1 & ~0xfu) << 32;   /* 64-bit BAR */
    if (base == 0 || base >= 0x100000000ULL) {
        hda_fail("HDA: BAR above 4 GiB");
        hlog("[hda] BAR above the identity map\n");
        return -1;
    }

    /* memory space + bus master, or no register or DMA access at all */
    uint32_t cmd = pci_read_dword(d->bus, d->device, d->function, 0x04);
    pci_write_dword(d->bus, d->device, d->function, 0x04, cmd | 0x0006);

    /* Alias the BAR into the uncached window: a *cached* read of a status
     * register never sees the device move on real hardware - polling loops
     * (RIRBWP, CIV, LPIB) would spin on a stale cache line forever and every
     * command would look unanswered. power.c does the same for ACPI tables
     * above 4 GiB. */
    hda_alias_bar(base);

    /* fresh command interface per controller: a fallback that switched the
     * PREVIOUS controller to immediate-mode verbs must not leak into this one */
    use_immediate = 0;

    ksprintf(buf, sizeof(buf), "[hda] controller %02x:%02x.%x bar=%x cmdr=%x gcap=%x\n",
             (unsigned)d->bus, (unsigned)d->device, (unsigned)d->function,
             (unsigned)base, (unsigned)pci_read_dword(d->bus, d->device, d->function, 0x04),
             (unsigned)r16(HDA_GCAP));
    hlog(buf);

    /* Reset the link and look for codecs. CRST=0 holds the WHOLE controller in
     * reset: R/W registers read their defaults and silently drop writes (that
     * is why a real board reported rp=0 wp=0 rb=0 cs=0 with no memory error -
     * and why QEMU never saw it, it does not model CRST reset gating). The
     * sequence therefore ENDS with CRST still asserted: codecs latch STATESTS
     * when the link reset is released (CRST 0->1), within 25 link frames. */
    uint16_t states = 0;
    for (int attempt = 0; attempt < 2 && states == 0; attempt++) {
        /* W1C stale presence BEFORE toggling: real hardware latches STATESTS
         * on the CRST 0->1 edge, QEMU latches it during the CRST=0 write -
         * clearing in between would erase QEMU's latch. */
        w32(HDA_STATESTS, 0x7fff);
        w32(HDA_GCTL, r32(HDA_GCTL) & ~(uint32_t)GCTL_CRST);
        for (int i = 0; i < 200; i++) { if (!(r32(HDA_GCTL) & GCTL_CRST)) break; pit_delay_ms(1); }
        pit_delay_ms(1);                 /* >=25 link frames before re-assert */

        w32(HDA_GCTL, r32(HDA_GCTL) | GCTL_CRST);
        for (int i = 0; i < 200; i++) { if (r32(HDA_GCTL) & GCTL_CRST) break; pit_delay_ms(1); }
        pit_delay_ms(2);                 /* codecs wake within 25 link frames */
        for (int i = 0; i < 200; i++) {
            states = (uint16_t)(r16(HDA_STATESTS) & 0x7fff);
            if (states != 0) break;
            pit_delay_ms(1);
        }
        /* live GCTL in the log: bit0 must still be 1 past this point */
        ksprintf(buf, sizeof(buf), "[hda] reset %d: gctl=%x statests=%x\n",
                 attempt, (unsigned)r32(HDA_GCTL), (unsigned)states);
        hlog(buf);
    }

    w32(HDA_WAKEEN, 0);
    w32(HDA_INTCTL, 0);                              /* polled */
    w32(HDA_INTSTS, 0xffffffff);

    if (states == 0) {
        hda_fail("HDA: no codec on link");
        hlog("[hda] no codec detected\n");
        return -1;
    }
    pit_delay_ms(5);                                 /* let the codecs settle */

    if (hda_dma_bufs() != 0) {
        hda_fail("HDA: out of memory");
        hlog("[hda] no memory for the DMA buffers\n");
        return -1;
    }

    /* DMA position buffer: optional per the spec, but some real controllers
     * misbehave with streams (or entirely) unless it is programmed. */
    w32(HDA_DPLBASE, (uint32_t)posb_phys | 0x1);
    w32(HDA_DPUBASE, (uint32_t)(posb_phys >> 32));

    /* CORB/RIRB: DMA off while re-wiring, force the 256-entry size (firmware
     * may leave the ring at 2 or 16 entries - the driver assumes 256 and a
     * stale size silently misroutes every verb), then the 64-bit bases of the
     * uncached rings, then the pointer resets. */
    w8(HDA_CORBCTL, 0);
    w8(HDA_RIRBCTL, 0);
    if (!(r8(HDA_CORBSIZE) & 0x40) || !(r8(HDA_RIRBSIZE) & 0x40)) {
        hda_fail("HDA: no 256-entry rings");
        hlog("[hda] controller lacks a 256-entry CORB/RIRB\n");
        return -1;
    }
    w8(HDA_CORBSIZE, 0x2);
    w8(HDA_RIRBSIZE, 0x2);
    w32(HDA_CORBLBASE, (uint32_t)corb_phys);
    w32(HDA_CORBUBASE, (uint32_t)(corb_phys >> 32));
    w32(HDA_RIRBLBASE, (uint32_t)rirb_phys);
    w32(HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    w16(HDA_CORBRP, CORBRP_RST);
    for (int i = 0; i < 100 && !(r16(HDA_CORBRP) & CORBRP_RST); i++) pit_delay_ms(1);
    w16(HDA_CORBRP, 0);
    w16(HDA_CORBWP, 0);
    w16(HDA_RIRBWP, RIRBWP_RST);
    w8(HDA_CORBSTS, 0x01);                       /* clear CORB memory error */
    w8(HDA_RINTCNT, 1);
    w8(HDA_RIRBCTL, RIRBCTL_DMA | RIRBCTL_IRQ);   /* IRQ latch ok, INTCTL off */
    w8(HDA_CORBCTL, CORBCTL_RUN);
    corb_wp = 0;
    rirb_last = 0;

    /* ---- codec enumeration: loosen the output-pin policy until some codec
     * yields a usable path (BIOS pin configuration is unreliable). rc=-2 from
     * the first verb means this codec address never answers - only declare
     * the link dead when NOT ONE present codec answered anything. */
    int found = 0;
    int link_dead = 0;
    for (probe_policy = 2; probe_policy >= 0 && !found && !link_dead; probe_policy--) {
        saw_out_pin = 0;
        int verb_ok = 0;
        for (int c = 0; c < 15; c++) {
            if (!(states & (1 << c))) continue;
            int rc = hda_probe_codec((uint8_t)c);
            if (rc == 0) { cad = (uint8_t)c; found = 1; break; }
            if (rc != -2) verb_ok = 1;    /* verbs alive, just no usable path */
        }
        if (!verb_ok) link_dead = 1;
    }
    probe_policy = 2;                    /* back to the strict default */
    if (!found) {
        if (link_dead || seen_vendor == 0) {
            ksprintf(hda_err, sizeof(hda_err), "HDA: verbs dead %s", verb_snap);
        } else if (!saw_out_pin) {
            ksprintf(hda_err, sizeof(hda_err), "HDA: %x:%x no out pin",
                     (unsigned)seen_vendor, (unsigned)seen_device);
        } else {
            ksprintf(hda_err, sizeof(hda_err), "HDA: %x:%x no dac path",
                     (unsigned)seen_vendor, (unsigned)seen_device);
        }
        hlog("[hda] no codec with a usable DAC/pin path\n");
        return -1;
    }

    /* ---- stream descriptor: first output stream ---- */
    uint16_t gcap = r16(HDA_GCAP);
    int oss = (gcap >> 12) & 0xf, iss = (gcap >> 8) & 0xf;
    if (oss == 0) {
        hda_fail("HDA: no output streams");
        hlog("[hda] controller has no output streams\n");
        return -1;
    }
    sd_index = (iss > 0)? iss : 0;
    stream_tag = (uint8_t)((sd_index & 0xf) + 1);

    hda_ok = 1;
    return 0;
}
#endif /* !HDA_HOST_TEST */

const char* hda_codec_name(void) { return hda_ok ? codec_name : ""; }

/* ---------------- stream format ---------------- */
/* rate = base * (mult + 1) / (div + 1), base 48 kHz or 44.1 kHz */
static int hda_format(uint32_t rate, uint16_t* out) {
    static const uint32_t base[2] = { 48000, 44100 };
    for (int b = 0; b < 2; b++) {
        for (int div = 0; div <= 7; div++) {
            for (int mult = 0; mult <= 3; mult++) {
                uint32_t r = base[b] * (uint32_t)(mult + 1) / (uint32_t)(div + 1);
                if (r == rate) {
                    uint16_t f = (uint16_t)((div << 8) | (mult << 11) |
                                            0x0010 /* 16-bit */ | 0x0001 /* 2ch */);
                    if (b == 1) f |= (uint16_t)0x4000;      /* 44.1 kHz base */
                    *out = f;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ---------------- stream / ring ---------------- */
static void hda_stream_program(uint16_t fmt) {
    uint32_t sd = sd_reg(sd_index);

    /* stream reset: SRST must read back set, then clear */
    w32(sd + SD_CTL, r32(sd + SD_CTL) | SD_CTL_SRST);
    for (int i = 0; i < 10000; i++) { if (r32(sd + SD_CTL) & SD_CTL_SRST) break; }
    w32(sd + SD_CTL, r32(sd + SD_CTL) & ~(uint32_t)SD_CTL_SRST);
    for (int i = 0; i < 10000; i++) { if (!(r32(sd + SD_CTL) & SD_CTL_SRST)) break; }

#ifdef HDA_HOST_TEST
    w32(sd + SD_BDLPL, (uint32_t)(uintptr_t)bdl);
    w32(sd + SD_BDLPU, (uint32_t)((uint64_t)(uintptr_t)bdl >> 32));
#else
    /* the controller DMA-reads the BDL, so it gets the PHYSICAL base of the
     * uncached window mapping, not the window's virtual address */
    w32(sd + SD_BDLPL, (uint32_t)bdl_phys);
    w32(sd + SD_BDLPU, (uint32_t)(bdl_phys >> 32));
#endif
    w32(sd + SD_CBL, HDA_RING);
    w16(sd + SD_LVI, HDA_NBUF - 1);
    w16(sd + SD_FMT, fmt);
    /* stream tag must match the converter's SET_CHANNEL_STREAMID below */
    w32(sd + SD_CTL, (r32(sd + SD_CTL) & ~(uint32_t)(0xf << 20)) |
                     ((uint32_t)stream_tag << 20));
}

void hda_close(void) {
    if (!hda_ok || ring == NULL) return;
    uint32_t sd = sd_reg(sd_index);
    w32(sd + SD_CTL, r32(sd + SD_CTL) & ~(uint32_t)SD_CTL_RUN);
    running = 0;
    /* the DMA buffers live in the persistent uncached window - nothing to free */
}

/* point every converter of this stream at the format and stream tag, so all
 * jacks in the primary association carry the same audio */
static void hda_set_stream_on_dacs(uint16_t fmt) {
    for (int i = 0; i < dac_count; i++) {
        hda_verb(cad, dacs[i], V_SET_STREAM_FMT, fmt, NULL);
        hda_verb(cad, dacs[i], V_SET_STREAM_ID, (uint32_t)((stream_tag << 4) | 0), NULL);
    }
}

int hda_open(uint32_t sample_rate) {
    if (!hda_ok) return -1;
    hda_close();

    uint16_t fmt = 0;
    if (hda_format(sample_rate, &fmt) != 0) {
        char buf[64];
        ksprintf(buf, sizeof(buf), "[hda] rate %u unsupported, using 48 kHz\n",
                 (unsigned)sample_rate);
        hlog(buf);
        hda_format(48000, &fmt);
        sample_rate = 48000;
    }
    cur_fmt = fmt;

#ifdef HDA_HOST_TEST
    ring = (uint8_t*)alloc_aligned(HDA_RING, 128, (void**)&ring_raw);
    bdl  = (hda_bdle_t*)alloc_aligned(sizeof(hda_bdle_t) * HDA_NBUF, 128, (void**)&bdl_raw);
    if (ring == NULL || bdl == NULL) {
        hlog("[hda] out of memory for the playback ring\n");
        return -1;
    }
    memset(ring, 0, HDA_RING);
#else
    if (!dma_ready || ring == NULL || bdl == NULL) {
        hlog("[hda] no DMA buffers\n");
        return -1;
    }
    memset(ring, 0, HDA_RING);
    memset(bdl, 0, sizeof(hda_bdle_t) * HDA_NBUF);
#endif
    for (int i = 0; i < HDA_NBUF; i++) {
#ifdef HDA_HOST_TEST
        bdl[i].addr  = (uint64_t)(uintptr_t)(ring + (size_t)i * HDA_SLOT);
#else
        bdl[i].addr  = ring_phys + (uint64_t)i * HDA_SLOT;
#endif
        bdl[i].len   = HDA_SLOT;                  /* bytes, 128-byte multiple */
        bdl[i].flags = 0;                         /* no completion IRQ */
    }

    /* converter(s): format then stream id, exactly like the stream descriptor */
    hda_set_stream_on_dacs(fmt);
    if (sel != 0) hda_verb(cad, sel, V_SET_CONNECT_SEL, (uint32_t)sel_idx, NULL);
    hda_verb(cad, pin, V_SET_PIN_CTL, PIN_CTL_OUT_EN, NULL);
    hda_stream_program(fmt);

    {
        char buf[96];
        ksprintf(buf, sizeof(buf), "[hda] stream %d tag %d at %u Hz (fmt 0x%x, %d dac)\n",
                 sd_index, (int)stream_tag, (unsigned)sample_rate, (unsigned)fmt, dac_count);
        hlog(buf);
    }

    wpos = 0;
    lpib_logged = 0;
    running = 0;
    was_paused = 0;
    frames_written = 0;
#ifndef HDA_HOST_TEST
    hlog_dump();        /* surface the stream line in /home/user/hda.log too */
#endif
    return 0;
}

int hda_write(const int16_t* pcm, int frames) {
    if (!hda_ok || ring == NULL || frames <= 0) return 0;

    uint32_t sd = sd_reg(sd_index);
    uint32_t lpib = r32(sd + SD_LPIB) % HDA_RING;
    /* one-shot: after ~1/5 s of accepted audio the DMA position must have
     * moved; lpib=0 here means the stream never started (controller side),
     * which looks exactly like a dead analog path from the outside */
    if (running && !lpib_logged && frames_written >= 8192) {
        lpib_logged = 1;
        char b[80];
        ksprintf(b, sizeof(b),
                 "[hda] dma check: lpib=%u of %u (%s)\n",
                 (unsigned)lpib, (unsigned)HDA_RING,
                 lpib != 0 ? "moving" : "NOT moving");
        hlog(b);
#ifndef HDA_HOST_TEST
        hlog_dump();    /* catch the dma check in /home/user/hda.log */
#endif
    }
    uint32_t in_flight = (uint32_t)((wpos - lpib + HDA_RING) % HDA_RING);
    /* never write into the slot the DAC is on (or is about to reach) */
    if (in_flight + HDA_SLOT > HDA_RING) return 0;

    uint32_t room = HDA_RING - HDA_SLOT - in_flight;
    uint32_t want = (uint32_t)frames * 4;
    if (want > room) want = room;
    want &= ~(uint32_t)3;                       /* keep whole stereo frames */

    uint32_t done = 0;
    while (done < want) {
        uint32_t chunk = want - done;
        if (chunk > HDA_RING - wpos) chunk = HDA_RING - wpos;
        memcpy(ring + wpos, (const uint8_t*)pcm + done, chunk);
        wpos = (uint32_t)((wpos + chunk) % HDA_RING);
        done += chunk;
    }
    frames_written += done / 4;
    return (int)(done / 4);
}

void hda_play(void) {
    if (!hda_ok || ring == NULL || running) return;
    uint32_t sd = sd_reg(sd_index);

    if (was_paused) {
        /* restart from a clean ring: both QEMU and real controllers reset the
         * position when the stream is reset, so the writer starts at 0 again */
        memset(ring, 0, HDA_RING);
        wpos = 0;
        hda_stream_program(cur_fmt);
        hda_set_stream_on_dacs(cur_fmt);
    }
    running = 1;
    was_paused = 0;
    w32(sd + SD_CTL, r32(sd + SD_CTL) | SD_CTL_RUN);
}

void hda_pause(void) {
    if (!hda_ok || ring == NULL || !running) return;
    uint32_t sd = sd_reg(sd_index);
    w32(sd + SD_CTL, r32(sd + SD_CTL) & ~(uint32_t)SD_CTL_RUN);
    running = 0;
    was_paused = 1;
}

int hda_playing(void) { return hda_ok && running; }

int hda_queued(void) {
    if (!hda_ok || ring == NULL) return 0;
    uint32_t lpib = r32(sd_reg(sd_index) + SD_LPIB) % HDA_RING;
    uint32_t in_flight = (uint32_t)((wpos - lpib + HDA_RING) % HDA_RING);
    return (int)(in_flight / 4);
}

uint64_t hda_written_total(void) { return frames_written; }
