#!/bin/bash
# Run the E1000 TCP debug scenario N times, show serial + pcap on failure.
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1
for i in 1 2 3; do
    echo "=== RUN $i ==="
    OUT=$(./tools/dbg_tcp33e.sh 2>&1)
    echo "$OUT" | grep -aE 'nettest:|tcp\]|e1000\] tx' | head -10
    if ! echo "$OUT" | grep -q 'nettest: recv'; then
        echo "--- FAIL: pcap ---"
        echo "$OUT" | sed -n '/=== pcap ===/,$p' | head -15
        echo "--- FAIL: serial tail ---"
        python3 tools/dump_serial.py "$HOME/ktest/serial_33e.log" 120000 | tail -25
    fi
done
