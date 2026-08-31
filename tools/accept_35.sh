#!/bin/bash
# Phase 3.5 acceptance: busybox wget fetches an HTTP file from the host.
# Exercises: /etc/hosts (musl getaddrinfo) -> TCP connect (Phase 3.3 stack)
#            -> HTTP GET -> body to stdout.
# Usage (WSL): ./tools/accept_35.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_35.log" "$TDIR/qmon_35" "$TDIR/http_35"
mkdir -p "$TDIR/www_35"
echo "kil0yos wget ok p35_marker" > "$TDIR/www_35/p35.txt"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

# HTTP server on the QEMU user-net host side (10.0.2.2, guest alias: kil0yos)
(cd "$TDIR/www_35" && python3 -m http.server 8000 >"$TDIR/http_35.log" 2>&1) &
HTTP_PID=$!
sleep 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_35.log" \
  -monitor unix:"$TDIR/qmon_35",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
# TCP sanity check with the Phase 3.3 probe against the same server
timeout 30 python3 tools/drive_keys.py "exec /bin/nettest 8000" "$TDIR/qmon_35"
sleep 10
timeout 30 python3 tools/drive_keys.py "wget -O - http://kil0yos:8000/p35.txt" "$TDIR/qmon_35"
sleep 15
timeout 20 python3 tools/drive_keys.py "echo p35_wget_ok" "$TDIR/qmon_35"
sleep 4

kill "$QPID" 2>/dev/null
kill "$HTTP_PID" 2>/dev/null || true
sleep 1
echo "=== parsed ==="
echo "=== http server log ==="
cat "$TDIR/http_35.log"
echo "=== guest serial ==="
python3 tools/dump_serial.py "$TDIR/serial_35.log" 300000 | \
  grep -aE 'nettest|wget|p35|p35_marker|HTTP|fail|PANIC|ENOSYS|error|reject' | head -40
