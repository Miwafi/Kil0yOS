#!/bin/bash
# Repro for: desktop shell hangs on location commands (cd ...).
# Flow: UEFI boot -> desktop -> "cd /" -> "echo after_cd_ok".
# after_cd_ok must appear on the serial mirror; TIMEOUT means hang.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
LOG="$TDIR/serial_cd_repro.log"
rm -f "$LOG" "$TDIR/qmon_cd"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_cd" || echo "TYPING FAILED: $1"
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
  -monitor unix:"$TDIR/qmon_cd",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!
trap 'kill "$QPID" 2>/dev/null' EXIT

wait_marker_in "$LOG" "Kil0yOS version" 60 || true
sleep 3

type_cmd "desktop"
wait_marker_in "$LOG" "Launching GOP desktop" 30 || true
sleep 3

echo "--- cd variants ---"
type_cmd "cd /"
if wait_marker_in "$LOG" "after_cd_ok" 20; then
    echo "CD_ROOT_OK (no hang)"
else
    echo "CD_ROOT_HANG"
fi

type_cmd "cd nosuchdir"
sleep 2
type_cmd "echo after_cd_bad_ok"
if wait_marker_in "$LOG" "after_cd_bad_ok" 20; then
    echo "CD_BADPATH_OK (no hang)"
else
    echo "CD_BADPATH_HANG"
fi

type_cmd "cd"
sleep 2
type_cmd "echo after_cd_bare_ok"
if wait_marker_in "$LOG" "after_cd_bare_ok" 20; then
    echo "CD_BARE_OK (no hang)"
else
    echo "CD_BARE_HANG"
fi

sleep 1
echo "=== serial tail ==="
python3 tools/dump_serial.py "$LOG" 4000000 2>/dev/null | tail -30
