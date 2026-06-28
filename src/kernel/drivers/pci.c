#include "drivers/pci.h"
#include "drivers/vga.h"
#include "drivers/io.h"
#include "mm/memory.h"
#include "lib/stdlib.h"

static pci_device_t* pci_devices = NULL;

static uint32_t pci_config_address(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset) {
    return (1 << 31) | (bus << 16) | (device << 11) | (function << 8) | (offset & 0xFC);
}

uint32_t pci_read_dword(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset) {
    outd(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    return ind(PCI_CONFIG_DATA);
}

uint16_t pci_read_word(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset) {
    outd(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    return (uint16_t)(ind(PCI_CONFIG_DATA) >> ((offset & 2) * 8));
}

uint8_t pci_read_byte(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset) {
    outd(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    return (uint8_t)(ind(PCI_CONFIG_DATA) >> ((offset & 3) * 8));
}

void pci_write_dword(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset, uint32_t value) {
    outd(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    outd(PCI_CONFIG_DATA, value);
}

void pci_write_word(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset, uint16_t value) {
    outd(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    uint32_t old = ind(PCI_CONFIG_DATA);
    uint16_t shift = (offset & 2) * 8;
    old &= ~(0xFFFF << shift);
    old |= ((uint32_t)value) << shift;
    outd(PCI_CONFIG_DATA, old);
}

void pci_write_byte(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset, uint8_t value) {
    outd(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    uint32_t old = ind(PCI_CONFIG_DATA);
    uint8_t shift = (offset & 3) * 8;
    old &= ~(0xFF << shift);
    old |= ((uint32_t)value) << shift;
    outd(PCI_CONFIG_DATA, old);
}

void pci_init() {
    klog("PCI: scanning buses...\n");

    for (int bus = 0; bus < 256; bus++) {
        for (int device = 0; device < 32; device++) {
            uint16_t vendor_id = pci_read_word(bus, device, 0, PCI_VENDOR_ID_OFFSET);
            if (vendor_id == 0xFFFF) continue;

            int functions = 1;
            uint8_t header_type = pci_read_byte(bus, device, 0, 0x0E);
            if ((header_type & 0x80) != 0) {
                functions = 8;
            }

            for (int function = 0; function < functions; function++) {
                vendor_id = pci_read_word(bus, device, function, PCI_VENDOR_ID_OFFSET);
                if (vendor_id == 0xFFFF) continue;

                uint16_t device_id = pci_read_word(bus, device, function, PCI_DEVICE_ID_OFFSET);
                uint8_t class_code = pci_read_byte(bus, device, function, PCI_CLASS_CODE_OFFSET);
                uint8_t subclass_code = pci_read_byte(bus, device, function, PCI_CLASS_CODE_OFFSET + 1);
                uint32_t bar0 = pci_read_dword(bus, device, function, PCI_BAR0_OFFSET);
                uint8_t irq = pci_read_byte(bus, device, function, PCI_INTERRUPT_LINE_OFFSET);

                pci_device_t* dev = (pci_device_t*)kmalloc(sizeof(pci_device_t));
                if (dev != NULL) {
                    dev->bus = bus;
                    dev->device = device;
                    dev->function = function;
                    dev->vendor_id = vendor_id;
                    dev->device_id = device_id;
                    dev->class_code = class_code;
                    dev->subclass_code = subclass_code;
                    dev->bar0 = bar0 & ~0xF;
                    dev->irq = irq;
                    dev->next = pci_devices;
                    pci_devices = dev;

                }
            }
        }
    }
}

pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    pci_device_t* dev = pci_devices;
    while (dev != NULL) {
        if (dev->vendor_id == vendor_id && dev->device_id == device_id) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass_code) {
    pci_device_t* dev = pci_devices;
    while (dev != NULL) {
        if (dev->class_code == class_code && dev->subclass_code == subclass_code) {
            return dev;
        }
        dev = dev->next;
    }
    return NULL;
}

pci_device_t* pci_get_device_list(void) {
    return pci_devices;
}

