#!/bin/bash
# Timing probe: after "cd /", send a marker echo every 3s for 10 markers
# and see when the guest starts executing them again.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
LOG="$TDIR/serial_cd_probe.log"
rm -f "$LOG" "$TDIR/qmon_probe"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_probe" || echo "TYPING FAILED: $1"
}
wait_marker_in() {
    local log="$1" pat="$2" tmo="${3:-60}"
    local i=0
    while [ "$i" -lt "$tmo" ]; do
        if python3 tools/dump_serial.py "$log" 4000000 2>/dev/null | grep -aq "$pat"; then
            echo "[marker] $pat (${i}s)"; return 0
        fi
        sleep 1; i=$((i+1))
    done
    echo "TIMEOUT waiting for: $pat"; return 1
}

qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
  -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$LOG" \
  -monitor unix:"$TDIR/qmon_probe",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!
trap 'kill "$QPID" 2>/dev/null' EXIT

wait_marker_in "$LOG" "Kil0yOS version" 60 || true
sleep 3

type_cmd "desktop"
wait_marker_in "$LOG" "Launching GOP desktop" 30 || true
sleep 3

echo "=== baseline echo (before cd) ==="
type_cmd "echo pre_cd_ok"
wait_marker_in "$LOG" "pre_cd_ok" 15 || true

echo "=== cd / then markers every 3s ==="
type_cmd "cd /"
for i in 1 2 3 4 5 6 7 8 9 10; do
    type_cmd "echo probe_$i"
    sleep 3
done

echo "=== final plain echo ==="
type_cmd "echo final_ok"
wait_marker_in "$LOG" "final_ok" 15 || true

sleep 1
echo "=== markers seen ==="
python3 tools/dump_serial.py "$LOG" 4000000 2>/dev/null | grep -aE "probe_[0-9]+|pre_cd_ok|final_ok"
