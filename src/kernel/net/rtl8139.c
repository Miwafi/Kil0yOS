#include "net/rtl8139.h"
#include "net/netif.h"
#include "drivers/pci.h"
#include "drivers/io.h"
#include "mm/memory.h"
#include "core/interrupts.h"
#include "core/isr.h"
#include "lib/string.h"

#define RTL8139_REG_MAC0    0x00
#define RTL8139_REG_MAR0    0x08
#define RTL8139_REG_TSD0    0x10
#define RTL8139_REG_TSAD0   0x20
#define RTL8139_REG_RBSTART 0x30
#define RTL8139_REG_ERBCNT  0x34
#define RTL8139_REG_ERSCR   0x36
#define RTL8139_REG_CR      0x37
#define RTL8139_REG_CAPR    0x38
#define RTL8139_REG_CBR     0x3A
#define RTL8139_REG_IMR     0x3C
#define RTL8139_REG_ISR     0x3E
#define RTL8139_REG_TCR     0x40
#define RTL8139_REG_RCR     0x44

#define CR_RST  0x10
#define CR_RE   0x08
#define CR_TE   0x04
#define CR_BUFE 0x01

#define INT_ROK 0x0001
#define INT_TOK 0x0004

#define RX_BUF_SIZE 8192
#define TX_BUF_SIZE 1792
#define TX_DESC_COUNT 4

#define TSD_TOK 0x00008000 /* transmit OK, set by hardware on completion */
#define TSD_OWN 0x00002000 /* set while the controller owns the buffer */

static uint16_t io_base = 0;
static uint8_t rtl_irq = 0;
static uint8_t* rx_buffer = NULL;
static uint16_t rx_offset = 0;
static uint8_t* tx_buffers[TX_DESC_COUNT];
static int tx_current = 0;
static uint8_t tx_pending_mask = 0;

/* Drain the RX ring. Called from the IRQ handler and via g_netif.poll
 * from the socket wait loops - required before enable_interrupts()
 * (e.g. DHCP autoconfig runs during boot with IF=0). */
void rtl8139_rx_poll(void) {
    if (rx_buffer == NULL || io_base == 0) return;

    /* rx_offset and CAPR are shared between the ISR and poll-context
     * callers - an interrupt landing mid-drain would replay the same
     * descriptor and could write CAPR backwards. */
    int enabled = irq_save();
    disable_interrupts();

    int drained = 0;
    while ((inb(io_base + RTL8139_REG_CR) & CR_BUFE) == 0 && drained < 32) {
        drained++;
        uint16_t capr = inw(io_base + RTL8139_REG_CAPR);
        /* CAPR holds the read pointer minus 0x10 (per RTL8139 datasheet):
         * with RxBufPtr=0 the register reads 0xFFF0. Add the 0x10 back
         * before converting to a ring offset. */
        uint16_t offset = (uint16_t)(capr + 0x10) % RX_BUF_SIZE;

        /* The 4-byte status+length header can straddle the end of the
         * ring: the hardware wraps DMA writes byte-wise, so a flat
         * *(uint32_t*) read here picks up data from the ring start and
         * yields a bogus length - which would desync CAPR from the
         * hardware write pointer. */
        uint32_t header = 0;
        for (int k = 0; k < 4; k++) {
            ((uint8_t*)&header)[k] = rx_buffer[(offset + k) % RX_BUF_SIZE];
        }
        uint16_t pkt_len = header >> 16;
        uint8_t pkt_status = header & 0xFF;

        if (pkt_len < 4 || pkt_len > RX_BUF_SIZE) {
            /* pkt_len comes straight from DMA: a bogus value would push
             * CAPR past the hardware write pointer and desync the ring
             * forever. Skip the 4-byte header and resync on the next pass. */
            rx_offset = (offset + 4 + 3) & ~3;
            while (rx_offset >= RX_BUF_SIZE) rx_offset -= RX_BUF_SIZE;
            outw(io_base + RTL8139_REG_CAPR, rx_offset - 0x10);
            continue;
        }

        /* Deliver frames. A frame wrapping the ring end is reassembled
         * byte-wise into a linear buffer first: the hardware writes it
         * wrapped, so a direct pointer would hand up stale bytes from
         * outside the ring (IP checksums drop them; ARP has none). */
        uint16_t frame_len = (uint16_t)(pkt_len - 4);   /* minus CRC */
        if ((pkt_status & 0x01) && pkt_len > 4 && frame_len <= NET_MAX_PACKET) {
            if (RX_BUF_SIZE - offset >= (uint16_t)(pkt_len + 4)) {
                netif_receive(rx_buffer + offset + 4, frame_len);
            } else {
                uint8_t lin[NET_MAX_PACKET];
                for (uint16_t k = 0; k < frame_len; k++) {
                    lin[k] = rx_buffer[(offset + 4 + k) % RX_BUF_SIZE];
                }
                netif_receive(lin, frame_len);
            }
        }

        rx_offset = (offset + pkt_len + 4 + 3) & ~3;
        while (rx_offset >= RX_BUF_SIZE) rx_offset -= RX_BUF_SIZE;
        outw(io_base + RTL8139_REG_CAPR, rx_offset - 0x10);
    }

    irq_restore(enabled);
}

static void rtl8139_irq_handler(interrupt_frame_t* frame) {
    (void)frame;

    uint16_t status = inw(io_base + RTL8139_REG_ISR);
    outw(io_base + RTL8139_REG_ISR, status);

    if (status & INT_ROK) {
        rtl8139_rx_poll();
    }

    if (rtl_irq != 0) {
        pic_send_eoi(rtl_irq);
    }
}

void rtl8139_get_mac(uint8_t* out_mac) {
    for (int i = 0; i < 6; i++) {
        out_mac[i] = inb(io_base + RTL8139_REG_MAC0 + i);
    }
}

int rtl8139_send(const uint8_t* data, uint16_t len) {
    if (len > TX_BUF_SIZE) return -1;
    if (io_base == 0) return -1;

    /* Sends run from both the ISR (ICMP echo replies) and poll context -
     * keep the descriptor index and buffer copy atomic. */
    int enabled = irq_save();
    disable_interrupts();

    int idx = tx_current;

    /* The 4 TX buffers are reused round-robin: never overwrite one the
     * controller may still be DMA-reading. TOK means the transfer
     * finished; OWN still set means it is in flight. */
    if (tx_pending_mask & (1u << idx)) {
        uint32_t tsd = ind(io_base + RTL8139_REG_TSD0 + idx * 4);
        int timeout = 1000000;
        while ((tsd & TSD_TOK) == 0 && (tsd & TSD_OWN) != 0 && --timeout > 0) {
            __asm__ volatile("pause");
            tsd = ind(io_base + RTL8139_REG_TSD0 + idx * 4);
        }
    }
    tx_pending_mask &= (uint8_t)~(1u << idx);

    memcpy(tx_buffers[idx], data, len);

    uint32_t phys = (uint32_t)vmm_get_phys((uint64_t)tx_buffers[idx]);
    if (phys == 0) {
        irq_restore(enabled);
        return -1;
    }

    outd(io_base + RTL8139_REG_TSAD0 + idx * 4, phys);
    outd(io_base + RTL8139_REG_TSD0 + idx * 4, len);
    tx_pending_mask |= (uint8_t)(1u << idx);

    tx_current = (tx_current + 1) % TX_DESC_COUNT;
    irq_restore(enabled);
    return 0;
}

int rtl8139_init(void) {
    pci_device_t* dev = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!dev) return -1;

    io_base = (uint16_t)(dev->bar0 & ~0x3);
    if (io_base == 0) return -1;

    /* Enable I/O space access AND bus mastering: with the PCI Command
     * Bus Master bit clear, the device's DMA address space is unmapped
     * (QEMU pci_set_master(false)) and every TX descriptor fetch reads
     * zeros - the NIC transmitted correctly-shaped all-zero frames. */
    {
        uint16_t cmd = pci_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET);
        cmd |= 0x0005; /* IO Space + Bus Master */
        pci_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET, cmd);
    }

    /* Software reset with timeout */
    outb(io_base + RTL8139_REG_CR, CR_RST);
    int timeout = 100000;
    while ((inb(io_base + RTL8139_REG_CR) & CR_RST) != 0) {
        if (--timeout <= 0) return -1;
        __asm__ volatile("pause");
    }

    /* Read MAC */
    uint8_t mac[6];
    rtl8139_get_mac(mac);
    for (int i = 0; i < 6; i++) {
        g_netif.mac[i] = mac[i];
    }

    /* Init RX buffer */
    rx_buffer = (uint8_t*)kcalloc(1, RX_BUF_SIZE + 16 + 2048);
    if (!rx_buffer) return -1;
    uint32_t rx_phys = (uint32_t)vmm_get_phys((uint64_t)rx_buffer);
    if (rx_phys == 0) return -1;
    outd(io_base + RTL8139_REG_RBSTART, rx_phys);
    rx_offset = 0;

    /* Init TX buffers */
    for (int i = 0; i < TX_DESC_COUNT; i++) {
        tx_buffers[i] = (uint8_t*)kcalloc(1, TX_BUF_SIZE);
        if (!tx_buffers[i]) return -1;
    }
    tx_current = 0;

    /* Setup interrupt mask */
    outw(io_base + RTL8139_REG_IMR, INT_ROK | INT_TOK);

    /* Configure receiver */
    uint32_t rcr = 0x0000000F; /* AAP | APM | AM | AB */
    rcr |= (7 << 8);           /* unlimited DMA burst */
    rcr |= (0 << 12);          /* 8K+16 buffer */
    outd(io_base + RTL8139_REG_RCR, rcr);

    /* Configure transmitter */
    outd(io_base + RTL8139_REG_TCR, 0);

    /* Enable receiver and transmitter */
    outb(io_base + RTL8139_REG_CR, CR_RE | CR_TE);

    /* Register IRQ handler */
    if (dev->irq != 0 && dev->irq < 16) {
        rtl_irq = dev->irq;
        register_irq_handler(dev->irq, rtl8139_irq_handler);
        pic_enable_irq(dev->irq);
    } else {
        rtl_irq = 0; /* unknown line: polling mode via g_netif.poll */
    }

    g_netif.send = rtl8139_send;
    g_netif.poll = rtl8139_rx_poll;
    g_netif.flags = 1;
    return 0;
}
