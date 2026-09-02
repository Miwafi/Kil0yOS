#!/bin/bash
# Quick DNS verification on the e1000 NIC (the user's failing scenario):
# boots with -device e1000, runs kilget update against the Aliyun mirror,
# and greps for the [dns] outcome. Acceptance passes when "resolve failed"
# is absent and the index fetch starts/completes.
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_dnse.log" "$TDIR/qmon_dnse"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_dnse.log" \
  -monitor unix:"$TDIR/qmon_dnse",server,nowait \
  -netdev user,id=net0 -device e1000,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_dnse" || echo "TYPING FAILED: $1"
}

type_cmd "mkdir /etc/kilget";                                              sleep 3
type_cmd "echo deb http://mirrors.aliyun.com/ubuntu/ jammy main > /etc/kilget/sources.list"; sleep 3
type_cmd "kilget update";                                                  sleep 60
type_cmd "echo DNSE_ALL_DONE";                                             sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== e1000 dns verification output ==="
python3 tools/dump_serial.py "$TDIR/serial_dnse.log" 1200000 | \
  grep -aE 'kilget|dns|http|DNSE_ALL_DONE|PANIC|panic|failed' | head -40
