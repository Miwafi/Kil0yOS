#!/bin/bash
# Phase 3.1 debug: can userland openat big files? busybox wc -c tests it.
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_wc.log" "$TDIR/qmon_wc"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_wc.log" \
  -monitor unix:"$TDIR/qmon_wc",server,nowait \
  -no-reboot &
QPID=$!
sleep 16

for cmd in "wc -c /lib/libc.so.6" "wc -c /lib/ld-musl-x86_64.so.1" "wc -c /lib64/ld-linux-x86-64.so.2" "ls /lib" "cat /bin/mini | wc -c"; do
  timeout 20 python3 tools/drive_keys.py "$cmd" "$TDIR/qmon_wc" || echo "TYPE FAIL: $cmd"
  sleep 3
done
kill "$QPID" 2>/dev/null
sleep 1
python3 tools/serial_tail.py "$TDIR/serial_wc.log" 2600
