/*
 * compat.c - Linux NIC driver compatibility runtime for Kil0yOS
 *
 * Implements the runtime half of include/linux/: printk, allocator,
 * jiffies, PCI driver-model matching (probe dispatch over the already
 * enumerated PCI device list), IRQ bridging with automatic EOI, skb
 * recycling and net_device registration into the kernel's global
 * netif. Linux drivers compiled into the kernel only see the API.
 */
#include "linux/kernel.h"
#include "linux/module.h"
#include "linux/pci.h"
#include "linux/io.h"
#include "linux/slab.h"
#include "linux/interrupt.h"
#include "linux/netdevice.h"

#include "drivers/video/vga.h"
#include "drivers/io.h"
#include "core/interrupts.h"
#include "core/isr.h"
#include "net/netif.h"
#include "lib/string.h"
#include "lib/stdlib.h"

/* The shim itself calls the project allocator directly; the Linux-named
 * macros in slab.h are for driver translation units only. */
#undef kmalloc
#undef kzalloc
#undef kcalloc
#undef kfree
#undef vmalloc
#undef vfree

/* ------------------------------------------------------------------ */
/* printk / jiffies                                                    */
/* ------------------------------------------------------------------ */

int printk(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    klog(buf);
    return 0;
}

unsigned long compat_jiffies(void) {
    return (unsigned long)(pit_uptime_us() / 10000u);   /* HZ = 100 */
}

/* ------------------------------------------------------------------ */
/* allocator                                                           */
/* ------------------------------------------------------------------ */

void* compat_kmalloc(size_t size, unsigned int flags) {
    void* p = kmalloc(size);
    if (p && (flags & __GFP_ZERO)) memset(p, 0, size);
    return p;
}

void* compat_kzalloc(size_t size, unsigned int flags) {
    (void)flags;
    void* p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void compat_kfree(void* ptr) {
    kfree(ptr);
}

/* ------------------------------------------------------------------ */
/* PCI driver model                                                    */
/* ------------------------------------------------------------------ */

static LIST_HEAD(compat_pci_drivers);
static struct pci_driver* compat_netdev_driver;   /* driver owning g_netif */

int pci_register_driver(struct pci_driver* drv) {
    list_add_tail(&drv->node, &compat_pci_drivers);

    for (pci_device_t* d = pci_get_device_list(); d; d = d->next) {
        const struct pci_device_id* id;
        for (id = drv->id_table; ; id++) {
            if (id->vendor == 0 && id->device == 0) break;   /* terminator */
            if (id->vendor != PCI_ANY_ID && id->vendor != d->vendor_id) continue;
            if (id->device != PCI_ANY_ID && id->device != d->device_id) continue;
            struct pci_dev pdev;
            compat_pci_dev_fill(&pdev, d);
            printk(KERN_INFO "compat: %s probing %04x:%04x\n",
                   drv->name, d->vendor_id, d->device_id);
            if (drv->probe && drv->probe(&pdev, id) == 0) {
                compat_netdev_driver = drv;
                printk(KERN_INFO "compat: %s claimed %04x:%04x irq=%d\n",
                       drv->name, d->vendor_id, d->device_id, pdev.irq);
                return 0;
            }
        }
    }
    return -ENODEV;
}

void pci_unregister_driver(struct pci_driver* drv) {
    list_del(&drv->node);
    if (compat_netdev_driver == drv) compat_netdev_driver = 0;
}

/* Fill a pci_dev and probe BAR sizes from config space (classic
 * write-0xFFFFFFFF/read-back trick, original value restored). */
void compat_pci_dev_fill(struct pci_dev* pdev, pci_device_t* core) {
    memset(pdev, 0, sizeof(*pdev));
    pdev->core    = core;
    pdev->vendor  = core->vendor_id;
    pdev->device  = core->device_id;
    pdev->irq     = core->irq;
    pdev->revision = pci_read_byte(core->bus, core->device, core->function, PCI_REVISION_OFFSET);
    pdev->class   = (pci_read_byte(core->bus, core->device, core->function, 0x0B) << 8) |
                     pci_read_byte(core->bus, core->device, core->function, 0x0A);
    pdev->devfn   = (core->device << 3) | core->function;

    for (int bar = 0; bar < 6; bar++) {
        uint16_t off = PCI_BASE_ADDRESS_0 + bar * 4;
        uint32_t orig = pci_read_dword(core->bus, core->device, core->function, off);
        if (orig == 0 || orig == 0xFFFFFFFF) continue;
        if (orig & 1) {                          /* I/O port BAR */
            pdev->resource_start[bar] = orig & ~0x3u;
            pdev->resource_flags[bar] = IORESOURCE_IO;
            pci_write_dword(core->bus, core->device, core->function, off, 0xFFFFFFFC);
            uint32_t sz = pci_read_dword(core->bus, core->device, core->function, off);
            pdev->resource_len[bar] = (~sz & 0xFFFC) + 4;
        } else if ((orig & 0x6) == 0) {          /* 32-bit MEM BAR */
            pdev->resource_start[bar] = orig & ~0xFu;
            pdev->resource_flags[bar] = IORESOURCE_MEM;
            pci_write_dword(core->bus, core->device, core->function, off, 0xFFFFFFF0);
            uint32_t sz = pci_read_dword(core->bus, core->device, core->function, off);
            pdev->resource_len[bar] = (~sz & 0xFFFFFFF0) + 0x10;
        }
        pci_write_dword(core->bus, core->device, core->function, off, orig);
    }
}

int pci_enable_device(struct pci_dev* dev) {
    pci_device_t* d = dev->core;
    uint16_t cmd = pci_read_word(d->bus, d->device, d->function, PCI_COMMAND);
    cmd |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    pci_write_word(d->bus, d->device, d->function, PCI_COMMAND, cmd);
    return 0;
}

int pci_enable_msi(struct pci_dev* dev) { (void)dev; return -EINVAL; }

int pci_alloc_irq_vectors(struct pci_dev* dev, int min_vecs, int max_vecs, int flags) {
    (void)max_vecs; (void)flags;
    return (min_vecs <= 1 && dev->irq) ? 1 : -ENODEV;
}

void pci_set_master(struct pci_dev* dev) {
    pci_device_t* d = dev->core;
    uint16_t cmd = pci_read_word(d->bus, d->device, d->function, PCI_COMMAND);
    cmd |= PCI_COMMAND_MASTER;
    pci_write_word(d->bus, d->device, d->function, PCI_COMMAND, cmd);
}

void __iomem* pci_ioremap_bar(struct pci_dev* dev, int bar) {
    if (!(dev->resource_flags[bar] & IORESOURCE_MEM)) return 0;
    return ioremap(dev->resource_start[bar], dev->resource_len[bar]);
}

int pci_request_regions(struct pci_dev* dev, const char* name) {
    (void)dev; (void)name;
    return 0;
}

void pci_disable_device(struct pci_dev* dev) { (void)dev; }

/* ------------------------------------------------------------------ */
/* IRQ bridging (wrapper transmits the EOI, driver returns IRQ_HANDLED)*/
/* ------------------------------------------------------------------ */

typedef struct compat_irq_entry {
    irqreturn_t (*handler)(int, void*);
    void* dev;
    char name[24];
} compat_irq_entry_t;

static compat_irq_entry_t compat_irqs[16];

static void compat_irq_trampoline(interrupt_frame_t* frame) {
    (void)frame;
    for (int i = 1; i < 16; i++) {          /* 0 = spurious/timer handled elsewhere */
        if (compat_irqs[i].handler) {
            compat_irqs[i].handler(i, compat_irqs[i].dev);
            pic_send_eoi((uint8_t)i);
        }
    }
}

int compat_request_irq(unsigned int irq, irqreturn_t (*handler)(int, void*),
                       unsigned long flags, const char* name, void* dev) {
    (void)flags;
    if (irq >= 16 || !handler) return -EINVAL;
    compat_irqs[irq].handler = handler;
    compat_irqs[irq].dev = dev;
    strncpy(compat_irqs[irq].name, name ? name : "?", sizeof(compat_irqs[irq].name) - 1);
    register_irq_handler((uint8_t)irq, compat_irq_trampoline);
    pic_enable_irq((uint8_t)irq);            /* unmask slave/master cascade */
    return 0;
}

void compat_free_irq(unsigned int irq, void* dev) {
    if (irq >= 16) return;
    if (compat_irqs[irq].dev == dev) {
        compat_irqs[irq].handler = 0;
        compat_irqs[irq].dev = 0;
    }
}

/* ------------------------------------------------------------------ */
/* skb + net_device                                                    */
/* ------------------------------------------------------------------ */

struct sk_buff* compat_alloc_skb(unsigned int size) {
    struct sk_buff* skb = compat_kzalloc(sizeof(*skb), GFP_KERNEL);
    if (!skb) return 0;
    skb->tail_alloc = size;
    skb->head = compat_kzalloc(size, GFP_KERNEL);
    if (!skb->head) { compat_kfree(skb); return 0; }
    skb->data = skb->head;
    skb->len = 0;
    return skb;
}

void compat_kfree_skb(struct sk_buff* skb) {
    if (!skb) return;
    compat_kfree(skb->head);
    compat_kfree(skb);
}

int netif_rx(struct sk_buff* skb) {
    if (!skb || skb->len == 0) { compat_kfree_skb(skb); return -1; }
    netif_receive(skb->data, (uint16_t)skb->len);
    compat_kfree_skb(skb);
    return 0;
}

/* xmit bridge: kernel netif hands raw bytes, we wrap a lightweight skb
 * around them and call the registered device's ndo_start_xmit. */
static int compat_xmit(const uint8_t* data, uint16_t len) {
    struct net_device* dev = compat_g_netdev();
    if (!dev || !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit) return -1;
    struct sk_buff skb;
    memset(&skb, 0, sizeof(skb));
    skb.dev = dev;
    skb.head = (__u8*)(uintptr_t)data;
    skb.data = skb.head;
    skb.len = len;
    skb.tail_alloc = len;
    return dev->netdev_ops->ndo_start_xmit(&skb, dev) == NETDEV_TX_OK ? 0 : -1;
}

static void compat_poll(void) {
    struct net_device* dev = compat_g_netdev();
    if (dev && dev->poll_hook) dev->poll_hook(dev);   /* interrupt-free rx drain */
}

struct net_device* compat_g_netdev(void) {
    return (struct net_device*)compat_netdev_priv;
}

void* compat_netdev_priv;   /* registered net_device */

int register_netdev(struct net_device* dev) {
    if (!dev || !dev->netdev_ops || !dev->netdev_ops->ndo_start_xmit)
        return -EINVAL;
    compat_netdev_priv = dev;
    for (int i = 0; i < ETH_ALEN; i++) g_netif.mac[i] = dev->dev_addr[i];
    g_netif.send = compat_xmit;
    g_netif.poll = compat_poll;
    g_netif.flags = 1;
    dev->flags |= IFF_UP | IFF_RUNNING;
    printk(KERN_INFO "compat: %s registered, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
           dev->name, dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
           dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5]);
    return 0;
}

void unregister_netdev(struct net_device* dev) {
    (void)dev;
    compat_netdev_priv = 0;
    g_netif.send = 0;
    g_netif.poll = 0;
    g_netif.flags = 0;
}

struct net_device* alloc_netdev(int sizeof_priv, const char* name,
                                void (*setup)(struct net_device*)) {
    struct net_device* dev = compat_kzalloc(sizeof(*dev) + sizeof_priv, GFP_KERNEL);
    if (!dev) return 0;
    if (sizeof_priv) dev->priv = (char*)dev + sizeof(*dev);
    strncpy(dev->name, name ? name : "eth%d", sizeof(dev->name) - 1);
    memset(dev->broadcast, 0xFF, ETH_ALEN);
    if (setup) setup(dev);
    return dev;
}

void free_netdev(struct net_device* dev) {
    compat_kfree(dev);
}

void eth_broadcast_addr(__u8* addr) {
    memset(addr, 0xFF, ETH_ALEN);
}

/* ------------------------------------------------------------------ */
/* initcall runner (called by main after PCI + netif are up)            */
/* ------------------------------------------------------------------ */

extern const struct compat_initcall __compat_initcall_start[];
extern const struct compat_initcall __compat_initcall_end[];

void compat_initcalls(void) {
    for (const struct compat_initcall* ic = __compat_initcall_start;
         ic < __compat_initcall_end; ic++) {
        printk(KERN_INFO "compat: initcall %s\n", ic->name ? ic->name : "?");
        ic->fn();
    }
}
