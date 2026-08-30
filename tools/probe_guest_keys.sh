#!/bin/bash
# Guest-side keyboard probe: type "x0x x" and read what the guest echoed.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial.log" "$TDIR/qmon"

cd "$(dirname "$0")/.."

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial.log" \
  -monitor unix:"$TDIR/qmon",server,nowait \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -no-reboot &

sleep 20
python3 tools/drive_keys.py "x0x x" "$TDIR/qmon"
sleep 4

# quit QEMU via the monitor (no guest typing needed)
python3 - "$TDIR/qmon" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(b'quit\n')
time.sleep(0.5)
s.close()
EOF

echo "=== serial tail (look for echo of x0x x) ==="
tail -c 1200 "$TDIR/serial.log" | tr -d '\000' | tail -25
