#include "net/e1000.h"
#include "net/netif.h"
#include "drivers/pci.h"
#include "drivers/io.h"
#include "drivers/vga.h"
#include "mm/memory.h"
#include "core/interrupts.h"
#include "core/isr.h"
#include "lib/string.h"

#define E1000_CTRL      0x00000
#define E1000_STATUS    0x00008
#define E1000_EECD      0x00010
#define E1000_EERD      0x00014
#define E1000_CTRL_EXT  0x00018
#define E1000_ICR       0x000C0
#define E1000_IMS       0x000D0
#define E1000_RCTL      0x00100
#define E1000_TCTL      0x00400
#define E1000_TIPG      0x00410
#define E1000_RDBAL     0x02800
#define E1000_RDBAH     0x02804
#define E1000_RDTR      0x02820
#define E1000_RDLEN     0x02808
#define E1000_RDH       0x02810
#define E1000_RDT       0x02818
#define E1000_TDBAL     0x03800
#define E1000_TDBAH     0x03804
#define E1000_TDLEN     0x03808
#define E1000_TDH       0x03810
#define E1000_TDT       0x03818
#define E1000_RA        0x05400

#define CTRL_RST        (1 << 26)
#define CTRL_SLU        (1 << 6)

#define EECD_AUTO_RD    0x00000200 /* NVM auto-read done (MAC in RAL/RAH) */
#define CTRL_EXT_EE_RST 0x00002000 /* Reinitialize from EEPROM/flash */

#define RCTL_EN         (1 << 1)
#define RCTL_SBP        (1 << 2)
#define RCTL_UPE        (1 << 3)
#define RCTL_MPE        (1 << 4)
#define RCTL_LBM_NO     (0 << 6)
#define RCTL_BAM        (1 << 15)
#define RCTL_BSIZE_2048 (0 << 16)
#define RCTL_BSEX       (0 << 25)
#define RCTL_SECRC      (1 << 26)

#define TCTL_EN         (1 << 1)
#define TCTL_PSP        (1 << 3)
#define TCTL_CT_SHIFT   4
#define TCTL_COLD_SHIFT 12
#define TCTL_CT         (0x10 << TCTL_CT_SHIFT)
#define TCTL_COLD       (0x40 << TCTL_COLD_SHIFT)

#define CMD_EOP         (1 << 0)
#define CMD_IFCS        (1 << 1)
#define CMD_RS          (1 << 3)

#define NUM_TX 32
#define NUM_RX 32

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

static volatile uint32_t* mmio_base = NULL;
static uint8_t e1000_irq = 0;

static e1000_tx_desc_t* tx_descs_raw = NULL;
static e1000_tx_desc_t* tx_descs = NULL;
static uint8_t* tx_bufs[NUM_TX];
static uint64_t tx_phys[NUM_TX];
static int tx_tail = 0;

static e1000_rx_desc_t* rx_descs_raw = NULL;
static e1000_rx_desc_t* rx_descs = NULL;
static uint8_t* rx_bufs[NUM_RX];
static uint64_t rx_phys[NUM_RX];
static int rx_head = 0;
static int rx_tail = NUM_RX - 1;

static inline uint32_t e1000_read(uint32_t reg) {
    return mmio_base[reg / 4];
}

static inline void e1000_write(uint32_t reg, uint32_t val) {
    mmio_base[reg / 4] = val;
}

static void e1000_get_mac_internal(uint8_t* mac);

static void e1000_get_mac_eerd(uint8_t* mac) {
    /* 82574L (E1000E) leaves RA empty after reset - the MAC lives in
     * flash and must be fetched word-by-word through EERD:
     * bit0=start, bits15:8=word index, bit1=done, bits31:16=data */
    for (int i = 0; i < 3; i++) {
        e1000_write(E1000_EERD, 0x1 | (i << 8));
        int t = 100000;
        /* done bit differs by generation: 8257x = bit1, 8254x = bit4 */
        while (!(e1000_read(E1000_EERD) & 0x12)) {
            if (--t <= 0) break;
            __asm__ volatile("pause");
        }
        uint32_t v = e1000_read(E1000_EERD) >> 16;
        mac[i * 2] = v & 0xFF;
        mac[i * 2 + 1] = (v >> 8) & 0xFF;
    }
}

void e1000_rx_poll(void);

static void e1000_get_mac_internal(uint8_t* mac) {
    uint32_t ral = e1000_read(E1000_RA);
    uint32_t rah = e1000_read(E1000_RA + 4);
    mac[0] = ral & 0xFF;
    mac[1] = (ral >> 8) & 0xFF;
    mac[2] = (ral >> 16) & 0xFF;
    mac[3] = (ral >> 24) & 0xFF;
    mac[4] = rah & 0xFF;
    mac[5] = (rah >> 8) & 0xFF;
    if ((ral | rah) == 0) {
        /* Empty RA: 82574L loads its MAC from flash via EERD instead */
        e1000_get_mac_eerd(mac);
    }
}

static void e1000_irq_handler(interrupt_frame_t* frame) {
    (void)frame;
    uint32_t icr = e1000_read(E1000_ICR);
    if (icr & (1 << 6)) { /* RXT0 */
        e1000_rx_poll();
    }
    if (e1000_irq != 0) {
        pic_send_eoi(e1000_irq);
    }
}

/* Poll RX descriptors — called from ISR and also from the socket wait
 * loops via g_netif.poll. */
void e1000_rx_poll(void) {
    if (mmio_base == NULL) return;

    /* rx_head/rx_tail and the descriptor status bytes are shared between
     * the ISR and poll-context callers - an interrupt landing mid-drain
     * would replay the same descriptor and double-advance the ring. */
    int enabled = irq_save();
    disable_interrupts();

    while (1) {
        int idx = rx_head;
        if ((rx_descs[idx].status & 0x01) == 0) break;
        uint16_t len = rx_descs[idx].length;
        if (len > 0) {
            netif_receive(rx_bufs[idx], len);
        }
        rx_descs[idx].status = 0;
        rx_head = (rx_head + 1) % NUM_RX;
        rx_tail = (rx_tail + 1) % NUM_RX;
        e1000_write(E1000_RDT, rx_tail);
    }

    irq_restore(enabled);
}

int e1000_send(const uint8_t* data, uint16_t len) {
    if (len > 2048 || mmio_base == NULL) return -1;

    /* Sends run from both the ISR (ICMP echo replies) and poll context -
     * keep descriptor state atomic and avoid re-entering the RX poll. */
    int enabled = irq_save();
    disable_interrupts();

    int idx = tx_tail;
    if ((tx_descs[idx].status & 0x01) == 0) {
        irq_restore(enabled);
        return -1;
    }
    memcpy(tx_bufs[idx], data, len);
    tx_descs[idx].addr = tx_phys[idx];
    tx_descs[idx].length = len;
    tx_descs[idx].cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    tx_descs[idx].status = 0;

    /* Memory barrier: ensure descriptor writes are visible before NIC reads them */
    __asm__ volatile("mfence" ::: "memory");

    int submitted = idx;
    tx_tail = (tx_tail + 1) % NUM_TX;
    e1000_write(E1000_TDT, tx_tail);

    /* Wait for TX completion via the descriptor DD bit (RS was requested):
     * TDH only proves the NIC fetched the descriptor, not that the frame
     * left the wire. Bounded wait - a hung TX is reported as failure. */
    int timeout = 1000000;
    while ((tx_descs[submitted].status & 0x01) == 0) {
        if (--timeout <= 0) {
            irq_restore(enabled);
            return -1;
        }
        __asm__ volatile("pause");
    }

    irq_restore(enabled);
    return 0;
}

int e1000_init(void) {
    pci_device_t* dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID);
    if (!dev) dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID_82545);
    if (!dev) dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID_82574);
    if (!dev) dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID_82574LA);
    if (!dev) return -1;

    /* Enable bus mastering and memory space access */
    uint16_t cmd = pci_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET);
    cmd |= 0x0006; /* Memory Space + Bus Master */
    pci_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET, cmd);

    /* Read 64-bit BAR properly */
    uint32_t bar0_raw = pci_read_dword(dev->bus, dev->device, dev->function, PCI_BAR0_OFFSET);
    uint32_t bar1 = pci_read_dword(dev->bus, dev->device, dev->function, PCI_BAR1_OFFSET);
    /* BAR0 layout: bit0 = 0 (memory) / 1 (I/O), bits 2:1 = mem type
     * (00 = 32-bit, 10 = 64-bit). Only a 64-bit memory BAR uses BAR1
     * as the upper half - composing it blindly turns e.g. QEMU's
     * 82540EM I/O BAR (0x0000c001) into a non-canonical pointer and
     * #GPs on the first MMIO read. */
    if (bar0_raw == 0 || bar0_raw == 0xFFFFFFFF) {
        klog("[e1000] bad BAR0\n");
        return -1;
    }
    if (bar0_raw & 0x1) {
        klog("[e1000] BAR0 is I/O space, no MMIO access\n");
        return -1;
    }

    uint64_t bar = bar0_raw & ~0xFULL;
    if ((bar0_raw & 0x6) == 0x4) { /* 64-bit memory BAR */
        if (bar1 == 0xFFFFFFFF) {
            klog("[e1000] bad BAR1\n");
            return -1;
        }
        bar |= ((uint64_t)(bar1 & ~0xFULL)) << 32;
    }

    mmio_base = (volatile uint32_t*)bar;

    /* Software reset */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_RST);
    /* flash-backed NVM parts (82574 "E1000E") also need the EEPROM
     * reinit kick after the global reset (Linux e1000e: EE_RST) */
    e1000_write(E1000_CTRL_EXT, e1000_read(E1000_CTRL_EXT) | CTRL_EXT_EE_RST);
    int timeout = 100000;
    while (e1000_read(E1000_CTRL) & CTRL_RST) {
        if (--timeout <= 0) {
            klog("[e1000] reset timeout\n");
            return -1;
        }
        __asm__ volatile("pause");
    }

    /* Wait for the NVM auto-read that loads the MAC into RAL/RAH
     * (Linux e1000e: e1000e_get_auto_rd_done). Virtual machines may
     * never set the bit - warn and continue rather than fail. */
    timeout = 50000; /* ~50ms via io_wait() */
    while (!(e1000_read(E1000_EECD) & EECD_AUTO_RD)) {
        if (--timeout <= 0) {
            klog("[e1000] NVM auto-read not done (continuing)\n");
            break;
        }
        io_wait();
    }

    /* Set link up */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | CTRL_SLU);

    /* Wait for link up (VirtualBox has 5s LinkUpDelay) */
    {
        int link_timeout = 5000000;
        while (!(e1000_read(E1000_STATUS) & 2)) {
            if (--link_timeout <= 0) break;
            __asm__ volatile("pause");
        }
    }

    /* Read MAC and write back to RA with AV=1 */
    uint8_t mac[6];
    e1000_get_mac_internal(mac);
    {
        char buf[80];
        static const char hex[] = "0123456789abcdef";
        char* p = buf;
        const char* s = "[e1000] 8086:";
        while (*s) *p++ = *s++;
        uint16_t did = (uint16_t)(dev->device_id & 0xFFFF);
        for (int i = 3; i >= 0; i--) *p++ = hex[(did >> (i * 4)) & 0xF];
        s = " at 0:";
        while (*s) *p++ = *s++;
        *p++ = '0' + dev->bus; *p++ = ':';
        *p++ = '0' + dev->device; *p++ = ':';
        *p++ = '0' + dev->function;
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
    for (int i = 0; i < 6; i++) {
        g_netif.mac[i] = mac[i];
    }
    e1000_write(E1000_RA, mac[0] | (mac[1] << 8) | (mac[2] << 16) | (mac[3] << 24));
    e1000_write(E1000_RA + 4, mac[4] | (mac[5] << 8) | (1 << 31));

    /* Clear multicast table */
    for (int i = 0; i < 128; i++) {
        e1000_write(0x05200 + i * 4, 0);
    }

    /* Init TX descriptors */
    tx_descs_raw = (e1000_tx_desc_t*)kcalloc(1, sizeof(e1000_tx_desc_t) * NUM_TX + 16);
    if (!tx_descs_raw) return -1;
    tx_descs = (e1000_tx_desc_t*)(((uint64_t)tx_descs_raw + 15) & ~15);
    uint64_t tx_desc_phys = vmm_get_phys((uint64_t)tx_descs);
    if (tx_desc_phys == 0) return -1;

    for (int i = 0; i < NUM_TX; i++) {
        tx_bufs[i] = (uint8_t*)kcalloc(1, 2048);
        if (!tx_bufs[i]) return -1;
        tx_phys[i] = vmm_get_phys((uint64_t)tx_bufs[i]);
        if (tx_phys[i] == 0) return -1;
        tx_descs[i].addr = tx_phys[i];
        tx_descs[i].status = 0x01; /* DD=1, initially available */
    }
    tx_tail = 0;

    e1000_write(E1000_TDBAL, (uint32_t)tx_desc_phys);
    e1000_write(E1000_TDBAH, (uint32_t)(tx_desc_phys >> 32));
    e1000_write(E1000_TDLEN, NUM_TX * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_TDT, 0);

    /* Init RX descriptors */
    rx_descs_raw = (e1000_rx_desc_t*)kcalloc(1, sizeof(e1000_rx_desc_t) * NUM_RX + 16);
    if (!rx_descs_raw) return -1;
    rx_descs = (e1000_rx_desc_t*)(((uint64_t)rx_descs_raw + 15) & ~15);
    uint64_t rx_desc_phys = vmm_get_phys((uint64_t)rx_descs);
    if (rx_desc_phys == 0) return -1;

    for (int i = 0; i < NUM_RX; i++) {
        rx_bufs[i] = (uint8_t*)kcalloc(1, 2048);
        if (!rx_bufs[i]) return -1;
        rx_phys[i] = vmm_get_phys((uint64_t)rx_bufs[i]);
        if (rx_phys[i] == 0) return -1;
        rx_descs[i].addr = rx_phys[i];
        rx_descs[i].status = 0;
    }
    rx_head = 0;

    e1000_write(E1000_RDBAL, (uint32_t)rx_desc_phys);
    e1000_write(E1000_RDBAH, (uint32_t)(rx_desc_phys >> 32));
    e1000_write(E1000_RDLEN, NUM_RX * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_RDT, NUM_RX - 1);
    rx_tail = NUM_RX - 1;

    /* Enable receiver */
    e1000_write(E1000_RCTL,
        RCTL_EN | RCTL_SBP | RCTL_UPE | RCTL_MPE |
        RCTL_LBM_NO | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    /* Enable transmitter */
    e1000_write(E1000_TIPG, 0x0060200A);
    e1000_write(E1000_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    /* Clear any pending interrupts from the init phase */
    e1000_read(E1000_ICR);

    /* Set receive delay timer to 0 (no delay) and enable RX interrupt */
    e1000_write(E1000_RDTR, 0);
    e1000_write(E1000_IMS, (1 << 6)); /* RXT0 */

    if (dev->irq != 0 && dev->irq < 16) {
        e1000_irq = dev->irq;
        register_irq_handler(dev->irq, e1000_irq_handler);
        pic_enable_irq(dev->irq);
    } else {
        /* Line unknown (e.g. 0xFF) or out of PIC range: run in pure
         * polling mode - the RX poll in the socket wait loops covers it. */
        e1000_irq = 0;
    }

    g_netif.send = e1000_send;
    g_netif.poll = e1000_rx_poll;
    g_netif.flags = 1;
    return 0;
}
