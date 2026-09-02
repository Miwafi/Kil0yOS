#!/bin/bash
# Verify the baked-in /etc/kilget/sources.list (default: Aliyun jammy):
# fresh boot, NO manual configuration, straight to 'kilget update'.
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_src.log" "$TDIR/qmon_src"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_src.log" \
  -monitor unix:"$TDIR/qmon_src",server,nowait \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_src" || echo "TYPING FAILED: $1"
}

type_cmd "cat /etc/kilget/sources.list";                                   sleep 4
type_cmd "kilget update";                                                  sleep 75
type_cmd "echo SRC_ALL_DONE";                                              sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== baked-in sources.list verification ==="
python3 tools/dump_serial.py "$TDIR/serial_src.log" 1200000 | \
  grep -aE 'kilget|sources.list|deb http|SRC_ALL_DONE|PANIC|panic|failed' | head -30
