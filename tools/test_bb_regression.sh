#!/bin/bash
# Phase 3.0 regression: busybox applets still work after auxv/ELF changes.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_bb.log" "$TDIR/qmon_bb"

cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_bb.log" \
  -monitor unix:"$TDIR/qmon_bb",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
timeout 20 python3 tools/drive_keys.py "uname -m" "$TDIR/qmon_bb" || echo "TYPING FAILED"
sleep 4
timeout 20 python3 tools/drive_keys.py "mkdir /tmp/regtest" "$TDIR/qmon_bb" || true
sleep 4
timeout 20 python3 tools/drive_keys.py "ls /tmp" "$TDIR/qmon_bb" || true
sleep 4
kill "$QPID" 2>/dev/null || true
sleep 1

echo "=== serial: busybox regression ==="
grep -aE 'x86_64|regtest|Kil0yOS|\\\\\\$' "$TDIR/serial_bb.log" | tail -12
