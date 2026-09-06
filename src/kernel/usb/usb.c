/* USB core: device table (tree), enumeration state machine, hotplug
 * polling (root ports + class-09 hubs) and HID dispatch.
 *
 * Concurrency discipline: every context that touches the device table,
 * the TD pool or QH elinks runs with IF=0, so the three execution
 * contexts are fully serialised on the single CPU:
 *   1. boot mainline  - usb_init() runs before enable_interrupts()
 *   2. IRQ0 tick      - usb_tick() (never re-enables IF, runs before
 *                       scheduler_tick can switch tasks)
 *   3. UHCI IRQ       - uhci_irq_handler (never re-enables IF)
 * All hardware-visible links (QH elink / TD link) are published with a
 * single aligned 32-bit store, which the host controller re-reads every
 * 1ms frame.  Error recovery and klogging inside the IRQ context are
 * deferred to usb_tick via the dev->dead flag.
 *
 * Hot-plug: UHCI root ports have no port-change IRQ, so usb_tick polls
 * them at ~1 s.  Devices hot-added behind QEMU's virtual hub (and real
 * external hubs) are found by polling class-09 hubs with GET_STATUS
 * control transfers; a connect event triggers the standard reset +
 * enumerate sequence on the hub port.  Devices we cannot drive mark
 * their port "ignored" so a still-connected device is not re-enumerated
 * every second; the mark is cleared when the port goes empty.
 */
#include "usb/usb.h"
#include "drivers/pci.h"
#include "timer/pit.h"
#include "lib/string.h"
#include "lib/stdlib.h"
#include "drivers/vga.h"

#define USB_TICKS_PER_POLL 100      /* IRQ0 @100Hz -> poll ports ~1s */

static usb_device_t devices[USB_MAX_DEVICES];
static int usb_ready;               /* set only when a UHCI is present */
static uint32_t tick_count;
static int root_ignored;            /* bit(port-1): unsupported device on
                                     * a root port - skip re-enumeration */

usb_device_t* usb_device_at(int slot) {
    if (slot < 0 || slot >= USB_MAX_DEVICES) return NULL;
    return &devices[slot];
}

/* --- descriptors ------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_desc_device_t;                 /* 18 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_desc_config_t;                 /* 9 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_desc_iface_t;                  /* 9 bytes */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_desc_endpoint_t;               /* 7 bytes */

#define DESC_DEVICE    0x01
#define DESC_CONFIG    0x02
#define DESC_STRING    0x03
#define DESC_IFACE     0x04
#define DESC_ENDPOINT  0x05

/* --- helpers ----------------------------------------------------------- */

static void klogf(const char* fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog(buf);
}

/* Find the device plugged into a given parent+port (parent is a hub
 * device slot or USB_ROOT_PARENT for the UHCI root ports). */
static usb_device_t* device_on_parent(int parent, int port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state != USB_DEV_FREE &&
            devices[i].parent == parent && devices[i].hub_port == port)
            return &devices[i];
    }
    return NULL;
}

/* Remember "this port holds a device we cannot drive" so the ~1 s poll
 * does not re-enumerate it forever; cleared when the port goes empty. */
static void mark_port_ignored(int parent, int port) {
    if (parent == USB_ROOT_PARENT) {
        root_ignored |= 1u << (port - 1);
    } else {
        usb_device_t* hub = usb_device_at(parent);
        if (hub) hub->hub_ignored |= (uint16_t)(1u << (port - 1));
    }
}

static void clear_port_ignored(int parent, int port) {
    if (parent == USB_ROOT_PARENT) {
        root_ignored &= ~(1u << (port - 1));
    } else {
        usb_device_t* hub = usb_device_at(parent);
        if (hub) hub->hub_ignored &= (uint16_t)~(1u << (port - 1));
    }
}

static void enum_fail(usb_device_t* dev, const char* stage, int rc) {
    klogf("[usb] enum failed at %s (rc=%d)\n", stage, rc);
    memset(dev, 0, sizeof(*dev));
    dev->state = USB_DEV_FREE;
}

static void unbind_device(usb_device_t* dev) {
    /* children first: a hub detach takes its downstream devices with it */
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state != USB_DEV_FREE && devices[i].parent == dev->slot)
            unbind_device(&devices[i]);
    }
    if (dev->state == USB_DEV_RUNNING) {
        uhci_unqueue_interrupt(dev);
        usb_hid_detach(dev);
    }
    if (dev->parent == USB_ROOT_PARENT) {
        klogf("[usb] addr %d detached (root port %d, %s)\n",
              dev->address, dev->hub_port,
              dev->if_protocol == 1 ? "kbd" :
              dev->if_protocol == 2 ? "mouse" : "device");
    } else {
        usb_device_t* hub = usb_device_at(dev->parent);
        klogf("[usb] addr %d detached (hub addr %d port %d, %s)\n",
              dev->address, hub ? hub->address : 0, dev->hub_port,
              dev->if_protocol == 1 ? "kbd" :
              dev->if_protocol == 2 ? "mouse" : "device");
    }
    memset(dev, 0, sizeof(*dev));
    dev->state = USB_DEV_FREE;
}

/* --- class-09 hub support ---------------------------------------------- */

static void enumerate_port(int parent_slot, int port_no);

/* Hub class feature selectors (USB 1.1 spec ch. 11): SET/CLEAR_FEATURE
 * wValue - note the change-selectors (C_*) use DIFFERENT values than the
 * status-bit masks above (PORT_RESET=4 but C_PORT_RESET=20). */
#define HUB_SET_FEATURE    0x03
#define HUB_CLEAR_FEATURE  0x01
#define HUB_F_PORT_RESET   0x04
#define HUB_F_C_CONNECTION 0x10
#define HUB_F_C_PORT_RESET 0x14

/* wPortStatus bits */
#define HUB_ST_CCS   0x0001
#define HUB_ST_PE    0x0002
#define HUB_ST_LSDA  0x0200
/* wPortChange bits */
#define HUB_CH_CSC   0x0001
#define HUB_CH_PRSC  0x0010

static int hub_get_status(usb_device_t* hub, int port,
                          uint16_t* status, uint16_t* change) {
    uint8_t buf[4];
    uint16_t got = 0;
    int rc = uhci_control_xfer(hub, 0xA3, 0x00, 0x0000, (uint16_t)port,
                               1, buf, 4, &got);
    if (rc != 0 || got < 4) return -1;
    *status = (uint16_t)(buf[0] | (buf[1] << 8));
    *change = (uint16_t)(buf[2] | (buf[3] << 8));
    return 0;
}

static int hub_feature(usb_device_t* hub, int port, uint8_t bReq,
                       uint16_t feature) {
    return uhci_control_xfer(hub, 0x23, bReq, feature, (uint16_t)port,
                             0, NULL, 0, NULL);
}

/* Reset one downstream hub port (blocking ~120 ms, IF=0 callers only). */
static int hub_port_reset(usb_device_t* hub, int port, uint8_t* low_speed) {
    if (hub_feature(hub, port, HUB_SET_FEATURE, HUB_F_PORT_RESET) != 0)
        return -1;
    pit_delay_ms(100);
    uint16_t st = 0, ch = 0;
    if (hub_get_status(hub, port, &st, &ch) != 0) return -1;
    if (ch & HUB_CH_CSC)
        hub_feature(hub, port, HUB_CLEAR_FEATURE, HUB_F_C_CONNECTION);
    if (ch & HUB_CH_PRSC)
        hub_feature(hub, port, HUB_CLEAR_FEATURE, HUB_F_C_PORT_RESET);
    if (!(st & HUB_ST_PE)) {
        klogf("[usb] hub addr %d port %d: enable failed (st=0x%04x)\n",
              hub->address, port, st);
        return -1;
    }
    *low_speed = (st & HUB_ST_LSDA) ? 1 : 0;
    return 0;
}

static void hub_attach(usb_device_t* dev) {
    uint8_t hd[9];
    uint16_t got = 0;
    int rc = uhci_control_xfer(dev, 0xA0, 0x06, 0x2900, 0, 1, hd, 9, &got);
    if (rc != 0 || got < 3 || hd[1] != 0x29 || hd[2] == 0 || hd[2] > 15) {
        mark_port_ignored(dev->parent, dev->hub_port);
        klogf("[usb] hub descriptor failed (rc=%d got=%d) - ignored\n",
              rc, got);
        memset(dev, 0, sizeof(*dev));
        dev->state = USB_DEV_FREE;
        return;
    }
    dev->hub_nports = hd[2];
    dev->state = USB_DEV_RUNNING;
    klogf("[usb] hub attached (addr %d, %d ports, %04x:%04x) usb_hub_ok\n",
          dev->address, dev->hub_nports, dev->vendor_id, dev->product_id);
}

/* Poll the downstream ports of one attached hub (~1 s cadence). */
static void hub_poll(usb_device_t* hub) {
    for (int p = 1; p <= hub->hub_nports; p++) {
        uint16_t st = 0, ch = 0;
        if (hub_get_status(hub, p, &st, &ch) != 0) continue;

        if (!(st & HUB_ST_CCS)) {
            if (ch & HUB_CH_CSC)
                hub_feature(hub, p, HUB_CLEAR_FEATURE, HUB_F_C_CONNECTION);
            clear_port_ignored(hub->slot, p);
            usb_device_t* child = device_on_parent(hub->slot, p);
            if (child) unbind_device(child);
            continue;
        }
        if (device_on_parent(hub->slot, p) != NULL) continue;
        if (hub->hub_ignored & (1u << (p - 1))) continue;
        klogf("[usb] device attached on hub addr %d port %d\n",
              hub->address, p);
        enumerate_port(hub->slot, p);
    }
}

/* --- enumeration -------------------------------------------------------- */

/* Synchronous enumeration of a freshly (re)powered port.  Blocks for a
 * few hundred ms via pit_delay_ms (poll-based, safe inside IRQ0). */
static void enumerate_port(int parent_slot, int port_no) {
    int slot = -1;
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state == USB_DEV_FREE) { slot = i; break; }
    }
    if (slot < 0) {
        klog("[usb] no free device slot\n");
        return;
    }

    usb_device_t* dev = &devices[slot];
    memset(dev, 0, sizeof(*dev));
    dev->state = USB_DEV_ENUMING;
    dev->slot  = slot;
    dev->parent = (parent_slot == USB_ROOT_PARENT)
                      ? USB_ROOT_PARENT : (uint8_t)parent_slot;
    dev->hub_port = (uint8_t)port_no;

    /* reset + speed detect (root: PORTSC, hub: SET_PORT_RESET) */
    if (parent_slot == USB_ROOT_PARENT) {
        uhci_port_reset(port_no);
        dev->low_speed = (uint8_t)uhci_port_low_speed(port_no);
    } else {
        usb_device_t* hub = usb_device_at(parent_slot);
        if (hub == NULL || hub->state != USB_DEV_RUNNING ||
            hub_port_reset(hub, port_no, &dev->low_speed) != 0) {
            enum_fail(dev, "hub_reset", -1);
            return;
        }
    }
    dev->ep_mps = 64;               /* optimistic; fixed after step 1 */

    uint8_t  buf[64];
    uint16_t got = 0;
    int rc;

    /* 1. GET_DESCRIPTOR(Device, 8) at address 0 -> bMaxPacketSize0 */
    rc = uhci_control_xfer(dev, 0x80, 0x06, 0x0100, 0, 1, buf, 8, &got);
    if (rc != 0 || got < 8) { enum_fail(dev, "get_desc8", rc); return; }
    dev->ep_mps = buf[7];
    if (dev->ep_mps < 8)  dev->ep_mps = 8;
    if (dev->ep_mps > 64) dev->ep_mps = 64;

    /* 2. SET_ADDRESS (unique 1..USB_MAX_DEVICES), then let it settle */
    rc = uhci_control_xfer(dev, 0x00, 0x05, (uint16_t)(slot + 1), 0,
                           0, NULL, 0, NULL);
    if (rc != 0) { enum_fail(dev, "set_address", rc); return; }
    dev->address = (uint8_t)(slot + 1);
    pit_delay_ms(10);

    /* 3. GET_DESCRIPTOR(Device, 18) -> class + vid:pid */
    rc = uhci_control_xfer(dev, 0x80, 0x06, 0x0100, 0, 1, buf, 18, &got);
    if (rc != 0 || got < 18) { enum_fail(dev, "get_desc18", rc); return; }
    dev->device_class = buf[4];
    dev->vendor_id  = (uint16_t)(buf[8] | (buf[9] << 8));
    dev->product_id = (uint16_t)(buf[10] | (buf[11] << 8));

    /* 4. GET_DESCRIPTOR(Config): 9-byte header then the full chain */
    rc = uhci_control_xfer(dev, 0x80, 0x06, 0x0200, 0, 1, buf, 9, &got);
    if (rc != 0 || got < 9) { enum_fail(dev, "get_cfg9", rc); return; }
    uint16_t total =
        (uint16_t)(((usb_desc_config_t*)buf)->wTotalLength);
    if (total < 9) total = 9;
    if (total > sizeof(buf)) total = sizeof(buf);
    rc = uhci_control_xfer(dev, 0x80, 0x06, 0x0200, 0, 1, buf, total, &got);
    if (rc != 0 || got < total) { enum_fail(dev, "get_cfg", rc); return; }

    /* 5. walk the descriptor chain: HID boot interface + its interrupt
     *    IN endpoint (or a hub interface) */
    int matched = 0;
    int saw_hub = 0;
    int off = 0;
    while (off + 2 <= (int)got) {
        uint8_t len = buf[off];
        if (len < 2 || off + len > (int)got) break;
        uint8_t type = buf[off + 1];
        if (type == DESC_IFACE) {
            usb_desc_iface_t* ifc = (usb_desc_iface_t*)(buf + off);
            if (ifc->bInterfaceClass == 3 &&       /* HID */
                ifc->bInterfaceSubClass == 1 &&    /* boot */
                (ifc->bInterfaceProtocol == 1 ||   /* keyboard */
                 ifc->bInterfaceProtocol == 2)) {  /* mouse */
                dev->if_class    = 3;
                dev->if_protocol = ifc->bInterfaceProtocol;
                matched = 1;
            } else {
                matched = 0;
            }
            if (ifc->bInterfaceClass == 9) saw_hub = 1;
        } else if (type == DESC_ENDPOINT && matched && dev->ep_addr == 0) {
            usb_desc_endpoint_t* ep = (usb_desc_endpoint_t*)(buf + off);
            if ((ep->bEndpointAddress & 0x80) &&         /* IN */
                (ep->bmAttributes & 0x03) == 0x03) {     /* interrupt */
                dev->ep_addr = ep->bEndpointAddress;
                uint16_t mps = ep->wMaxPacketSize;
                if (mps < 1) mps = 1;
                if (mps > sizeof(dev->int_buf)) mps = sizeof(dev->int_buf);
                dev->ep_mps = (uint8_t)mps;
                dev->ep_interval = ep->bInterval;
            }
        }
        off += len;
    }

    /* 6a. hub dispatch: SET_CONFIGURATION, then read the hub descriptor */
    if (dev->device_class == 9 || saw_hub) {
        rc = uhci_control_xfer(dev, 0x00, 0x09, 0x0001, 0, 0, NULL, 0, NULL);
        if (rc != 0) { enum_fail(dev, "set_config", rc); return; }
        hub_attach(dev);
        return;
    }

    /* 6b. unsupported device: mark the port so we do not re-enumerate
     *     it every second until it goes away */
    if (!matched || dev->ep_addr == 0) {
        mark_port_ignored(parent_slot, port_no);
        klogf("[usb] unsupported device %04x:%04x on %s port %d - ignored\n",
              dev->vendor_id, dev->product_id,
              parent_slot == USB_ROOT_PARENT ? "root" : "hub", port_no);
        memset(dev, 0, sizeof(*dev));
        dev->state = USB_DEV_FREE;
        return;
    }

    /* 6c. SET_CONFIGURATION(1) + HID boot protocol (SET_PROTOCOL(0),
     *     SET_IDLE(0)); class requests carry bmRequestType 0x21 */
    rc = uhci_control_xfer(dev, 0x00, 0x09, 0x0001, 0, 0, NULL, 0, NULL);
    if (rc != 0) { enum_fail(dev, "set_config", rc); return; }
    uhci_control_xfer(dev, 0x21, 0x0B, 0x0000, 0, 0, NULL, 0, NULL);
    uhci_control_xfer(dev, 0x21, 0x0A, 0x0000, 0, 0, NULL, 0, NULL);

    /* 7. bind the HID driver and start the interrupt IN pipe */
    usb_hid_attach(dev);
    uhci_queue_interrupt(dev);
    dev->state = USB_DEV_RUNNING;
    klogf("[usb] %s plugged (addr %d, %04x:%04x, ep %02x mps %d, %s)\n",
          dev->if_protocol == 1 ? "usb_kbd" : "usb_mouse",
          dev->address, dev->vendor_id, dev->product_id,
          dev->ep_addr, dev->ep_mps,
          dev->low_speed ? "low-speed" : "full-speed");
    if (dev->if_protocol == 1) klog("[usb] usb_kbd_plugged\n");
    else                       klog("[usb] usb_mouse_plugged\n");
}

/* --- boot init ---------------------------------------------------------- */

void usb_init(void) {
    pci_device_t* pci = pci_find_class(0x0C, 0x03);
    if (pci == NULL ||
        pci_read_byte(pci->bus, pci->device, pci->function, 0x09) != 0x00) {
        klog("[usb] no UHCI controller - USB disabled\n");
        return;
    }
    if (uhci_init(pci) != 0) return;

    usb_ready = 1;
    for (int port = 1; port <= 2; port++) {
        uhci_clear_port_change(port);
        if (!uhci_port_connected(port)) continue;
        enumerate_port(USB_ROOT_PARENT, port);
    }
    klog("[usb] usb_enumerated\n");
}

/* --- IRQ0 tick ---------------------------------------------------------- */

void usb_tick(void) {
    if (!usb_ready) return;

    /* every tick: consume completed interrupt TDs (reports @ ~100Hz) */
    uhci_idle_requeue();

    /* ~1s cadence: root-hub + hub port polling (no port-change IRQ) */
    if (++tick_count < USB_TICKS_PER_POLL) return;
    tick_count = 0;

    for (int port = 1; port <= 2; port++) {
        int conn = uhci_port_connected(port);
        uhci_clear_port_change(port);
        usb_device_t* dev = device_on_parent(USB_ROOT_PARENT, port);
        if (!conn) {
            clear_port_ignored(USB_ROOT_PARENT, port);
            if (dev != NULL) unbind_device(dev);
        } else if (dev == NULL &&
                   !(root_ignored & (1u << (port - 1)))) {
            klogf("[usb] device attached on port %d\n", port);
            enumerate_port(USB_ROOT_PARENT, port);
        }
    }

    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t* dev = &devices[i];
        if (dev->state == USB_DEV_RUNNING && dev->hub_nports > 0)
            hub_poll(dev);
    }

    /* fatal TD errors seen by the IRQ/tick path */
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (devices[i].state == USB_DEV_RUNNING && devices[i].dead)
            unbind_device(&devices[i]);
    }
}

/* --- diagnostics (shell `usb` command) ---------------------------------- */

void usb_dump(void) {
    klog("[usb] ==== USB status ====\n");
    if (!usb_ready) {
        klog("[usb] no UHCI controller (USB disabled)\n");
        return;
    }
    uhci_dump_controller();
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_device_t* dev = &devices[i];
        if (dev->state == USB_DEV_FREE) {
            klogf("[usb] slot %d: free\n", i);
            continue;
        }
        if (dev->parent == USB_ROOT_PARENT) {
            klogf("[usb] slot %d: addr %d root port %d %s %s\n", i,
                  dev->address, dev->hub_port,
                  dev->state == USB_DEV_RUNNING ? "running" : "enumerating",
                  dev->low_speed ? "ls" : "fs");
        } else {
            usb_device_t* hub = usb_device_at(dev->parent);
            klogf("[usb] slot %d: addr %d hub %d port %d %s %s\n", i,
                  dev->address, hub ? hub->address : 0, dev->hub_port,
                  dev->state == USB_DEV_RUNNING ? "running" : "enumerating",
                  dev->low_speed ? "ls" : "fs");
        }
        if (dev->state != USB_DEV_RUNNING) continue;
        if (dev->hub_nports > 0) {
            klogf("[usb]   hub %04x:%04x ports %d ignored 0x%02x\n",
                  dev->vendor_id, dev->product_id,
                  dev->hub_nports, dev->hub_ignored);
        }
        if (dev->if_protocol == 1 || dev->if_protocol == 2) {
            klogf("[usb]   %s %04x:%04x ep %02x mps %d iv %d\n",
                  dev->if_protocol == 1 ? "kbd" : "mouse",
                  dev->vendor_id, dev->product_id,
                  dev->ep_addr, dev->ep_mps, dev->ep_interval);
            klogf("[usb]   nak %u reports %u %s\n",
                  dev->nak_count, dev->report_count,
                  dev->dead ? "DEAD" : "ok");
        }
    }
}
