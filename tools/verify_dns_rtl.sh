#!/bin/bash
# Control experiment: identical flow to verify_dns_e1000.sh but on rtl8139
# (the NIC the aliyun acceptance originally passed with). Distinguishes an
# e1000-specific TCP fault from an external network problem (mirror/host).
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_dnsr.log" "$TDIR/qmon_dnsr"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_dnsr.log" \
  -monitor unix:"$TDIR/qmon_dnsr",server,nowait \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_dnsr" || echo "TYPING FAILED: $1"
}

type_cmd "mkdir /etc/kilget";                                              sleep 3
type_cmd "echo deb http://mirrors.aliyun.com/ubuntu/ jammy main > /etc/kilget/sources.list"; sleep 3
type_cmd "kilget update";                                                  sleep 60
type_cmd "echo DNSR_ALL_DONE";                                             sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== rtl8139 control output ==="
python3 tools/dump_serial.py "$TDIR/serial_dnsr.log" 1200000 | \
  grep -aE 'kilget|dns|http|DNSR_ALL_DONE|PANIC|panic|failed' | head -40
