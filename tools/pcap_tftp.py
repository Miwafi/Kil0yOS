#!/usr/bin/env python3
"""Dump UDP packets (src ip:port > dst ip:port, len, tftp opcode/block) from a pcap."""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else None
show = int(sys.argv[2]) if len(sys.argv) > 2 else 40
tail = int(sys.argv[3]) if len(sys.argv) > 3 else 0

data = open(path, 'rb').read()
magic = data[:4]
if magic == b'\xd4\xc3\xb2\xa1':
    endian = '<'
elif magic == b'\xa1\xb2\xc3\xd4':
    endian = '>'
else:
    print('not a pcap'); sys.exit(1)

off = 24
rows = []
n = 0
while off + 16 <= len(data):
    ts_sec, ts_usec, incl, orig = struct.unpack(endian + 'IIII', data[off:off+16])
    off += 16
    pkt = data[off:off+incl]
    off += incl
    n += 1
    if len(pkt) < 14: continue
    eth_type = struct.unpack('>H', pkt[12:14])[0]
    if eth_type != 0x0800: continue
    ip = pkt[14:]
    if len(ip) < 20: continue
    ihl = (ip[0] & 0xF) * 4
    proto = ip[9]
    if proto != 17: continue
    src = '.'.join(str(b) for b in ip[12:16])
    dst = '.'.join(str(b) for b in ip[16:20])
    udp = ip[ihl:]
    if len(udp) < 8: continue
    sp, dp, ulen, cks = struct.unpack('>HHHH', udp[:8])
    payload = udp[8:]
    info = ''
    if len(payload) >= 4:
        op, blk = struct.unpack('>HH', payload[:4])
        info = f'op={op} blk={blk} plen={len(payload)}'
        if op == 1:
            info += ' rrq=' + repr(payload[2:20])
    rows.append((ts_sec + ts_usec/1e6, f'{src}:{sp} > {dst}:{dp} len={ulen} {info}'))

print(f'total packets: {n}, udp shown: {len(rows)}')
sel = rows[-tail:] if tail else rows[:show]
for t, line in sel:
    print(f'{t%1000:12.6f} {line}')
