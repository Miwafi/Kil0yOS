#!/bin/bash
# Phase 3.3 debug: E1000 variant with SLIRP packet capture.
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_33e.log" "$TDIR/qmon_33e" "$TDIR/net_33e.pcap"
mkdir -p "$TDIR/www_33"
echo "kil0yos tcp ok" > "$TDIR/www_33/nettest.txt"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

(cd "$TDIR/www_33" && python3 -m http.server 8000 >/dev/null 2>&1) &
HTTP_PID=$!
sleep 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -netdev user,id=n0 \
  -device e1000,netdev=n0 \
  -object filter-dump,id=f0,netdev=n0,file="$TDIR/net_33e.pcap" \
  -serial file:"$TDIR/serial_33e.log" \
  -monitor unix:"$TDIR/qmon_33e",server,nowait \
  -no-reboot &
QPID=$!

sleep 16
timeout 30 python3 tools/drive_keys.py "exec /bin/nettest 8000" "$TDIR/qmon_33e"
sleep 8
kill "$QPID" 2>/dev/null
kill "$HTTP_PID" 2>/dev/null
sleep 1
echo "=== serial ==="
python3 tools/dump_serial.py "$TDIR/serial_33e.log" 200000 | \
  grep -aE 'nettest|PANIC|fail' | head -10
echo "=== pcap ==="
python3 - "$TDIR/net_33e.pcap" <<'EOF'
import sys, struct
data = open(sys.argv[1], 'rb').read()
off = 24
n = 0
t0 = None
while off + 16 <= len(data) and n < 40:
    ts, tus, incl, orig = struct.unpack('<IIII', data[off:off+16])
    off += 16
    pkt = data[off:off+incl]
    off += incl
    n += 1
    if len(pkt) < 34: continue
    et = struct.unpack('>H', pkt[12:14])[0]
    if et != 0x0800: continue
    ip = pkt[14:]
    if len(ip) < 20: continue
    proto = ip[9]
    ihl = (ip[0] & 0xF) * 4
    total = struct.unpack('>H', ip[2:4])[0]
    tcp = ip[ihl:total]
    sp, dp, seq, ack = struct.unpack('>HHII', tcp[:12])
    doff = (tcp[12] >> 4) * 4
    flags = tcp[13]
    fs = ''.join(f for b, f in ((0x02,'S'),(0x10,'A'),(0x08,'P'),(0x01,'F'),(0x04,'R')) if flags & b)
    if t0 is None: t0 = ts + tus / 1e6
    print(f"t={ts + tus/1e6 - t0:8.3f} {sp} -> {dp} seq={seq} ack={ack} flags={fs} len={len(tcp)-doff}")
EOF
