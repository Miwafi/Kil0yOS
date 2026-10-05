#!/bin/bash
# kilinstall smoke (v3.10.0), two serial phases:
#   Phase 1 (install): q35 + ISO boot (Live mode, RAM root) + blank 64 MiB
#     AHCI disk -> type "kilinstall" -> it writes GRUB (MBR + core.img) and
#     an ext2 partition holding the live tree + the pass-1 kernel ELF.
#   Phase 2 (boot from disk): restart QEMU with ONLY the AHCI disk ->
#     SeaBIOS boots the installed GRUB -> kernel from the ext2 root ->
#     assert the boot banner, "ls /boot" shows the kernel image and
#     "cat /boot/.kilinstall" reads back the marker.
# Serial runs in phases serially (never alongside other smokes).
# Usage (WSL): bash tools/smoke_install.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_inst1.log" "$TDIR/serial_inst2.log" "$TDIR/qmon_inst"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1
cp build/kil0yos.iso /tmp/kil0yos_inst.iso || { echo "ISO_COPY_FAILED"; exit 1; }
dd if=/dev/zero of=/tmp/kinst_disk.img bs=1M count=64 status=none

FAIL=0

# ---------------- phase 1: install ----------------
qemu-system-x86_64 -M q35 -cdrom /tmp/kil0yos_inst.iso -m 512M -display none \
  -serial file:"$TDIR/serial_inst1.log" \
  -monitor unix:"$TDIR/qmon_inst",server,nowait \
  -drive file=/tmp/kinst_disk.img,format=raw,if=none,id=sata0 \
  -device ide-hd,drive=sata0,bus=ide.0 \
  -no-reboot &
QPID=$!

i=0
while [ $i -lt 90 ]; do
  grep -aq "Kil0yOS version" "$TDIR/serial_inst1.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done
[ $i -lt 90 ] || { echo "BOOT_TIMEOUT_P1"; kill $QPID; exit 1; }
sleep 8

echo "=== kilinstall ==="
timeout 20 python3 tools/drive_keys.py "kilinstall" "$TDIR/qmon_inst" \
  || { echo "TYPING_FAILED"; FAIL=1; }
i=0
while [ $i -lt 240 ]; do
  grep -aq "written to sd0p1" "$TDIR/serial_inst1.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done

grep -a "kilinstall:" "$TDIR/serial_inst1.log" | tail -8

grep -aq "kilinstall: install mode" "$TDIR/serial_inst1.log" || { echo "not install mode"; FAIL=1; }
grep -aq "written to sd0p1" "$TDIR/serial_inst1.log" || { echo "install did not finish"; FAIL=1; }
grep -aqE "EXCEPTION|PANIC" "$TDIR/serial_inst1.log" && { echo "exception/panic in log"; FAIL=1; }

kill "$QPID" 2>/dev/null
sleep 1
[ "$FAIL" -eq 0 ] || { echo "INSTALL_SMOKE_FAILED"; exit 1; }

# ---------------- phase 2: boot from disk ----------------
rm -f "$TDIR/qmon_inst"
qemu-system-x86_64 -M q35 -m 512M -display none \
  -serial file:"$TDIR/serial_inst2.log" \
  -monitor unix:"$TDIR/qmon_inst",server,nowait \
  -drive file=/tmp/kinst_disk.img,format=raw,if=none,id=sata0 \
  -device ide-hd,drive=sata0,bus=ide.0 \
  -no-reboot &
QPID=$!

i=0
while [ $i -lt 90 ]; do
  grep -aq "Kil0yOS version" "$TDIR/serial_inst2.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done
[ $i -lt 90 ] || { echo "BOOT_TIMEOUT_P2"; kill $QPID; exit 1; }
sleep 8

echo "=== disk boot checks ==="
timeout 20 python3 tools/drive_keys.py "ls /boot" "$TDIR/qmon_inst" \
  || { echo "TYPING_FAILED"; FAIL=1; }
i=0
while [ $i -lt 30 ]; do
  grep -aq "kil0yos.bin" "$TDIR/serial_inst2.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done

timeout 20 python3 tools/drive_keys.py "cat /boot/.kilinstall" "$TDIR/qmon_inst" \
  || { echo "TYPING_FAILED"; FAIL=1; }
i=0
while [ $i -lt 30 ]; do
  grep -aq "kil0yos-installed" "$TDIR/serial_inst2.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done

echo "--- serial tail (phase 2) ---"
tail -20 "$TDIR/serial_inst2.log"
grep -aq "kil0yos.bin" "$TDIR/serial_inst2.log" || { echo "/boot/kil0yos.bin missing"; FAIL=1; }
grep -aq "kil0yos-installed" "$TDIR/serial_inst2.log" || { echo "marker missing"; FAIL=1; }
grep -aqE "EXCEPTION|PANIC" "$TDIR/serial_inst2.log" && { echo "exception/panic in log"; FAIL=1; }

kill "$QPID" 2>/dev/null
sleep 1

if [ "$FAIL" -eq 0 ]; then
    echo "INSTALL_BOOT_OK"
    exit 0
else
    echo "INSTALL_SMOKE_FAILED"
    exit 1
fi
