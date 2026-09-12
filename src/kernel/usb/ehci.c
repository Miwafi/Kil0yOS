/* EHCI (USB 2.0) host controller driver - high-speed control + bulk.
 *
 * Scope: this HCD exists to drive USB 2.0 Wi-Fi dongles (MT7601U), which
 * never fall back to full-speed, so UHCI alone cannot see them.  It is
 * deliberately minimal:
 *   - poll-based, no interrupts (USBINTR = 0, like UHCI)
 *   - one outstanding transfer at a time (matches the enumeration and
 *     MT7601U bring-up call patterns; MT7601U is the only consumer)
 *   - async schedule only (control + bulk); no periodic schedule
 *   - high-speed devices only; a full/low-speed device on an EHCI root
 *     port without a live companion handoff is marked unsupported
 *
 * Memory discipline mirrors uhci.c: all QH/qTD structures come from PMM
 * pages (identity map VA == PA, 32-byte aligned), never kmalloc.  Every
 * hardware-visible pointer is a single aligned 32-bit store.
 *
 * QH layout is the EHCI spec order (horizontal link, endpoint
 * characteristics, endpoint capabilities, then the 9-word transfer
 * overlay) - the overlay is preloaded with the first qTD so the
 * controller picks the chain up on the next schedule scan.
 */
#include "usb/ehci.h"
#include "usb/usb.h"
#include "drivers/io.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

/* --- MMIO register offsets from the operational base ------------------- */
#define EHCI_USBCMD        0x00
#define EHCI_USBSTS        0x04
#define EHCI_USBINTR       0x08
#define EHCI_CTRLDSSEG     0x10
#define EHCI_PERIODICLIST  0x14
#define EHCI_ASYNCLIST     0x18
#define EHCI_CONFIGFLAG    0x40
#define EHCI_PORTSC(n)     (0x44 + ((n) - 1) * 4)   /* n = 1..N_PORTS */

/* USBCMD */
#define EHCI_CMD_RUN     0x00000001u
#define EHCI_CMD_HCRESET 0x00000002u
#define EHCI_CMD_ASE     0x00000020u
#define EHCI_CMD_IAAD    0x00000040u

/* USBSTS */
#define EHCI_STS_IAA 0x00000020u   /* interrupt on async advance */
#define EHCI_STS_HCH 0x00001000u   /* HC halted */

/* PORTSC bits (W1C: CSC 2, PEC 8, OCC 0x20) */
#define EHCI_PSC_CCS     0x00000001u
#define EHCI_PSC_CSC     0x00000002u
#define EHCI_PSC_PE      0x00000004u
#define EHCI_PSC_PEC     0x00000008u
#define EHCI_PSC_OCC     0x00000020u
#define EHCI_PSC_RESET   0x00000100u
#define EHCI_PSC_PP      0x00001000u

/* --- link pointers / qTD ----------------------------------------------- */
#define EHCI_LINK_TERM 0x00000001u
#define EHCI_LINK_QH   0x00000002u   /* type field 3:2 -> 1 = QH */

#define EHCI_QTD_PID_SETUP 0u
#define EHCI_QTD_PID_OUT   1u
#define EHCI_QTD_PID_IN    2u

/* qTD token (word 2 of the qTD) */
#define EHCI_QTD_TOGGLE    (1u << 31)
#define EHCI_QTD_LEN(x)    (((uint32_t)(x) & 0x7FFF) << 16)
#define EHCI_QTD_IOC       (1u << 15)
#define EHCI_QTD_CERR(x)   (((uint32_t)(x) & 0x3) << 10)
#define EHCI_QTD_PID(x)    (((uint32_t)(x) & 0x3) << 8)
#define EHCI_QTD_ACTIVE    (1u << 7)
#define EHCI_QTD_HALTED    (1u << 6)
#define EHCI_QTD_DBERR     (1u << 5)
#define EHCI_QTD_BABBLE    (1u << 4)
#define EHCI_QTD_XACTERR   (1u << 3)
#define EHCI_QTD_ACTLEN(x) (uint16_t)(((x) >> 16) & 0x7FFF)

/* QH endpoint-characteristics word (0x04) */
#define EHCI_QH_RL(x)    (((uint32_t)(x) & 0xF) << 28)
#define EHCI_QH_C        (1u << 27)
#define EHCI_QH_MPS(x)   (((uint32_t)(x) & 0x7FF) << 16)
#define EHCI_QH_H        (1u << 15)
#define EHCI_QH_DTC      (1u << 14)
#define EHCI_QH_EPS_HS   (2u << 12)
#define EHCI_QH_EP(x)    (((uint32_t)(x) & 0xF) << 7)
#define EHCI_QH_ADDR(x)  ((uint32_t)(x) & 0x7F)

/* QH endpoint-capabilities word (0x08) */
#define EHCI_QH_MULT(x)  (((uint32_t)(x) & 0x3) << 30)

#define EHCI_QTD_MAX       16    /* qTDs per transfer */
#define EHCI_STAGE_SIZE    16384 /* one staging buffer per transfer */
#define EHCI_CTRL_TIMEOUT_MS 1000
#define EHCI_BULK_TIMEOUT_MS 5000

/* Spec-order QH: 12 dwords of hardware state, padded to 64-byte stride. */
typedef struct {
    uint32_t link;       /* 0x00: horizontal: next QH          */
    uint32_t epchar;     /* 0x04: addr/ep/speed/mps/C/H        */
    uint32_t epcap;      /* 0x08: mult/port/hub/smask/cmask    */
    uint32_t current;    /* 0x0C: overlay: current qTD         */
    uint32_t next_qtd;   /* 0x10: overlay: next qTD            */
    uint32_t alt_qtd;    /* 0x14: overlay: alt next qTD        */
    uint32_t token;      /* 0x18: overlay: qTD token           */
    uint32_t buf[5];     /* 0x1C..0x2C: overlay buffer ptrs    */
    uint32_t pad[3];
} ehci_qh_t;

typedef struct {
    uint32_t next, alt, token, buf[5];
} ehci_qtd_t;

static volatile uint8_t* op_base;     /* operational register base (MMIO) */
static int n_ports;
static int ehci_up;

static ehci_qh_t*  async_head;        /* H-flagged head, self-linked */
static ehci_qh_t*  xfer_qh;           /* single active-transfer QH */
static ehci_qtd_t* qtd_pool[EHCI_QTD_MAX];

static int xfer_busy;

/* --- helpers ------------------------------------------------------------ */

static inline uint32_t rd32(unsigned off) {
    volatile uint32_t* p = (volatile uint32_t*)(op_base + off);
    return *p;
}
static inline void wr32(unsigned off, uint32_t v) {
    volatile uint32_t* p = (volatile uint32_t*)(op_base + off);
    *p = v;
}

/* Physical address of a kernel VA, computed from the live page-table
 * entry.  The VMM maps PMM frames at non-identity VAs, so this must go
 * through the PTE (vmm_get_phys trusts the walk too, but its result has
 * proven unreliable for PMM-frame VAs; the PTE is ground truth). */
static uint32_t va_phys(const void* va) {
    uint64_t v = (uint64_t)va;
    uint64_t pte = vmm_get_pte(v);
    if (!(pte & 1)) return 0;                   /* not present */
    if (pte & 0x80)                             /* PS: 2MiB leaf */
        return (uint32_t)((pte & 0x000FFFFFE00000ULL) | (v & 0x1FFFFF));
    return (uint32_t)((pte & 0x000FFFFFFFFFF000ULL) | (v & 0xFFF));
}

/* Fill one qTD.  next/alt are patched by the caller after the chain is
 * built; buf may be any (page-crossing) address: the first word carries
 * the intra-page offset, each further word one following page base. */
static void qtd_fill(ehci_qtd_t* td, uint8_t pid, uint16_t len,
                     int toggle, uint32_t phys) {
    td->alt  = EHCI_LINK_TERM;          /* short packets stop the chain */
    td->token = EHCI_QTD_ACTIVE | EHCI_QTD_CERR(3) | EHCI_QTD_PID(pid) |
                EHCI_QTD_LEN(len) | (toggle ? EHCI_QTD_TOGGLE : 0);
    uint32_t page = phys & ~0xFFFu;
    td->buf[0] = phys;
    for (int i = 1; i < 5; i++) {
        page += 0x1000;
        td->buf[i] = page & ~0xFFFu;
    }
}

/* --- init ---------------------------------------------------------------- */

int ehci_init(pci_device_t* pci) {
    uint32_t bar0 = pci_read_dword(pci->bus, pci->device, pci->function, 0x10);
    if ((bar0 & 0x1) || (bar0 & ~0xF) == 0) {
        klog("[usb] EHCI BAR0 missing (not MMIO)\n");
        return -1;
    }
    volatile uint8_t* cap = (volatile uint8_t*)(uint64_t)(bar0 & ~0xFu);

    /* enable memory space + bus master */
    uint32_t cmd = pci_read_dword(pci->bus, pci->device, pci->function,
                                  PCI_COMMAND_OFFSET);
    pci_write_dword(pci->bus, pci->device, pci->function, PCI_COMMAND_OFFSET,
                    (cmd & 0xFFFF) | 0x0006);

    uint8_t caplen = cap[0];
    uint32_t hcsparams = *(volatile uint32_t*)(cap + 4);
    n_ports = hcsparams & 0xF;
    if (caplen == 0 || n_ports == 0 || n_ports > 15) {
        klog("[usb] EHCI caps bogus\n");
        return -1;
    }
    op_base = cap + caplen;

    /* ---- BIOS handoff: claim ownership from SMI firmware code ----
     * HCCPARAMS (cap+8) bits 15:8 = first Extended Capability offset in
     * bytes.  Capability ID 1 is USBLEGSUP: bit16 = BIOS owned (RO),
     * bit24 = OS owned (write 1 to request).  Without this the BIOS's
     * SMI handler fights our register writes and enumeration fails. */
    {
        uint32_t hccparams = *(volatile uint32_t*)(cap + 8);
        uint32_t ecp = (hccparams >> 8) & 0xFF;
        if (ecp) {
            volatile uint32_t* lc = (volatile uint32_t*)(cap + ecp);
            int guard = 32;                    /* capability chain bound */
            while (guard-- > 0) {
                uint32_t v = *lc;
                if ((v & 0xFF) == 1) {         /* USBLEGSUP */
                    if (v & (1u << 16)) {      /* BIOS owned */
                        *lc = v | (1u << 24);  /* request OS ownership */
                        int t = 5000;
                        while ((*lc & (1u << 16)) && t-- > 0) pit_delay_ms(1);
                        klog(t > 0 ? "[usb] EHCI: BIOS handoff ok\n"
                                   : "[usb] EHCI: BIOS handoff timeout\n");
                    }
                    break;
                }
                uint32_t nx = (v >> 8) & 0xFF;
                if (!nx) break;
                lc += nx;
            }
        }
    }
    /* stop, then reset the host controller */
    wr32(EHCI_USBCMD, rd32(EHCI_USBCMD) & ~EHCI_CMD_RUN);
    {
        int t = 2000;
        while (!(rd32(EHCI_USBSTS) & EHCI_STS_HCH) && t-- > 0) pit_delay_ms(1);
    }
    wr32(EHCI_USBCMD, rd32(EHCI_USBCMD) | EHCI_CMD_HCRESET);
    {
        int t = 2000;
        while ((rd32(EHCI_USBCMD) & EHCI_CMD_HCRESET) && t-- > 0) pit_delay_ms(1);
        if (t <= 0) { klog("[usb] EHCI HCRESET timeout\n"); return -1; }
    }

    /* static pool: async head + transfer QH + qTDs, all in one page */
    uint8_t* pool = (uint8_t*)pmm_alloc_page();
    if (pool == NULL) { klog("[usb] EHCI pool alloc failed\n"); return -1; }
    memset(pool, 0, 4096);

    async_head = (ehci_qh_t*)(pool + 0);
    xfer_qh    = (ehci_qh_t*)(pool + 64);
    for (int i = 0; i < EHCI_QTD_MAX; i++)
        qtd_pool[i] = (ehci_qtd_t*)(pool + 128 + 32 * i);

    /* async head: HS, H-flagged (RL=0), self-linked, empty overlay */
    async_head->epchar = EHCI_QH_EPS_HS | EHCI_QH_MPS(64) | EHCI_QH_H;
    async_head->epcap  = EHCI_QH_MULT(1);
    async_head->link   = va_phys(async_head) | EHCI_LINK_QH;
    async_head->current = EHCI_LINK_TERM;

    wr32(EHCI_CTRLDSSEG, 0);
    wr32(EHCI_ASYNCLIST, va_phys(async_head) | EHCI_LINK_QH);
    wr32(EHCI_USBINTR, 0);                      /* poll-based */
    wr32(EHCI_CONFIGFLAG, 1);                   /* route ports to EHCI */
    wr32(EHCI_USBCMD, EHCI_CMD_RUN | EHCI_CMD_ASE);

    ehci_up = 1;
    char buf[96];
    ksprintf(buf, sizeof(buf),
             "[usb] EHCI %02x:%02x.%d mmio=%x ports=%d usb_ehci_ok\n",
             pci->bus, pci->device, pci->function, (uint32_t)(uint64_t)cap,
             n_ports);
    klog(buf);
    return 0;
}

int ehci_ready(void)        { return ehci_up; }
int ehci_port_count(void)   { return ehci_up ? n_ports : 0; }

/* --- async transfer engine ----------------------------------------------
 * One QH carries the whole qTD chain.  The overlay is preloaded with the
 * first qTD (current/next/alt/token/buffers) so the controller starts
 * the chain on the next schedule scan; on completion the controller
 * itself advances the overlay from qTD to qTD.  Publish = point
 * async_head at the transfer QH (single aligned store + doorbell);
 * retire = restore the self-loop.  Completion is polled on the LAST
 * qTD's token in guest memory (the controller writes it back). */

static void ehci_ring_doorbell(void) {
    wr32(EHCI_USBCMD, rd32(EHCI_USBCMD) | EHCI_CMD_IAAD);
    int t = 100;
    while (!(rd32(EHCI_USBSTS) & EHCI_STS_IAA) && t-- > 0) pit_delay_ms(1);
    wr32(EHCI_USBSTS, EHCI_STS_IAA);            /* W1C */
}

static int ehci_run_transfer(usb_device_t* dev, int is_control,
                             uint8_t ep_num, uint16_t mps,
                             int dir_in, uint8_t* data, uint16_t len,
                             uint16_t* recv_len) {
    if (xfer_busy) return -4;
    xfer_busy = 1;
    if (recv_len) *recv_len = 0;

    static uint8_t stage[EHCI_STAGE_SIZE];      /* DMA staging */
    static uint8_t setup[8];                    /* control request block */
    if (len > EHCI_STAGE_SIZE) len = EHCI_STAGE_SIZE;

    ehci_qtd_t* tds[EHCI_QTD_MAX] = {0};
    uint16_t req_len[EHCI_QTD_MAX] = {0};   /* SW copy: HW overwrites the
                                             * token length field with the
                                             * REMAINING byte count */
    int n = 0;

    if (is_control) {
        setup[0] = dev->pending_bmReq;
        setup[1] = dev->pending_bReq;
        setup[2] = (uint8_t)(dev->pending_wValue & 0xFF);
        setup[3] = (uint8_t)(dev->pending_wValue >> 8);
        setup[4] = (uint8_t)(dev->pending_wIndex & 0xFF);
        setup[5] = (uint8_t)(dev->pending_wIndex >> 8);
        setup[6] = (uint8_t)(len & 0xFF);
        setup[7] = (uint8_t)(len >> 8);

        qtd_fill(qtd_pool[0], EHCI_QTD_PID_SETUP, 8, 1, va_phys(setup));
        req_len[0] = 8;
        tds[n++] = qtd_pool[0];

        if (len > 0) {
            if (!dir_in) memcpy(stage, data, len);
            uint16_t remaining = len;
            uint32_t off = 0;
            int toggle = 1;
            while (remaining > 0 && n < EHCI_QTD_MAX - 1) {
                uint16_t chunk = remaining > mps ? mps : remaining;
                qtd_fill(qtd_pool[n],
                         dir_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
                         chunk, toggle, va_phys(stage) + off);
                tds[n] = qtd_pool[n];
                req_len[n] = chunk;
                toggle ^= 1;
                off += chunk;
                remaining -= chunk;
                n++;
            }
        }
        /* status stage: opposite direction of the data stage, DATA1 */
        uint8_t pid = (len > 0 && dir_in) ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN;
        qtd_fill(qtd_pool[n], pid, 0, 1, 0);
        tds[n] = qtd_pool[n];
        req_len[n] = 0;
        n++;
    } else {
        if (!dir_in && len > 0) memcpy(stage, data, len);
        uint16_t remaining = len;
        uint32_t off = 0;
        while (remaining > 0 && n < EHCI_QTD_MAX) {
            uint16_t chunk = remaining > EHCI_STAGE_SIZE
                                 ? EHCI_STAGE_SIZE : remaining;
            qtd_fill(qtd_pool[n],
                     dir_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
                     chunk, 0, va_phys(stage) + off);
            tds[n] = qtd_pool[n];
            req_len[n] = chunk;
            off += chunk;
            remaining -= chunk;
            n++;
        }
        if (n == 0) { xfer_busy = 0; return -4; }
    }

    /* chain the qTDs; the last one terminates */
    for (int i = 0; i < n - 1; i++)
        tds[i]->next = va_phys(tds[i + 1]);
    tds[n - 1]->next = EHCI_LINK_TERM;

    /* ---- preload the transfer QH overlay with the first qTD ---- */
    xfer_qh->epchar = EHCI_QH_ADDR(dev->address) |
                      EHCI_QH_EP(ep_num) |
                      EHCI_QH_EPS_HS | EHCI_QH_MPS(mps) |
                      (is_control ? EHCI_QH_C : 0);
    xfer_qh->epcap  = EHCI_QH_MULT(1);
    xfer_qh->link   = va_phys(async_head) | EHCI_LINK_QH;
    xfer_qh->current = va_phys(tds[0]);
    xfer_qh->next_qtd = tds[0]->next;
    xfer_qh->alt_qtd = tds[0]->alt;
    xfer_qh->token = tds[0]->token;
    for (int i = 0; i < 5; i++) xfer_qh->buf[i] = tds[0]->buf[i];

    /* head -> transfer QH, single aligned store, then ring the doorbell */
    async_head->link = va_phys(xfer_qh) | EHCI_LINK_QH;
    ehci_ring_doorbell();

    /* ---- wait for the LAST qTD to retire ---- */
    int timeout = is_control ? EHCI_CTRL_TIMEOUT_MS : EHCI_BULK_TIMEOUT_MS;
    int elapsed = 0;
    while (elapsed < timeout) {
        if (!(tds[n - 1]->token & EHCI_QTD_ACTIVE)) break;
        pit_delay_ms(1);
        elapsed++;
    }

    int rc = 0;
    uint16_t got = 0;
    if (tds[n - 1]->token & EHCI_QTD_ACTIVE) {
        rc = -1;                                /* timeout */
        char dbg[128];
        ksprintf(dbg, sizeof(dbg),
                 "[ehci] TIMEOUT sts=%08x qh cur=%08x tok=%08x | "
                 "td0 t=%08x tdL t=%08x\n",
                 rd32(EHCI_USBSTS), xfer_qh->current, xfer_qh->token,
                 tds[0]->token, tds[n - 1]->token);
        klog(dbg);
    } else {
        for (int i = 0; i < n; i++) {
            uint32_t t = tds[i]->token;
            if (t & EHCI_QTD_ACTIVE) break;     /* retired early (short IN) */
            got = (uint16_t)(got + (req_len[i] - EHCI_QTD_ACTLEN(t)));
            if (t & EHCI_QTD_HALTED) {
                rc = (t & (EHCI_QTD_BABBLE | EHCI_QTD_DBERR)) ? -3 : -2;
                break;
            }
            if (t & EHCI_QTD_XACTERR) { rc = -3; break; }
        }
    }

    if (dir_in && len > 0 && (rc == 0 || rc == -1)) {
        uint16_t cpy = got < len ? got : len;
        memcpy(data, stage, cpy);
        if (recv_len) *recv_len = cpy;
    }

    /* ---- unlink: restore the head self-loop ---- */
    async_head->link = va_phys(async_head) | EHCI_LINK_QH;
    ehci_ring_doorbell();
    pit_delay_ms(2);                            /* let the frame retire */
    xfer_busy = 0;
    return rc;
}

/* --- public transfer API -------------------------------------------------- */

int ehci_control_xfer(usb_device_t* dev, uint8_t bmReq, uint8_t bReq,
                      uint16_t wValue, uint16_t wIndex, int dir_in,
                      uint8_t* data, uint16_t len, uint16_t* recv_len) {
    if (!ehci_up) return -5;
    dev->pending_bmReq = bmReq; dev->pending_bReq = bReq;
    dev->pending_wValue = wValue; dev->pending_wIndex = wIndex;
    return ehci_run_transfer(dev, 1, 0, dev->ep_mps ? dev->ep_mps : 64,
                             dir_in, data, len, recv_len);
}

int ehci_bulk_xfer(usb_device_t* dev, uint8_t ep_num, int dir_in,
                   uint8_t* data, uint16_t len, uint16_t* recv_len) {
    if (!ehci_up) return -5;
    return ehci_run_transfer(dev, 0, ep_num,
                             dir_in ? dev->bulk_in_mps : dev->bulk_out_mps,
                             dir_in, data, len, recv_len);
}

/* --- root hub ports ------------------------------------------------------- */

int ehci_port_connected(int port) {
    if (!ehci_up || port < 1 || port > n_ports) return 0;
    return (rd32(EHCI_PORTSC(port)) & EHCI_PSC_CCS) ? 1 : 0;
}

/* Reset the port.  Returns 0 when the attached device is high-speed
 * (hardware sets PE), -1 for full/low-speed devices: those need a
 * companion-controller handoff which we do not implement, and the
 * controller would never address them. */
int ehci_port_reset(int port) {
    if (!ehci_up || port < 1 || port > n_ports) return -1;
    unsigned p = EHCI_PORTSC(port);
    wr32(p, rd32(p) | EHCI_PSC_RESET);
    pit_delay_ms(50);
    wr32(p, rd32(p) & ~EHCI_PSC_RESET);
    pit_delay_ms(20);

    uint32_t s = rd32(p);
    wr32(p, s | EHCI_PSC_CSC | EHCI_PSC_PEC | EHCI_PSC_OCC);   /* W1C */
    if (!(rd32(p) & EHCI_PSC_PE)) return -1;
    return 0;
}

void ehci_clear_port_change(int port) {
    if (!ehci_up || port < 1 || port > n_ports) return;
    unsigned p = EHCI_PORTSC(port);
    wr32(p, rd32(p) | EHCI_PSC_CSC | EHCI_PSC_PEC | EHCI_PSC_OCC);
}

void ehci_dump_controller(void) {
    char buf[96];
    ksprintf(buf, sizeof(buf),
             "[usb] EHCI cmd=0x%08x sts=0x%08x ports=%d async=0x%08x\n",
             rd32(EHCI_USBCMD), rd32(EHCI_USBSTS), n_ports,
             rd32(EHCI_ASYNCLIST));
    klog(buf);
    for (int p = 1; p <= n_ports; p++) {
        uint32_t s = rd32(EHCI_PORTSC(p));
        ksprintf(buf, sizeof(buf),
                 "[usb] eport %d: ccs=%d pe=%d csc=%d\n",
                 p, s & 1, (s >> 2) & 1, (s >> 1) & 1);
        klog(buf);
    }
}
