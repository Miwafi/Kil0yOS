#!/bin/bash
# Verify the full-size index: fresh boot (baked-in Aliyun sources.list),
# 'kilget update' must pull ALL of jammy main (~53k paragraphs, was capped
# at 4096), then 'apt-get install libx11-dev' must resolve the dependency
# chain (libx11-6 -> libxcb1 ...) that previously failed with
# "depends on 'libxcb1' which is not in the index".
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_x11.log" "$TDIR/qmon_x11"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_x11.log" \
  -monitor unix:"$TDIR/qmon_x11",server,nowait \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_x11" || echo "TYPING FAILED: $1"
}

type_cmd "kilget update";                                                  sleep 150
type_cmd "kilget show libxcb1";                                            sleep 6
type_cmd "apt-get install libx11-dev";                                     sleep 100
type_cmd "echo X11_ALL_DONE";                                              sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== full-index / libx11-dev verification ==="
python3 tools/dump_serial.py "$TDIR/serial_x11.log" 1600000 | \
  grep -aE 'kilget|apt-get|libx11|libxcb|x11proto|X11_ALL_DONE|PANIC|panic|failed|not in the index|unable' | head -50
