#!/bin/bash
# Phase 4.3 acceptance: REAL Ubuntu packages (libc6 chain) through the
# kilget pipeline. Exercises: 5.3 MB multi-window HTTP download, SHA256
# verification of a real archive, tar.gz extraction of a real payload
# (13.4 MB installed size, usrmerge /usr paths, symlinks skipped),
# topological install across a real dependency CYCLE (libc6 <-> libgcc-s1).
# Usage (WSL): ./tools/accept_p43.sh   (run tools/make_repo43.sh first)
TDIR="$HOME/ktest"
rm -f "$TDIR/serial_p43.log" "$TDIR/qmon_p43"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

python3 -u -m http.server 8001 --directory "$TDIR/p43repo" >"$TDIR/http_p43.log" 2>&1 &
HPID=$!
sleep 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_p43.log" \
  -monitor unix:"$TDIR/qmon_p43",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 20 python3 tools/drive_keys.py "$1" "$TDIR/qmon_p43" || echo "TYPING FAILED: $1"
}

type_cmd "mkdir /etc/kilget";                                            sleep 3
type_cmd "echo deb http://10.0.2.2:8001 . > /etc/kilget/sources.list";   sleep 3
type_cmd "kilget update";                                                sleep 10
# deepest first: gcc-14-base -> libc6 -> libgcc-s1 (cycle back-edge ignored)
type_cmd "kilget install libgcc-s1";                                     sleep 150
type_cmd "dpkg -l";                                                      sleep 4
type_cmd "ls /usr/lib/x86_64-linux-gnu";                                 sleep 5
type_cmd "dpkg -L libc6";                                                sleep 5
# The installed real glibc must be EXECUTABLE byte-perfect: ld-linux with
# no args prints its usage banner (proves the 13.4 MB payload survived
# download + SHA256 + ar/tar extraction + FAT cluster writes).
type_cmd "exec /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2";          sleep 6
# regression: the embedded glibc dynamic probe still runs
type_cmd "exec /bin/hello-glibc";                                        sleep 6
type_cmd "echo P43_ALL_DONE";                                            sleep 4

kill "$QPID" 2>/dev/null
kill "$HPID" 2>/dev/null
sleep 1
echo "=== host-side http.server log ==="
cat "$TDIR/http_p43.log"
echo "=== phase 4.3 acceptance output ==="
python3 tools/dump_serial.py "$TDIR/serial_p43.log" 9000000 | \
  grep -aE 'dpkg|kilget|\[deb\]|\[tar\]|libc6|libgcc|gcc-14|P43_ALL_DONE|hello|ld-linux|ld.so|PANIC|panic|failed|corrupt|mismatch|not implemented|Usage' | head -100
