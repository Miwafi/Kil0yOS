#!/bin/bash
# Phase 3.4 acceptance: busybox nslookup via UDP DNS (QEMU SLIRP 10.0.2.3).
# Requires: /etc/resolv.conf in guest, sc_read on socket fds, poll.
# Usage (WSL): ./tools/accept_34.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_34.log" "$TDIR/qmon_34"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_34.log" \
  -monitor unix:"$TDIR/qmon_34",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
# nslookup retries twice with 5 s intervals; allow enough time
timeout 20 python3 tools/drive_keys.py "nslookup example.org" "$TDIR/qmon_34"
sleep 20
timeout 20 python3 tools/drive_keys.py "echo p34_dns_ok" "$TDIR/qmon_34"
sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== parsed ==="
python3 tools/dump_serial.py "$TDIR/serial_34.log" 300000 | \
  grep -aE 'nslookup|p34|Address|Name:|Server|fail|PANIC|ENOSYS' | head -30
