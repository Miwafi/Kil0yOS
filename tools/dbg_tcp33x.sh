#!/bin/bash
# Phase 3.3 isolation test: explicit -netdev/-device only (no filter-dump).
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_33x.log" "$TDIR/qmon_33x"
mkdir -p "$TDIR/www_33"
echo "kil0yos tcp ok" > "$TDIR/www_33/nettest.txt"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

(cd "$TDIR/www_33" && python3 -m http.server 8000 >/dev/null 2>&1) &
HTTP_PID=$!
sleep 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -netdev user,id=n0 \
  -device e1000,netdev=n0 \
  -serial file:"$TDIR/serial_33x.log" \
  -monitor unix:"$TDIR/qmon_33x",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
timeout 30 python3 tools/drive_keys.py "exec /bin/nettest 8000" "$TDIR/qmon_33x"
sleep 8
kill "$QPID" 2>/dev/null
kill "$HTTP_PID" 2>/dev/null
sleep 1
echo "=== parsed ==="
python3 tools/dump_serial.py "$TDIR/serial_33x.log" 200000 | \
  grep -aE 'nettest|fail' | head -10
