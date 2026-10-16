/* RTL8111F/G (Realtek RTL8168/8111 PCIe Gigabit) driver.
 *
 * Programming interface follows the Linux r8169 driver (project-local copy
 * at LinuxSample/linux-master/drivers/net/ethernet/realtek/r8169_main.c):
 * MMIO BAR0, 16-byte TX/RX descriptors, ChipCmd reset, NPQ TX kick, and
 * per-family init workarounds (ERI register sequence + EPHY patch table
 * selected by xid at probe). ASPM/CLKREQ are kept disabled on both the
 * chip and the PCIe link control - r8169 documents ASPM as a typical
 * cause of tx timeouts / dead devices on this family.
 *
 * Currently supports:
 *   RTL8111F / MAC_VER_35/36 (xid 0x480/0x481)  - rtl_hw_start_8168f
 *   RTL8111GN / MAC_VER_40   (xid 0x4c0)        - rtl_hw_start_8168g + ephy_g_1
 *   RTL8111GU / MAC_VER_42   (xid 0x509)        - rtl_hw_start_8168g + ephy_g_2
 *
 * Modelled after e1000.c (MMIO + descriptor rings + sync TX wait) and
 * rtl8139.c (netif hookups, IRQ-in-handler EOI, polling fallback).
 */

#include "net/realtek/rtl8111.h"
#include "net/netif.h"
#include "drivers/pci.h"
#include "drivers/io.h"
#include "drivers/video/vga.h"
#include "mm/memory.h"
#include "core/interrupts.h"
#include "core/isr.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include <stdarg.h>

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

/* Scratch buffer for ksprintf-formatted diagnostics (single-threaded init path). */
static char rtl8111_tmp[128];

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

/* rtl_ephy_init(r8169 e_info_8168g_1): RTL8111GN (MAC_VER_40, xid 0x4c0) */
static void rtl8111_ephy_init_8168g_1(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x00, 0x0008, 0x0000 },
        { 0x0c, 0x3ff0, 0x0820 },
        { 0x1e, 0x0000, 0x0001 },
        { 0x19, 0x8000, 0x0000 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8168g_2): RTL8111GU (MAC_VER_42, xid 0x509) */
static void rtl8111_ephy_init_8168g_2(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x00, 0x0008, 0x0000 },
        { 0x0c, 0x3ff0, 0x0820 },
        { 0x19, 0xffff, 0x7c00 },
        { 0x1e, 0xffff, 0x20eb },
        { 0x0d, 0xffff, 0x1666 },
        { 0x00, 0xffff, 0x10a3 },
        { 0x06, 0xffff, 0xf050 },
        { 0x04, 0x0000, 0x0010 },
        { 0x1d, 0x4000, 0x0000 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8168d_4): RTL8168dp/8111dp (MAC_VER_28/31) */
static void rtl8111_ephy_init_8168d_4(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x0b, 0x0000, 0x0048 },
        { 0x19, 0x0020, 0x0050 },
        { 0x0c, 0x0100, 0x0020 },
        { 0x10, 0x0004, 0x0000 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8168e_1): RTL8168e/8111e (MAC_VER_32/33) */
static void rtl8111_ephy_init_8168e_1(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x00, 0x0200, 0x0100 },
        { 0x00, 0x0000, 0x0004 },
        { 0x06, 0x0002, 0x0001 },
        { 0x06, 0x0000, 0x0030 },
        { 0x07, 0x0000, 0x2000 },
        { 0x00, 0x0000, 0x0020 },
        { 0x03, 0x5800, 0x2000 },
        { 0x03, 0x0000, 0x0001 },
        { 0x01, 0x0800, 0x1000 },
        { 0x07, 0x0000, 0x4000 },
        { 0x1e, 0x0000, 0x2000 },
        { 0x19, 0xffff, 0xfe6c },
        { 0x0a, 0x0000, 0x0040 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8168e_2): RTL8168evl/8111evl (MAC_VER_34) */
static void rtl8111_ephy_init_8168e_2(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x09, 0x0000, 0x0080 },
        { 0x19, 0x0000, 0x0224 },
        { 0x00, 0x0000, 0x0004 },
        { 0x0c, 0x3df0, 0x0200 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8411): RTL8411 (MAC_VER_38) */
static void rtl8111_ephy_init_8411(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x06, 0x00c0, 0x0020 },
        { 0x0f, 0xffff, 0x5200 },
        { 0x19, 0x0000, 0x0224 },
        { 0x00, 0x0000, 0x0008 },
        { 0x0c, 0x3df0, 0x0200 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8411_2): RTL8411b (MAC_VER_44) */
static void rtl8111_ephy_init_8411b(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x00, 0x0008, 0x0000 },
        { 0x0c, 0x37d0, 0x0820 },
        { 0x1e, 0x0000, 0x0001 },
        { 0x19, 0x8021, 0x0000 },
        { 0x1e, 0x0000, 0x2000 },
        { 0x0d, 0x0100, 0x0200 },
        { 0x00, 0x0000, 0x0080 },
        { 0x06, 0x0000, 0x0010 },
        { 0x04, 0x0000, 0x0010 },
        { 0x1d, 0x0000, 0x4000 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8168h_1): RTL8168h/8111h/M (MAC_VER_46) */
static void rtl8111_ephy_init_8168h_1(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x1e, 0x0800, 0x0001 },
        { 0x1d, 0x0000, 0x0800 },
        { 0x05, 0xffff, 0x2089 },
        { 0x06, 0xffff, 0x5881 },
        { 0x04, 0xffff, 0x854a },
        { 0x01, 0xffff, 0x068b },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8168ep_3): RTL8168ep/8111ep (MAC_VER_51, 实体机 RTL8111GN 最可能是这个!) */
static void rtl8111_ephy_init_8168ep_3(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x00, 0x0000, 0x0080 },
        { 0x0d, 0x0100, 0x0200 },
        { 0x19, 0x8021, 0x0000 },
        { 0x1e, 0x0000, 0x2000 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_ephy_init(r8169 e_info_8117): RTL8168fp/8117 (MAC_VER_52) */
static void rtl8111_ephy_init_8117(void) {
    static const struct { uint8_t reg; uint16_t mask, bits; } table[] = {
        { 0x19, 0x0040, 0x1100 },
        { 0x59, 0x0040, 0x1100 },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint16_t w = (rtl8111_ephy_read(table[i].reg) & ~table[i].mask) | table[i].bits;
        rtl8111_ephy_write(table[i].reg, w);
    }
}

/* rtl_hw_start_8168g (r8169): RTL8168G/8111G MAC init sequence.
 * Key differences from 8168F: FIFO sizes 0x08/0x10 instead of 0x10/0x10,
 * pause thresholds set via ERI 0xcc/0xd0 (these are the FINAL values - do
 * NOT overwrite them). ERI 0x2f8 magic write, RXDV gate disabled (MISC bit
 * 19), Config3 L2/L3 disabled. */
static void rtl8111_hw_start_8168g(void) {
    /* rtl_set_fifo_size(0x08, 0x10, 0x02, 0x06) */
    rtl8111_eri_write(0xc8, R8168_ERI_MASK_1111, (0x08 << 16) | 0x02);
    rtl8111_eri_write(0xe8, R8168_ERI_MASK_1111, (0x10 << 16) | 0x06);
    /* rtl8168g_set_pause_thresholds(0x38, 0x48) - final values, not ASPM! */
    rtl8111_eri_write(0xcc, R8168_ERI_MASK_0001, 0x38);
    rtl8111_eri_write(0xd0, R8168_ERI_MASK_0001, 0x48);
    /* rtl_reset_packet_filter: clear then set ERI 0xdc bit0 */
    rtl8111_eri_w0w1(0xdc, 0, 1 << 0);
    rtl8111_eri_w0w1(0xdc, 1 << 0, 0);
    /* ERI magic for 8168G family */
    rtl8111_eri_write(0x2f8, R8168_ERI_MASK_0011, 0x1d8f);
    /* rtl_disable_rxdvgate: clear MISC bit 19 (RXDV_GATED_EN) */
    wr32(R8168_MISC, rd32(R8168_MISC) & ~R8168_MISC_RXDVGATE);
    rtl8111_eri_write(0xc0, R8168_ERI_MASK_0011, 0x0000);
    rtl8111_eri_write(0xb8, R8168_ERI_MASK_0011, 0x0000);
    /* rtl_w0w1_eri(0x2fc, 0x01, 0x06) */
    rtl8111_eri_w0w1(0x2fc, 0x01, 0x06);
    /* rtl_eri_clear_bits(0x1b0, BIT(12)) */
    rtl8111_eri_w0w1(0x1b0, 0, 1 << 12);
    /* rtl_pcie_state_l2l3_disable: clear Config3 Rdy_to_L23 */
    wr8(R8168_CONFIG3, rd8(R8168_CONFIG3) & ~(uint8_t)R8168_CFG3_RDY_L23);
}

/* rtl_hw_start_8168ep (r8169 base for VER_51/52): EP/FP family.
 * Similar to 8168g but with different pause thresholds (0x2f/0x5f). */
static void rtl8111_hw_start_8168ep(void) {
    rtl8111_eri_write(0xc8, R8168_ERI_MASK_1111, (0x08 << 16) | 0x02);
    rtl8111_eri_write(0xe8, R8168_ERI_MASK_1111, (0x10 << 16) | 0x06);
    /* Pause thresholds for EP/FP family: 0x2f low, 0x5f high */
    rtl8111_eri_write(0xcc, R8168_ERI_MASK_0001, 0x2f);
    rtl8111_eri_write(0xd0, R8168_ERI_MASK_0001, 0x5f);
    rtl8111_eri_w0w1(0xdc, 0, 1 << 0);
    rtl8111_eri_w0w1(0xdc, 1 << 0, 0);
    rtl8111_eri_write(0x5f0, R8168_ERI_MASK_0011, 0x4f87);
    wr32(R8168_MISC, rd32(R8168_MISC) & ~R8168_MISC_RXDVGATE);
    rtl8111_eri_write(0xc0, R8168_ERI_MASK_0011, 0x0000);
    rtl8111_eri_write(0xb8, R8168_ERI_MASK_0011, 0x0000);
    rtl8111_eri_w0w1(0x2fc, 0x01, 0x06);
    wr8(R8168_CONFIG3, rd8(R8168_CONFIG3) & ~(uint8_t)R8168_CFG3_RDY_L23);
}

/* rtl_hw_start_8168h_1 extra ERI for H-family (VER_46): same base as G,
 * additional 0xdc bit set, 0x5f0 write, DLLPR PFM off */
static void rtl8111_hw_start_8168h_extras(void) {
    /* rtl_eri_set_bits(tp, 0xdc, 0x001c): set bits 2,3,4 of 0xdc */
    rtl8111_eri_w0w1(0xdc, (1 << 2) | (1 << 3) | (1 << 4), 0);
    rtl8111_eri_write(0x5f0, R8168_ERI_MASK_0011, 0x4f87);
    rtl8111_eri_w0w1(0x1b0, 0, 1 << 12);
    /* DLLPR &= ~PFM_EN (0x06) */
    wr8(R8168_DLLPR, rd8(R8168_DLLPR) & ~(uint8_t)R8168_DLLPR_PFM_EN);
}

/* hw_start_8117 extra ERI for FP family (VER_52): 0xd4 bit set */
static void rtl8111_hw_start_8117_extras(void) {
    rtl8111_eri_w0w1(0xd4, 1 << 4, 0);
}

/* rtl_hw_start_8168e_2 (VER_34): VER_32-34 share this base, FIFO same as F
 * but pause thresholds are unique. */
static void rtl8111_hw_start_8168e_2(void) {
    rtl8111_eri_write(0xc0, R8168_ERI_MASK_0011, 0x0000);
    rtl8111_eri_write(0xb8, R8168_ERI_MASK_1111, 0x0000);
    rtl8111_eri_write(0xc8, R8168_ERI_MASK_1111, (0x10 << 16) | 0x02);
    rtl8111_eri_write(0xe8, R8168_ERI_MASK_1111, (0x10 << 16) | 0x06);
    rtl8111_eri_w0w1(0x1d0, 1 << 1, 0);
    rtl8111_eri_w0w1(0xdc, 0, 1 << 0);
    rtl8111_eri_w0w1(0xdc, 1 << 0, 0);
    rtl8111_eri_w0w1(0x1b0, 1 << 4, 0);
    rtl8111_eri_write(0xcc, R8168_ERI_MASK_1111, 0x00000050);
    rtl8111_eri_write(0xd0, R8168_ERI_MASK_1111, 0x07ff0060);
}

/* rtl_hw_start_8168d (VER_25/26): very old MAC, minimal init */
static void rtl8111_hw_start_8168d(void) {
    /* rtl_disable_clock_request: clear Config2 CLKREQ */
    wr8(R8168_CONFIG2, rd8(R8168_CONFIG2) & ~(uint8_t)R8168_CFG2_CLKREQ_EN);
}

/* rtl_hw_start_8168d_4 (VER_28/31): ephy + enable clock request */
static void rtl8111_hw_start_8168d_4(void) {
    wr8(R8168_CONFIG2, rd8(R8168_CONFIG2) | (uint8_t)R8168_CFG2_CLKREQ_EN);
}

/* Low-power knobs common to ALL G/F family variants (PFM_EN on, SPI_EN off).
 * H-family is the ONLY exception - hw_start_8168h_1 explicitly clears
 * DLLPR PFM_EN and must NOT have a generic lowpower block afterwards. */
static void rtl8111_hw_lowpower_generic(void) {
    wr8(R8168_MCU, rd8(R8168_MCU) & ~(uint8_t)R8168_MCU_NOW_IS_OOB);
    wr8(R8168_DLLPR, rd8(R8168_DLLPR) | R8168_DLLPR_PFM_EN);
    wr32(R8168_MISC, rd32(R8168_MISC) | R8168_MISC_PWM_EN);
    wr8(R8168_CONFIG5, rd8(R8168_CONFIG5) & ~(uint8_t)R8168_CFG5_SPI_EN);
}

/* Low-power knobs for H-family - hw_start_8168h_extras() clears DLLPR PFM_EN
 * explicitly, so we omit the generic DLLPR write here. We DO still set the
 * other common knobs. */
static void rtl8111_hw_lowpower_h(void) {
    wr8(R8168_MCU, rd8(R8168_MCU) & ~(uint8_t)R8168_MCU_NOW_IS_OOB);
    wr32(R8168_MISC, rd32(R8168_MISC) | R8168_MISC_PWM_EN);
    wr8(R8168_CONFIG5, rd8(R8168_CONFIG5) & ~(uint8_t)R8168_CFG5_SPI_EN);
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
    /* Final pause thresholds for F-family */
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

/* Per-xid detection dispatch. xid = (TxConfig >> 20) & 0xfcf.
 * All chips share PCI ID 0x10ec:0x8168, xid is the version key.
 * Falls back to G-family (most recent) when unknown - better compatibility
 * than F-family on brand-new revs. */
static struct rtl_chip_id rtl_xid_detect(uint32_t xid) {
    switch (xid) {
        /* --- D-family (VER_25..31, oldest) --- */
        case RTL_XID_D1:  return (struct rtl_chip_id){ RTL_MAC_D, RTL_HW_D,      "RTL8168d/8111d"   };
        case RTL_XID_D2:  return (struct rtl_chip_id){ RTL_MAC_D, RTL_HW_D,      "RTL8168d/8111d"   };
        case RTL_XID_DP1: return (struct rtl_chip_id){ RTL_MAC_D, RTL_HW_DP4,    "RTL8168dp/8111dp" };
        case RTL_XID_DP2: return (struct rtl_chip_id){ RTL_MAC_D, RTL_HW_DP4,    "RTL8168dp/8111dp" };
        /* --- F-family (VER_32..38) --- */
        case RTL_XID_E1:  return (struct rtl_chip_id){ RTL_MAC_F, RTL_HW_E,      "RTL8168e/8111e"   };
        case RTL_XID_E2:  return (struct rtl_chip_id){ RTL_MAC_F, RTL_HW_E,      "RTL8168e/8111e"   };
        case RTL_XID_EVL: return (struct rtl_chip_id){ RTL_MAC_F, RTL_HW_EVL,    "RTL8168evl/8111evl" };
        case RTL_XID_F1:  return (struct rtl_chip_id){ RTL_MAC_F, RTL_HW_F,      "RTL8168f/8111f"   };
        case RTL_XID_F2:  return (struct rtl_chip_id){ RTL_MAC_F, RTL_HW_F,      "RTL8168f/8111f"   };
        case RTL_XID_8411:return (struct rtl_chip_id){ RTL_MAC_F, RTL_HW_8411,   "RTL8411"          };
        /* --- G-family (VER_40..52, newest) --- */
        case RTL_XID_G:   return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_G_BASE, "RTL8168g/8111g"   };
        case RTL_XID_GU:  return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_GU,     "RTL8168gu/8111gu" };
        case RTL_XID_8411B:return(struct rtl_chip_id){ RTL_MAC_G, RTL_HW_8411B,  "RTL8411b"         };
        case RTL_XID_H1:  return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_H,      "RTL8168h/8111h"   };
        case RTL_XID_H2:  return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_H,      "RTL8168M"         };
        case RTL_XID_EP:  return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_EP,     "RTL8168ep/8111ep (GN?)" };
        case RTL_XID_FP1: return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_FP,     "RTL8168fp/RTL8117" };
        case RTL_XID_FP2: return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_FP,     "RTL8168fp/RTL8117" };
        default: {
            /* Unknown xid - fall back to G-family (most recent rev, widest coverage) */
            static char unknown[64];
            ksprintf(unknown, sizeof(unknown), "unknown(0x%x)->G-fallback", (unsigned)xid);
            return (struct rtl_chip_id){ RTL_MAC_G, RTL_HW_G_BASE, unknown };
        }
    }
}

int rtl8111_init(void) {
    pci_device_t* dev = pci_find_device(RTL8111_VENDOR_ID, RTL8111_DEVICE_ID);
    if (!dev) return -1;

    /* Enable memory space access AND bus mastering: with Bus Master clear
     * the device's DMA address space is unmapped (see rtl8139/e1000). */
    uint16_t cmd = pci_read_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET);
    cmd |= 0x0006; /* Memory Space + Bus Master */
    pci_write_word(dev->bus, dev->device, dev->function, PCI_COMMAND_OFFSET, cmd);

    /* Dump BAR0..BAR5 raw values - CRITICAL for diagnosing "BAR0 is I/O"
     * type mismatches. Realtek PCIe devices ALWAYS have at least one MMIO
     * BAR; which one it is depends on the chip revision/BIOS assignment. */
    for (int i = 0; i < 6; i++) {
        uint32_t raw = pci_read_dword(dev->bus, dev->device, dev->function,
                                       PCI_BAR0_OFFSET + i * 4);
        klog_hex("[rtl8111] BAR", (uint64_t)i);
        klog_hex("  raw ", raw);
    }

    /* Search BAR0..BAR5 for the first MEMORY BAR (bit 0 clear).
     * Some RTL8168 variants use BAR0 as I/O and BAR2 as MMIO, or the BIOS
     * may remap the layout; never assume BAR0 == MMIO.
     *
     * PCI Memory BAR encoding (low 4 bits):
     *   bit 0 = 0 (memory), bits [2:1] = 00 (32b) or 10 (64b)
     * PCI I/O BAR encoding:
     *   bit 0 = 1
     */
    uint32_t bar0_raw = 0, bar1 = 0;
    int bar_index = -1;
    for (int i = 0; i < 6; i++) {
        uint32_t raw = pci_read_dword(dev->bus, dev->device, dev->function,
                                       PCI_BAR0_OFFSET + i * 4);
        if (raw == 0 || raw == 0xFFFFFFFF) continue;       /* unused BAR */
        if (raw & 0x1) continue;                            /* I/O BAR, skip */
        bar0_raw = raw;
        bar_index = i;
        /* 64-bit memory BAR? bits [2:1] == 10 */
        if ((raw & 0x6) == 0x4 && i < 5) {
            bar1 = pci_read_dword(dev->bus, dev->device, dev->function,
                                   PCI_BAR0_OFFSET + (i + 1) * 4);
            if (bar1 == 0xFFFFFFFF) {
                ksprintf(rtl8111_tmp, sizeof(rtl8111_tmp),
                          "BAR%d bad upper half", i);
                rtl8111_log(rtl8111_tmp);
                return -1;
            }
        } else {
            bar1 = 0;
        }
        break;
    }
    if (bar_index < 0) {
        rtl8111_log("no usable MMIO BAR found in BAR0..5");
        return -1;
    }
    ksprintf(rtl8111_tmp, sizeof(rtl8111_tmp),
             "using BAR%d (raw=0x%x upper=0x%x)", bar_index, bar0_raw, bar1);
    rtl8111_log(rtl8111_tmp);

    uint64_t bar = bar0_raw & ~0xFULL;
    if ((bar0_raw & 0x6) == 0x4) { /* 64-bit memory BAR */
        bar |= ((uint64_t)(bar1 & ~0xFULL)) << 32;
    }
    mmio8 = (volatile uint8_t*)bar;
    klog_hex("[rtl8111] mmio_base", (uint64_t)bar);

    /* Chip version (r8169: xid = (TxConfig >> 20) & 0xfcf; RTL8111F = 0x48x) */
    uint32_t txconfig = rd32(R8168_TX_CONFIG);
    if (txconfig == 0xFFFFFFFF) {
        rtl8111_log("MMIO dead");
        return -1;
    }
    uint32_t xid = (txconfig >> 20) & RTL_XID_MASK;
    klog_hex("[rtl8111] TxConfig xid", xid);

    /* Dispatch chip info from the full xid table */
    struct rtl_chip_id chip = rtl_xid_detect(xid);
    {
        char buf[128];
        ksprintf(buf, sizeof(buf), "[rtl8111] chip: %s (family=%d, hw_variant=%d)",
                 chip.name, chip.family, chip.hw);
        klog(buf);
    }

    /* Software reset: write CmdReset, poll until self-cleared.
     * Real silicon can be significantly slower than QEMU's emulated device -
     * bump timeout from 100k to 500k iterations. */
    wr8(R8168_CHIP_CMD, R8168_CMD_RESET);
    int timeout = 500000;
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
    /* Config regs must be unlocked to touch Config2/Config3/Config5 */
    wr8(R8168_CFG9346, R8168_CFG_UNLOCK);
    /* Chip-side ASPM/clock-request off (r8169 does this before EPHY access) */
    wr8(R8168_CONFIG2, rd8(R8168_CONFIG2) & ~(uint8_t)R8168_CFG2_CLKREQ_EN);
    wr8(R8168_CONFIG5, rd8(R8168_CONFIG5) & ~(uint8_t)R8168_CFG5_ASPM_EN);
    /* CPlusCmd: no offloads, no INTT */
    wr16(R8168_CPLUS_CMD, 0);
    /* Early TX threshold (8168E-VL and later: r8169 EarlySize) */
    wr8(R8168_MAX_TX_PKT, R8168_EARLY_SIZE);

    /* Dispatch MAC init sequence by hw variant. Every variant in
     * Linux's rtl_hw_start_* dispatch table has a unique ERI sequence and
     * often a unique ephy patch - using the wrong sequence breaks link-up.
     *
     * NOTE: Each hw variant below handles ITS OWN low-power knobs (MCU,
     * DLLPR, MISC, Config5) inline. We intentionally do NOT have a single
     * shared "common knobs" block after the switch, because H-family
     * (hw_start_8168h_1) clears DLLPR PFM_EN explicitly - a shared block
     * would re-set it and contradict the per-variant init. */
    switch (chip.hw) {
        case RTL_HW_G_BASE:
            rtl8111_hw_start_8168g();
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_GU:
            rtl8111_hw_start_8168g();
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_8411B:
            rtl8111_hw_start_8168g();
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_H:
            rtl8111_hw_start_8168g();
            rtl8111_hw_start_8168h_extras(); /* DLLPR &= ~PFM_EN is inside */
            rtl8111_hw_lowpower_h();        /* no DLLPR write here to avoid clobbering */
            break;
        case RTL_HW_EP:
            rtl8111_hw_start_8168ep();
            rtl8111_hw_lowpower_generic();  /* EP family clears TX_10M_PS_EN only, PFM_EN stays */
            break;
        case RTL_HW_FP:
            rtl8111_hw_start_8168ep();
            rtl8111_hw_start_8117_extras();
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_F:
            rtl8111_hw_start_8168f();
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_8411:
            rtl8111_hw_start_8168f();
            wr8(R8168_CONFIG3, rd8(R8168_CONFIG3) & ~(uint8_t)R8168_CFG3_RDY_L23);
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_E:
            wr8(R8168_MISC, rd8(R8168_MISC) | (1 << 12) | (1 << 13)); /* TXPLA_RST */
            wr8(R8168_MISC, rd8(R8168_MISC) & ~((uint8_t)(1 << 12) | (1 << 13)));
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_EVL:
            rtl8111_hw_start_8168e_2();
            rtl8111_hw_lowpower_generic();
            break;
        case RTL_HW_D:
            rtl8111_hw_start_8168d();
            wr8(R8168_MCU, rd8(R8168_MCU) & ~(uint8_t)R8168_MCU_NOW_IS_OOB);
            break;
        case RTL_HW_DP4:
            rtl8111_hw_start_8168d_4();
            wr8(R8168_MCU, rd8(R8168_MCU) & ~(uint8_t)R8168_MCU_NOW_IS_OOB);
            break;
        default:
            rtl8111_hw_start_8168g();
            rtl8111_hw_lowpower_generic();
            break;
    }

    /* PCIe-side ASPM/CLKREQ off (r8169 pci_disable_link_state) */
    rtl8111_pcie_disable_aspm(dev->bus, dev->device, dev->function);

    /* PHY side patches - switch on hw variant */
    switch (chip.hw) {
        case RTL_HW_G_BASE:  rtl8111_ephy_init_8168g_1();  break;
        case RTL_HW_GU:      rtl8111_ephy_init_8168g_2();  break;
        case RTL_HW_8411B:   rtl8111_ephy_init_8411b();    break;
        case RTL_HW_H:       rtl8111_ephy_init_8168h_1(); break;
        case RTL_HW_EP:      rtl8111_ephy_init_8168ep_3();break;
        case RTL_HW_FP:      rtl8111_ephy_init_8117();    break;
        case RTL_HW_F:       rtl8111_ephy_init_8168f_1(); break;
        case RTL_HW_8411:    rtl8111_ephy_init_8411();     break;
        case RTL_HW_E:       rtl8111_ephy_init_8168e_1(); break;
        case RTL_HW_EVL:     rtl8111_ephy_init_8168e_2(); break;
        case RTL_HW_D:       break; /* no ephy patch for VER_25/26 */
        case RTL_HW_DP4:     rtl8111_ephy_init_8168d_4(); break;
        default:             rtl8111_ephy_init_8168g_1(); break;
    }
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
