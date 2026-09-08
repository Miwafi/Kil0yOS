#!/bin/bash
# one-off forensic probe: who prints "no suitable video mode found"?
cd /mnt/c/Users/19423/Desktop/Programs/Kil0yOS || exit 1

echo "=== 1) multiboot2 header tags in boot.asm ==="
grep -nE 'TAG|tag|framebuffer|fb_' src/boot/boot.asm | head -40

echo
echo "=== 2) extract GRUB EFI binary from ISO ==="
rm -rf ~/ktest/iso_probe; mkdir -p ~/ktest/iso_probe/m
# mount-less extract via xorriso (available since grub-mkrescue needs it)
xorriso -osirrox on -indev build/kil0yos.iso -extract / ~/ktest/iso_probe/m >/dev/null 2>&1
find ~/ktest/iso_probe/m -type f | head -30
EFI=$(find ~/ktest/iso_probe/m -name 'BOOTX64.EFI' -o -name 'grubx64.efi' | head -1)
echo "EFI binary: $EFI"
if [ -n "$EFI" ]; then
  echo "--- strings in GRUB EFI binary ---"
  strings "$EFI" | grep -aiE 'no suitable video mode|no console|video mode found' | head -10
fi

echo
echo "=== 3) same strings inside kernel.bin ==="
strings build/kernel.bin | grep -aiE 'no suitable video mode|no console will be available' | head -5

echo
echo "=== 4) grub-mkrescue embedded config (memdisk) ==="
strings /usr/lib/grub/x86_64-efi/monolithic/grubx64.efi 2>/dev/null | grep -aiE 'no suitable video mode|no console' | head -5
