#!/bin/bash
# Phase 3.1 acceptance: glibc dynamic hello + musl dynamic hello + busybox.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_31.log" "$TDIR/qmon_31"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_31.log" \
  -monitor unix:"$TDIR/qmon_31",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
timeout 20 python3 tools/drive_keys.py "exec /bin/hello-glibc" "$TDIR/qmon_31"
sleep 6
timeout 20 python3 tools/drive_keys.py "exec /bin/hello-pthread" "$TDIR/qmon_31"
sleep 5
timeout 20 python3 tools/drive_keys.py "exec /bin/hello-dyn" "$TDIR/qmon_31"
sleep 5
timeout 20 python3 tools/drive_keys.py "echo p31_busybox_ok" "$TDIR/qmon_31"
sleep 5
kill "$QPID" 2>/dev/null
sleep 1
echo "=== parsed ==="
python3 tools/dump_serial.py "$TDIR/serial_31.log" 100000 | grep -aE 'hello|argc|pt:|p31|ENOSYS|PANIC|fail|error while loading' | head -30
