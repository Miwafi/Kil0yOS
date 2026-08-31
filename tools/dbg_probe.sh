#!/bin/bash
# Phase 3.1: run probe-ld to find which libc-load syscall fails for ld.so.
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_probe.log" "$TDIR/qmon_probe"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_probe.log" \
  -monitor unix:"$TDIR/qmon_probe",server,nowait \
  -no-reboot &
QPID=$!
sleep 16
timeout 20 python3 tools/drive_keys.py "exec /bin/probe-ld" "$TDIR/qmon_probe" || echo "TYPE FAIL"
sleep 8
kill "$QPID" 2>/dev/null
sleep 1
python3 tools/serial_tail.py "$TDIR/serial_probe.log" 1600
