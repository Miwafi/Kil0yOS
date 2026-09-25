/* RTL8111F (Realtek RTL8168/8111 PCIe Gigabit) driver.
 *
 * Chip interface follows the Linux r8169 driver (project-local copy at
 * LinuxSample/linux-master/drivers/net/ethernet/realtek/r8169_main.c):
 * MMIO BAR0, 16-byte TX/RX descriptors, ChipCmd reset, NPQ TX kick, and the
 * RTL8111F-specific init workarounds (ERI register sequence + EPHY patch
 * table from rtl_hw_start_8168f/rtl_hw_start_8168f_1). ASPM/CLKREQ are kept
 * disabled on both the chip and the PCIe link control - r8169 documents ASPM
 * as a typical cause of tx timeouts / dead devices on this family.
 *
 * Modelled after e1000.c (MMIO + descriptor rings + sync TX wait) and
 * rtl8139.c (netif hookups, IRQ-in-handler EOI, polling fallback).
 */

#include "net/rtl8111.h"
#include "net/netif.h"
#include "drivers/pci.h"
#include "drivers/io.h"
#include "drivers/vga.h"
#include "mm/memory.h"
#include "core/interrupts.h"
#include "core/isr.h"
#include "lib/string.h"

#define NUM_TX 16
#define NUM_RX 16
#define BUF_SIZE 2048              /* one frame buffer per descriptor */
#define RING_ALIGN 256             /* descriptor ring base alignment */

/* PCI capability list */
#define PCI_CAP_PTR_OFFSET 0x34
#define PCI_CAP_ID_EXP     0x10
#define PCIE_LNKCTL        0x10
#define PCIE_LNKCTL_ASPM   0x0003  /* L0s | L1 */
#define PCIE_LNKCTL_CLKREQ 0x0100

static volatile uint8_t* mmio8 = NULL;
static uint8_t rtl8111_irq = 0;

static rtl8111_desc_t* tx_descs = NULL;
static void* tx_ring_raw = NULL;
static uint8_t* tx_bufs = NULL;
static uint64_t tx_bufs_phys = 0;
static int tx_cur = 0;

static rtl8111_desc_t* rx_descs = NULL;
static void* rx_ring_raw = NULL;
static uint8_t* rx_bufs = NULL;
static uint64_t rx_bufs_phys = 0;
static int rx_cur = 0;

static inline uint8_t rd8(uint32_t reg)  { return mmio8[reg]; }
static inline void wr8(uint32_t reg, uint8_t v)  { mmio8[reg] = v; }
static inline uint16_t rd16(uint32_t reg) { return *(volatile uint16_t*)(mmio8 + reg); }
static inline void wr16(uint32_t reg, uint16_t v) { *(volatile uint16_t*)(mmio8 + reg) = v; }
static inline uint32_t rd32(uint32_t reg) { return *(volatile uint32_t*)(mmio8 + reg); }
static inline void wr32(uint32_t reg, uint32_t v) { *(volatile uint32_t*)(mmio8 + reg) = v; }

static void rtl8111_log(const char* s) {
    klog("[rtl8111] ");
    klog(s);
    klog("\n");
}

/* ---- ERI (extended register) command channel ---- */

static uint32_t rtl8111_eri_read(uint32_t addr) {
    wr32(R8168_ERIAR, R8168_ERI_READ_CMD | R8168_ERI_TYPE_EXGMAC |
         R8168_ERI_MASK_1111 | addr);
    int t = 100000;
    while ((rd32(R8168_ERIAR) & R8168_ERI_FLAG) == 0) {
        if (--t <= 0) return 0;
        __asm__ volatile("pause");
    }
    return rd32(R8168_ERIDR);
}

static void rtl8111_eri_write(uint32_t addr, uint32_t mask, uint32_t val) {
    wr32(R8168_ERIDR, val);
    wr32(R8168_ERIAR, R8168_ERI_WRITE_CMD | R8168_ERI_TYPE_EXGMAC |
         mask | addr);
    int t = 100000;
    while (rd32(R8168_ERIAR) & R8168_ERI_FLAG) {
        if (--t <= 0) return;
        __asm__ volatile("pause");
    }
}

static void rtl8111_eri_w0w1(uint32_t addr, uint32_t p, uint32_t m) {
    uint32_t val = rtl8111_eri_read(addr);
    rtl8111_eri_write(addr, R8168_ERI_MASK_1111, (val & ~m) | p);
}

/* ---- EPHY (integrated PHY side register) channel ---- */

static uint16_t rtl8111_ephy_read(uint8_t reg) {
    wr32(R8168_EPHYAR, ((uint32_t)(reg & R8168_EPHY_REG_MASK)) << R8168_EPHY_REG_SHIFT);
    int t = 100000;
    while ((rd32(R8168_EPHYAR) & R8168_EPHY_FLAG) == 0) {
        if (--t <= 0) return 0;
        __asm__ volatile("pause");
    }
    return (uint16_t)(rd32(R8168_EPHYAR) & 0xffff);
}

static void rtl8111_ephy_write(uint8_t reg, uint16_t val) {
    wr32(R8168_EPHYAR, R8168_EPHY_WRITE_CMD |
         ((uint32_t)(reg & R8168_EPHY_REG_MASK)) << R8168_EPHY_REG_SHIFT | val);
    int t = 100000;
    while (rd32(R8168_EPHYAR) & R8168_EPHY_FLAG) {
        if (--t <= 0) return;
        __asm__ volatile("pause");
    }
}

/* rtl_ephy_init(r8169 e_info_8168f_1): read-modify-write PHY side patches */
static void rtl8111_ephy_init_8168f_1(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x06, 0x00c0, 0x0020 },
        { 0x08, 0x0001, 0x0002 },
        { 0x09, 0x0000, 0x0080 },
        { 0x19, 0x0000, 0x0224 },
        { 0x00, 0x0000, 0x0008 },
        { 0x0c, 0x3df0, 0x0200 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* Clear ASPM L0s/L1 and CLKREQ in the PCIe Link Control register.
 * r8169 comment: "ASPM compatibility issues are a typical reason for tx
 * timeouts"; pci_disable_link_state(PCIE_LINK_STATE_L1) at probe. */
static void rtl8111_pcie_disable_aspm(uint16_t bus, uint16_t device, uint16_t fn) {
    uint8_t ptr = pci_read_byte(bus, device, fn, PCI_CAP_PTR_OFFSET);
    for (int guard = 48; ptr != 0 && guard > 0; guard--) {
        uint8_t id = pci_read_byte(bus, device, fn, ptr);
        if (id == PCI_CAP_ID_EXP) {
            uint16_t lnkctl = pci_read_word(bus, device, fn, ptr + PCIE_LNKCTL);
            lnkctl &= ~(uint16_t)(PCIE_LNKCTL_ASPM | PCIE_LNKCTL_CLKREQ);
            pci_write_word(bus, device, fn, ptr + PCIE_LNKCTL, lnkctl);
            return;
        }
        ptr = pci_read_byte(bus, device, fn, (uint32_t)ptr + 1);
    }
}

/* rtl_hw_start_8168f (r8169): ERI workaround sequence for the 8168F MAC */
static void rtl8111_hw_start_8168f(void) {
    /* rtl_set_def_aspm_entry_latency: L0 7us, L1 16us (0x27) */
    rtl8111_eri_write(0xcc, R8168_ERI_MASK_0001, 0x27);
    rtl8111_eri_write(0xd0, R8168_ERI_MASK_0001, 0x00);
    rtl8111_eri_write(0xc0, R8168_ERI_MASK_0011, 0x0000);
    rtl8111_eri_write(0xb8, R8168_ERI_MASK_1111, 0x0000);
    /* rtl_set_fifo_size(0x10, 0x10, 0x02, 0x06) - pairs with TXCFG_AUTO_FIFO */
    rtl8111_eri_write(0xc8, R8168_ERI_MASK_1111, (0x10 << 16) | 0x02);
    rtl8111_eri_write(0xe8, R8168_ERI_MASK_1111, (0x10 << 16) | 0x06);
    /* rtl_reset_packet_filter: clear then set ERI 0xdc bit0 */
    rtl8111_eri_w0w1(0xdc, 0, 1 << 0);
    rtl8111_eri_w0w1(0xdc, 1 << 0, 0);
    rtl8111_eri_w0w1(0x1b0, 1 << 4, 0);
    rtl8111_eri_w0w1(0x1d0, (1 << 4) | (1 << 1), 0);
    rtl8111_eri_write(0xcc, R8168_ERI_MASK_1111, 0x00000050);
    rtl8111_eri_write(0xd0, R8168_ERI_MASK_1111, 0x00000060);
}

void rtl8111_rx_poll(void);

static void rtl8111_irq_handler(interrupt_frame_t* frame) {
    (void)frame;
    uint16_t status = rd16(R8168_INTR_STATUS);
    wr16(R8168_INTR_STATUS, status);   /* W1C */
    if (status & (R8168_INT_RXOK | R8168_INT_RXERR | R8168_INT_RXOVF)) {
        rtl8111_rx_poll();
    }
    if (rtl8111_irq != 0) {
        pic_send_eoi(rtl8111_irq);
    }
}

/* Drain the RX ring. Called from the ISR and via g_netif.poll from the
 * socket wait loops (required while IF=0, e.g. DHCP during boot). */
void rtl8111_rx_poll(void) {
    if (mmio8 == NULL || rx_descs == NULL) return;

    /* rx_cur is shared between the ISR and poll-context callers - an
     * interrupt landing mid-drain would replay the same descriptor. */
    int enabled = irq_save();
    disable_interrupts();

    int drained = 0;
    for (int n = 0; n < 32; n++) {
        rtl8111_desc_t* d = &rx_descs[rx_cur];
        uint32_t status = d->opts1;
        if (status & R8168_DESC_OWN) break;   /* NIC has not filled it yet */

        uint32_t eor = (rx_cur == NUM_RX - 1) ? R8168_DESC_EOR : 0;
        uint16_t wire_len = (uint16_t)(status & R8168_RX_LEN_MASK);
        /* Error frames are dropped; single-fragment frames only (frames
         * larger than RX_MAX_SIZE get split by the hardware - none should
         * occur with RMS=BUF_SIZE, but FS/LS guards against desync). */
        if ((status & R8168_RX_ERR_MASK) == 0 &&
            (status & R8168_DESC_FS) && (status & R8168_DESC_LS) &&
            wire_len >= 64 && wire_len <= BUF_SIZE) {
            /* the hardware frame carries a 4-byte FCS */
            netif_receive(rx_bufs + rx_cur * BUF_SIZE, (uint16_t)(wire_len - 4));
        }
        __asm__ volatile("mfence" ::: "memory");
        /* hand the buffer back: address is fixed, only opts1 changes */
        d->opts1 = R8168_DESC_OWN | eor | BUF_SIZE;
        rx_cur = (rx_cur + 1) % NUM_RX;
        drained++;
    }
    if (drained != 0) {
        rtl8111_log("rx drained");
    }

    irq_restore(enabled);
}

int rtl8111_send(const uint8_t* data, uint16_t len) {
    if (len == 0 || len > BUF_SIZE || mmio8 == NULL || tx_descs == NULL) return -1;

    /* Sends run from both the ISR (ICMP echo replies) and poll context -
     * keep the descriptor index and buffer copy atomic. */
    int enabled = irq_save();
    disable_interrupts();

    int idx = tx_cur;
    /* Round-robin reuse: never overwrite a descriptor the controller may
     * still be DMA-reading (Own set = in flight). Bounded wait. */
    if (tx_descs[idx].opts1 & R8168_DESC_OWN) {
        int timeout = 1000000;
        while (tx_descs[idx].opts1 & R8168_DESC_OWN) {
            if (--timeout <= 0) {
                rtl8111_log("tx desc stuck");
                irq_restore(enabled);
                return -1;
            }
            __asm__ volatile("pause");
        }
    }

    memcpy(tx_bufs + idx * BUF_SIZE, data, len);
    uint32_t eor = (idx == NUM_TX - 1) ? R8168_DESC_EOR : 0;
    tx_descs[idx].addr = tx_bufs_phys + (uint64_t)idx * BUF_SIZE;
    tx_descs[idx].opts2 = 0;
    __asm__ volatile("mfence" ::: "memory");
    /* Own must be written after addr/len are visible to DMA */
    tx_descs[idx].opts1 = R8168_DESC_FS | R8168_DESC_LS | eor | len;
    __asm__ volatile("mfence" ::: "memory");
    tx_descs[idx].opts1 |= R8168_DESC_OWN;
    wr8(R8168_TX_POLL, R8168_NPQ);

    /* Wait for completion (NIC clears Own once the frame left the wire).
     * Bounded wait - a hung TX is reported as failure. */
    int timeout = 1000000;
    while (tx_descs[idx].opts1 & R8168_DESC_OWN) {
        if (--timeout <= 0) {
            rtl8111_log("tx timeout");
            irq_restore(enabled);
            return -1;
        }
        __asm__ volatile("pause");
    }

    tx_cur = (tx_cur + 1) % NUM_TX;
    irq_restore(enabled);
    return 0;
}

int rtl8111_init(void) {
    pci_device_t* dev = pci_find_device(RTL8111_VENDOR_ID, RTL8111_DEVICE_ID);
    if (!dev) return -1;

    /* Enable memory space access AND bus mastering: with Bus Master clear
     * the device's DMA address space is unmapped (see rtl8139/e1000). */
    uint16_t cmd = pci_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET);
    cmd |= 0x0006; /* Memory Space + Bus Master */
    pci_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET, cmd);

    /* Read the 64-bit BAR0 properly (e1000 pattern). RTL8168 BAR0 is MMIO,
     * BAR1 is I/O - composing BAR1 blindly would corrupt the address. */
    uint32_t bar0_raw = pci_read_dword(dev->bus, dev->device, dev->function, PCI_BAR0_OFFSET);
    uint32_t bar1 = pci_read_dword(dev->bus, dev->device, dev->function, PCI_BAR1_OFFSET);
    if (bar0_raw == 0 || bar0_raw == 0xFFFFFFFF) {
        rtl8111_log("bad BAR0");
        return -1;
    }
    if (bar0_raw & 0x1) {
        rtl8111_log("BAR0 is I/O space, no MMIO access");
        return -1;
    }
    uint64_t bar = bar0_raw & ~0xFULL;
    if ((bar0_raw & 0x6) == 0x4) { /* 64-bit memory BAR */
        if (bar1 == 0xFFFFFFFF) {
            rtl8111_log("bad BAR1");
            return -1;
        }
        bar |= ((uint64_t)(bar1 & ~0xFULL)) << 32;
    }
    mmio8 = (volatile uint8_t*)bar;

    /* Chip version (r8169: xid = (TxConfig >> 20) & 0xfcf; RTL8111F = 0x48x) */
    uint32_t txconfig = rd32(R8168_TX_CONFIG);
    if (txconfig == 0xFFFFFFFF) {
        rtl8111_log("MMIO dead");
        return -1;
    }
    klog_hex("[rtl8111] TxConfig xid", (txconfig >> 20) & 0xfcf);

    /* Software reset: write CmdReset, poll until self-cleared */
    wr8(R8168_CHIP_CMD, R8168_CMD_RESET);
    int timeout = 100000;
    while (rd8(R8168_CHIP_CMD) & R8168_CMD_RESET) {
        if (--timeout <= 0) {
            rtl8111_log("reset timeout");
            return -1;
        }
        __asm__ volatile("pause");
    }

    /* Read MAC (IDR0 survives the soft reset) */
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) {
        mac[i] = rd8(R8168_MAC0 + i);
        g_netif.mac[i] = mac[i];
    }

    /* Allocate descriptor rings (256-byte aligned) and frame buffers.
     * kcalloc + vmm_get_phys, same DMA model as e1000 (identity map, PCIe
     * is DMA-coherent on this platform). */
    tx_ring_raw = kcalloc(1, NUM_TX * sizeof(rtl8111_desc_t) + RING_ALIGN);
    rx_ring_raw = kcalloc(1, NUM_RX * sizeof(rtl8111_desc_t) + RING_ALIGN);
    tx_bufs = (uint8_t*)kcalloc(1, NUM_TX * BUF_SIZE);
    rx_bufs = (uint8_t*)kcalloc(1, NUM_RX * BUF_SIZE);
    if (!tx_ring_raw || !rx_ring_raw || !tx_bufs || !rx_bufs) {
        rtl8111_log("alloc failed");
        return -1;
    }
    tx_descs = (rtl8111_desc_t*)(((uint64_t)tx_ring_raw + RING_ALIGN - 1) & ~(uint64_t)(RING_ALIGN - 1));
    rx_descs = (rtl8111_desc_t*)(((uint64_t)rx_ring_raw + RING_ALIGN - 1) & ~(uint64_t)(RING_ALIGN - 1));
    uint64_t tx_desc_phys = vmm_get_phys((uint64_t)tx_descs);
    uint64_t rx_desc_phys = vmm_get_phys((uint64_t)rx_descs);
    tx_bufs_phys = vmm_get_phys((uint64_t)tx_bufs);
    rx_bufs_phys = vmm_get_phys((uint64_t)rx_bufs);
    if (tx_desc_phys == 0 || rx_desc_phys == 0 ||
        tx_bufs_phys == 0 || rx_bufs_phys == 0) {
        rtl8111_log("vmm_get_phys failed");
        return -1;
    }

    /* TX ring: all descriptors CPU-owned (Own=0), RingEnd on the last one */
    for (int i = 0; i < NUM_TX; i++) {
        tx_descs[i].opts1 = (i == NUM_TX - 1) ? R8168_DESC_EOR : 0;
        tx_descs[i].opts2 = 0;
        tx_descs[i].addr = tx_bufs_phys + (uint64_t)i * BUF_SIZE;
    }
    /* RX ring: every descriptor handed to the NIC */
    for (int i = 0; i < NUM_RX; i++) {
        rx_descs[i].opts1 = R8168_DESC_OWN | BUF_SIZE |
                            ((i == NUM_RX - 1) ? R8168_DESC_EOR : 0);
        rx_descs[i].opts2 = 0;
        rx_descs[i].addr = rx_bufs_phys + (uint64_t)i * BUF_SIZE;
    }
    tx_cur = 0;
    rx_cur = 0;

    /* ---- rtl_hw_start (r8169) ---- */
    /* Config regs must be unlocked to touch Config2/Config5 */
    wr8(R8168_CFG9346, R8168_CFG_UNLOCK);
    /* Chip-side ASPM/clock-request off (r8169 does this before EPHY access) */
    wr8(R8168_CONFIG2, rd8(R8168_CONFIG2) & ~(uint8_t)R8168_CFG2_CLKREQ_EN);
    wr8(R8168_CONFIG5, rd8(R8168_CONFIG5) & ~(uint8_t)R8168_CFG5_ASPM_EN);
    /* CPlusCmd: no offloads, no INTT */
    wr16(R8168_CPLUS_CMD, 0);
    /* Early TX threshold (8168E-VL and later: r8169 EarlySize) */
    wr8(R8168_MAX_TX_PKT, R8168_EARLY_SIZE);
    /* 8168F MAC workarounds */
    rtl8111_hw_start_8168f();
    /* PCIe-side ASPM/CLKREQ off (r8169 pci_disable_link_state) */
    rtl8111_pcie_disable_aspm(dev->bus, dev->device, dev->function);
    /* Low-power knobs from rtl_hw_start_8168f */
    wr8(R8168_MCU, rd8(R8168_MCU) & ~(uint8_t)R8168_MCU_NOW_IS_OOB);
    wr8(R8168_DLLPR, rd8(R8168_DLLPR) | R8168_DLLPR_PFM_EN);
    wr32(R8168_MISC, rd32(R8168_MISC) | R8168_MISC_PWM_EN);
    wr8(R8168_CONFIG5, rd8(R8168_CONFIG5) & ~(uint8_t)R8168_CFG5_SPI_EN);
    /* PHY side patches */
    rtl8111_ephy_init_8168f_1();
    /* Disable interrupt coalescing */
    wr16(R8168_INTR_MITIGATE, 0);

    wr16(R8168_RX_MAX_SIZE, BUF_SIZE);
    /* Descriptor base addresses: High first, then Low */
    wr32(R8168_TXDESC_HI, (uint32_t)(tx_desc_phys >> 32));
    wr32(R8168_TXDESC_LO, (uint32_t)(tx_desc_phys & 0xFFFFFFFF));
    wr32(R8168_RXDESC_HI, (uint32_t)(rx_desc_phys >> 32));
    wr32(R8168_RXDESC_LO, (uint32_t)(rx_desc_phys & 0xFFFFFFFF));
    /* Config regs are writable again only while unlocked */
    wr8(R8168_CFG9346, R8168_CFG_LOCK);

    rd8(R8168_CHIP_CMD); /* PCI commit read */

    /* Enable TX + RX, then configure filters and Tx/Rx config */
    wr8(R8168_CHIP_CMD, R8168_CMD_TX_EN | R8168_CMD_RX_EN);
    wr32(R8168_RX_CONFIG, R8168_RXC_128_INT_EN | R8168_RXC_MULTI_EN |
         R8168_RXC_DMA_UNL |
         R8168_RXC_ACCEPT_BC | R8168_RXC_ACCEPT_MC | R8168_RXC_ACCEPT_MY);
    wr32(R8168_TX_CONFIG, R8168_TXC_IFG_SHORTEST | R8168_TXC_DMA_UNL |
         R8168_TXC_AUTO_FIFO);
    /* Multicast filter off (accept bits above cover our needs) */
    wr32(R8168_MAR0, 0);
    wr32(R8168_MAR0 + 4, 0);

    /* Interrupt mask: r8169 rtl_set_irq_mask for VER_07 and later */
    wr16(R8168_INTR_MASK, R8168_IRQ_MASK);
    wr16(R8168_INTR_STATUS, 0xFFFF); /* clear stale status */

    /* Probe log (e1000 style, hand-built hex) */
    {
        char buf[96];
        static const char hex[] = "0123456789abcdef";
        char* p = buf;
        const char* s = "[rtl8111] 10ec:8168 at ";
        while (*s) *p++ = *s++;
        *p++ = hex[(dev->bus >> 4) & 0xF]; *p++ = hex[dev->bus & 0xF]; *p++ = ':';
        *p++ = hex[(dev->device >> 4) & 0xF]; *p++ = hex[dev->device & 0xF]; *p++ = ':';
        *p++ = hex[(dev->function >> 4) & 0xF]; *p++ = hex[dev->function & 0xF];
        s = " mac=";
        while (*s) *p++ = *s++;
        for (int i = 0; i < 6; i++) {
            *p++ = hex[(mac[i] >> 4) & 0xF];
            *p++ = hex[mac[i] & 0xF];
            if (i < 5) *p++ = ':';
        }
        s = " irq=0x";
        while (*s) *p++ = *s++;
        *p++ = hex[(dev->irq >> 4) & 0xF];
        *p++ = hex[dev->irq & 0xF];
        *p++ = '\n'; *p = 0;
        klog(buf);
    }

    /* Wait for link (bounded; PHY autonegotiates on its own). Each poll is
     * an MMIO read - mark the wait on the serial log so a slow cold start
     * is not mistaken for a hang. */
    rtl8111_log("waiting for link...");
    timeout = 5000000;
    while (!(rd8(R8168_PHYSTATUS) & R8168_PHYST_LINK)) {
        if (--timeout <= 0) break;
        __asm__ volatile("pause");
    }
    if (rd8(R8168_PHYSTATUS) & R8168_PHYST_LINK) {
        uint8_t ps = rd8(R8168_PHYSTATUS);
        if (ps & R8168_PHYST_1000M)      rtl8111_log("link up 1000M");
        else if (ps & R8168_PHYST_100M)  rtl8111_log("link up 100M");
        else                             rtl8111_log("link up 10M");
    } else {
        rtl8111_log("link down (continuing)");
    }

    /* IRQ registration; unknown line -> pure polling via g_netif.poll */
    if (dev->irq != 0 && dev->irq < 16) {
        rtl8111_irq = dev->irq;
        register_irq_handler(dev->irq, rtl8111_irq_handler);
        pic_enable_irq(dev->irq);
    } else {
        rtl8111_irq = 0;
    }

    g_netif.send = rtl8111_send;
    g_netif.poll = rtl8111_rx_poll;
    g_netif.flags = 1;
    return 0;
}
