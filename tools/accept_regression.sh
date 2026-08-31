#!/bin/bash
# Full Phase 3 regression: static musl, dynamic musl, glibc dynamic,
# busybox applets and DNS - all in one QEMU boot.
# Usage (WSL): ./tools/accept_regression.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_reg.log" "$TDIR/qmon_reg"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_reg.log" \
  -monitor unix:"$TDIR/qmon_reg",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
timeout 20 python3 tools/drive_keys.py "exec /bin/hello-lnx" "$TDIR/qmon_reg";  sleep 5
timeout 20 python3 tools/drive_keys.py "exec /bin/hello-dyn" "$TDIR/qmon_reg";  sleep 5
timeout 20 python3 tools/drive_keys.py "exec /bin/hello-glibc" "$TDIR/qmon_reg"; sleep 5
timeout 20 python3 tools/drive_keys.py "uname -m" "$TDIR/qmon_reg";             sleep 4
timeout 20 python3 tools/drive_keys.py "mkdir /tmp/regtest" "$TDIR/qmon_reg";   sleep 4
timeout 20 python3 tools/drive_keys.py "ls /tmp" "$TDIR/qmon_reg";              sleep 4
timeout 30 python3 tools/drive_keys.py "nslookup example.org" "$TDIR/qmon_reg"; sleep 18
timeout 20 python3 tools/drive_keys.py "echo REG_ALL_DONE" "$TDIR/qmon_reg";    sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== regression output ==="
python3 tools/dump_serial.py "$TDIR/serial_reg.log" 400000 | \
  grep -aE 'hello|Hello|x86_64|regtest|REG_ALL_DONE|example.org|Address|Name:|PANIC|ENOSYS' | head -40
