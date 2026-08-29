#!/usr/bin/env bash
# Diagnose why the second ping command never runs: boot, ping gw, screenshot,
# ping public, screenshot, dump everything.
set -u
cd /mnt/c/Users/19423/Desktop/Programs/Kil0yOS
rm -f /tmp/qmon_diag.sock /tmp/diag1.ppm /tmp/diag2.ppm /tmp/diag3.ppm

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512 -display none \
  -monitor unix:/tmp/qmon_diag.sock,server,nowait -no-reboot \
  -netdev user,id=n1 -device e1000,netdev=n1 \
  -object filter-dump,id=f,netdev=n1,file=/tmp/diag.pcap \
  -serial file:/tmp/serial_diag.log \
  >/tmp/qemu_diag.log 2>&1 &
QPID=$!
sleep 16

exec 3<>/tmp/qmon_diag.sock
mon() { echo "$1" >&3; sleep 0.15; }

for k in p i n g spc 1 0 dot 0 dot 2 dot 2 ret; do mon "sendkey $k"; done
sleep 8
mon "screendump /tmp/diag1.ppm"
sleep 1

for k in p i n g spc 1 1 4 dot 1 1 4 dot 1 1 4 dot 1 1 4 ret; do mon "sendkey $k"; done
sleep 12
mon "screendump /tmp/diag2.ppm"
sleep 1

mon quit
sleep 1
kill $QPID 2>/dev/null

echo "=== pcap ==="
python3 tools/pcap_dump.py /tmp/diag.pcap 2>/dev/null | tail -14
echo "=== serial ping trace ==="
grep -a 'ping' /tmp/serial_diag.log | head -12
