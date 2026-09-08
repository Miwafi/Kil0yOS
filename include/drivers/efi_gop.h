#ifndef EFI_GOP_H
#define EFI_GOP_H

#include "lib/types.h"

/* UEFI Graphics Output Protocol support, called by the kernel itself.
 *
 * GRUB keeps EFI boot services alive for us (multiboot2 header EFI_BS tag,
 * type 7) and hands over the EFI system table through multiboot2 boot-info
 * tag type 12 plus the "boot services alive" marker (type 18).  While the
 * services are still up we locate GOP, select a 1024x768x32 mode, snapshot
 * the EFI memory map and then call ExitBootServices ourselves.
 * BIOS boots never see the tags and fall straight back to VGA.
 *
 * Must be called first thing in kernel_main: IF=0, no heap, no klog yet
 * (it logs straight to COM1). */

void efi_gop_init(uint64_t mb_info_phys);

/* EFI memory map snapshot (valid after efi_gop_init; only when GRUB kept
 * boot services alive, in which case the multiboot2 mmap tag is absent) */
int      efi_memory_map_available(void);
const uint8_t* efi_memory_map_ptr(void);
uint32_t efi_memory_map_size(void);        /* bytes */
uint32_t efi_memory_map_desc_size(void);   /* >= 40 */
uint32_t efi_memory_map_desc_version(void);

/* framebuffer reservation helpers for the PMM (0 when inactive) */
uint64_t fb_base(void);
uint64_t fb_size(void);

#endif /* EFI_GOP_H */
