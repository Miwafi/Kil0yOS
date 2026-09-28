/*
 * rtl8139_compat.c - Linux-style RTL8139 driver demonstrating the
 * Kil0yOS Linux-NIC-driver compatibility layer.
 *
 * This is a functional transplant of the core data path of Linux'
 * 8139too.c: PCI probe via id_table, pci_ioremap_bar on MMIO BAR1,
 * coherent RX ring + 4 TX descriptors, request_irq with IRQ_HANDLED,
 * netif_rx() upcall and ndo_start_xmit. All Linux APIs resolve to the
 * compat layer (include/linux/), none to real kernel code.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/delay.h>

#define DRV_NAME "8139compat"
#define RX_BUF_LEN_IDX 3              /* 8 KiB << 3 = 64 KiB */
#define RX_BUF_LEN  (8192u << RX_BUF_LEN_IDX)
#define NUM_TX_DESC 4
#define TX_BUF_SIZE 1536
#define RX_FRAME_HDR 4               /* status word + length word */

/* register offsets (MMIO) */
enum { IDR0 = 0x00, MAR0 = 0x08, TSR0 = 0x10, RCW0 = 0x20,
       RBSTART = 0x30, CR = 0x37, CAPR = 0x38, CBR = 0x3A,
       IMR = 0x3C, ISR = 0x3E, TCR = 0x40, RCR = 0x44,
       MSR = 0x58, BMCR = 0x62 };

enum CR_bits { CR_TE = 0x04, CR_RE = 0x08, CR_RST = 0x10 };
enum ISR_bits { ISR_ROK = 0x01, ISR_RER = 0x02, ISR_TOK = 0x04, ISR_TER = 0x08 };
enum TSD_bits { TSD_OWN = 0x80000000u, TSD_TOK = 0x2000, TSD_LEN_MASK = 0x1FFF };

struct rtl8139_compat_priv {
    void __iomem* mmio_addr;
    struct pci_dev* pdev;
    struct net_device* dev;
    dma_addr_t rx_ring_dma;
    __u8* rx_ring;
    unsigned int cur_rx;
    unsigned int dirty_rx;
    dma_addr_t tx_buf_dma[NUM_TX_DESC];
    __u8* tx_buf[NUM_TX_DESC];
    struct sk_buff* tx_skb[NUM_TX_DESC];
    unsigned int cur_tx, dirty_tx;
    int irq_registered;
};

static int rtl8139_compat_open(struct net_device* dev);
static int rtl8139_compat_start_xmit(struct sk_buff* skb, struct net_device* dev);

static void rtl8139_compat_setup(struct net_device* dev) {
    static const struct net_device_ops rtl8139_compat_ops = {
        .ndo_open       = rtl8139_compat_open,
        .ndo_stop       = 0,
        .ndo_start_xmit = rtl8139_compat_start_xmit,
        .ndo_set_rx_mode = 0,
    };
    dev->netdev_ops = &rtl8139_compat_ops;
}

/* ---------------- receive ---------------- */

static void rtl8139_compat_rx(struct net_device* dev) {
    struct rtl8139_compat_priv* tp = netdev_priv(dev);
    void __iomem* ioaddr = tp->mmio_addr;

    while (1) {
        unsigned int ring_offset = tp->cur_rx % RX_BUF_LEN;
        __u16 rx_status = readw(tp->rx_ring + ring_offset);
        __u16 wire_len  = readw(tp->rx_ring + ring_offset + 2);
        __u16 rx_size;
        if (!(rx_status & 0x01) || wire_len == 0) break;  /* no pending frame */
        rx_size = wire_len - 4;                  /* drop CRC */

        if (rx_size == 0 || rx_size > 1536) {    /* defensive: desync guard */
            writew(ISR_ROK, ioaddr + ISR);
            break;
        }

        struct sk_buff* skb = compat_alloc_skb(rx_size);
        if (!skb) {
            dev->stats.rx_dropped++;
            tp->cur_rx = (tp->cur_rx + rx_size + RX_FRAME_HDR + 3) & ~3u;
            continue;
        }
        memcpy(skb_put(skb, rx_size),
               tp->rx_ring + ring_offset + RX_FRAME_HDR, rx_size);
        skb->dev = dev;
        dev->stats.rx_packets++;
        dev->stats.rx_bytes += rx_size;
        netif_rx(skb);

        /* hardware consumed: 4B header + wire_len (payload+CRC), pad to 4 */
        tp->cur_rx = (tp->cur_rx + wire_len + RX_FRAME_HDR + 3) & ~3u;
        if (tp->cur_rx >= RX_BUF_LEN) tp->cur_rx -= RX_BUF_LEN;
        writew((unsigned int)(tp->cur_rx % RX_BUF_LEN) - 16u, ioaddr + CAPR);
    }
}

static irqreturn_t rtl8139_compat_interrupt(int irq, void* dev_instance) {
    struct net_device* dev = dev_instance;
    struct rtl8139_compat_priv* tp = netdev_priv(dev);
    void __iomem* ioaddr = tp->mmio_addr;
    __u16 status = readw(ioaddr + ISR);

    if (status == 0xFFFF) return IRQ_NONE;   /* device gone */
    writew(status, ioaddr + ISR);            /* W1C acknowledge */

    if (status & ISR_ROK) rtl8139_compat_rx(dev);

    if (status & (ISR_TOK | ISR_TER)) {      /* free completed TX skbs */
        while (tp->dirty_tx != tp->cur_tx) {
            int entry = tp->dirty_tx % NUM_TX_DESC;
            if (readl(ioaddr + TSR0 + entry * 4) & TSD_OWN) break;
            if (tp->tx_skb[entry]) {
                compat_kfree_skb(tp->tx_skb[entry]);
                tp->tx_skb[entry] = 0;
            }
            tp->dirty_tx++;
            dev->stats.tx_packets++;
        }
    }
    return IRQ_HANDLED;
}

/* ---------------- transmit ---------------- */

static int rtl8139_compat_start_xmit(struct sk_buff* skb, struct net_device* dev) {
    struct rtl8139_compat_priv* tp = netdev_priv(dev);
    void __iomem* ioaddr = tp->mmio_addr;
    int entry = tp->cur_tx % NUM_TX_DESC;

    __u32 tsr = readl(ioaddr + TSR0 + entry * 4);
    if (tsr & TSD_OWN) {
        /* all descriptors busy */
        return NETDEV_TX_BUSY;
    }
    if (tp->tx_skb[entry]) compat_kfree_skb(tp->tx_skb[entry]);

    unsigned int len = skb->len;
    if (len < 60) {                          /* pad to minimum Ethernet */
        memset(tp->tx_buf[entry] + len, 0, 60 - len);
        len = 60;
    }
    memcpy(tp->tx_buf[entry], skb->data, skb->len);
    dev->stats.tx_bytes += len;

    writel(tp->tx_buf_dma[entry], ioaddr + RCW0 + entry * 4);
    writel(len | TSD_OWN, ioaddr + TSR0 + entry * 4);
    tp->tx_skb[entry] = skb;                 /* freed at completion IRQ */
    tp->cur_tx++;
    return NETDEV_TX_OK;
}

/* ---------------- open / init ---------------- */

static int rtl8139_compat_open(struct net_device* dev) {
    struct rtl8139_compat_priv* tp = netdev_priv(dev);
    void __iomem* ioaddr = tp->mmio_addr;

    /* soft reset */
    writeb(CR_RST, ioaddr + CR);
    for (int i = 0; i < 100000 && (readb(ioaddr + CR) & CR_RST); i++)
        udelay(10);

    tp->cur_rx = tp->dirty_rx = 0;
    tp->cur_tx = tp->dirty_tx = 0;

    writel(tp->rx_ring_dma, ioaddr + RBSTART);
    writew(0xFFF0u, ioaddr + CAPR);          /* read pointer = -16 (8139too) */
    for (int i = 0; i < NUM_TX_DESC; i++)
        writel(tp->tx_buf_dma[i], ioaddr + RCW0 + i * 4);
    writel(0, ioaddr + RCR);
    /* FIFOth=7<<13 | RX_BUF_LEN_IDX=3 (64K, matches RX_BUF_LEN)<<11 |
     * DMA burst=7<<8 | accept AP+AM+AB+APM=0xF */
    writel(0x0000EF0Fu, ioaddr + RCR);
    writel(0x00000000, ioaddr + TCR);

    writew(ISR_ROK | ISR_RER | ISR_TOK | ISR_TER, ioaddr + ISR);
    writew(ISR_ROK | ISR_TOK, ioaddr + IMR);

    writeb(CR_TE | CR_RE, ioaddr + CR);
    dev->poll_hook = rtl8139_compat_rx;      /* main-loop rx drain */
    netif_carrier_on(dev);
    return 0;
}

static int rtl8139_compat_probe(struct pci_dev* pdev, const struct pci_device_id* id) {
    (void)id;
    if (!(pci_resource_flags(pdev, 1) & IORESOURCE_MEM)) return -ENODEV;

    struct net_device* dev = alloc_netdev(sizeof(struct rtl8139_compat_priv),
                                          "eth0", rtl8139_compat_setup);
    if (!dev) return -ENOMEM;
    struct rtl8139_compat_priv* tp = netdev_priv(dev);

    pci_enable_device(pdev);
    pci_set_master(pdev);
    tp->pdev = pdev;
    tp->dev = dev;
    tp->mmio_addr = pci_ioremap_bar(pdev, 1);
    dev->irq = pdev->irq;
    dev->base_addr = pdev->resource_start[0];
    dev->mem_start = pdev->resource_start[1];
    dev->mem_end = dev->mem_start + pdev->resource_len[1] - 1;

    /* MAC lives at IDR0 in MMIO space */
    for (int i = 0; i < ETH_ALEN; i++)
        dev->dev_addr[i] = readb(tp->mmio_addr + IDR0 + i);

    tp->rx_ring = dma_alloc_coherent(pdev, RX_BUF_LEN, &tp->rx_ring_dma, GFP_KERNEL);
    tp->tx_buf[0] = dma_alloc_coherent(pdev, TX_BUF_SIZE * NUM_TX_DESC,
                                       &tp->tx_buf_dma[0], GFP_KERNEL);
    if (!tp->rx_ring || !tp->tx_buf[0]) {
        printk(KERN_ERR DRV_NAME ": dma alloc failed\n");
        free_netdev(dev);
        return -ENOMEM;
    }
    for (int i = 1; i < NUM_TX_DESC; i++) {
        tp->tx_buf_dma[i] = tp->tx_buf_dma[0] + i * TX_BUF_SIZE;
        tp->tx_buf[i] = tp->tx_buf[0] + i * TX_BUF_SIZE;
    }

    if (compat_request_irq(pdev->irq, rtl8139_compat_interrupt, IRQF_SHARED,
                           DRV_NAME, dev) == 0)
        tp->irq_registered = 1;
    else
        printk(KERN_WARNING DRV_NAME ": request_irq(%d) failed, poll only\n", pdev->irq);

    rtl8139_compat_open(dev);
    return register_netdev(dev);
}

static const struct pci_device_id rtl8139_compat_tbl[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8139), .driver_data = 0 },
    { PCI_DEVICE(PCI_VENDOR_ID_DLINK,   0x1300), .driver_data = 1 },
    { PCI_DEVICE(PCI_VENDOR_ID_REALTEK, 0x8138), .driver_data = 2 },
    { 0, }
};

static struct pci_driver rtl8139_compat_driver = {
    .name     = DRV_NAME,
    .id_table = rtl8139_compat_tbl,
    .probe    = rtl8139_compat_probe,
    .remove   = 0,
};

/* Built-in init: register the driver with the compat PCI core. The
 * module_init() macro drops this into .compat_initcall for
 * compat_initcalls() to run after PCI enumeration. */
static int rtl8139_compat_init_entry(void) {
    return pci_register_driver(&rtl8139_compat_driver);
}

module_init(rtl8139_compat_init_entry);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kil0yOS compat demo");
MODULE_DESCRIPTION("Linux-style RTL8139 driver on the compat layer");
