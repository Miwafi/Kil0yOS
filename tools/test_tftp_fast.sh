#!/bin/bash
# Fast probe: boot, type tftp busybox, capture ~30s, dump diagnostics.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
cp "$HOME/busybox-1.36.1/busybox" "$TDIR/busybox"

rm -f "$TDIR/serial.log" "$TDIR/qemu.log" "$TDIR/qmon" "$TDIR/net.pcap"

cd "$(dirname "$0")/.."

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial.log" \
  -monitor unix:"$TDIR/qmon",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -object filter-dump,id=f0,netdev=net0,file="$TDIR/net.pcap" \
  -no-reboot &
QPID=$!

sleep 16; echo "step: typing tftp command"
timeout 20 python3 tools/drive_keys.py "tftp busybox" "$TDIR/qmon" || echo "TYPING FAILED"
sleep 25
kill "$QPID" 2>/dev/null || true
sleep 1

echo "=== serial tftp/rtl lines ==="
grep -aE 'tftp|rtl|cbr|CBR' "$TDIR/serial.log" | tail -30
