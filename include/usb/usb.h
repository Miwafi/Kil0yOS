#ifndef USB_H
#define USB_H

#include "lib/types.h"
#include "drivers/pci.h"

/* ------------------------------------------------------------------ */
/* USB (UHCI / USB 1.1) plug-and-play: HCD + core + HID boot keyboard */
/* and mouse.  Execution-context discipline (see usb.c header): every  */
/* code path that touches shared USB state runs with IF=0.            */
/* ------------------------------------------------------------------ */

/* --- UHCI register offsets from the I/O BAR --- */
#define UHCI_USBCMD       0x00
#define UHCI_USBSTS       0x02
#define UHCI_USBINTR      0x04
#define UHCI_USBFRNUM     0x06
#define UHCI_USBFLBASEADD 0x08
#define UHCI_USBSOF       0x0C
#define UHCI_PORTSC(p)    (0x10 + ((p) - 1) * 2)   /* p = 1..2 */

/* USBCMD bits */
#define UHCI_CMD_RS      0x0001   /* run/stop */
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET  0x0004   /* global reset */
#define UHCI_CMD_CF      0x0040   /* config flag */

/* USBSTS bits (write-1-to-clear on 0x1F) */
#define UHCI_STS_USBINT  0x0001   /* IOC interrupt */
#define UHCI_STS_ERROR   0x0002
#define UHCI_STS_RD      0x0004   /* resume detect */
#define UHCI_STS_HSE     0x0008   /* host system error */
#define UHCI_STS_HCPE    0x0010   /* host controller process error */
#define UHCI_STS_HCH     0x0020   /* halted */

/* USBINTR bits */
#define UHCI_INTR_TIMEOUT 0x0001
#define UHCI_INTR_RESUME  0x0002
#define UHCI_INTR_IOC     0x0004
#define UHCI_INTR_SP      0x0008

/* PORTSC bits */
#define UHCI_PSC_CCS   0x0001   /* current connect status (RO) */
#define UHCI_PSC_CSC   0x0002   /* connect status change (W1C) */
#define UHCI_PSC_PE    0x0004   /* port enabled */
#define UHCI_PSC_PEC   0x0008   /* port enable change (W1C) */
#define UHCI_PSC_LSDA  0x0100   /* low-speed device attached (RO) */
#define UHCI_PSC_PR    0x0200   /* port reset */
#define UHCI_PSC_RES1  0x0080   /* always reads 1 - write back as 1 */

/* --- Transfer Descriptors (hardware part is the first 16 bytes;
 * struct padded to 32B so a TD pool at 32B stride is 16B-aligned) --- */
typedef struct usb_td {
    uint32_t link;      /* bits 31:4 addr, 2 VF, 1 QH, 0 terminate */
    uint32_t status;
    uint32_t token;
    uint32_t buffer;    /* 32-bit physical data buffer */
    uint32_t sw[4];     /* software scratch (keeps 32B stride) */
} __attribute__((packed)) usb_td_t;

typedef struct usb_qh {
    uint32_t hlink;     /* horizontal: next QH (bit1 = QH) */
    uint32_t elink;     /* element: first TD of the queue */
} __attribute__((packed)) usb_qh_t;

#define TD_LINK_TERMINATE 0x00000001u
#define TD_LINK_QH        0x00000002u

/* TD status word */
#define TD_CTRL_SP        (1u << 29)  /* short packet detect */
#define TD_CTRL_C_ERR     (3u << 27)  /* 3 errors before giving up */
#define TD_CTRL_LS        (1u << 26)  /* low-speed device */
#define TD_CTRL_IOS       (1u << 25)  /* isochronous select */
#define TD_CTRL_IOC       (1u << 24)  /* interrupt on complete */
#define TD_CTRL_ACTIVE    (1u << 23)
#define TD_CTRL_STALLED   (1u << 22)
#define TD_CTRL_DBUFERR   (1u << 21)
#define TD_CTRL_BABBLE    (1u << 20)
#define TD_CTRL_NAK       (1u << 19)
#define TD_CTRL_CRCTIMEO  (1u << 18)
#define TD_CTRL_BITSTUFF  (1u << 17)
#define TD_CTRL_ACTLEN    0x7FFu      /* bytes received - 1 */

/* TD token word */
#define TD_PID_SETUP 0x2Du
#define TD_PID_IN    0x69u
#define TD_PID_OUT   0xE1u
#define TD_PID(x)            (((uint32_t)(x)) & 0xFF)
#define TD_TOKEN_PID(x)      (((x) & 0xFF))
#define TD_TOKEN_ADDR_SHIFT  8
#define TD_TOKEN_EP_SHIFT    15
#define TD_TOKEN_TOGGLE      (1u << 19)
#define TD_TOKEN_MAXLEN(x)   ((((uint32_t)(x) ? (uint32_t)(x) - 1 : 0x7FF) & 0x7FF) << 21)

/* --- device model --- */
#define USB_MAX_DEVICES 4
#define USB_MAX_INTERFACES 4
#define USB_ROOT_PARENT 0xFF      /* dev->parent sentinel: UHCI root port */
#define USB_ROOT_EHCI   0xFE      /* dev->parent sentinel: EHCI root port */
#define USB_ROOT_XHCI   0xFD      /* dev->parent sentinel: xHCI root port */

#define USB_HCD_UHCI 0
#define USB_HCD_EHCI 1
#define USB_HCD_XHCI 2

#define USB_MAX_BULK_IN   2       /* MT7601U: pkt rx + cmd resp */
#define USB_MAX_BULK_OUT  6       /* MT7601U: inband + 4 AC + HCCA */

typedef struct usb_device usb_device_t;
typedef void (*usb_report_fn)(usb_device_t* dev, const uint8_t* data, int len);

typedef enum usb_dev_state {
    USB_DEV_FREE = 0,
    USB_DEV_ENUMING,
    USB_DEV_RUNNING
} usb_dev_state_t;

struct usb_device {
    usb_dev_state_t state;
    int      slot;            /* 0..USB_MAX_DEVICES-1, index into int_qh[] */
    uint8_t  address;         /* 1..USB_MAX_DEVICES; 0 before SET_ADDRESS */
    uint8_t  parent;          /* device slot of parent hub (USB_ROOT_PARENT
                               * = UHCI root port, USB_ROOT_EHCI = EHCI) */
    uint8_t  hub_port;        /* port number on parent (root: 1..2) */
    uint8_t  low_speed;
    uint8_t  hcd;             /* USB_HCD_*: which controller owns this dev */
    uint8_t  device_class;    /* bDeviceClass (9 = hub) */
    uint8_t  if_class;        /* 3 = HID */
    uint8_t  if_protocol;     /* HID bInterfaceProtocol: 1 kbd, 2 mouse */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  ep_addr;         /* interrupt IN endpoint (e.g. 0x81) */
    uint8_t  ep_mps;          /* wMaxPacketSize */
    uint8_t  ep_interval;     /* bInterval, frames (informational) */
    uint8_t  hub_nports;      /* hub devices: downstream port count */
    uint16_t hub_ignored;     /* hub devices: bit(p-1) = port p holds an
                               * unsupported device, do not re-enumerate
                               * until it is disconnected */
    uint8_t  toggle;          /* DATA toggle for the interrupt IN pipe */
    int      int_idle;        /* TD completed with NAK: tick requeues */
    int      dead;            /* fatal TD error: unbind in usb_tick */
    uint32_t nak_count;
    uint32_t report_count;
    usb_report_fn on_report;
    usb_td_t* int_td;         /* interrupt IN TD (in the UHCI TD pool) */
    uint8_t  int_buf[8];      /* DMA buffer for the interrupt report */

    /* --- vendor (Wi-Fi) device extensions (EHCI bulk pipes) --- */
    uint8_t  is_wifi;         /* matched the MT7601U ID table */
    uint8_t  bulk_in[USB_MAX_BULK_IN];    /* IN endpoint numbers (0x81...) */
    uint8_t  bulk_out[USB_MAX_BULK_OUT];  /* OUT endpoint numbers (1..5) */
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    /* control-transfer request block (EHCI path builds SETUP from this) */
    uint8_t  pending_bmReq, pending_bReq;
    uint16_t pending_wValue, pending_wIndex;
    /* --- xHCI extensions --- */
    uint8_t  xhci_slot;       /* slot id (== USB address once addressed) */
    uint8_t  xhci_in_ep;      /* interrupt IN endpoint address (0x81) */
};

/* core (usb.c) */
usb_device_t* usb_device_at(int slot);  /* device-table slot lookup (uhci.c) */
void usb_init(void);
void usb_tick(void);          /* IRQ0 context, every 10ms (isr.c) */
void usb_dump(void);          /* shell `usb` command */

/* HCD-dispatched control transfer: routes by dev->hcd to the UHCI or the
 * EHCI queue.  Same semantics as uhci_control_xfer. */
int usb_control_xfer(usb_device_t* dev,
                     uint8_t bmReq, uint8_t bReq,
                     uint16_t wValue, uint16_t wIndex,
                     int dir_in,
                     uint8_t* data, uint16_t len,
                     uint16_t* recv_len);
/* Blocking bulk transfer over the EHCI schedule (USB 2.0 devices only). */
int usb_bulk_xfer(usb_device_t* dev, uint8_t ep_num, int dir_in,
                  uint8_t* data, uint16_t len, uint16_t* recv_len);

/* HID drivers (hid.c) */
void usb_hid_attach(usb_device_t* dev);
void usb_hid_detach(usb_device_t* dev);
void usb_hid_probe_tick(void);  /* ~1s: unbind silent HID pipes, resume PS/2 */

/* UHCI HCD (uhci.c) */
int  uhci_init(pci_device_t* pci);          /* 0 = ok */
void uhci_dump_controller(void);            /* registers + ports via klog */
int  uhci_control_xfer(usb_device_t* dev,
                       uint8_t bmReq, uint8_t bReq,
                       uint16_t wValue, uint16_t wIndex,
                       int dir_in,          /* 1 = IN data stage */
                       uint8_t* data, uint16_t len,   /* OUT payload / IN capacity */
                       uint16_t* recv_len); /* actual bytes received (IN) */
void uhci_queue_interrupt(usb_device_t* dev);
void uhci_unqueue_interrupt(usb_device_t* dev);
int  uhci_port_connected(int port);
int  uhci_port_low_speed(int port);
void uhci_port_reset(int port);             /* blocking ~70ms */
void uhci_clear_port_change(int port);
void uhci_idle_requeue(void);               /* called from usb_tick */

#endif
