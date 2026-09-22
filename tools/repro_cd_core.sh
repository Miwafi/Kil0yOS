#!/bin/bash
# Core repro, N runs: boot -> desktop -> "cd /" -> "echo marker_<i>".
# Reports how often the marker is lost.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

RUNS=${1:-3}
FAILS=0
for run in $(seq 1 "$RUNS"); do
    LOG="$TDIR/serial_cd_core_$run.log"
    rm -f "$LOG" "$TDIR/qmon_core"
    qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
      -cdrom build/kil0yos.iso -m 512M -display none \
      -serial file:"$LOG" \
      -monitor unix:"$TDIR/qmon_core",server,nowait \
      -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
      -no-reboot &
    QPID=$!
    wait_marker() {
        local pat="$1" tmo="${2:-30}"
        local i=0
        while [ "$i" -lt "$tmo" ]; do
            if python3 tools/dump_serial.py "$LOG" 4000000 2>/dev/null | grep -aq "$pat"; then
                return 0
            fi
            sleep 1; i=$((i+1))
        done
        return 1
    }
    wait_marker "Kil0yOS version" 60
    sleep 3
    timeout 30 python3 tools/drive_keys.py "desktop" "$TDIR/qmon_core" >/dev/null 2>&1
    wait_marker "Launching GOP desktop" 30
    sleep 3
    timeout 30 python3 tools/drive_keys.py "cd /" "$TDIR/qmon_core" >/dev/null 2>&1
    sleep 1
    timeout 30 python3 tools/drive_keys.py "echo marker_$run" "$TDIR/qmon_core" >/dev/null 2>&1
    sleep 2
    timeout 30 python3 tools/drive_keys.py "echo tail_$run" "$TDIR/qmon_core" >/dev/null 2>&1
    sleep 3
    if wait_marker "marker_$run" 10; then
        echo "run $run: MARKER_OK"
    else
        echo "run $run: MARKER_LOST"
        FAILS=$((FAILS+1))
    fi
    kill "$QPID" 2>/dev/null
    sleep 1
done
echo "summary: $FAILS/$RUNS lost"
