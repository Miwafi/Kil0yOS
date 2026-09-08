#!/bin/bash
# GOP firmware matrix probe: which OVMF variant exposes GOP to GRUB?
# builds a tiny test ISO whose grub.cfg runs `videoinfo` on COM1 before
# booting the real kernel, then tries 3 firmware configurations.
set -u
cd /mnt/c/Users/19423/Desktop/Programs/Kil0yOS || exit 1
T="$HOME/ktest/gopfw"
mkdir -p "$T"

echo "=== build test ISO with videoinfo ==="
rm -rf "$T/iso"; mkdir -p "$T/iso/boot/grub"
cp build/kernel.bin "$T/iso/boot/kil0yos.bin"
cat > "$T/iso/boot/grub/grub.cfg" <<'EOF'
serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1
terminal_input serial console
terminal_output serial console
set timeout=1
set default=0
menuentry 'v' {
  insmod all_video
  videoinfo
  multiboot2 /boot/kil0yos.bin
  boot
}
EOF
grub-mkrescue -o "$T/test.iso" "$T/iso" >/dev/null 2>&1 || { echo "grub-mkrescue FAILED"; exit 1; }
echo "test.iso built: $(stat -c%s "$T/test.iso") bytes"

run_q() { # run_q <name> <extra qemu args...>
  local name="$1"; shift
  echo
  echo "=== firmware: $name ==="
  rm -f "$T/serial_$name.log"
  timeout 25 qemu-system-x86_64 \
    -cdrom "$T/test.iso" -m 512M -display none \
    -serial file:"$T/serial_$name.log" \
    -no-reboot "$@" &
  local pid=$!
  # wait for kernel banner or videoinfo results
  for i in $(seq 1 20); do
    sleep 1
    if grep -aq "gop_ok\|gop_fail\|List of supported video" "$T/serial_$name.log" 2>/dev/null; then
      sleep 2; break
    fi
  done
  kill $pid 2>/dev/null; wait $pid 2>/dev/null
  echo "--- videoinfo / GOP evidence in $name ---"
  grep -aE "EFIGOP|EFI GOP|gop_ok|gop_fail|List of supported|No video mode|error" "$T/serial_$name.log" | head -15
  echo "--- log size: $(stat -c%s "$T/serial_$name.log" 2>/dev/null) ---"
}

# copy writable vars for pflash
cp /usr/share/OVMF/OVMF_VARS_4M.fd "$T/VARS.fd" 2>/dev/null

run_q "legacy_bios" # OVMF.fd baseline
echo
echo "=== firmware: OVMF.fd via -bios ==="
rm -f "$T/serial_ovmf2m.log"
timeout 25 qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
  -cdrom "$T/test.iso" -m 512M -display none \
  -serial file:"$T/serial_ovmf2m.log" -no-reboot &
P=$!
for i in $(seq 1 20); do sleep 1; grep -aq "gop_ok\|gop_fail\|List of supported video" "$T/serial_ovmf2m.log" 2>/dev/null && { sleep 2; break; }; done
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "--- videoinfo / GOP evidence in OVMF.fd ---"
grep -aE "EFIGOP|EFI GOP|gop_ok|gop_fail|List of supported|No video mode|error" "$T/serial_ovmf2m.log" | head -15

echo
echo "=== firmware: 4M pflash pair ==="
rm -f "$T/serial_ovmf4m.log"
timeout 25 qemu-system-x86_64 \
  -drive if=pflash,format=raw,unit=0,file=/usr/share/OVMF/OVMF_CODE_4M.fd,readonly=on \
  -drive if=pflash,format=raw,unit=1,file="$T/VARS.fd" \
  -cdrom "$T/test.iso" -m 512M -display none \
  -serial file:"$T/serial_ovmf4m.log" -no-reboot &
P=$!
for i in $(seq 1 20); do sleep 1; grep -aq "gop_ok\|gop_fail\|List of supported video" "$T/serial_ovmf4m.log" 2>/dev/null && { sleep 2; break; }; done
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "--- videoinfo / GOP evidence in OVMF 4M pflash ---"
grep -aE "EFIGOP|EFI GOP|gop_ok|gop_fail|List of supported|No video mode|error" "$T/serial_ovmf4m.log" | head -15

echo
echo "=== firmware: OVMF.fd + explicit -vga std ==="
rm -f "$T/serial_vgastd.log"
timeout 25 qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -vga std \
  -cdrom "$T/test.iso" -m 512M -display none \
  -serial file:"$T/serial_vgastd.log" -no-reboot &
P=$!
for i in $(seq 1 20); do sleep 1; grep -aq "gop_ok\|gop_fail\|List of supported video" "$T/serial_vgastd.log" 2>/dev/null && { sleep 2; break; }; done
kill $P 2>/dev/null; wait $P 2>/dev/null
echo "--- videoinfo / GOP evidence in vga std ---"
grep -aE "EFIGOP|EFI GOP|gop_ok|gop_fail|List of supported|No video mode|error" "$T/serial_vgastd.log" | head -15

echo
echo "=== DONE ==="
