#!/bin/bash
# Build a real-hardware-friendly USB image: MBR + 30MB FAT16 partition +
# GRUB (i386-pc) assembled by hand. No root, no loop devices needed.
#
# Why not the hybrid ISO for bare metal: real BIOSes are barely tested on
# ISO9660-over-USB (int 13h geometry quirks crash GRUB while probing the
# filesystem - observed as a reboot on `ls (hd0)`), while FAT16-on-USB is
# the path every classic LiveUSB uses.
#
# Layout:
#   LBA 0            GRUB boot.img (MBR, partition entry patched in)
#   LBA 1..          GRUB core.img (MBR gap, boot.img default kernel_sector=1)
#   LBA 2048..       FAT16: /boot/kil0yos.bin, /boot/grub/{grub.cfg,i386-pc/*.mod}
#
# Boot path: the embedded early config loads multiboot2 from the FAT
# partition and boots the kernel directly (unattended, no menu). If it
# fails, normal falls back to /boot/grub/grub.cfg (menu with insmod
# multiboot2 - the command.lst needed for command auto-loading is NOT
# copied, so menu entries must insmod explicitly).
#
# Usage (WSL): tools/make_usb.sh [output.img]
# Flash with Rufus (DD mode) or dd the .img onto the stick.
set -e

OUT=${1:-build/kil0yos-usb.img}
KERNEL=build/kernel.bin
CFG=grub.cfg
GRUBDIR=/usr/lib/grub/i386-pc

if [ ! -f "$KERNEL" ]; then
    echo "error: $KERNEL not found - run 'make' first" >&2
    exit 1
fi
for tool in mformat mcopy mmd grub-mkimage; do
    command -v $tool >/dev/null || { echo "error: $tool missing (install mtools grub-pc-bin)" >&2; exit 1; }
done

PART_START=2048
PART_SECTORS=61440          # 30MB -> FAT16 (FAT32 needs >=33MB)
IMG_SECTORS=$((PART_START + PART_SECTORS))
OFFSET=$((PART_START * 512))

dd if=/dev/zero of="$OUT" bs=512 count=$IMG_SECTORS status=none

# --- FAT16 filesystem + payload at the partition offset ---
mformat -i "$OUT@@$OFFSET" -C -T $PART_SECTORS -v KIL0YOS
mmd   -i "$OUT@@$OFFSET" ::/boot ::/boot/grub ::/boot/grub/i386-pc
mcopy -i "$OUT@@$OFFSET" "$KERNEL" ::/boot/kil0yos.bin
mcopy -i "$OUT@@$OFFSET" "$CFG"    ::/boot/grub/grub.cfg
mcopy -i "$OUT@@$OFFSET" -s "$GRUBDIR"/*.mod ::/boot/grub/i386-pc/

# --- GRUB core for BIOS: prefix (hd0,msdos1)/boot/grub ---
# The early config boots the kernel directly. insmod multiboot2 is
# required because command auto-loading relies on command.lst which is
# not present on the stick. Serial mirroring keeps boot failures visible
# in headless QEMU and on a real machine's COM1 (VGA console is kept).
EARLY=$(mktemp)
cat > "$EARLY" <<'EOF'
serial --unit=0 --speed=115200
terminal_output --append serial
insmod multiboot2
multiboot2 /boot/kil0yos.bin
boot
EOF
grub-mkimage -O i386-pc -o build/grub-core.img -p '(hd0,msdos1)/boot/grub' \
    -c "$EARLY" biosdisk part_msdos fat normal serial terminal
rm -f "$EARLY"

# --- menu fallback: grub.cfg with explicit insmod (no command.lst) ---
FALLBACK=$(mktemp)
{ printf 'insmod multiboot2\n'; cat "$CFG"; } > "$FALLBACK"
mcopy -i "$OUT@@$OFFSET" -o "$FALLBACK" ::/boot/grub/grub.cfg
rm -f "$FALLBACK"

# boot.img -> MBR; wipe all 4 partition slots (the boot.img template
# carries non-zero garbage in slots 2-4 which breaks part_msdos probing),
# then patch in one active FAT16 partition entry (LBA 2048, 61440 sectors,
# CHS fields 0xFEFFFF for max compatibility).
dd if="$GRUBDIR/boot.img" of="$OUT" bs=512 count=1 conv=notrunc status=none
dd if=/dev/zero of="$OUT" bs=1 seek=446 count=64 conv=notrunc status=none
printf '\x80\xfe\xff\xff\x0e\xfe\xff\xff\x00\x08\x00\x00\x00\xf0\x00\x00' \
    | dd of="$OUT" bs=1 seek=446 conv=notrunc status=none
# core.img -> MBR gap at LBA 1 (boot.img's default kernel_sector is 1)
dd if=build/grub-core.img of="$OUT" bs=512 seek=1 conv=notrunc status=none
rm -f build/grub-core.img

echo "USB image written: $OUT ($(( IMG_SECTORS * 512 / 1024 / 1024 ))MB)"
echo "Flash with Rufus (DD mode) or: dd if=$OUT of=/dev/sdX bs=4M status=progress"
