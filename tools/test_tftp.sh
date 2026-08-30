#!/bin/bash
# Phase 1.4 acceptance: boot Kil0yOS in QEMU, pull the busybox binary from
# QEMU's built-in TFTP server (10.0.2.2) via the shell `tftp` command, then
# shut down. Verification: klog lines on serial.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
cp "$HOME/busybox-1.36.1/busybox" "$TDIR/busybox"
ls -l "$TDIR/busybox"

rm -f "$TDIR/serial.log" "$TDIR/qemu.log" "$TDIR/qmon"

cd "$(dirname "$0")/.."

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial.log" \
  -monitor unix:"$TDIR/qmon",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -object filter-dump,id=f0,netdev=net0,file="$TDIR/net.pcap" \
  -d int,cpu_reset -D "$TDIR/qemu.log" \
  -no-reboot &
QPID=$!
echo "qemu pid=$QPID"

sleep 20; echo "step: typing tftp command"
timeout 20 python3 tools/drive_keys.py "tftp busybox" "$TDIR/qmon" || echo "TYPING FAILED"
echo "step: waiting for download/install (up to 180s)"
installed=""
for i in $(seq 1 180); do
  # 'installed' also appears in boot logs ([user] /bin/busybox installed):
  # match the shell TFTP success marker only.
  if grep -aq 'tftp: installed' "$TDIR/serial.log" 2>/dev/null; then
    installed=yes
    break
  fi
  # abort early if the guest reset (triple fault) or exited
  if grep -aq 'Triple' "$TDIR/qemu.log" 2>/dev/null; then
    echo "step: guest triple-faulted, aborting wait"
    break
  fi
  kill -0 "$QPID" 2>/dev/null || { echo "step: qemu exited"; break; }
  sleep 1
done
[ -n "$installed" ] && echo "step: install detected" || echo "step: install NOT detected, shutting down anyway"

if [ -n "$installed" ]; then
  echo "step: running busybox applet checks"
  sleep 3
  # Marker must be lowercase: QEMU sendkey only accepts lowercase key
  # names, so uppercase letters are silently dropped by drive_keys.py
  timeout 20 python3 tools/drive_keys.py "/bin/busybox echo tftpok42" "$TDIR/qmon" || echo "TYPING FAILED"
  sleep 4
  timeout 20 python3 tools/drive_keys.py "/bin/busybox mkdir /bin/tftpdir" "$TDIR/qmon" || echo "TYPING FAILED"
  sleep 4
  timeout 20 python3 tools/drive_keys.py "/bin/busybox ls /bin" "$TDIR/qmon" || echo "TYPING FAILED"
  sleep 4
fi

echo "step: typing shutdown"
timeout 15 python3 tools/drive_keys.py "shutdown" "$TDIR/qmon" || echo "SHUTDOWN TYPING FAILED"
sleep 6
kill "$QPID" 2>/dev/null || true
sleep 1

echo "=== serial: tftp/net/installed lines ==="
grep -aE 'tftp|dhcp|installed|applet|fetching' "$TDIR/serial.log" | head -40
echo "=== serial tail ==="
tail -c 2000 "$TDIR/serial.log" | tr -d '\000'
