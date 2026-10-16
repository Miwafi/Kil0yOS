#ifndef RTL8111_H
#define RTL8111_H

#include "lib/types.h"

/* RTL8111F/G = Realtek RTL8168/8111 PCIe Gigabit family (0x10EC:0x8168).
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

/* Config2 / Config3 / Config5 */
#define R8168_CFG2_CLKREQ_EN  (1 << 7)
#define R8168_CFG3_RDY_L23    (1 << 1)     /* L23 Enable, PCIe state L2/L3 */
#define R8168_CFG5_ASPM_EN    (1 << 0)
#define R8168_CFG5_SPI_EN     (1 << 3)

/* MISC bits */
#define R8168_MISC_RXDVGATE   (1 << 19)    /* RXDV_GATED_EN */

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

/* Config3 register offset (Config2=0x53, Config3=0x54, Config4=0x55, Config5=0x56) */
#define R8168_CONFIG3         0x54    /* 8-bit */

/* EPHY access channel */
#define R8168_EPHY_FLAG       0x80000000u
#define R8168_EPHY_WRITE_CMD  0x80000000u
#define R8168_EPHY_REG_SHIFT  16
#define R8168_EPHY_REG_MASK   0x1f

/* Chip xid (extended version id) extracted from (TxConfig >> 20) & 0xfcf.
 * The xid uniquely identifies the MAC revision inside the 0x10ec:0x8168
 * PCI device family (RTL8168/8111 Gigabit series). Values from Linux r8169
 * rtl_chip_infos[] mask/val pairs (mask=0x7cf for all entries below).
 *
 * Supported chips (VER_25..VER_52, all VID:DID = 10ec:8168):
 *   G-family (VER_40..52):  8168fp/8117, 8168ep/8111ep, 8168h/8111h,
 *                           8411b, 8168gu/8111gu, 8168g/8111g/8111GN
 *   F-family (VER_32..38):  8411, 8168f/8111f, 8168evl/8111evl,
 *                           8168e/8111e
 *   D-family (VER_25..31):  8168d/8111d, 8168dp/8111dp
 */
#define RTL_XID_MASK          0xfcf

/* --- G-family (VER_40..52) --- */
#define RTL_XID_G             0x4c0    /* RTL8168g/8111g       VER_40 */
#define RTL_XID_GU            0x509    /* RTL8168gu/8111gu     VER_42 */
#define RTL_XID_8411B         0x5c8    /* RTL8411b             VER_44 */
#define RTL_XID_H1            0x541    /* RTL8168h/8111h       VER_46 */
#define RTL_XID_H2            0x6c0    /* RTL8168M             VER_46 */
#define RTL_XID_EP            0x502    /* RTL8168ep/8111ep     VER_51 (GN?) */
#define RTL_XID_FP1           0x54a    /* RTL8168fp/8117       VER_52 */
#define RTL_XID_FP2           0x54b    /* RTL8168fp/8117       VER_52 */

/* --- F-family (VER_32..38) --- */
#define RTL_XID_F1            0x480    /* RTL8168f/8111f       VER_35 */
#define RTL_XID_F2            0x481    /* RTL8168f/8111f       VER_36 */
#define RTL_XID_8411          0x488    /* RTL8411              VER_38 */
#define RTL_XID_E1            0x2c1    /* RTL8168e/8111e       VER_32 */
#define RTL_XID_E2            0x2c0    /* RTL8168e/8111e       VER_33 */
#define RTL_XID_EVL           0x2c8    /* RTL8168evl/8111evl   VER_34 */

/* --- D-family (VER_25..31) --- */
#define RTL_XID_D1            0x281    /* RTL8168d/8111d       VER_25 */
#define RTL_XID_D2            0x280    /* RTL8168d/8111d       VER_26 */
#define RTL_XID_DP1           0x28a    /* RTL8168dp/8111dp     VER_28 */
#define RTL_XID_DP2           0x28b    /* RTL8168dp/8111dp     VER_31 */

enum rtl_mac_family {
    RTL_MAC_G,      /* VER_40..52: 8111G/GN/GU/H/EP/FP/117 */
    RTL_MAC_F,      /* VER_32..38: 8111F/E/EVL, 8411       */
    RTL_MAC_D,      /* VER_25..31: 8111D/DP                */
    RTL_MAC_UNKNOWN
};

/* hw_start variant selected per xid (from Linux rtl_hw_start_* dispatch) */
enum rtl_hw_variant {
    RTL_HW_G_BASE,     /* rtl_hw_start_8168g   : VER_40 base */
    RTL_HW_GU,         /* rtl_hw_start_8168g_2 : VER_42       */
    RTL_HW_8411B,      /* rtl_hw_start_8411_2  : VER_44       */
    RTL_HW_H,          /* rtl_hw_start_8168h_1 : VER_46       */
    RTL_HW_EP,         /* rtl_hw_start_8168ep_3: VER_51       */
    RTL_HW_FP,         /* rtl_hw_start_8117    : VER_52       */
    RTL_HW_F,          /* rtl_hw_start_8168f   : VER_35/36    */
    RTL_HW_8411,       /* rtl_hw_start_8411    : VER_38       */
    RTL_HW_E,          /* rtl_hw_start_8168e_1 : VER_32/33    */
    RTL_HW_EVL,        /* rtl_hw_start_8168e_2 : VER_34       */
    RTL_HW_D,          /* rtl_hw_start_8168d   : VER_25/26    */
    RTL_HW_DP4,        /* rtl_hw_start_8168d_4 : VER_28/31    */
    RTL_HW_UNKNOWN,
};

/* Per-xid dispatch result (filled by rtl_xid_detect) */
struct rtl_chip_id {
    enum rtl_mac_family family;
    enum rtl_hw_variant hw;
    const char* name;      /* short display string */
};

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
