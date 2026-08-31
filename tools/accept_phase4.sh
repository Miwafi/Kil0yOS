#!/bin/bash
# Phase 4 end-to-end acceptance: dpkg -i (local .deb via TFTP),
# dpkg -l/-L/-r, and the kilget repo client (apt-get equivalent:
# update -> install with dependency-ordered download over HTTP).
# Usage (WSL): ./tools/accept_phase4.sh     (run tools/make_test_repo.sh first)
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_p4.log" "$TDIR/qmon_p4"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

# host HTTP server for the guest repo (slirp: 10.0.2.2:8000 -> host:8000)
python3 -m http.server 8000 --directory "$TDIR/p4repo" >/dev/null 2>&1 &
HPID=$!
sleep 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_p4.log" \
  -monitor unix:"$TDIR/qmon_p4",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 20 python3 tools/drive_keys.py "$1" "$TDIR/qmon_p4" || echo "TYPING FAILED: $1"
}

type_cmd "mkdir /etc/kilget";                                            sleep 3
type_cmd "echo deb http://10.0.2.2:8000 . > /etc/kilget/sources.list";   sleep 3
# --- 4.2: local .deb install (TFTP fetch first) ---
type_cmd "tftp 10.0.2.2 hk.deb /tmp/hk.deb";                             sleep 6
type_cmd "dpkg -i /tmp/hk.deb";                                          sleep 10
type_cmd "dpkg -l";                                                      sleep 3
type_cmd "dpkg -L hello-kit";                                            sleep 3
type_cmd "exec /bin/hello-kit";                                          sleep 5
type_cmd "dpkg -r hello-kit";                                            sleep 5
type_cmd "exec /bin/hello-kit";                                          sleep 4
# --- 4.4/4.5: repo client (dep-ordered install: hello-kit then -data) ---
type_cmd "kilget update";                                                sleep 10
type_cmd "kilget install hello-kit-data";                                sleep 30
type_cmd "exec /bin/hello-kit";                                          sleep 5
type_cmd "dpkg -l";                                                      sleep 3
type_cmd "echo P4_ALL_DONE";                                             sleep 4

kill "$QPID" 2>/dev/null
kill "$HPID" 2>/dev/null
sleep 1
echo "=== phase 4 acceptance output ==="
python3 tools/dump_serial.py "$TDIR/serial_p4.log" 600000 | \
  grep -aE 'dpkg|kilget|\[deb\]|\[tar\]|hello|Hello|P4_ALL_DONE|hk\.deb|PANIC|panic|failed|not implemented' | head -60
