#!/bin/bash
# Quick check: just exec /bin/hello-lnx and dump kernel diagnostics.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_hl.log" "$TDIR/qmon_hl"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_hl.log" \
  -monitor unix:"$TDIR/qmon_hl",server,nowait \
  -no-reboot &
QPID=$!
sleep 16
timeout 20 python3 tools/drive_keys.py "exec /bin/hello-lnx" "$TDIR/qmon_hl"
sleep 6
kill "$QPID" 2>/dev/null
sleep 1
python3 tools/dump_serial.py "$TDIR/serial_hl.log" 400000 | grep -aA3 'exec /bin/hello-lnx'
