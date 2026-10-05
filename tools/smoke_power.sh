#!/bin/bash
# Power-subsystem smoke (v3.10.0 P1-P2):
#   1) boot - the "[power] DSDT \_S5 parsed" klog line must appear
#   2) type `shutdown` - ACPI S5 must terminate QEMU within 15 s
# Usage (WSL): bash tools/smoke_power.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_pow.log" "$TDIR/qmon_pow"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1
cp build/kil0yos.iso /tmp/kil0yos_pow.iso || { echo "ISO_COPY_FAILED"; exit 1; }

FAIL=0
qemu-system-x86_64 -cdrom /tmp/kil0yos_pow.iso -m 512M -display none \
  -serial file:"$TDIR/serial_pow.log" \
  -monitor unix:"$TDIR/qmon_pow",server,nowait \
  -no-reboot &
QPID=$!

i=0
while [ $i -lt 90 ]; do
  grep -aq "Kil0yOS version" "$TDIR/serial_pow.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done
[ $i -lt 90 ] || { echo "BOOT_TIMEOUT"; kill $QPID; exit 1; }
sleep 6

echo "=== 1) DSDT _S5 parse line ==="
grep -a "\[power\]" "$TDIR/serial_pow.log" | head -3
grep -aq "DSDT .*_S5 parsed" "$TDIR/serial_pow.log" || { echo "S5_PARSE_MISSING"; FAIL=1; }

echo "=== 2) shutdown ==="
# drive_keys.py may exit non-zero when QEMU powers off mid-session
# (socket dies after the last key) - the real assertion is the exit below.
timeout 20 python3 tools/drive_keys.py "shutdown" "$TDIR/qmon_pow" || true

DEAD=0
for i in $(seq 1 15); do
  if ! kill -0 "$QPID" 2>/dev/null; then DEAD=1; break; fi
  sleep 1
done
if [ "$DEAD" -eq 1 ]; then
    echo "QEMU_EXITED (ACPI S5 ok)"
    wait "$QPID" 2>/dev/null
else
    echo "QEMU_STILL_ALIVE after 15s - shutdown FAILED"
    kill "$QPID" 2>/dev/null
    FAIL=1
fi

grep -aqE "EXCEPTION|PANIC" "$TDIR/serial_pow.log" && { echo "exception/panic in log"; FAIL=1; }

if [ "$FAIL" -eq 0 ]; then
    echo "POWER_SMOKE_OK"
    exit 0
else
    echo "POWER_SMOKE_FAILED"
    exit 1
fi
