#!/bin/bash
# Memory-subsystem smoke (v3.9.0 M1-M3):
#   1) boot, capture the boot-time "[pmm]" klog line (O(1) stats path)
#   2) busybox exec twice (uname -m / echo) - the invlpg TLB path in
#      vmm_map_page maps thousands of pages per exec
#   3) serial log must be free of EXCEPTION/PANIC and [heap] corruption
# Usage (WSL): bash tools/smoke_mem.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_mem.log" "$TDIR/qmon_mem"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1
cp build/kil0yos.iso /tmp/kil0yos_mem.iso || { echo "ISO_COPY_FAILED"; exit 1; }

FAIL=0
qemu-system-x86_64 -cdrom /tmp/kil0yos_mem.iso -m 512M -display none \
  -serial file:"$TDIR/serial_mem.log" \
  -monitor unix:"$TDIR/qmon_mem",server,nowait \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

i=0
while [ $i -lt 90 ]; do
  grep -aq "Kil0yOS version" "$TDIR/serial_mem.log" 2>/dev/null && break
  sleep 1; i=$((i+1))
done
[ $i -lt 90 ] || { echo "BOOT_TIMEOUT"; kill $QPID; exit 1; }
sleep 8

echo "=== 1) boot-time [pmm] line ==="
grep -a "\[pmm\]" "$TDIR/serial_mem.log" | head -3

echo "=== 2) exec #1 (uname -m) ==="
timeout 20 python3 tools/drive_keys.py "uname -m" "$TDIR/qmon_mem" \
  || { echo "TYPING_FAILED"; FAIL=1; }
sleep 6

echo "=== 3) exec #2 (echo) ==="
timeout 20 python3 tools/drive_keys.py "echo exec2ok" "$TDIR/qmon_mem" \
  || { echo "TYPING_FAILED"; FAIL=1; }
sleep 6

kill "$QPID" 2>/dev/null
sleep 1

echo "=== serial extract ==="
grep -aE "x86_64|exec2ok|EXCEPTION|PANIC|\[heap\]|#PF" "$TDIR/serial_mem.log" | tail -10

grep -aqE "EXCEPTION|PANIC" "$TDIR/serial_mem.log" && { echo "exception/panic in log"; FAIL=1; }
grep -aq "\[heap\].*CORRUPT" "$TDIR/serial_mem.log" && { echo "heap corruption in log"; FAIL=1; }
grep -aq "x86_64" "$TDIR/serial_mem.log" || { echo "exec regression FAILED"; FAIL=1; }
grep -aq "exec2ok" "$TDIR/serial_mem.log" || { echo "second exec FAILED"; FAIL=1; }

if [ "$FAIL" -eq 0 ]; then
    echo "MEM_SMOKE_OK"
    exit 0
else
    echo "MEM_SMOKE_FAILED"
    exit 1
fi
