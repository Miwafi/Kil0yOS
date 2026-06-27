#ifndef PCI_H
#define PCI_H

#include "lib/types.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_VENDOR_ID_OFFSET    0x00
#define PCI_DEVICE_ID_OFFSET    0x02
#define PCI_COMMAND_OFFSET      0x04
#define PCI_STATUS_OFFSET       0x06
#define PCI_CLASS_CODE_OFFSET   0x0B
#define PCI_REVISION_OFFSET     0x08
#define PCI_BAR0_OFFSET         0x10
#define PCI_BAR1_OFFSET         0x14
#define PCI_INTERRUPT_LINE_OFFSET 0x3C


typedef struct pci_device {
    uint16_t bus;
    uint16_t device;
    uint16_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass_code;
    uint32_t bar0;
    uint8_t irq;
    struct pci_device* next;
} pci_device_t;

void pci_init();
pci_device_t* pci_find_device(uint16_t vendor_id, uint16_t device_id);
pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass_code);
uint32_t pci_read_dword(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset);
uint16_t pci_read_word(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset);
uint8_t pci_read_byte(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset);
void pci_write_dword(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset, uint32_t value);
void pci_write_word(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset, uint16_t value);
void pci_write_byte(uint16_t bus, uint16_t device, uint16_t function, uint16_t offset, uint8_t value);

#endif
//guys this is a pice of shit and need to be NUKED