/* MediaTek MT7601U USB Wi-Fi dongle driver - phase 1: bring-up.
 *
 * Ported from Linux drivers/net/wireless/mediatek/mt7601u (GPL-2.0):
 * usb.c (vendor requests), mcu.c (firmware upload), regs.h/usb_regs.h
 * (register map), eeprom.c (eFuse MAC read).  All sequences are 1:1
 * with the upstream driver; only the transport layer differs (our
 * usb_control_xfer / usb_bulk_xfer instead of Linux URB helpers).
 *
 * The firmware image (45 KB, MediaTek-redistributable, from
 * linux-firmware mediatek/mt7601u.bin) is embedded in .rodata via the
 * build system's incbin blob pattern (see Makefile).
 *
 * Context discipline: attach runs in the enumeration context (IF=0,
 * boot or IRQ0 usb_tick path) and blocks with pit_delay_ms polling -
 * the same discipline as every other USB control/bulk caller here.
 */
#include "net/mt7601u.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

/* --- USB IDs (linux mt7601u_device_table) ------------------------------- */
static const uint16_t mt_id_table[][2] = {
    { 0x0b05, 0x17d3 },   /* ASUS */
    { 0x0e8d, 0x760a },
    { 0x0e8d, 0x760b },
    { 0x13d3, 0x3431 },
    { 0x13d3, 0x3434 },
    { 0x148f, 0x7601 },   /* MediaTek MT7601U */
    { 0x148f, 0x760a },
    { 0x148f, 0x760b },
    { 0x148f, 0x760c },
    { 0x148f, 0x760d },
    { 0x2001, 0x3d04 },   /* D-Link */
    { 0x2717, 0x4106 },   /* Xiaomi */
    { 0x2955, 0x0001 },
    { 0x2955, 0x1001 },
    { 0x2955, 0x1003 },
    { 0x2a5f, 0x1000 },
    { 0x7392, 0x7710 },   /* Edimax */
};

int mt7601u_matches(uint16_t vendor_id, uint16_t product_id) {
    int n = (int)(sizeof(mt_id_table) / sizeof(mt_id_table[0]));
    for (int i = 0; i < n; i++) {
        if (mt_id_table[i][0] == vendor_id &&
            mt_id_table[i][1] == product_id) return 1;
    }
    return 0;
}

/* --- vendor register map (regs.h / mcu.h) -------------------------------- */
#define MT_ASIC_VERSION           0x0000
#define MT_EFUSE_CTRL             0x0024
#define MT_EFUSE_CTRL_AOUT_M      0x0000003Fu   /* GENMASK(5,0)   */
#define MT_EFUSE_CTRL_MODE_M      0x000000C0u   /* GENMASK(7,6)   */
#define MT_EFUSE_CTRL_AIN_M       0x03FF0000u   /* GENMASK(25,16) */
#define MT_EFUSE_CTRL_KICK        0x40000000u   /* BIT(30)        */
#define MT_EFUSE_CTRL_SEL         0x80000000u   /* BIT(31)        */
#define MT_EFUSE_DATA(n)          (0x0028 + (n) * 4)
#define MT_FCE_DMA_ADDR           0x0230
#define MT_FCE_DMA_LEN            0x0234
#define MT_USB_DMA_CFG            0x0238
#define MT_USB_DMA_CFG_TX_CLR     0x00080000u   /* BIT(19)        */
#define MT_USB_DMA_CFG_RX_BULK_EN 0x00400000u   /* BIT(22)        */
#define MT_USB_DMA_CFG_TX_BULK_EN 0x00800000u   /* BIT(23)        */
#define MT_PBF_SYS_CTRL           0x0400
#define MT_PBF_CFG                0x0404
#define MT_PBF_CFG_TX0Q_EN        0x00000001u
#define MT_PBF_CFG_TX1Q_EN        0x00000002u
#define MT_PBF_CFG_TX2Q_EN        0x00000004u
#define MT_PBF_CFG_TX3Q_EN        0x00000008u
#define MT_FCE_PSE_CTRL           0x0800
#define MT_TX_CPU_FROM_FCE_BASE_PTR    0x09A0
#define MT_TX_CPU_FROM_FCE_MAX_COUNT   0x09A4
#define MT_TX_CPU_FROM_FCE_CPU_DESC_IDX 0x09A8
#define MT_FCE_PDMA_GLOBAL_CONF   0x09C4
#define MT_FCE_SKIP_FS            0x0A6C
#define MT_MAC_CSR0               0x1000
#define MT_MCU_COM_REG0           0x0730
#define MT_MCU_COM_REG1           0x0734
#define MT_MCU_DLM_OFFSET         0x80000
#define MT_MCU_IVB_SIZE           0x40

/* vendor request opcodes (usb.h) */
#define MT_VEND_DEV_MODE   1
#define MT_VEND_WRITE      2
#define MT_VEND_MULTI_READ 7
#define MT_VEND_WRITE_FCE  0x42
#define MT_VEND_DEV_MODE_RESET 1

#define MT_VEND_TYPE_VENDOR 0x40   /* USB_TYPE_VENDOR | USB_RECIP_DEVICE */

/* bulk endpoint roles (descriptor order, matches Linux in_eps/out_eps) */
#define MT_EP_IN_PKT_RX      0
#define MT_EP_IN_CMD_RESP    1
#define MT_EP_OUT_INBAND_CMD 0

/* firmware image layout */
typedef struct {
    uint32_t ilm_len;
    uint32_t dlm_len;
    uint16_t build_ver;
    uint16_t fw_ver;
    uint8_t  pad[4];
    char     build_time[16];
} mt_fw_header_t;                       /* 32 bytes */

#define MT_FW_URB_MAX_PAYLOAD 0x3800
#define MT_DMA_HDR_LEN 4

/* --- state ---------------------------------------------------------------- */
typedef struct {
    usb_device_t* usb;
    int      fw_running;
    uint8_t  mac[6];
    uint32_t asic_rev;
    uint32_t mac_rev;
    uint16_t fw_ver;
    char     fw_build[17];
} mt7601u_dev_t;

static mt7601u_dev_t g_mt;

/* --- vendor request primitives (usb.c port) ------------------------------- */

/* one vendor control message; bmRequestType = dir | VENDOR|DEVICE */
static int vend_req(usb_device_t* dev, uint8_t req, int dir_in,
                    uint16_t val, uint16_t offset,
                    uint8_t* buf, uint16_t len, uint16_t* recv) {
    uint16_t got = 0;
    int rc = usb_control_xfer(dev,
                              (uint8_t)((dir_in ? 0x80 : 0x00) |
                                        MT_VEND_TYPE_VENDOR),
                              req, val, offset, dir_in,
                              buf, len, recv ? recv : &got);
    return rc;
}

/* rr: MT_VEND_MULTI_READ, IN, returns 4 bytes LE */
int mt7601u_rr(usb_device_t* dev, uint16_t offset, uint32_t* value) {
    uint8_t buf[4];
    uint16_t got = 0;
    *value = 0xFFFFFFFFu;
    for (int i = 0; i < 3; i++) {          /* bounded retries like Linux */
        int rc = vend_req(dev, MT_VEND_MULTI_READ, 1, 0, offset,
                          buf, 4, &got);
        if (rc == 0 && got == 4) {
            *value = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                     ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            return 0;
        }
        pit_delay_ms(5);
    }
    return -1;
}

/* wr: two halfword MT_VEND_WRITE requests (low, high) */
int mt7601u_wr(usb_device_t* dev, uint16_t offset, uint32_t value) {
    int rc = vend_req(dev, MT_VEND_WRITE, 0,
                      (uint16_t)(value & 0xFFFF), offset, NULL, 0, NULL);
    if (rc != 0) return rc;
    return vend_req(dev, MT_VEND_WRITE, 0,
                    (uint16_t)(value >> 16), (uint16_t)(offset + 2),
                    NULL, 0, NULL);
}

static int mt7601u_rmw(usb_device_t* dev, uint16_t offset,
                       uint32_t mask, uint32_t set) {
    uint32_t val;
    if (mt7601u_rr(dev, offset, &val) != 0) return -1;
    val = (val & ~mask) | set;
    return mt7601u_wr(dev, offset, val);
}

/* poll a register field with 1 ms steps */
static int mt7601u_poll(usb_device_t* dev, uint16_t offset,
                        uint32_t mask, uint32_t expect, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        uint32_t v;
        if (mt7601u_rr(dev, offset, &v) == 0 && (v & mask) == expect)
            return 0;
        pit_delay_ms(1);
    }
    return -1;
}

/* --- firmware upload (mcu.c port) ----------------------------------------- */

/* __mt7601u_dma_fw: one TXINFO-wrapped chunk pushed over the inband
 * bulk OUT pipe, FCE DMA registers pointing at dst_addr */
static int dma_fw_chunk(usb_device_t* dev, const uint8_t* data,
                        uint32_t len, uint32_t dst_addr) {
    static uint8_t buf[MT_DMA_HDR_LEN + MT_FW_URB_MAX_PAYLOAD + 4];

    /* TXINFO: TYPE=DMA_PACKET(0), D_PORT=CPU_TX_PORT(2)<<27, LEN */
    uint32_t info = ((uint32_t)2 << 27) | (len & 0xFFFF);
    buf[0] = (uint8_t)(info & 0xFF);
    buf[1] = (uint8_t)((info >> 8) & 0xFF);
    buf[2] = (uint8_t)((info >> 16) & 0xFF);
    buf[3] = (uint8_t)((info >> 24) & 0xFF);
    memcpy(buf + MT_DMA_HDR_LEN, data, len);
    memset(buf + MT_DMA_HDR_LEN + len, 0, 4);

    uint32_t len4 = (len + 3u) & ~3u;
    if (mt7601u_wr(dev, MT_FCE_DMA_ADDR, dst_addr) != 0) return -1;
    if (mt7601u_wr(dev, MT_FCE_DMA_LEN, len4 << 16) != 0) return -1;

    uint16_t sent = 0;
    int rc = mt7601u_bulk_out(dev, buf, (uint16_t)(MT_DMA_HDR_LEN + len4 + 4));
    if (rc != 0) return rc;
    (void)sent;

    /* kick the CPU: bump the descriptor index */
    uint32_t idx;
    if (mt7601u_rr(dev, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, &idx) != 0)
        return -1;
    if (mt7601u_wr(dev, MT_TX_CPU_FROM_FCE_CPU_DESC_IDX, idx + 1) != 0)
        return -1;

    /* FCE signals the transfer done via COM_REG1 bit31 */
    return mt7601u_poll(dev, MT_MCU_COM_REG1, 0x80000000u, 0x80000000u, 500);
}

/* mt7601u_dma_fw: chunked upload, max 0x3800 payload per URB */
static int dma_fw(usb_device_t* dev, const uint8_t* data,
                  uint32_t len, uint32_t dst_addr) {
    while (len > 0) {
        uint32_t n = len > MT_FW_URB_MAX_PAYLOAD ? MT_FW_URB_MAX_PAYLOAD : len;
        if (dma_fw_chunk(dev, data, n, dst_addr) != 0) return -1;
        data += n;
        dst_addr += n;
        len -= n;
    }
    return 0;
}

static int firmware_running(usb_device_t* dev) {
    uint32_t v;
    if (mt7601u_rr(dev, MT_MCU_COM_REG0, &v) != 0) return 0;
    return v == 1;
}

/* mt7601u_upload_firmware + mt7601u_load_firmware (the pre/post register
 * dance) fused into one bring-up pass */
static int mt7601u_load_firmware(usb_device_t* dev) {
    const uint8_t* fw = mt7601u_fw_start;
    uint32_t fw_size = (uint32_t)(mt7601u_fw_end - mt7601u_fw_start);

    if (fw_size < sizeof(mt_fw_header_t) + MT_MCU_IVB_SIZE) {
        klog("[mt7601u] firmware blob missing/truncated\n");
        return -1;
    }

    mt_fw_header_t hdr;
    memcpy(&hdr, fw, sizeof(hdr));
    uint32_t ilm_len = hdr.ilm_len;
    uint32_t dlm_len = hdr.dlm_len;
    if (ilm_len <= MT_MCU_IVB_SIZE ||
        fw_size != sizeof(hdr) + ilm_len + dlm_len) {
        klog("[mt7601u] invalid firmware image\n");
        return -1;
    }

    char buf[96];
    ksprintf(buf, sizeof(buf),
             "[mt7601u] fw version %d.%d.%02d build %04x %.16s\n",
             (hdr.fw_ver >> 12) & 0xF, (hdr.fw_ver >> 8) & 0xF,
             hdr.fw_ver & 0xF, hdr.build_ver, hdr.build_time);
    klog(buf);

    if (firmware_running(dev)) {
        klog("[mt7601u] firmware already running\n");
        g_mt.fw_running = 1;
        return 0;
    }

    /* ---- the load_firmware register dance, 1:1 ---- */
    mt7601u_wr(dev, 0x94C, 0);
    mt7601u_wr(dev, MT_FCE_PSE_CTRL, 0);

    /* vendor reset: MT_VEND_DEV_MODE, out, val = MT_VEND_DEV_MODE_RESET */
    vend_req(dev, MT_VEND_DEV_MODE, 0, MT_VEND_DEV_MODE_RESET, 0,
             NULL, 0, NULL);
    pit_delay_ms(5);

    mt7601u_wr(dev, 0x0A44, 0);
    mt7601u_wr(dev, 0x0230, 0x84210);
    mt7601u_wr(dev, 0x0400, 0x80C00);
    mt7601u_wr(dev, 0x0800, 1);

    mt7601u_rmw(dev, MT_PBF_CFG, 0,
                MT_PBF_CFG_TX0Q_EN | MT_PBF_CFG_TX1Q_EN |
                MT_PBF_CFG_TX2Q_EN | MT_PBF_CFG_TX3Q_EN);

    mt7601u_wr(dev, MT_FCE_PSE_CTRL, 1);
    mt7601u_wr(dev, MT_USB_DMA_CFG,
               MT_USB_DMA_CFG_RX_BULK_EN | MT_USB_DMA_CFG_TX_BULK_EN);
    {
        uint32_t v;
        if (mt7601u_rr(dev, MT_USB_DMA_CFG, &v) == 0)
            mt7601u_wr(dev, MT_USB_DMA_CFG, v & ~MT_USB_DMA_CFG_TX_CLR);
    }

    mt7601u_wr(dev, MT_TX_CPU_FROM_FCE_BASE_PTR, 0x400230);
    mt7601u_wr(dev, MT_TX_CPU_FROM_FCE_MAX_COUNT, 1);
    mt7601u_wr(dev, MT_FCE_PDMA_GLOBAL_CONF, 0x44);
    mt7601u_wr(dev, MT_FCE_SKIP_FS, 3);

    /* ---- upload: ILM (minus the IVB) then DLM ---- */
    const uint8_t* ivb  = fw + sizeof(hdr);
    const uint8_t* ilm  = fw + sizeof(hdr) + MT_MCU_IVB_SIZE;
    uint32_t ilm_load = ilm_len - MT_MCU_IVB_SIZE;

    if (dma_fw(dev, ilm, ilm_load, MT_MCU_IVB_SIZE) != 0) {
        klog("[mt7601u] ILM upload failed\n");
        return -1;
    }
    if (dma_fw(dev, ilm + ilm_load, dlm_len, MT_MCU_DLM_OFFSET) != 0) {
        klog("[mt7601u] DLM upload failed\n");
        return -1;
    }

    /* boot the MCU: DEV_MODE 0x12 carries the 64-byte IVB */
    uint8_t ivb_copy[MT_MCU_IVB_SIZE];
    memcpy(ivb_copy, ivb, MT_MCU_IVB_SIZE);
    if (vend_req(dev, MT_VEND_DEV_MODE, 0, 0x12, 0,
                 ivb_copy, MT_MCU_IVB_SIZE, NULL) != 0) {
        klog("[mt7601u] MCU boot request failed\n");
        return -1;
    }

    for (int i = 0; i < 100 && !firmware_running(dev); i++)
        pit_delay_ms(10);
    if (!firmware_running(dev)) {
        klog("[mt7601u] firmware did not start (timeout)\n");
        return -1;
    }

    g_mt.fw_running = 1;
    klog("[mt7601u] firmware running mt7601u_fw_ok\n");
    return 0;
}

/* --- eFuse (eeprom.c port, MAC only) -------------------------------------- */

/* read 16 bytes from the eFuse row containing addr */
static int efuse_read_row(usb_device_t* dev, uint16_t addr, uint8_t* out16) {
    uint32_t val;
    if (mt7601u_rr(dev, MT_EFUSE_CTRL, &val) != 0) return -1;

    val &= ~(MT_EFUSE_CTRL_AIN_M | MT_EFUSE_CTRL_MODE_M);
    val |= ((uint32_t)(addr & ~0xF) << 16) | MT_EFUSE_CTRL_KICK;
    if (mt7601u_wr(dev, MT_EFUSE_CTRL, val) != 0) return -1;

    if (mt7601u_poll(dev, MT_EFUSE_CTRL, MT_EFUSE_CTRL_KICK, 0, 100) != 0)
        return -1;

    if (mt7601u_rr(dev, MT_EFUSE_CTRL, &val) != 0) return -1;
    if ((val & MT_EFUSE_CTRL_AOUT_M) == MT_EFUSE_CTRL_AOUT_M) {
        memset(out16, 0xFF, 16);            /* unmapped row */
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t w;
        if (mt7601u_rr(dev, (uint16_t)MT_EFUSE_DATA(i), &w) != 0) return -1;
        out16[i * 4 + 0] = (uint8_t)(w & 0xFF);
        out16[i * 4 + 1] = (uint8_t)((w >> 8) & 0xFF);
        out16[i * 4 + 2] = (uint8_t)((w >> 16) & 0xFF);
        out16[i * 4 + 3] = (uint8_t)((w >> 24) & 0xFF);
    }
    return 0;
}

/* MT_EE_MAC_ADDR = 0x04: six bytes spanning row 0x00 offsets 4..9 */
static int mt7601u_read_mac(usb_device_t* dev, uint8_t* mac) {
    uint8_t row[16];
    if (efuse_read_row(dev, 0x00, row) != 0) return -1;
    memcpy(mac, row + 4, 6);
    return 0;
}

/* --- bring-up --------------------------------------------------------------- */

static int mt7601u_wait_asic_ready(usb_device_t* dev) {
    for (int i = 0; i < 10; i++) {
        uint32_t v;
        if (mt7601u_rr(dev, MT_MAC_CSR0, &v) == 0 && v != 0 && v != 0xFFFFFFFFu)
            return 0;
        pit_delay_ms(10);
    }
    return -1;
}

int mt7601u_attach(usb_device_t* dev) {
    memset(&g_mt, 0, sizeof(g_mt));
    g_mt.usb = dev;

    if (mt7601u_wait_asic_ready(dev) != 0) {
        klog("[mt7601u] ASIC never became ready\n");
        return -1;
    }
    mt7601u_rr(dev, MT_ASIC_VERSION, &g_mt.asic_rev);
    mt7601u_rr(dev, MT_MAC_CSR0, &g_mt.mac_rev);

    char buf[96];
    ksprintf(buf, sizeof(buf),
             "[mt7601u] ASIC revision %08x MAC revision %08x\n",
             g_mt.asic_rev, g_mt.mac_rev);
    klog(buf);
    if ((g_mt.asic_rev >> 16) != 0x7601) {
        klog("[mt7601u] not an MT7601 - refusing\n");
        return -1;
    }

    if (mt7601u_load_firmware(dev) != 0) return -1;

    if (mt7601u_read_mac(dev, g_mt.mac) == 0) {
        ksprintf(buf, sizeof(buf),
                 "[mt7601u] MAC %02x:%02x:%02x:%02x:%02x:%02x (eFuse)\n",
                 g_mt.mac[0], g_mt.mac[1], g_mt.mac[2],
                 g_mt.mac[3], g_mt.mac[4], g_mt.mac[5]);
        klog(buf);
    } else {
        klog("[mt7601u] eFuse MAC read failed\n");
    }

    /* phase 2 hook: 802.11 station (scan/auth/assoc/WPA2) plugs in here */
    return 0;
}

void mt7601u_detach(usb_device_t* dev) {
    (void)dev;
    klog("[mt7601u] detached\n");
    g_mt.usb = NULL;
    g_mt.fw_running = 0;
}

/* --- data-path primitives (phase 2 consumers) ------------------------------- */

int mt7601u_bulk_out(usb_device_t* dev, const uint8_t* data, uint16_t len) {
    return usb_bulk_xfer(dev, dev->bulk_out[MT_EP_OUT_INBAND_CMD], 0,
                         (uint8_t*)data, len, NULL);
}

int mt7601u_bulk_in(usb_device_t* dev, uint8_t* data, uint16_t len,
                    uint16_t* recv) {
    return usb_bulk_xfer(dev, dev->bulk_in[MT_EP_IN_PKT_RX], 1,
                         data, len, recv);
}

/* --- diagnostics -------------------------------------------------------------- */

void mt7601u_dump(usb_device_t* dev) {
    char buf[96];
    ksprintf(buf, sizeof(buf),
             "[mt7601u] %s fw=%d asic=%08x mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
             dev->state == USB_DEV_RUNNING ? "present" : "gone",
             g_mt.fw_running, g_mt.asic_rev,
             g_mt.mac[0], g_mt.mac[1], g_mt.mac[2],
             g_mt.mac[3], g_mt.mac[4], g_mt.mac[5]);
    klog(buf);
}
