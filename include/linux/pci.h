/*
 * linux/pci.h - PCI driver model on Kil0yOS
 *
 * struct pci_dev wraps the project's pci_device_t. The compat core
 * (src/kernel/compat/linux/compat.c) walks the already-enumerated PCI
 * list and calls probe() for each id-table match — the same contract
 * as Linux' pci_register_driver().
 */
#ifndef _COMPAT_LINUX_PCI_H
#define _COMPAT_LINUX_PCI_H

#include "drivers/pci.h"
#include "linux/types.h"
#include "linux/io.h"

#define PCI_ANY_ID (~0u)

#define PCI_VENDOR_ID_REALTEK 0x10EC
#define PCI_VENDOR_ID_DLINK   0x1186
#define PCI_VENDOR_ID_INTEL   0x8086

#define PCI_BASE_ADDRESS_0 0x10
#define PCI_BASE_ADDRESS_1 0x14
#define PCI_BASE_ADDRESS_2 0x18
#define PCI_BASE_ADDRESS_3 0x1C
#define PCI_BASE_ADDRESS_4 0x20
#define PCI_BASE_ADDRESS_5 0x24

#define PCI_COMMAND         0x04
#define PCI_COMMAND_IO      0x1
#define PCI_COMMAND_MEMORY  0x2
#define PCI_COMMAND_MASTER  0x4

#define PCI_LATENCY_TIMER   0x0D
#define PCI_INTERRUPT_LINE  0x3C
#define PCI_CACHE_LINE_SIZE 0x0C

/* device_id / vendor id with PCI_ANY_ID support */
struct pci_device_id {
    __u32 vendor, device;
    __u32 subvendor, subdevice;
    __u32 class, class_mask;
    unsigned long driver_data;
};

#define PCI_DEVICE(vend, dev)          \
    .vendor = (vend), .device = (dev), \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

struct pci_dev {
    struct pci_device* core;    /* underlying Kil0yOS device node */
    __u16 vendor, device;
    __u8  irq;                  /* interrupt line */
    __u8  revision;
    __u32 class;
    int   devfn;
    /* config-space helpers resolvable at probe time */
    unsigned long resource_start[6];
    unsigned long resource_len[6];
    int   resource_flags[6];    /* IORESOURCE_IO / IORESOURCE_MEM */
    void* priv;                 /* driver may reuse for convenience */
};

#define IORESOURCE_IO  0x00000100
#define IORESOURCE_MEM 0x00000200

#define PCI_IRQ_LEGACY 0x1
#define PCI_IRQ_MSI    0x2
#define PCI_IRQ_INTX   0x4

struct pci_driver {
    const char* name;
    const struct pci_device_id* id_table;
    int (*probe)(struct pci_dev* dev, const struct pci_device_id* id);
    void (*remove)(struct pci_dev* dev);
    int  (*suspend)(struct pci_dev* dev);
    int  (*resume)(struct pci_dev* dev);
    struct list_head node;
};

int  pci_register_driver(struct pci_driver* drv);
void pci_unregister_driver(struct pci_driver* drv);
void compat_pci_dev_fill(struct pci_dev* pdev, pci_device_t* core);
int  pci_enable_device(struct pci_dev* dev);
int  pci_enable_msi(struct pci_dev* dev);
int  pci_alloc_irq_vectors(struct pci_dev* dev, int min_vecs, int max_vecs, int flags);
void pci_set_master(struct pci_dev* dev);

#define pci_resource_start(dev, bar) ((dev)->resource_start[bar])
#define pci_resource_len(dev, bar)   ((dev)->resource_len[bar])
#define pci_resource_flags(dev, bar) ((dev)->resource_flags[bar])

void __iomem* pci_ioremap_bar(struct pci_dev* dev, int bar);
int pci_request_regions(struct pci_dev* dev, const char* name);
void pci_disable_device(struct pci_dev* dev);

#endif /* _COMPAT_LINUX_PCI_H */
