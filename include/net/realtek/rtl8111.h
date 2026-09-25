#ifndef RTL8111_H
#define RTL8111_H

#include "lib/types.h"

/* RTL8111F = Realtek RTL8168/8111 PCIe Gigabit family (0x10EC:0x8168).
 * Programming interface verified against Linux r8169 (r8169_main.c). */

#define RTL8111_VENDOR_ID   0x10EC
#define RTL8111_DEVICE_ID   0x8168

/* MMIO register offsets (BAR0) */
#define R8168_MAC0          0x00    /* 6 bytes, kept over soft reset */
#define R8168_MAR0          0x08    /* multicast filter, 2 dwords */
#define R8168_TXDESC_LO     0x20
#define R8168_TXDESC_HI     0x24
#define R8168_CHIP_CMD      0x37    /* 8-bit */
#define R8168_TX_POLL       0x38    /* 8-bit */
#define R8168_INTR_MASK     0x3C    /* 16-bit */
#define R8168_INTR_STATUS   0x3E    /* 16-bit, write-1-to-clear */
#define R8168_TX_CONFIG     0x40    /* 32-bit */
#define R8168_RX_CONFIG     0x44    /* 32-bit */
#define R8168_CFG9346       0x50    /* 8-bit config-regs lock */
#define R8168_CONFIG2       0x53    /* 8-bit */
#define R8168_CONFIG5       0x56    /* 8-bit */
#define R8168_PHYAR         0x60    /* GMII/MII access */
#define R8168_PHYSTATUS     0x6C    /* 8-bit link status */
#define R8168_RX_MAX_SIZE   0xDA    /* 16-bit */
#define R8168_ERIDR         0x70    /* ERI data */
#define R8168_ERIAR         0x74    /* ERI command */
#define R8168_EPHYAR        0x80    /* EPHY access */
#define R8168_DLLPR         0xD0    /* 8-bit */
#define R8168_MCU           0xD3    /* 8-bit */
#define R8168_CPLUS_CMD     0xE0    /* 16-bit */
#define R8168_INTR_MITIGATE 0xE2    /* 16-bit */
#define R8168_RXDESC_LO     0xE4
#define R8168_RXDESC_HI     0xE8
#define R8168_MAX_TX_PKT    0xEC    /* 8-bit, unit of 128 bytes */
#define R8168_MISC          0xF0    /* 32-bit */

/* ChipCmd bits */
#define R8168_CMD_RESET     0x10
#define R8168_CMD_RX_EN     0x08
#define R8168_CMD_TX_EN     0x04

/* TxPoll */
#define R8168_NPQ           0x40    /* kick normal-priority TX queue */

/* IntrMask/IntrStatus bits (same layout as 8139; the re-ordered set is 8125) */
#define R8168_INT_RXOK      0x0001
#define R8168_INT_RXERR     0x0002
#define R8168_INT_TXOK      0x0004
#define R8168_INT_TXERR     0x0008
#define R8168_INT_RXOVF     0x0010
#define R8168_INT_LINKCHG   0x0020
#define R8168_INT_FIFOOVF   0x0040
/* r8169 rtl_set_irq_mask for every chip >= VER_07 */
#define R8168_IRQ_MASK      (R8168_INT_RXOK | R8168_INT_RXERR | \
                             R8168_INT_TXOK | R8168_INT_TXERR | R8168_INT_LINKCHG)

/* RxConfig accept bits (same positions as 8139) */
#define R8168_RXC_ACCEPT_ALL   0x01
#define R8168_RXC_ACCEPT_MY    0x02
#define R8168_RXC_ACCEPT_MC    0x04
#define R8168_RXC_ACCEPT_BC    0x08
#define R8168_RXC_DMA_UNL      (7 << 8)
#define R8168_RXC_MULTI_EN     (1 << 14)   /* 8111c and later */
#define R8168_RXC_128_INT_EN   (1 << 15)   /* 8111c and later */

/* TxConfig */
#define R8168_TXC_IFG_SHIFT    24
#define R8168_TXC_IFG_SHORTEST (3 << R8168_TXC_IFG_SHIFT)
#define R8168_TXC_AUTO_FIFO    (1 << 7)    /* 8111e-vl and later */
#define R8168_TXC_DMA_UNL      (7 << 8)

/* Cfg9346 */
#define R8168_CFG_UNLOCK     0xC0
#define R8168_CFG_LOCK       0x00

/* Config2 / Config5 */
#define R8168_CFG2_CLKREQ_EN  (1 << 7)
#define R8168_CFG5_ASPM_EN    (1 << 0)
#define R8168_CFG5_SPI_EN     (1 << 3)

/* PHYstatus bits */
#define R8168_PHYST_LINK     0x02
#define R8168_PHYST_FULLDUP  0x01
#define R8168_PHYST_10M      0x04
#define R8168_PHYST_100M     0x08
#define R8168_PHYST_1000M    0x10

/* MCU / DLLPR / MISC low-power bits */
#define R8168_MCU_NOW_IS_OOB (1 << 7)
#define R8168_DLLPR_PFM_EN   (1 << 6)
#define R8168_MISC_PWM_EN    (1 << 22)

/* ERI command channel (ERIDR/ERIAR) */
#define R8168_ERI_FLAG          0x80000000u
#define R8168_ERI_WRITE_CMD     0x80000000u
#define R8168_ERI_READ_CMD      0x00000000u
#define R8168_ERI_TYPE_EXGMAC   (0x00 << 16)
#define R8168_ERI_MASK_SHIFT    12
#define R8168_ERI_MASK_0001     (0x1u << R8168_ERI_MASK_SHIFT)
#define R8168_ERI_MASK_0011     (0x3u << R8168_ERI_MASK_SHIFT)
#define R8168_ERI_MASK_1111     (0xFu << R8168_ERI_MASK_SHIFT)

/* EPHY access channel */
#define R8168_EPHY_FLAG       0x80000000u
#define R8168_EPHY_WRITE_CMD  0x80000000u
#define R8168_EPHY_REG_SHIFT  16
#define R8168_EPHY_REG_MASK   0x1f

/* MaxTxPacketSize: EarlySize value used by r8169 for 8168E-VL and later */
#define R8168_EARLY_SIZE      0x27

/* Descriptor bits (opts1, first doubleword) */
#define R8168_DESC_OWN    (1u << 31)  /* descriptor owned by NIC */
#define R8168_DESC_EOR    (1u << 30)  /* end of ring */
#define R8168_DESC_FS     (1u << 29)  /* first segment */
#define R8168_DESC_LS     (1u << 28)  /* last segment */
/* RX hardware write-back status in opts1 */
#define R8168_RX_RWT      (1u << 22)  /* rx watchdog timer expired */
#define R8168_RX_RES      (1u << 21)  /* receive error summary */
#define R8168_RX_RUNT     (1u << 20)
#define R8168_RX_CRC      (1u << 19)
#define R8168_RX_ERR_MASK (R8168_RX_RWT | R8168_RX_RES | R8168_RX_RUNT | R8168_RX_CRC)
/* length field: 14 bits, frame includes the 4-byte FCS */
#define R8168_RX_LEN_MASK 0x3FFFu

/* 16-byte descriptor, r8169 layout */
typedef struct {
    uint32_t opts1;   /* command (TX) / status (RX write-back) */
    uint32_t opts2;   /* checksum/VLAN (unused here) */
    uint64_t addr;    /* 64-bit buffer address */
} __attribute__((packed)) rtl8111_desc_t;

int rtl8111_init(void);
int rtl8111_send(const uint8_t* data, uint16_t len);
void rtl8111_rx_poll(void);

#endif
