#!/bin/bash
# Phase 3.x debug: run a user program, capture full serial, strip QEMU timestamps.
# Usage: dbg_glibc_ld.sh <prog-path> [grep-regex]
TDIR="$HOME/ktest"
PROG="${1:-/bin/hello-glibc}"
GREP="${2:-libc|link map|version|symbol|relocation|scope|open|ERROR|error|protect|mmap}"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_dbg.log" "$TDIR/qmon_dbg"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_dbg.log" \
  -monitor unix:"$TDIR/qmon_dbg",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
timeout 20 python3 tools/drive_keys.py "exec $PROG" "$TDIR/qmon_dbg" || echo "TYPING FAILED"
sleep 10
kill "$QPID" 2>/dev/null
sleep 1
echo "=== parsed ==="
python3 tools/parse_serial.py "$TDIR/serial_dbg.log" 2>/dev/null | grep -aE "$GREP" | head -120
