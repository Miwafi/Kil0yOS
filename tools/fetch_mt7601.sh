#!/bin/bash
set -e
WS=/mnt/c/Users/19423/Desktop/Programs/Kil0yOS/build/mt7601_ref
mkdir -p "$WS" && cd "$WS"
CGIT=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/net/wireless/mediatek/mt7601u
for f in usb.c usb.h mcu.c mcu.h dma.h usb_regs.h init.c mt7601u.h eeprom.c; do
  [ -s "$f" ] && continue
  for i in 1 2 3 4 5; do
    curl -sf -m 30 -o "$f" "$CGIT/$f" && { echo "OK $f ($(wc -c < "$f"))"; break; }
    sleep 2
  done
done
if [ ! -s mt7601u.bin ]; then
  curl -sf -m 60 -o mt7601u.bin \
    "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek/mt7601u.bin" \
    && echo "FW OK ($(wc -c < mt7601u.bin))"
fi
md5sum mt7601u.bin
ls -la
