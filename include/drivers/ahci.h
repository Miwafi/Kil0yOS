#ifndef AHCI_H
#define AHCI_H

/* Minimal polling AHCI (SATA) driver. Registers detected disks as sd0/sd1
 * on the blkdev layer. No interrupts (GHC.IE=0), no NCQ (slot 0 only),
 * no ATAPI, no port multiplication, no power management. */

void ahci_init(void);

#endif
