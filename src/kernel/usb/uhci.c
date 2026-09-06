/* UHCI (USB 1.1) host controller driver.
 *
 * Single controller instance (QEMU piix3-usb-uhci at 00:01.2).  The
 * schedule skeleton is fully static - frame list entries point at
 * int_qh[0] -> int_qh[1] -> int_qh[2] -> int_qh[3] -> ctrl_qh ->
 * bulk_qh -> TERMINATE and the horizontal links never change after
 * uhci_init().  Binding a device only swaps its slot's int_qh elink
 * between TERMINATE and a TD pointer with one aligned 32-bit store.
 *
 * All TD/QH/frame-list memory comes from PMM pages (identity map:
 * VA == PA), never from kmalloc - the heap guarantees only 8-byte
 * alignment while the hardware needs 16.
 *
 * Context rules: this file runs in boot mainline (IF=0), IRQ0 tick
 * (uhci_idle_requeue) and the UHCI IRQ handler.  None of them ever
 * re-enables interrupts, so the three contexts are serialised.
 */
#include "usb/usb.h"
#include "drivers/io.h"
#include "core/interrupts.h"
#include "core/isr.h"
#include "mm/memory.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

#define USB_CTRL_TIMEOUT_MS 250
#define UHCI_TD_COUNT 24

void uhci_irq_handler(interrupt_frame_t* frame);  /* defined below */

static uint16_t io_base;
static uint8_t  ctrl_irq;
static int      uhci_ready;

static uint32_t* frame_list;          /* VA; 1024 entries */
static uint32_t  frame_list_phys;

static usb_qh_t* ctrl_qh;
static usb_qh_t* bulk_qh;
static usb_qh_t* int_qh[USB_MAX_DEVICES];

static usb_td_t* td_pool[UHCI_TD_COUNT];
static uint32_t  td_bitmap;           /* bit set = in use */

static int ctrl_busy;                 /* one outstanding control transfer */

/* --- helpers --------------------------------------------------------- */

static void td_fill(usb_td_t* td, uint32_t link, uint8_t pid,
                    uint8_t addr, uint8_t ep, uint16_t explen,
                    int toggle, int low_speed, uint32_t buf_phys, int ioc) {
    td->link = link;
    td->status = TD_CTRL_ACTIVE | TD_CTRL_C_ERR | (ioc ? TD_CTRL_IOC : 0) |
                 (low_speed ? TD_CTRL_LS : 0);
    td->token = TD_PID(pid) |
                ((uint32_t)addr << TD_TOKEN_ADDR_SHIFT) |
                ((uint32_t)(ep & 0xF) << TD_TOKEN_EP_SHIFT) |
                (toggle ? TD_TOKEN_TOGGLE : 0) |
                TD_TOKEN_MAXLEN(explen);
    td->buffer = buf_phys;
}

static int td_alloc(usb_td_t** out, uint32_t* phys_out) {
    for (int i = 0; i < UHCI_TD_COUNT; i++) {
        if (!(td_bitmap & (1u << i))) {
            td_bitmap |= (1u << i);
            *out = td_pool[i];
            *phys_out = (uint32_t)(uint64_t)td_pool[i];
            return 0;
        }
    }
    return -1;
}

static void td_free(usb_td_t* td) {
    for (int i = 0; i < UHCI_TD_COUNT; i++) {
        if (td_pool[i] == td) {
            td_bitmap &= ~(1u << i);
            return;
        }
    }
}

/* Physical address of a kernel VA under the identity map. */
static uint32_t va_phys(const void* va) {
    return (uint32_t)vmm_get_phys((uint64_t)(uint64_t*)(void*)va);
}

/* --- init ------------------------------------------------------------ */

int uhci_init(pci_device_t* pci) {
    /* UHCI lives in I/O space on BAR4 (0x20), not BAR0 - QEMU puts the
     * 32-byte register set there ("BAR4: I/O at ..." in info pci). */
    uint32_t bar4 = pci_read_dword(pci->bus, pci->device, pci->function, 0x20);
    if ((bar4 & 0x1) == 0 || (bar4 & ~0x3) == 0) {
        klog("[usb] UHCI BAR4 missing (no I/O space)\n");
        return -1;
    }
    io_base = (uint16_t)(bar4 & ~0x3);
    ctrl_irq = pci->irq;

    /* enable I/O space + bus master (same as rtl8139) */
    uint32_t cmd = pci_read_dword(pci->bus, pci->device, pci->function,
                                  PCI_COMMAND_OFFSET);
    pci_write_dword(pci->bus, pci->device, pci->function, PCI_COMMAND_OFFSET,
                    (cmd & 0xFFFF) | 0x0005);

    /* reset: global then host controller */
    uint16_t cmdr = io_base + UHCI_USBCMD;
    outw(cmdr, inw(cmdr) | UHCI_CMD_GRESET);
    pit_delay_ms(10);
    outw(cmdr, inw(cmdr) & ~UHCI_CMD_GRESET);
    outw(cmdr, inw(cmdr) | UHCI_CMD_HCRESET);
    {
        int t = 100;
        while ((inw(cmdr) & UHCI_CMD_HCRESET) != 0 && t-- > 0) pit_delay_ms(1);
        if (t <= 0) { klog("[usb] HCRESET timeout\n"); return -1; }
    }

    /* frame list page */
    frame_list = (uint32_t*)pmm_alloc_page();
    if (frame_list == NULL) { klog("[usb] frame list alloc failed\n"); return -1; }
    frame_list_phys = (uint32_t)(uint64_t)frame_list;

    /* struct pool page: ctrl_qh, bulk_qh, 4x int_qh, TD pool */
    uint8_t* pool = (uint8_t*)pmm_alloc_page();
    if (pool == NULL) { klog("[usb] pool alloc failed\n"); return -1; }
    memset(pool, 0, 4096);

    ctrl_qh      = (usb_qh_t*)(pool + 0);
    bulk_qh      = (usb_qh_t*)(pool + 16);
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        int_qh[i] = (usb_qh_t*)(pool + 32 + 16 * i);
        int_qh[i]->hlink = TD_LINK_TERMINATE;
        int_qh[i]->elink = TD_LINK_TERMINATE;
    }
    for (int i = 0; i < USB_MAX_DEVICES - 1; i++)
        int_qh[i]->hlink = ((uint32_t)(uint64_t)int_qh[i + 1]) | TD_LINK_QH;
    int_qh[USB_MAX_DEVICES - 1]->hlink =
        ((uint32_t)(uint64_t)ctrl_qh) | TD_LINK_QH;
    ctrl_qh->hlink = ((uint32_t)(uint64_t)bulk_qh) | TD_LINK_QH;
    ctrl_qh->elink = TD_LINK_TERMINATE;
    bulk_qh->hlink = TD_LINK_TERMINATE;
    bulk_qh->elink = TD_LINK_TERMINATE;

    for (int i = 0; i < UHCI_TD_COUNT; i++) {
        td_pool[i] = (usb_td_t*)(pool + 128 + 32 * i);
        td_pool[i]->link = TD_LINK_TERMINATE;
    }
    td_bitmap = 0;

    for (int i = 0; i < 1024; i++)
        frame_list[i] = ((uint32_t)(uint64_t)int_qh[0]) | TD_LINK_QH;

    outd(io_base + UHCI_USBFLBASEADD, frame_list_phys);
    outw(io_base + UHCI_USBFRNUM, 0);
    /* USBINTR stays 0: HID interrupt TDs complete every frame (NAKs
     * included) and IOC would raise a ~1kHz IRQ storm.  All report
     * delivery is polled from usb_tick -> uhci_idle_requeue at 100 Hz;
     * the IRQ handler stays registered as a safety net only. */
    outw(io_base + UHCI_USBINTR, 0);

    outw(cmdr, UHCI_CMD_RS | UHCI_CMD_CF);

    if (ctrl_irq >= 1 && ctrl_irq <= 15) {
        register_irq_handler(ctrl_irq, uhci_irq_handler);
        pic_enable_irq(ctrl_irq);
    } else {
        klog("[usb] bad PCI IRQ line, running IRQ-free (tick-poll only)\n");
    }
    uhci_ready = 1;

    char buf[96];
    ksprintf(buf, sizeof(buf),
             "[usb] UHCI %02x:%02x.%d io=0x%04x irq=%d flist=0x%08x usb_uhci_ok\n",
             pci->bus, pci->device, pci->function, io_base, ctrl_irq,
             frame_list_phys);
    klog(buf);
    return 0;
}

/* --- control transfers ----------------------------------------------- *
 * Chain: SETUP (DATA0) -> n x DATA (DATA1 alternating, SP on IN) ->
 * STATUS (opposite direction, DATA1, zero length).  The whole chain is
 * linked into ctrl_qh with ONE aligned 32-bit store; the last TD
 * carries IOC so the UHCI IRQ handler sees nothing while we poll. */

static int ctrl_wait(usb_td_t** tds, int ntd, uint16_t* recv_total) {
    usb_td_t* last = tds[ntd - 1];
    int elapsed = 0;
    int nak_retries = 0;
    while (elapsed < USB_CTRL_TIMEOUT_MS) {
        if (!(last->status & TD_CTRL_ACTIVE)) break;
        pit_delay_ms(1);
        elapsed++;
    }
    if (last->status & TD_CTRL_ACTIVE) {
        /* timeout: harvest whatever completed (short-packet early
         * retirement) and report error */
        uint16_t got = 0;
        for (int i = 1; i < ntd - 1; i++) {
            if (tds[i]->status & TD_CTRL_ACTIVE) break;
            got += (uint16_t)((tds[i]->status & TD_CTRL_ACTLEN) + 1);
        }
        *recv_total = got;
        return -1;
    }
    /* decode errors across the chain */
    for (int i = 0; i < ntd; i++) {
        uint32_t st = tds[i]->status;
        if (st & TD_CTRL_ACTIVE) continue;      /* chain retired early */
        if (st & TD_CTRL_STALLED) return -2;
        if (st & (TD_CTRL_BABBLE | TD_CTRL_DBUFERR |
                  TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF)) return -3;
        if ((st & TD_CTRL_NAK) && i == ntd - 1) {
            /* NAK on the status stage: re-arm and keep waiting, but
             * bound it - a wedged device must not hang the boot line */
            if (++nak_retries > 100) return -1;
            last->status = (last->status & ~TD_CTRL_NAK) | TD_CTRL_ACTIVE;
            elapsed = 0;
            i = -1;
            continue;
        }
    }
    uint16_t got = 0;
    for (int i = 1; i < ntd - 1; i++) {
        if (tds[i]->status & TD_CTRL_ACTIVE) break;
        got += (uint16_t)((tds[i]->status & TD_CTRL_ACTLEN) + 1);
    }
    *recv_total = got;
    return 0;
}

int uhci_control_xfer(usb_device_t* dev, uint8_t bmReq, uint8_t bReq,
                      uint16_t wValue, uint16_t wIndex, int dir_in,
                      uint8_t* data, uint16_t len, uint16_t* recv_len) {
    if (ctrl_busy) return -4;
    ctrl_busy = 1;
    if (recv_len) *recv_len = 0;

    /* request block (8 bytes, little endian) */
    static uint8_t setup[8];
    static uint8_t stage[256];               /* DMA staging (IN) */
    setup[0] = bmReq;
    setup[1] = bReq;
    setup[2] = (uint8_t)(wValue & 0xFF);
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)(wIndex & 0xFF);
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)(len & 0xFF);
    setup[7] = (uint8_t)(len >> 8);
    if (dir_in && len > sizeof(stage)) len = sizeof(stage);

    usb_td_t* tds[16];
    uint32_t phys[16];
    int n = 0;
    int ls = dev->low_speed;

    if (td_alloc(&tds[n], &phys[n]) != 0) { ctrl_busy = 0; return -4; }
    n++;
    /* SETUP TD: link patched after the whole chain is built */
    td_fill(tds[0], 0, TD_PID_SETUP, dev->address, 0, 8, 0, ls,
            va_phys(setup), 0);

    int rc = 0;
    if (len > 0) {
        if (!dir_in) memcpy(stage, data, len);
        uint16_t remaining = len;
        uint32_t off = 0;
        int toggle = 1;
        while (remaining > 0 && n < 14) {
            uint16_t chunk = remaining > dev->ep_mps ? dev->ep_mps : remaining;
            if (td_alloc(&tds[n], &phys[n]) != 0) { rc = -4; goto out; }
            td_fill(tds[n], 0, dir_in ? TD_PID_IN : TD_PID_OUT,
                    dev->address, 0, chunk, toggle, ls,
                    va_phys(stage) + off, 0);
            if (dir_in) tds[n]->status |= TD_CTRL_SP;
            toggle ^= 1;
            off += chunk;
            remaining -= chunk;
            n++;
        }
    }
    /* status stage: opposite direction of the data stage */
    {
        uint8_t pid = (len > 0 && dir_in) ? TD_PID_OUT : TD_PID_IN;
        if (td_alloc(&tds[n], &phys[n]) != 0) { rc = -4; goto out; }
        td_fill(tds[n], TD_LINK_TERMINATE, pid, dev->address, 0, 0, 1, ls,
                0, 1);
        n++;
    }
    for (int i = 0; i < n - 1; i++)
        tds[i]->link = phys[i + 1];

    /* publish the chain with one atomic store */
    ctrl_qh->elink = phys[0];

    uint16_t got = 0;
    rc = ctrl_wait(tds, n, &got);

    if (dir_in && len > 0) {
        if (rc == 0 || rc == -1) {
            uint16_t cpy = got < len ? got : len;
            memcpy(data, stage, cpy);
            if (recv_len) *recv_len = cpy;
        }
    }

out:
    ctrl_qh->elink = TD_LINK_TERMINATE;
    pit_delay_ms(2);                          /* let the frame retire */
    for (int i = 0; i < n; i++) td_free(tds[i]);
    ctrl_busy = 0;
    return rc;
}

/* --- interrupt IN queue ---------------------------------------------- */

void uhci_queue_interrupt(usb_device_t* dev) {
    usb_td_t* td;
    uint32_t phys;
    if (td_alloc(&td, &phys) != 0) { dev->int_td = NULL; return; }
    dev->int_td = td;
    /* ioc=0: interrupt TDs complete every frame (NAKs included) - IOC here
     * would raise a ~1kHz interrupt storm.  Reports are consumed by
     * usb_tick -> uhci_idle_requeue at 100 Hz instead. */
    td_fill(td, TD_LINK_TERMINATE, TD_PID_IN, dev->address,
            dev->ep_addr & 0xF, dev->ep_mps, dev->toggle, dev->low_speed,
            va_phys(dev->int_buf), 0);
    int_qh[dev->slot]->elink = phys;
}

void uhci_unqueue_interrupt(usb_device_t* dev) {
    int_qh[dev->slot]->elink = TD_LINK_TERMINATE;
    pit_delay_ms(2);                          /* let the frame retire */
    if (dev->int_td) { td_free(dev->int_td); dev->int_td = NULL; }
}

/* Consume one completed interrupt TD: NAK -> counter only, data ->
 * on_report, error -> mark dead.  Re-arms the TD in all non-dead cases.
 * Callers: uhci_irq_handler (IF=0) and uhci_idle_requeue (IRQ0, IF=0);
 * the two contexts are serialised and the first one to run re-arms the
 * TD, so a completion is delivered exactly once. */
static void int_td_service(usb_device_t* dev) {
    uint32_t st = dev->int_td->status;
    if (st & TD_CTRL_ACTIVE) return;          /* still in flight */

    if (st & (TD_CTRL_STALLED | TD_CTRL_BABBLE | TD_CTRL_DBUFERR |
              TD_CTRL_CRCTIMEO | TD_CTRL_BITSTUFF)) {
        dev->dead = 1;                        /* unbind deferred to usb_tick */
        int_qh[dev->slot]->elink = TD_LINK_TERMINATE;
        return;
    }
    if (st & TD_CTRL_NAK) {
        dev->nak_count++;                     /* no data this poll */
    } else {
        int len = (int)((st & TD_CTRL_ACTLEN) + 1);
        if (len > dev->ep_mps) len = dev->ep_mps;
        dev->toggle ^= 1;
        dev->report_count++;
        if (dev->on_report) dev->on_report(dev, dev->int_buf, len);
    }
    td_fill(dev->int_td, TD_LINK_TERMINATE, TD_PID_IN, dev->address,
            dev->ep_addr & 0xF, dev->ep_mps, dev->toggle, dev->low_speed,
            va_phys(dev->int_buf), 0);
    /* On a successful IN completion QEMU advances the QH elink to this
     * TD's link (TERMINATE) and writes it back into guest memory - the
     * re-armed TD would never be fetched again.  Restore the elink each
     * service (NAK leaves the elink untouched; writing it back is
     * idempotent). */
    int_qh[dev->slot]->elink = (uint32_t)(uint64_t)dev->int_td;
}

/* Called from usb_tick (IRQ0, every 10ms): re-arm/service interrupt TDs
 * that completed (NAK or data) so devices are polled at ~100 Hz no matter
 * which NAK semantics this QEMU build implements, and even if the UHCI
 * IRQ is never delivered (USBINTR stays 0). */
void uhci_idle_requeue(void) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t* dev = usb_device_at(i);
        if (!dev || dev->state != USB_DEV_RUNNING || !dev->int_td) continue;
        int_td_service(dev);
    }
}

/* --- IRQ handler ------------------------------------------------------ */

void uhci_irq_handler(interrupt_frame_t* frame) {
    (void)frame;
    if (!uhci_ready) { pic_send_eoi(ctrl_irq); return; }

    uint16_t sts = inw(io_base + UHCI_USBSTS);
    if (!(sts & (UHCI_STS_USBINT | UHCI_STS_ERROR))) {
        pic_send_eoi(ctrl_irq);               /* spurious / shared */
        return;
    }
    outw(io_base + UHCI_USBSTS, sts & 0x1F);  /* W1C */

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t* dev = usb_device_at(i);
        if (!dev || dev->state != USB_DEV_RUNNING || !dev->int_td) continue;
        int_td_service(dev);
    }

    pic_send_eoi(ctrl_irq);
}

/* --- root hub ports --------------------------------------------------- */

static uint16_t portsc(int port) { return io_base + UHCI_PORTSC(port); }

int uhci_port_connected(int port) {
    return (inw(portsc(port)) & UHCI_PSC_CCS) ? 1 : 0;
}

int uhci_port_low_speed(int port) {
    return (inw(portsc(port)) & UHCI_PSC_LSDA) ? 1 : 0;
}

void uhci_port_reset(int port) {
    uint16_t p = portsc(port);
    /* reset: PR 50ms -> settle -> enable.  All writes keep RES1. */
    outw(p, (inw(p) | UHCI_PSC_PR) | UHCI_PSC_RES1);
    pit_delay_ms(50);
    outw(p, (inw(p) & ~UHCI_PSC_PR) | UHCI_PSC_RES1);
    pit_delay_ms(10);
    outw(p, (inw(p) | UHCI_PSC_PE) | UHCI_PSC_RES1);
    pit_delay_ms(10);
}

void uhci_clear_port_change(int port) {
    uint16_t p = portsc(port);
    outw(p, (inw(p) | UHCI_PSC_CSC | UHCI_PSC_PEC) | UHCI_PSC_RES1);
}

/* --- diagnostics -------------------------------------------------------- */

void uhci_dump_controller(void) {
    char buf[96];
    ksprintf(buf, sizeof(buf),
             "[usb] UHCI io=0x%04x irq=%d cmd=0x%04x sts=0x%04x flist=0x%08x\n",
             io_base, ctrl_irq, inw(io_base + UHCI_USBCMD),
             inw(io_base + UHCI_USBSTS), frame_list_phys);
    klog(buf);
    for (int port = 1; port <= 2; port++) {
        uint16_t s = inw(portsc(port));
        ksprintf(buf, sizeof(buf),
                 "[usb] port %d: sc=0x%04x ccs=%d pe=%d ls=%d csc=%d\n",
                 port, s, s & 1, (s >> 2) & 1, (s >> 8) & 1, (s >> 1) & 1);
        klog(buf);
    }
}
