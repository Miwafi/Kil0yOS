#!/bin/bash
# ext2 mount (mnt) smoke (v3.10.0):
#   q35 machine so the test disk hangs off the ICH9 AHCI controller (sd0),
#   blank 64 MiB SATA disk, ISO boot (Live mode - RAM root), then drive the
#   shell over the QEMU monitor: "mnt selftest" runs
#   mkfs -> mount -> create/write/read-back (300 KiB pattern exercising
#   direct + singly + doubly indirect blocks) -> umount on the raw device.
#   The serial log must contain MNT_SELFTEST_OK and no EXCEPTION/PANIC.
# Usage (WSL): bash tools/smoke_mnt.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_mnt.log" "$TDIR/qmon_mnt"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1
cp build/kil0yos.iso /tmp/kil0yos_mnt.iso || { echo "ISO_COPY_FAILED"; exit 1; }
dd if=/dev/zero of=/tmp/kmnt_disk.img bs=1M count=64 status=none

FAIL=0
qemu-system-x86_64 -M q35 -cdrom /tmp/kil0yos_mnt.iso -m 512M -display none \
  -serial file:"$TDIR/serial_mnt.log" \
  -monitor unix:"$TDIR/qmon_mnt",server,nowait \
  -drive file=/tmp/kmnt_disk.img,format=raw,if=none,id=sata0 \
  -device ide-hd,drive=sata0,bus=ide.0 \
  -no-reboot &
QPID=$!

i=0
while [ $i -lt 90 ]; do
  grep -aq "Kil0yOS version" "$TDIR/serial_mnt.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done
[ $i -lt 90 ] || { echo "BOOT_TIMEOUT"; kill $QPID; exit 1; }
sleep 8

echo "=== sd0 registration ==="
grep -a "\[ahci\]" "$TDIR/serial_mnt.log" | head -3
grep -aq "registered:" "$TDIR/serial_mnt.log" || { echo "sd0 missing"; FAIL=1; }

echo "=== mnt selftest ==="
timeout 20 python3 tools/drive_keys.py "mnt selftest" "$TDIR/qmon_mnt" \
  || { echo "TYPING_FAILED"; FAIL=1; }
i=0
while [ $i -lt 90 ]; do
  grep -aq "MNT_SELFTEST" "$TDIR/serial_mnt.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done

grep -aE "\[mkfs\]|\[ext2\]|\[mnt\]" "$TDIR/serial_mnt.log" | tail -8

grep -aq "MNT_SELFTEST_OK" "$TDIR/serial_mnt.log" || { echo "selftest FAILED"; FAIL=1; }
grep -aqE "EXCEPTION|PANIC" "$TDIR/serial_mnt.log" && { echo "exception/panic in log"; FAIL=1; }

kill "$QPID" 2>/dev/null
sleep 1

if [ "$FAIL" -eq 0 ]; then
    echo "MNT_SMOKE_OK"
    exit 0
else
    echo "MNT_SMOKE_FAILED"
    exit 1
fi
