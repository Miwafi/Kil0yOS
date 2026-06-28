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

static uint16_t io_base = 0;
static uint8_t rtl_irq = 0;
static uint8_t* rx_buffer = NULL;
static uint16_t rx_offset = 0;
static uint8_t* tx_buffers[TX_DESC_COUNT];
static int tx_current = 0;

static void rtl8139_irq_handler(interrupt_frame_t* frame) {
    (void)frame;

    uint16_t status = inw(io_base + RTL8139_REG_ISR);
    outw(io_base + RTL8139_REG_ISR, status);

    if (status & INT_ROK) {
        while ((inb(io_base + RTL8139_REG_CR) & CR_BUFE) == 0) {
            uint16_t capr = inw(io_base + RTL8139_REG_CAPR);
            uint16_t offset = capr % RX_BUF_SIZE;

            uint8_t* pkt = rx_buffer + offset;
            uint32_t header = *(uint32_t*)pkt;
            uint16_t pkt_len = header >> 16;
            uint8_t pkt_status = header & 0xFF;

            if ((pkt_status & 0x01) && pkt_len >= 4) {
                uint16_t data_len = pkt_len - 4;
                if (data_len > 0) {
                    netif_receive(pkt + 4, data_len);
                }
            }

            rx_offset = (offset + pkt_len + 4 + 3) & ~3;
            if (rx_offset >= RX_BUF_SIZE) rx_offset -= RX_BUF_SIZE;
            outw(io_base + RTL8139_REG_CAPR, rx_offset - 0x10);
        }
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

    int idx = tx_current;
    memcpy(tx_buffers[idx], data, len);

    uint32_t phys = (uint32_t)vmm_get_phys((uint64_t)tx_buffers[idx]);
    if (phys == 0) return -1;

    outd(io_base + RTL8139_REG_TSAD0 + idx * 4, phys);
    outd(io_base + RTL8139_REG_TSD0 + idx * 4, len);

    tx_current = (tx_current + 1) % TX_DESC_COUNT;
    return 0;
}

int rtl8139_init(void) {
    pci_device_t* dev = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!dev) return -1;

    io_base = (uint16_t)(dev->bar0 & ~0x3);
    if (io_base == 0) return -1;

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
    if (dev->irq != 0 && dev->irq != 0xFF) {
        rtl_irq = dev->irq;
        register_irq_handler(dev->irq, rtl8139_irq_handler);
        pic_enable_irq(dev->irq);
    }

    g_netif.send = rtl8139_send;
    g_netif.flags = 1;
    return 0;
}
