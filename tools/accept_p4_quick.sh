#!/bin/bash
# Quick Phase 4 probe: local .deb install + deep-dir file creation.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_q.log" "$TDIR/qmon_q"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_q.log" \
  -monitor unix:"$TDIR/qmon_q",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 20 python3 tools/drive_keys.py "$1" "$TDIR/qmon_q" || echo "TYPING FAILED: $1"
}

type_cmd "tftp 10.0.2.2 hk.deb /tmp/hk.deb";  sleep 6
type_cmd "dpkg -i /tmp/hk.deb";               sleep 12
type_cmd "mkdir /a";                          sleep 2
type_cmd "mkdir /a/b";                        sleep 2
type_cmd "mkdir /a/b/c";                      sleep 2
type_cmd "echo testdata > /a/b/c/f.txt";      sleep 3
type_cmd "cat /a/b/c/f.txt";                  sleep 3
type_cmd "ls /a/b/c";                         sleep 3
type_cmd "echo Q4_DONE";                      sleep 3

kill "$QPID" 2>/dev/null
sleep 1
echo "=== quick probe output ==="
python3 tools/dump_serial.py "$TDIR/serial_q.log" 400000 | \
  grep -aE 'dpkg|\[deb\]|\[tar\]|testdata|Q4_DONE|f.txt|PANIC|panic|failed' | head -30
