#!/bin/bash
# Phase 3.3 acceptance: TCP 3-way handshake + data + close against a
# Linux host HTTP server (QEMU user-net: guest -> 10.0.2.2).
# Usage (WSL): ./tools/accept_33.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_33.log" "$TDIR/qmon_33" "$TDIR/http_33"
mkdir -p "$TDIR/www_33"
echo "kil0yos tcp ok" > "$TDIR/www_33/nettest.txt"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

# HTTP server on the host side of QEMU user-net (10.0.2.2)
(cd "$TDIR/www_33" && python3 -m http.server 8000 >/dev/null 2>&1) &
HTTP_PID=$!
sleep 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_33.log" \
  -monitor unix:"$TDIR/qmon_33",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
timeout 30 python3 tools/drive_keys.py "exec /bin/nettest 8000" "$TDIR/qmon_33"
sleep 8
timeout 20 python3 tools/drive_keys.py "echo p33_tcp_ok" "$TDIR/qmon_33"
sleep 4

kill "$QPID" 2>/dev/null
kill "$HTTP_PID" 2>/dev/null
sleep 1
echo "=== parsed ==="
python3 tools/dump_serial.py "$TDIR/serial_33.log" 200000 | \
  grep -aE 'nettest|p33|HTTP|kil0yos tcp ok|PANIC|fail' | head -30
