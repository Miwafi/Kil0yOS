/* xHCI (USB 3.0) host controller driver - phase 1: HID keyboards.
 *
 * Modern real machines have no UHCI companion, and the EHCI spec forbids
 * driving full-speed devices from root ports (see ehci.c) - so a USB
 * keyboard plugged into any port of a current board is only reachable
 * through xHCI, which natively schedules FS/LS/HS/SS on all its ports.
 *
 * Design (matches the poll-based conventions of this USB stack):
 *   - interrupter IE stays clear; events are drained from usb_tick and
 *     from inline blocking waits (command completion, control transfer)
 *   - one command in flight; completion tracked via the event ring
 *   - device model: EnableSlot -> AddressDevice(BAA) assigns the slot id
 *     as the USB address; ep0 lives on its own transfer ring
 *   - HID interrupt IN: one Normal TRB outstanding, re-armed from the
 *     TransferEvent handler (dev->on_report) - reports at human typing
 *     speed never come close to the 64-entry ring
 *
 * All structures come from PMM pages (identity map, < 4GB), every
 * hardware-visible pointer is a single aligned store.  Only the low
 * 32 bits of 64-bit register pairs are ever non-zero.
 */
#include "usb/xhci.h"
#include "usb/usb.h"
#include "drivers/io.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

/* --- capability / operational register offsets -------------------------- */
#define XHCI_CAPLENGTH    0x00           /* byte */
#define XHCI_HCSPARAMS1   0x04
#define XHCI_HCCPARAMS1   0x10
#define XHCI_DBOFF        0x14
#define XHCI_RTSOFF       0x18

/* operational */
#define XHCI_USBCMD       0x00
#define XHCI_USBSTS       0x04
#define XHCI_PAGESIZE     0x08
#define XHCI_CRCR         0x18           /* 64-bit: command ring control */
#define XHCI_DCBAAP       0x30           /* 64-bit: device context array */
#define XHCI_CONFIG       0x38           /* MaxSlotsEn (byte 0) */
#define XHCI_PORTSC(n)    (0x400 + ((n) - 1) * 16)

/* USBCMD */
#define XHCI_CMD_RUN      0x00000001u
#define XHCI_CMD_HCRST    0x00000002u
#define XHCI_CMD_INTE     0x00000004u
#define XHCI_CMD_HSEE     0x00000008u
#define XHCI_CMD_SLOTS(x) (((uint32_t)(x) & 0x3FF) << 10)  /* Max Slots En */

/* USBSTS */
#define XHCI_STS_HCH      0x00000001u    /* HC halted */
#define XHCI_STS_HSE      0x00000004u
#define XHCI_STS_EINT     0x00000008u
#define XHCI_STS_CNR      0x00000800u    /* controller not ready */

/* PORTSC (16-byte stride) */
#define XHCI_PSC_CCS      0x00000001u
#define XHCI_PSC_PED      0x00000002u
#define XHCI_PSC_OCA      0x00000008u
#define XHCI_PSC_RESET    0x00000010u    /* port reset */
#define XHCI_PSC_PLS      0x000001E0u
#define XHCI_PSC_PP       0x00000200u    /* port power */
#define XHCI_PSC_SPEED    0x00003C00u    /* bits 13:10 */
#define XHCI_PSC_CAS      0x01000000u
#define XHCI_PSC_CSC      0x00020000u    /* W1C */
#define XHCI_PSC_PEC      0x00040000u    /* W1C */
#define XHCI_PSC_WRC      0x00080000u    /* W1C */
#define XHCI_PSC_PRC      0x00100000u    /* W1C */
#define XHCI_PSC_PLC      0x00200000u    /* W1C */
#define XHCI_PSC_CEC      0x00400000u    /* W1C */

/* port speed codes (PORTSC bits 13:10) */
#define XHCI_SPEED_FS     1
#define XHCI_SPEED_LS     2
#define XHCI_SPEED_HS     3
#define XHCI_SPEED_SS     4
#define XHCI_SPEED_SSP    5

/* --- TRBs ---------------------------------------------------------------- */
#define XHCI_TRB_CYCLE    (1u << 0)
#define XHCI_TRB_ENT      (1u << 1)
#define XHCI_TRB_TC       (1u << 2)
#define XHCI_TRB_IOC      (1u << 5)
#define XHCI_TRB_IDT      (1u << 6)
#define XHCI_TRB_BAA      (1u << 9)
#define XHCI_TRB_TYPE(x)  (((uint32_t)(x) & 0x3F) << 10)
#define XHCI_TRB_GET_TYPE(f) (((f) >> 10) & 0x3F)
#define XHCI_TRB_TX_IN    (1u << 16)
#define XHCI_TRB_TX_TYPE_OUT (2u << 16)
#define XHCI_TRB_TX_TYPE_IN  (3u << 16)
#define XHCI_TRB_LEN(x)   (((uint32_t)(x) & 0x1FFFF) << 0)

#define XHCI_TRB_NORMAL    1
#define XHCI_TRB_SETUP     2
#define XHCI_TRB_DATA      3
#define XHCI_TRB_STATUS    4
#define XHCI_TRB_LINK      6
#define XHCI_TRB_ENAB_SLOT 9
#define XHCI_TRB_ADDR_DEV  11
#define XHCI_TRB_CONFIG_EP 12
#define XHCI_TRB_NOOP      23
#define XHCI_TRB_EV_TRANSFER 32
#define XHCI_TRB_EV_CMD     33
#define XHCI_TRB_EV_PORT    34

/* event TRB word2 */
#define XHCI_EV_CODE(x)    (((x) >> 24) & 0xFF)
#define XHCI_EV_LEN(x)     (((x) >> 16) & 0xFF)
#define XHCI_EV_SLOT(f3)   (((f3) >> 24) & 0xFF)
#define XHCI_EV_EP(f3)     (((f3) >> 16) & 0x1F)

/* completion codes */
#define XHCI_COMP_SUCCESS  1
#define XHCI_COMP_SHORT    13

/* rings */
#define XHCI_RING_N        64            /* TRBs per ring */
#define XHCI_RING_BYTES    (XHCI_RING_N * 16)

/* --- typedefs ------------------------------------------------------------ */
typedef struct { uint32_t f0, f1, f2, f3; } xhci_trb_t;

typedef struct {
    xhci_trb_t* ring;                 /* VA */
    uint32_t    pa;                   /* ring physical base */
    int         enq;                  /* enqueue index */
    int         cyc;                  /* enqueue cycle */
} xhci_ring_t;

typedef struct {
    usb_device_t* dev;
    xhci_ring_t   ep0;                /* control transfer ring */
    xhci_ring_t   in;                 /* interrupt IN ring */
    uint8_t*      in_buf;             /* report DMA buffer (VA) */
    uint32_t      in_buf_pa;
    uint16_t      in_req_len;
    uint8_t       in_started;
    uint8_t       in_ep_id;           /* doorbell endpoint id */
    uint32_t*     dev_ctx;            /* output device context (VA) */
    uint32_t*     in_ctx;             /* input context (VA) */
} xhci_slot_t;

/* --- state --------------------------------------------------------------- */
static volatile uint8_t* xop;         /* operational base */
static volatile uint8_t* xrun;        /* runtime base */
static volatile uint32_t* xdb;        /* doorbell array */
static int n_ports;
static int max_slots;
static int ctx_size;                  /* 32 or 64 bytes */
static int xhci_up;

static uint32_t*    dcaa;             /* device context base address array */
static xhci_ring_t  cmd_ring;
static xhci_trb_t*  ev_ring;          /* event ring VA */
static uint32_t     ev_ring_pa;
static int          ev_enq, ev_cyc;

static xhci_slot_t  slots[16];
static int          slot_count;

static volatile int cmd_done;         /* set by the event drainer */
static volatile uint32_t cmd_status;  /* completion code */
static volatile uint32_t cmd_slot;

/* --- MMIO helpers -------------------------------------------------------- */
static inline uint32_t r32(volatile uint8_t* base, unsigned off) {
    return *(volatile uint32_t*)(base + off);
}
static inline void w32(volatile uint8_t* base, unsigned off, uint32_t v) {
    *(volatile uint32_t*)(base + off) = v;
}
static inline void w64(volatile uint8_t* base, unsigned off, uint64_t v) {
    *(volatile uint32_t*)(base + off) = (uint32_t)v;
    *(volatile uint32_t*)(base + off + 4) = (uint32_t)(v >> 32);
}

static uint32_t va_phys(const void* va) {
    uint64_t pte = vmm_get_pte((uint64_t)va);
    if (!(pte & 1)) return 0;
    if (pte & 0x80)                             /* 2MiB leaf */
        return (uint32_t)((pte & 0x000FFFFFE00000ULL) | ((uint64_t)va & 0x1FFFFF));
    return (uint32_t)((pte & 0x000FFFFFFFFFF000ULL) | ((uint64_t)va & 0xFFF));
}

/* --- rings ---------------------------------------------------------------- */

/* write one TRB into a ring at enq, handling the LINK-TRB wrap */
static void ring_put(xhci_ring_t* r, const xhci_trb_t* t) {
    if (r->enq == XHCI_RING_N - 1) {
        /* last slot carries the link back to the ring start */
        xhci_trb_t link;
        link.f0 = r->pa;
        link.f1 = 0;
        link.f2 = 0;
        link.f3 = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | r->cyc;
        r->ring[r->enq] = link;
        r->enq = 0;
        r->cyc ^= 1;
    }
    xhci_trb_t t2 = *t;
    t2.f3 = (t2.f3 & ~XHCI_TRB_CYCLE) | (uint32_t)r->cyc;
    r->ring[r->enq] = t2;
    r->enq++;
}

static void ring_init(xhci_ring_t* r, uint8_t* page) {
    r->ring = (xhci_trb_t*)page;
    r->pa   = va_phys(page);
    r->enq  = 0;
    r->cyc  = 1;
    memset(page, 0, XHCI_RING_BYTES);
}

/* doorbell: DB target 0 = command ring; endpoint id = 2*ep + dir */
static void xhci_doorbell(uint32_t slot, uint32_t target) {
    xdb[slot] = target & 0xFFFF;
}

/* --- event ring ------------------------------------------------------------ */

static uint64_t last_cmd_pa;
static int      last_ep_event;         /* endpoint id of last transfer event */

static int drain_events(void) {
    int n = 0;
    for (;;) {
        volatile xhci_trb_t* e = &ev_ring[ev_enq];
        if ((e->f3 & XHCI_TRB_CYCLE) != (uint32_t)ev_cyc) break;

        uint32_t type = XHCI_TRB_GET_TYPE(e->f3);
        uint32_t code = XHCI_EV_CODE(e->f2);

        if (type == XHCI_TRB_EV_CMD) {
            last_cmd_pa = (uint64_t)e->f0 | ((uint64_t)e->f1 << 32);
            cmd_status = code;
            cmd_slot = XHCI_EV_SLOT(e->f3);
            cmd_done = 1;
        } else if (type == XHCI_TRB_EV_TRANSFER) {
            uint32_t slot = XHCI_EV_SLOT(e->f3);
            uint32_t epid = XHCI_EV_EP(e->f3);
            last_ep_event = (int)epid;
            if (slot < (uint32_t)slot_count && slots[slot].dev &&
                slots[slot].in_started && epid == slots[slot].in_ep_id) {
                xhci_slot_t* s = &slots[slot];
                if (code == XHCI_COMP_SUCCESS || code == XHCI_COMP_SHORT) {
                    uint16_t residual = XHCI_EV_LEN(e->f2);
                    int delivered = (int)s->in_req_len - (int)residual;
                    if (delivered > 0 && s->dev && s->dev->on_report) {
                        s->dev->on_report(s->dev, s->in_buf, delivered);
                    }
                }
                /* re-arm one Normal TRB (same buffer) */
                xhci_trb_t t;
                t.f0 = s->in_buf_pa;
                t.f1 = 0;
                t.f2 = XHCI_TRB_LEN(s->in_req_len);
                t.f3 = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC;
                ring_put(&s->in, &t);
                xhci_doorbell(slot, s->in_ep_id);
            }
        }
        /* PortStatusChange and others: acknowledged by advancing ERDP
         * (port state is polled from PORTSC directly) */

        ev_enq++;
        if (ev_enq == XHCI_RING_N) { ev_enq = 0; ev_cyc ^= 1; }
        n++;
    }
    if (n > 0) {
        /* publish the new dequeue pointer and clear the event-pending bit */
        w64(xrun, 0x20 + 0x18, ev_ring_pa + (uint32_t)ev_enq * 16);
        w32(xrun, 0x20, r32(xrun, 0x20) | 1u);      /* IMAN.IP W1C */
    }
    return n;
}

/* wait for a command completion (drains events while waiting) */
static int wait_command(int timeout_ms) {
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        drain_events();
        if (cmd_done) return 0;
        pit_delay_ms(1);
        elapsed++;
    }
    return -1;
}

/* --- command submission ----------------------------------------------------- */

static int submit_command(const xhci_trb_t* t, int timeout_ms) {
    cmd_done = 0;
    cmd_status = 0;
    cmd_slot = 0;

    ring_put(&cmd_ring, t);
    xhci_doorbell(0, 0);                        /* ring 0, target 0 */

    int rc = wait_command(timeout_ms);
    if (rc != 0) return -1;
    return 0;
}

/* --- init ---------------------------------------------------------------- */

int xhci_init(pci_device_t* pci) {
    uint32_t bar0 = pci_read_dword(pci->bus, pci->device, pci->function, 0x10);
    if ((bar0 & 0x1) || (bar0 & ~0xF) == 0) {
        klog("[usb] xHCI BAR0 missing\n");
        return -1;
    }
    volatile uint8_t* cap = (volatile uint8_t*)(uint64_t)(bar0 & ~0xFu);

    uint32_t cmd = pci_read_dword(pci->bus, pci->device, pci->function,
                                  PCI_COMMAND_OFFSET);
    pci_write_dword(pci->bus, pci->device, pci->function, PCI_COMMAND_OFFSET,
                    (cmd & 0xFFFF) | 0x0006);   /* MEM + bus master */

    uint8_t caplen = cap[0];
    uint32_t hcs1 = *(volatile uint32_t*)(cap + XHCI_HCSPARAMS1);
    uint32_t hcc1 = *(volatile uint32_t*)(cap + XHCI_HCCPARAMS1);
    n_ports = (hcs1 >> 24) & 0xFF;
    max_slots = hcs1 & 0xFF;
    ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    if (caplen == 0 || n_ports == 0 || n_ports > 32 || max_slots == 0) {
        klog("[usb] xHCI caps bogus\n");
        return -1;
    }
    if (max_slots > 16) max_slots = 16;         /* slot table bound */

    xop  = cap + caplen;
    {
        uint32_t rtsoff = *(volatile uint32_t*)(cap + XHCI_RTSOFF) & ~0x1Fu;
        uint32_t dboff  = *(volatile uint32_t*)(cap + XHCI_DBOFF) & ~0x3u;
        xrun = cap + rtsoff;
        xdb  = (volatile uint32_t*)(cap + dboff);
    }

    /* ---- BIOS handoff: claim ownership from SMI firmware code ----
     * Real firmware often leaves the controller BIOS-owned; our register
     * writes then fight the SMI handler and every enumeration fails.
     * Walk the extended capabilities for USB Legacy Support (ID 1) and,
     * if the BIOS-owned semaphore is set, request OS ownership. */
    {
        uint32_t xecp = (hcc1 >> 16) & 0xFFFF;
        if (xecp) {
            volatile uint32_t* lc = (volatile uint32_t*)(cap + xecp * 4);
            int guard = 32;                    /* extcap chain bound */
            while (guard-- > 0) {
                uint32_t v = *lc;
                if ((v & 0xFF) == 1) {         /* USBLEGSUP */
                    if (v & (1u << 16)) {      /* BIOS owned */
                        *lc = v | (1u << 24);  /* request OS ownership */
                        int t = 5000;
                        while ((*lc & (1u << 16)) && t-- > 0) pit_delay_ms(1);
                        klog(t > 0 ? "[usb] xHCI: BIOS handoff ok\n"
                                   : "[usb] xHCI: BIOS handoff timeout\n");
                    }
                    break;
                }
                uint32_t nx = (v >> 8) & 0xFF;
                if (!nx) break;
                lc += nx;
            }
        }
    }

    /* ---- Intel PCH port routing: BIOSes boot with every USB2 port
     * routed to EHCI, so full/high-speed devices never appear on the
     * xHCI ports (they show up on the EHCI root instead, which we
     * cannot drive without a companion).  Flip all switchable ports
     * to xHCI (XUSB2PR, mask in USB2PRM) and enable SuperSpeed on the
     * USB3 ports (USB3PSSEN, mask in USB3PRM) - the same handoff
     * Linux does in usb_enable_intel_xhci_ports(). */
    if (pci->vendor_id == 0x8086) {
        uint16_t b = pci->bus, d = pci->device, f = pci->function;
        uint32_t mask = pci_read_dword(b, d, f, 0xDC);
        if (mask) pci_write_dword(b, d, f, 0xD8, mask);  /* USB3 SS en */
        mask = pci_read_dword(b, d, f, 0xD4);
        if (mask) {
            pci_write_dword(b, d, f, 0xD0, mask);        /* to xHCI  */
            klog("[usb] xHCI: Intel USB2 ports rerouted from EHCI\n");
        }
    }
    /* halt (if running) and reset the controller */
    w32(xop, XHCI_USBCMD, r32(xop, XHCI_USBCMD) & ~XHCI_CMD_RUN);
    {
        int t = 1000;
        while (!(r32(xop, XHCI_USBSTS) & XHCI_STS_HCH) && t-- > 0) pit_delay_ms(1);
    }
    w32(xop, XHCI_USBCMD, r32(xop, XHCI_USBCMD) | XHCI_CMD_HCRST);
    {
        int t = 2000;
        while ((r32(xop, XHCI_USBCMD) & XHCI_CMD_HCRST) && t-- > 0) pit_delay_ms(1);
        if (t <= 0) { klog("[usb] xHCI HCRST timeout\n"); return -1; }
        /* CNR: controller not ready - real chips need a beat after reset */
        t = 1000;
        while ((r32(xop, XHCI_USBSTS) & XHCI_STS_CNR) && t-- > 0) pit_delay_ms(1);
    }

    /* ---- allocate everything ---- */
    uint8_t* dcaa_page = (uint8_t*)pmm_alloc_page();
    uint8_t* cmd_page  = (uint8_t*)pmm_alloc_page();
    uint8_t* ev_page   = (uint8_t*)pmm_alloc_page();
    uint8_t* erst_page = (uint8_t*)pmm_alloc_page();
    if (!dcaa_page || !cmd_page || !ev_page || !erst_page) {
        klog("[usb] xHCI alloc failed\n");
        return -1;
    }
    memset(dcaa_page, 0, 4096);
    memset(cmd_page, 0, 4096);
    memset(ev_page, 0, 4096);
    memset(erst_page, 0, 4096);

    dcaa = (uint32_t*)dcaa_page;
    slot_count = max_slots;
    memset(slots, 0, sizeof(slots));

    ring_init(&cmd_ring, cmd_page);

    ev_ring = (xhci_trb_t*)ev_page;
    ev_ring_pa = va_phys(ev_page);
    ev_enq = 0;
    ev_cyc = 1;

    /* ERST: single segment pointing at the event ring */
    volatile uint32_t* erst = (volatile uint32_t*)erst_page;
    erst[0] = ev_ring_pa;
    erst[1] = 0;
    erst[2] = XHCI_RING_N;
    erst[3] = 0;

    /* ---- program the controller ----
     * (CONFIG.MaxSlotsEn, DCBAAP @0x30, CRCR @0x18 - spec offsets) */
    w32(xop, XHCI_CONFIG, max_slots);
    w64(xop, XHCI_DCBAAP, va_phys(dcaa_page));
    w64(xop, XHCI_CRCR, va_phys(cmd_page) | 1u);   /* low: PA | RCS */

    /* interrupter 0: runtime base + 0x20 */
    w32(xrun, 0x20 + 0x08, 1);                    /* ERSTSZ: one segment */
    w64(xrun, 0x20 + 0x10, va_phys(erst_page));   /* ERSTBA */
    w64(xrun, 0x20 + 0x18, ev_ring_pa);           /* ERDP */
    /* IE stays clear: poll-based, like the other HCDs */

    w32(xop, XHCI_USBCMD, XHCI_CMD_RUN | XHCI_CMD_SLOTS(max_slots));
    {
        int t = 1000;
        while ((r32(xop, XHCI_USBSTS) & XHCI_STS_HCH) && t-- > 0) pit_delay_ms(1);
    }

    xhci_up = 1;
    char b[96];
    ksprintf(b, sizeof(b),
             "[usb] xHCI %02x:%02x.%d mmio=%x ports=%d slots=%d usb_xhci_ok\n",
             pci->bus, pci->device, pci->function, (uint32_t)(uint64_t)cap,
             n_ports, max_slots);
    klog(b);
    return 0;
}

int xhci_ready(void)      { return xhci_up; }
int xhci_port_count(void) { return xhci_up ? n_ports : 0; }

/* --- ports ------------------------------------------------------------------ */

int xhci_port_connected(int port) {
    if (!xhci_up || port < 1 || port > n_ports) return 0;
    return (r32(xop, XHCI_PORTSC(port)) & XHCI_PSC_CCS) ? 1 : 0;
}

/* reset; returns the negotiated speed code (1=FS 2=LS 3=HS 4/5=SS), 0=none */
int xhci_port_reset(int port) {
    if (!xhci_up || port < 1 || port > n_ports) return 0;
    unsigned p = XHCI_PORTSC(port);

    w32(xop, p, r32(xop, p) | XHCI_PSC_RESET);
    {
        int t = 500;
        while ((r32(xop, p) & XHCI_PSC_RESET) && t-- > 0) pit_delay_ms(1);
    }
    pit_delay_ms(20);

    uint32_t s = r32(xop, p);
    w32(xop, p, s | XHCI_PSC_CSC | XHCI_PSC_PEC | XHCI_PSC_WRC |
                XHCI_PSC_PRC | XHCI_PSC_PLC | XHCI_PSC_CEC);   /* W1C */

    uint32_t speed = (s & XHCI_PSC_SPEED) >> 10;
    if (speed == 0) return 0;
    return (int)speed;
}

void xhci_clear_port_change(int port) {
    if (!xhci_up || port < 1 || port > n_ports) return;
    unsigned p = XHCI_PORTSC(port);
    w32(xop, p, r32(xop, p) | XHCI_PSC_CSC | XHCI_PSC_PEC | XHCI_PSC_WRC |
                XHCI_PSC_PRC | XHCI_PSC_PLC | XHCI_PSC_CEC);
}

/* --- enumeration (EnableSlot + AddressDevice) ------------------------------- */

int xhci_enumerate(usb_device_t* dev, int port, int speed) {
    if (!xhci_up || slot_count == 0) return -1;

    /* ---- EnableSlot ---- */
    xhci_trb_t cmd;
    cmd.f0 = cmd.f1 = cmd.f2 = 0;
    cmd.f3 = XHCI_TRB_TYPE(XHCI_TRB_ENAB_SLOT);
    if (submit_command(&cmd, 500) != 0 || cmd_status != XHCI_COMP_SUCCESS ||
        cmd_slot == 0) {
        char eb[80];
        ksprintf(eb, sizeof(eb),
                 "[xhci] EnableSlot failed (code=%d slot=%d)\n",
                 cmd_status, cmd_slot);
        klog(eb);
        return -1;
    }
    uint32_t slot = cmd_slot;
    if (slot >= (uint32_t)slot_count) return -1;

    xhci_slot_t* s = &slots[slot];
    memset(s, 0, sizeof(*s));
    s->dev = dev;
    dev->xhci_slot = (uint8_t)slot;

    /* ---- contexts ---- */
    uint8_t* dev_page = (uint8_t*)pmm_alloc_page();
    uint8_t* in_page  = (uint8_t*)pmm_alloc_page();
    uint8_t* ep0_page = (uint8_t*)pmm_alloc_page();
    uint8_t* inb_page = (uint8_t*)pmm_alloc_page();
    if (!dev_page || !in_page || !ep0_page || !inb_page) {
        klog("[xhci] slot ctx alloc failed\n");
        return -1;
    }
    memset(dev_page, 0, 4096);
    memset(in_page, 0, 4096);
    memset(ep0_page, 0, 4096);
    memset(inb_page, 0, 4096);

    s->dev_ctx = (uint32_t*)dev_page;
    s->in_ctx  = (uint32_t*)in_page;
    dcaa[slot] = va_phys(dev_page);

    ring_init(&s->ep0, ep0_page);
    s->in_buf = inb_page;
    s->in_buf_pa = va_phys(inb_page);

    /* input context: [control ctx][slot ctx][ep0 ctx] at ctx_size stride */
    uint32_t* ctrl = s->in_ctx;
    uint32_t* sctx = (uint32_t*)(in_page + ctx_size);
    uint32_t* ep0c = (uint32_t*)(in_page + 2 * ctx_size);

    ctrl[1] = 0x3;                    /* A0 (slot) + A1 (ep0) */

    /* slot ctx: context entries = 1, speed, root port */
    sctx[0] = (1u << 27) | (((uint32_t)speed & 0xF) << 20);
    sctx[1] = ((uint32_t)port & 0xFF) << 16;

    /* ep0 ctx: control, max packet per speed, ring running */
    uint16_t mps = (speed == XHCI_SPEED_HS) ? 64 :
                   (speed >= XHCI_SPEED_SS) ? 512 : 8;
    dev->ep_mps = (uint8_t)mps;
    ep0c[0] = 0;                                  /* interval (exp) */
    ep0c[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
    ep0c[2] = va_phys(s->ep0.ring) | 1u;          /* dequeue lo | cycle */
    ep0c[3] = 0;                                  /* dequeue hi */
    ep0c[4] = 8;                                  /* average TRB length */

    cmd.f0 = va_phys(in_page);
    cmd.f1 = 0;
    cmd.f2 = 0;
    cmd.f3 = (slot << 24) |                         /* slot id: ctrl[31:24] */
             XHCI_TRB_TYPE(XHCI_TRB_ADDR_DEV);   /* no BSR: address immediately */
    if (submit_command(&cmd, 500) != 0 || cmd_status != XHCI_COMP_SUCCESS) {
        char b[80];
        ksprintf(b, sizeof(b), "[xhci] AddressDevice failed (code %d)\n",
                 cmd_status);
        klog(b);
        return -1;
    }
    dev->address = (uint8_t)slot;

    char b[80];
    ksprintf(b, sizeof(b),
             "[xhci] slot %d addressed (port %d, speed %d)\n",
             slot, port, speed);
    klog(b);
    return 0;
}

/* --- ep0 control transfers --------------------------------------------------- */

int xhci_control_xfer(usb_device_t* dev, uint8_t bmReq, uint8_t bReq,
                      uint16_t wValue, uint16_t wIndex, int dir_in,
                      uint8_t* data, uint16_t len, uint16_t* recv_len) {
    uint32_t slot = dev->xhci_slot;
    if (!xhci_up || slot == 0 || slot >= (uint32_t)slot_count) return -5;
    xhci_slot_t* s = &slots[slot];

    if (recv_len) *recv_len = 0;

    static uint8_t setup[8];
    static uint8_t stage[512];
    if (len > sizeof(stage)) len = (uint16_t)sizeof(stage);
    if (!dir_in && len > 0) memcpy(stage, data, len);

    setup[0] = bmReq;
    setup[1] = bReq;
    setup[2] = (uint8_t)(wValue & 0xFF);
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)(wIndex & 0xFF);
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)(len >> 8);

    /* SETUP TRB: 8 bytes inline (IDT) */
    xhci_trb_t t_setup;
    t_setup.f0 = (uint32_t)setup[0] | ((uint32_t)setup[1] << 8) |
                 ((uint32_t)setup[2] << 16) | ((uint32_t)setup[3] << 24);
    t_setup.f1 = (uint32_t)setup[4] | ((uint32_t)setup[5] << 8) |
                 ((uint32_t)setup[6] << 16) | ((uint32_t)setup[7] << 24);
    t_setup.f2 = XHCI_TRB_LEN(8);
    t_setup.f3 = XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT;

    xhci_trb_t t_data;
    t_data.f0 = va_phys(stage);
    t_data.f1 = 0;
    t_data.f2 = XHCI_TRB_LEN(len);
    t_data.f3 = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) |
                (len > 0 ? (dir_in ? XHCI_TRB_TX_TYPE_IN : XHCI_TRB_TX_TYPE_OUT)
                         : 0);

    xhci_trb_t t_status;
    t_status.f0 = 0;
    t_status.f1 = 0;
    t_status.f2 = 0;
    t_status.f3 = XHCI_TRB_TYPE(XHCI_TRB_STATUS) |
                  (dir_in ? 0 : XHCI_TRB_TX_IN) |   /* opposite of data */
                  XHCI_TRB_IOC;

    ring_put(&s->ep0, &t_setup);
    if (len > 0) ring_put(&s->ep0, &t_data);
    ring_put(&s->ep0, &t_status);
    xhci_doorbell(slot, 1);                     /* ep0 target = 1 */

    /* wait for the status-stage TransferEvent (the one with IOC) */
    int elapsed = 0;
    while (elapsed < 1200) {
        drain_events();
        if (last_ep_event == 1) break;          /* transfer on ep0 */
        pit_delay_ms(1);
        elapsed++;
    }
    if (elapsed >= 1200) return -1;

    /* for control IN the received data sits in the staging buffer; the
     * exact transferred count is folded into the standard flow (control
     * transfers here only fetch whole descriptors <= mps per stage) */
    if (dir_in && len > 0) {
        memcpy(data, stage, len);
        if (recv_len) *recv_len = len;
    }
    return 0;
}

/* --- interrupt IN endpoint ---------------------------------------------------- */

int xhci_start_interrupt_in(usb_device_t* dev, uint8_t ep_addr,
                            uint16_t mps, uint8_t interval) {
    (void)interval;                        /* fixed ~1ms exponent below */
    uint32_t slot = dev->xhci_slot;
    if (!xhci_up || slot == 0 || slot >= (uint32_t)slot_count) return -1;
    if (!(ep_addr & 0x80)) return -1;
    uint8_t epnum = ep_addr & 0x0F;
    if (epnum != 1) return -1;              /* phase 1: EP1 IN only */

    xhci_slot_t* s = &slots[slot];
    uint8_t* ring_page = (uint8_t*)pmm_alloc_page();
    uint8_t* in2_page = (uint8_t*)pmm_alloc_page();
    if (!ring_page || !in2_page) return -1;
    memset(ring_page, 0, 4096);
    memset(in2_page, 0, 4096);

    ring_init(&s->in, ring_page);
    s->in_ep_id = (uint8_t)(epnum * 2 + 1);         /* EP1 IN -> 3 */
    s->in_req_len = mps;
    s->in_started = 1;

    /* input context: control + slot + ep1-in */
    uint32_t* ctrl = (uint32_t*)in2_page;
    uint32_t* sctx = (uint32_t*)(in2_page + ctx_size);
    uint32_t* epc  = (uint32_t*)(in2_page + 4 * ctx_size);   /* ep idx 3 */

    ctrl[1] = 0x1 | (1u << 3);              /* QEMU: add and 3 == 1; EP1-IN = ep idx 3 */
    sctx[0] = (3u << 27);                   /* context entries = 3 */
    epc[0] = 4u << 16;                      /* interval: 2^4 usframes */
    epc[1] = (3u << 1) | (7u << 3) | ((uint32_t)mps << 16);
    epc[2] = va_phys(s->in.ring) | 1u;      /* dequeue lo | cycle */
    epc[3] = 0;                             /* dequeue hi */
    epc[4] = 8;                             /* average TRB length */

    xhci_trb_t cmd;
    cmd.f0 = va_phys(in2_page);
    cmd.f1 = 0;
    cmd.f2 = 0;
    cmd.f3 = (slot << 24) |                         /* slot id: ctrl[31:24] */
             XHCI_TRB_TYPE(XHCI_TRB_CONFIG_EP);
    if (submit_command(&cmd, 500) != 0 || cmd_status != XHCI_COMP_SUCCESS) {
{
        char eb2[80];
        ksprintf(eb2, sizeof(eb2),
                 "xhci: ConfigureEndpoint failed (code %d)\n", cmd_status);
        klog(eb2);
    }
        s->in_started = 0;
        return -1;
    }

    /* submit the first report TRB and ring the endpoint */
    xhci_trb_t t;
    t.f0 = s->in_buf_pa;
    t.f1 = 0;
    t.f2 = XHCI_TRB_LEN(s->in_req_len);
    t.f3 = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC;
    ring_put(&s->in, &t);
    xhci_doorbell(slot, s->in_ep_id);
    return 0;
}

void xhci_stop_interrupt_in(usb_device_t* dev) {
    uint32_t slot = dev->xhci_slot;
    if (slot == 0 || slot >= (uint32_t)slot_count) return;
    slots[slot].in_started = 0;
    slots[slot].dev = NULL;
}

/* --- poll ---------------------------------------------------------------------- */

void xhci_poll(void) {
    if (!xhci_up) return;
    drain_events();
}

/* --- diagnostics ------------------------------------------------------------------ */

void xhci_dump_controller(void) {
    char b[96];
    ksprintf(b, sizeof(b),
             "[usb] xHCI cmd=0x%08x sts=0x%08x ports=%d slots=%d\n",
             r32(xop, XHCI_USBCMD), r32(xop, XHCI_USBSTS), n_ports,
             max_slots);
    klog(b);
    for (int p = 1; p <= n_ports; p++) {
        uint32_t s = r32(xop, XHCI_PORTSC(p));
        ksprintf(b, sizeof(b),
                 "[usb] xport %d: ccs=%d ped=%d speed=%d pr=%d\n",
                 p, s & 1, (s >> 1) & 1, (s >> 10) & 0xF, (s >> 4) & 1);
        klog(b);
    }
}
