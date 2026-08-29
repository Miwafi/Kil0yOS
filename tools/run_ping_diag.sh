#!/usr/bin/env bash
# Run ping regression and dump diagnostics in one shot (WSL /tmp is volatile).
set -u
cd /mnt/c/Users/19423/Desktop/Programs/Kil0yOS

echo "=== regression ==="
timeout 70 python3 tools/ping_qemu_test.py 2>&1 | grep -E 'gateway|public|RESULT|FAIL'

echo "=== boot + ping trace (serial) ==="
grep -aE 'ping|DHCP ok|net:' /tmp/serial_ping.log | head -14

echo "=== pcap ==="
python3 tools/pcap_dump.py /tmp/ping_dump.pcap 2>/dev/null | tail -12

echo "=== serial tail ==="
tail -c 1200 /tmp/serial_ping.log
