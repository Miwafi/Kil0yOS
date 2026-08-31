#!/bin/bash
# Real-world mirror acceptance: sources.list in full Debian format against
# the Aliyun Ubuntu mirror over QEMU slirp (DNS via 10.0.2.3 proxy, outbound
# TCP through the host). Validates: hostname DNS resolution + dists-layout
# index fetch (dists/jammy/main/binary-amd64/Packages.gz, ~1.6 MB over the
# real internet) + kernel-side gunzip + index merge. Package INSTALL from a
# real mirror still requires data.tar.{xz,zst} support (kernel inflates
# gzip only), so this test stops at 'kilget update' + 'kilget show'.
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_aliyun.log" "$TDIR/qmon_aliyun"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_aliyun.log" \
  -monitor unix:"$TDIR/qmon_aliyun",server,nowait \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_aliyun" || echo "TYPING FAILED: $1"
}

type_cmd "mkdir /etc/kilget";                                              sleep 3
type_cmd "echo deb http://mirrors.aliyun.com/ubuntu/ jammy main > /etc/kilget/sources.list"; sleep 3
type_cmd "kilget update";                                                  sleep 90
type_cmd "kilget show libc6";                                              sleep 6
type_cmd "echo ALIYUN_ALL_DONE";                                           sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== real-mirror acceptance output ==="
python3 tools/dump_serial.py "$TDIR/serial_aliyun.log" 1200000 | \
  grep -aE 'kilget|dns|http|libc6|ALIYUN_ALL_DONE|PANIC|panic|failed' | head -40
