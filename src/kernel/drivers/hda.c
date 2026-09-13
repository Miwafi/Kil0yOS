/* Intel High Definition Audio controller + codec driver (PCI class 0x0403).
 *
 * Covers the Realtek ALC family (vendor 0x10EC) and any other HDA codec with
 * a discoverable DAC -> pin path: the controller is reset, CORB/RIRB command
 * rings are brought up, the codec's audio function group is walked to find an
 * output pin whose connection graph reaches a DAC, that path is powered to D0
 * and unmuted (plus EAPD on pins that need it - mandatory on most Realteks),
 * and a single output stream descriptor plays a 32-slot cyclic BDL ring.
 *
 * Register access is MMIO through BAR0; the kernel identity-maps the first
 * 4 GiB, so BAR/BDL addresses double as pointers. Polled: no interrupts. */
#include <stdint.h>
#include "drivers/io.h"
#include "drivers/pci.h"
#include "drivers/hda.h"
#include "mm/memory.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "timer/pit.h"

extern void klog(const char* s);

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
#define HDA_CORBSIZE  0x4e    /* u8, bits1:0: 0=2 1=16 2=256 entries */

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

#define PAR_VENDOR_ID     0x00
#define PAR_NODE_COUNT    0x04
#define PAR_FUNC_TYPE     0x05
#define PAR_WIDGET_CAP    0x09
#define PAR_CONNLIST_LEN  0x0e
#define PAR_AMP_OUT_CAP   0x12
#define PAR_AMP_IN_CAP    0x0d
#define PAR_PIN_CAP       0x0c

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
static uint32_t* corb_raw;  static uint32_t* corb;
static uint64_t* rirb_raw;  static uint64_t* rirb;
static uint16_t corb_wp;
static uint16_t rirb_last;
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
static int16_t* ring_raw;               /* allocation base */
static uint8_t* ring;                   /* 128-byte aligned cyclic buffer */
static hda_bdle_t* bdl_raw;
static hda_bdle_t* bdl;
static uint32_t wpos;                   /* writer byte offset in the ring */
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

static void* alloc_aligned(size_t size, size_t align, void** raw) {
    uint8_t* p = (uint8_t*)kmalloc(size + align);
    if (p == NULL) return NULL;
    *raw = p;
    return (void*)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
}

/* ---------------- CORB/RIRB command interface ---------------- */
static int hda_cmd(uint32_t cmd, uint32_t* resp) {
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
    klog("[hda] command timeout (codec not answering)\n");
    return -1;
}

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
    /* 0 dB sits at the offset step (see HDA 1.0a 7.3.4.10); with no amp
     * capabilities the widget cannot be attenuated, so leave it alone.
     * Bit 7 stays 0 = unmuted. */
    if ((caps & AMPCAP_STEPS) == 0) return;
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

/* Walk one codec: find its AFG, an output pin with a DAC behind it, and
 * unmute that path. Returns 0 on success. */
static int hda_probe_codec(uint8_t c) {
    vendor = (uint16_t)(hda_param(c, 0, PAR_VENDOR_ID) >> 16);
    device = (uint16_t)(hda_param(c, 0, PAR_VENDOR_ID) & 0xffff);

    uint32_t nc = hda_param(c, 0, PAR_NODE_COUNT);
    int first = (int)(nc >> 16), count = (int)(nc & 0xffff);   /* start / count */

    afg = 0;
    for (int i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(first + i);
        if ((hda_param(c, nid, PAR_FUNC_TYPE) & 0xff) == GRP_AUDIO_FUNC) { afg = nid; break; }
    }
    if (afg == 0) return -1;

    hda_verb(c, afg, V_SET_FUNC_RESET, 0, NULL);
    pit_delay_ms(2);

    nc = hda_param(c, afg, PAR_NODE_COUNT);
    first = (int)(nc >> 16);
    count = (int)(nc & 0xffff);
    if (count > 64) count = 64;

    /* prefer a line-out / speaker pin, else any output-capable pin */
    dac = 0;
    int best_score = -1;
    path_node_t best_path[8];
    int best_len = 0;
    for (int i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(first + i);
        uint32_t wcaps = hda_param(c, nid, PAR_WIDGET_CAP);
        if (WCAP_TYPE(wcaps) != WID_PIN) continue;
        uint32_t pcaps = hda_param(c, nid, PAR_PIN_CAP);
        if (!(pcaps & PINCAP_OUT)) continue;

        uint32_t cfg = 0;
        hda_verb(c, nid, V_GET_CONFIG_DEF, 0, &cfg);
        if (DEFCFG_PORT_CONN(cfg) == 0x1) continue;      /* no physical connection */
        int dev = (int)DEFCFG_DEVICE(cfg);
        int score = (dev == 0x0 /* line out */) ? 3 : (dev == 0x1 /* speaker */) ? 2 : 1;

        path_len = 0;
        if (hda_find_dac(c, nid, 0) && score > best_score) {
            best_score = score;
            dac = path[path_len - 1].nid;
            pin = nid;
            pin_eapd = (pcaps & PINCAP_EAPD) ? 1 : 0;
            sel = 0; sel_idx = 0;
            for (int k = 0; k < path_len - 1; k++) {
                if (path[k].type == WID_AUD_SEL) { sel = path[k].nid; sel_idx = path[k].in_idx; }
            }
            best_len = path_len;
            for (int k = 0; k < path_len; k++) best_path[k] = path[k];
        }
        path_len = 0;
    }
    if (dac == 0) return -1;
    for (int k = 0; k < best_len; k++) path[k] = best_path[k];
    path_len = best_len;

    /* power + unmute everything on the path, then enable the pin */
    for (int k = 0; k < path_len; k++) {
        uint8_t nid = path[k].nid;
        hda_widget_power(c, nid);
        hda_set_amp(c, nid, 0, 0, hda_param(c, nid, PAR_AMP_OUT_CAP));
        if (path[k].type == WID_AUD_MIX) {
            hda_set_amp(c, nid, 1, path[k].in_idx, hda_param(c, nid, PAR_AMP_IN_CAP));
        }
    }
    hda_widget_power(c, pin);
    hda_set_amp(c, pin, 0, 0, hda_param(c, pin, PAR_AMP_OUT_CAP));
    hda_verb(c, pin, V_SET_PIN_CTL, PIN_CTL_OUT_EN, NULL);
    if (sel != 0) hda_verb(c, sel, V_SET_CONNECT_SEL, (uint32_t)sel_idx, NULL);
    /* EAPD powers the external amplifier on most Realteks - without it the
     * line-out/headphone jack stays silent */
    if (pin_eapd) hda_verb(c, pin, V_SET_EAPD, 0x02, NULL);

    if (vendor == 0x10ec) {
        char buf[8];
        ksprintf(codec_name, sizeof(codec_name), "Realtek ALC%s", hex4(device, buf));
    } else {
        char buf[8];
        ksprintf(codec_name, sizeof(codec_name), "codec %x:%s", vendor, hex4(device, buf));
    }

    {
        char buf[128];
        ksprintf(buf, sizeof(buf), "[hda] %s cad=%d afg=%d dac=%d pin=%d eapd=%d\n",
                 codec_name, (int)c, (int)afg, (int)dac, (int)pin, pin_eapd);
        klog(buf);
    }
    return 0;
}

/* ---------------- controller bring-up ---------------- */
int hda_init(void) {
    if (hda_ok) return 0;

    pci_device_t* d = pci_find_class(0x04, 0x03);
    if (d == NULL) return -1;                       /* no HDA controller */

    uint32_t bar0 = pci_read_dword(d->bus, d->device, d->function, 0x10);
    uint32_t bar1 = pci_read_dword(d->bus, d->device, d->function, 0x14);
    if (bar0 & 1) {
        klog("[hda] BAR0 is I/O space\n");
        return -1;                                  /* spec requires MMIO */
    }
    uint64_t base = bar0 & ~0xfu;
    if ((bar0 & 0x6) == 0x4) base |= (uint64_t)(bar1 & ~0xfu) << 32;   /* 64-bit BAR */
    if (base == 0 || base >= 0x100000000ULL) {
        klog("[hda] BAR above the identity map\n");
        return -1;
    }

    /* memory space + bus master, or no register or DMA access at all */
    uint32_t cmd = pci_read_dword(d->bus, d->device, d->function, 0x04);
    pci_write_dword(d->bus, d->device, d->function, 0x04, cmd | 0x0006);

    mmio = (volatile uint8_t*)(uintptr_t)base;
    klog("[hda] controller found\n");

    /* reset: CRST self-clears once the codecs are ready */
    w32(HDA_GCTL, r32(HDA_GCTL) & ~(uint32_t)GCTL_CRST);
    for (int i = 0; i < 100000; i++) { if (!(r32(HDA_GCTL) & GCTL_CRST)) break; }
    w32(HDA_GCTL, r32(HDA_GCTL) | GCTL_CRST);
    int up = 0;
    for (int i = 0; i < 100000; i++) { if (r32(HDA_GCTL) & GCTL_CRST) { up = 1; break; } }
    if (!up) {
        klog("[hda] controller reset timeout\n");
        return -1;
    }
    w32(HDA_GCTL, r32(HDA_GCTL) & ~(uint32_t)GCTL_CRST);
    for (int i = 0; i < 100000; i++) { if (!(r32(HDA_GCTL) & GCTL_CRST)) break; }

    w32(HDA_WAKEEN, 0);
    w32(HDA_INTCTL, 0);                              /* polled */
    w32(HDA_INTSTS, 0xffffffff);
    pit_delay_ms(2);

    uint16_t states = 0;
    for (int i = 0; i < 200; i++) {
        states = (uint16_t)(r16(HDA_STATESTS) & 0x7fff);
        if (states != 0) break;
        pit_delay_ms(1);
    }
    if (states == 0) {
        klog("[hda] no codec detected\n");
        return -1;
    }

    corb = (uint32_t*)alloc_aligned(HDA_CORB_LEN * 4, 128, (void**)&corb_raw);
    rirb = (uint64_t*)alloc_aligned(HDA_RIRB_LEN * 8, 128, (void**)&rirb_raw);
    if (corb == NULL || rirb == NULL) {
        klog("[hda] out of memory for command rings\n");
        if (corb_raw) kfree(corb_raw);
        if (rirb_raw) kfree(rirb_raw);
        corb = NULL; rirb = NULL;
        return -1;
    }
    memset(corb, 0, HDA_CORB_LEN * 4);
    memset(rirb, 0, HDA_RIRB_LEN * 8);

    if ((r8(HDA_CORBSIZE) & 0x3) < 0x2) w8(HDA_CORBSIZE, 0x2);   /* 256 entries */
    if ((r8(HDA_RIRBSIZE) & 0x3) < 0x2) w8(HDA_RIRBSIZE, 0x2);
    w32(HDA_CORBLBASE, (uint32_t)(uintptr_t)corb);
    w32(HDA_CORBUBASE, (uint32_t)((uint64_t)(uintptr_t)corb >> 32));
    w32(HDA_RIRBLBASE, (uint32_t)(uintptr_t)rirb);
    w32(HDA_RIRBUBASE, (uint32_t)((uint64_t)(uintptr_t)rirb >> 32));

    w16(HDA_CORBRP, CORBRP_RST);
    pit_delay_ms(1);
    w16(HDA_CORBRP, 0);
    w16(HDA_RIRBWP, RIRBWP_RST);
    w8(HDA_RINTCNT, 1);
    w8(HDA_RIRBCTL, RIRBCTL_DMA | RIRBCTL_IRQ);   /* IRQ latch ok, INTCTL off */
    w8(HDA_CORBCTL, CORBCTL_RUN);
    corb_wp = 0;
    rirb_last = 0;

    /* ---- codec enumeration ---- */
    int found = -1;
    for (int c = 0; c < 15; c++) {
        if (!(states & (1 << c))) continue;
        if (hda_probe_codec((uint8_t)c) == 0) { cad = (uint8_t)c; found = c; break; }
    }
    if (found < 0) {
        klog("[hda] no codec with a usable DAC/pin path\n");
        return -1;
    }

    /* ---- stream descriptor: first output stream ---- */
    uint16_t gcap = r16(HDA_GCAP);
    int oss = (gcap >> 12) & 0xf, iss = (gcap >> 8) & 0xf;
    if (oss == 0) {
        klog("[hda] controller has no output streams\n");
        return -1;
    }
    sd_index = (iss > 0)? iss : 0;
    stream_tag = (uint8_t)((sd_index & 0xf) + 1);

    hda_ok = 1;
    return 0;
}

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

    w32(sd + SD_BDLPL, (uint32_t)(uintptr_t)bdl);
    w32(sd + SD_BDLPU, (uint32_t)((uint64_t)(uintptr_t)bdl >> 32));
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
    if (bdl_raw) { kfree(bdl_raw); bdl_raw = NULL; bdl = NULL; }
    if (ring_raw) { kfree(ring_raw); ring_raw = NULL; ring = NULL; }
}

int hda_open(uint32_t sample_rate) {
    if (!hda_ok) return -1;
    hda_close();

    uint16_t fmt = 0;
    if (hda_format(sample_rate, &fmt) != 0) {
        char buf[64];
        ksprintf(buf, sizeof(buf), "[hda] rate %u unsupported, using 48 kHz\n",
                 (unsigned)sample_rate);
        klog(buf);
        hda_format(48000, &fmt);
        sample_rate = 48000;
    }
    cur_fmt = fmt;

    ring = (uint8_t*)alloc_aligned(HDA_RING, 128, (void**)&ring_raw);
    bdl  = (hda_bdle_t*)alloc_aligned(sizeof(hda_bdle_t) * HDA_NBUF, 128, (void**)&bdl_raw);
    if (ring == NULL || bdl == NULL) {
        hda_close();
        klog("[hda] out of memory for the playback ring\n");
        return -1;
    }
    memset(ring, 0, HDA_RING);
    for (int i = 0; i < HDA_NBUF; i++) {
        bdl[i].addr  = (uint64_t)(uintptr_t)(ring + (size_t)i * HDA_SLOT);
        bdl[i].len   = HDA_SLOT;                  /* bytes, 128-byte multiple */
        bdl[i].flags = 0;                         /* no completion IRQ */
    }

    /* converter: format then stream id, exactly like the stream descriptor */
    hda_verb(cad, dac, V_SET_STREAM_FMT, fmt, NULL);
    hda_verb(cad, dac, V_SET_STREAM_ID, (uint32_t)((stream_tag << 4) | 0), NULL);
    if (sel != 0) hda_verb(cad, sel, V_SET_CONNECT_SEL, (uint32_t)sel_idx, NULL);
    hda_verb(cad, pin, V_SET_PIN_CTL, PIN_CTL_OUT_EN, NULL);
    hda_stream_program(fmt);

    {
        char buf[80];
        ksprintf(buf, sizeof(buf), "[hda] stream %d tag %d at %u Hz (fmt 0x%x)\n",
                 sd_index, (int)stream_tag, (unsigned)sample_rate, (unsigned)fmt);
        klog(buf);
    }

    wpos = 0;
    running = 0;
    was_paused = 0;
    frames_written = 0;
    return 0;
}

int hda_write(const int16_t* pcm, int frames) {
    if (!hda_ok || ring == NULL || frames <= 0) return 0;

    uint32_t sd = sd_reg(sd_index);
    uint32_t lpib = r32(sd + SD_LPIB) % HDA_RING;
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
        hda_verb(cad, dac, V_SET_STREAM_FMT, cur_fmt, NULL);
        hda_verb(cad, dac, V_SET_STREAM_ID, (uint32_t)((stream_tag << 4) | 0), NULL);
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
