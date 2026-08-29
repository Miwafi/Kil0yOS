#!/usr/bin/env python3
"""Dump a pcap produced by QEMU filter-dump: one line per packet."""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/ping_dump.pcap'
with open(path, 'rb') as f:
    d = f.read()
off = 24
base = None
while off + 16 <= len(d):
    ts, tus, caplen, ol = struct.unpack('<IIII', d[off:off + 16])
    off += 16
    pkt = d[off:off + caplen]
    off += caplen
    t = ts + tus / 1e6
    if base is None:
        base = t
    info = 'len=%d' % caplen
    if len(pkt) >= 14:
        et = struct.unpack('>H', pkt[12:14])[0]
        if et == 0x0806 and len(pkt) >= 42:
            op = struct.unpack('>H', pkt[20:22])[0]
            spa = '.'.join(str(b) for b in pkt[28:32])
            tpa = '.'.join(str(b) for b in pkt[38:42])
            info += ' ARP %s %s->%s' % ('req' if op == 1 else 'rep', spa, tpa)
        elif et == 0x0800 and len(pkt) >= 34:
            ip = pkt[14:]
            ihl = (ip[0] & 0xF) * 4
            proto = ip[9]
            src = '.'.join(str(b) for b in ip[12:16])
            dst = '.'.join(str(b) for b in ip[16:20])
            if proto == 1 and len(ip) >= ihl + 8:
                it = ip[ihl]
                info += ' ICMP %s %s->%s' % ('req' if it == 8 else 'rep', src, dst)
            elif proto == 17:
                info += ' UDP %s->%s' % (src, dst)
            else:
                info += ' IP proto=%d %s->%s' % (proto, src, dst)
    print('%9.3f %s' % (t - base, info))
