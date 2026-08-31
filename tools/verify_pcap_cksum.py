#!/usr/bin/env python3
"""Verify TCP checksums + simulate the kernel's exact C algorithm."""
import sys, struct

def cksum_be(data):
    if len(data) & 1:
        data += b'\0'
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i+1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

def cksum_le_bytes(data):
    """kernel-style: sum bytes as LE 16-bit words."""
    if len(data) & 1:
        data += b'\0'
    s = 0
    for i in range(0, len(data), 2):
        s += data[i] | (data[i+1] << 8)
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

data = open(sys.argv[1], 'rb').read()
off = 24
shown = 0
while off + 16 <= len(data) and shown < 8:
    ts, tus, incl, orig = struct.unpack('<IIII', data[off:off+16])
    off += 16
    pkt = data[off:off+incl]
    off += incl
    shown += 1
    if len(pkt) < 34: continue
    if struct.unpack('>H', pkt[12:14])[0] != 0x0800: continue
    ip = pkt[14:]
    ihl = (ip[0] & 0xF) * 4
    total = struct.unpack('>H', ip[2:4])[0]
    proto = ip[9]
    src, dst = ip[12:16], ip[16:20]
    print(f"ip src={'.'.join(str(b) for b in src)} dst={'.'.join(str(b) for b in dst)} proto={proto} total={total}")
    if proto != 6: continue
    tcp = ip[ihl:total]
    doff = (tcp[12] >> 4) * 4
    ph = src + dst + struct.pack('>BBH', 0, 6, len(tcp))
    tcp0 = tcp[:16] + b'\0\0' + tcp[18:]
    c_be = cksum_be(ph + tcp0)
    c_le = cksum_le_bytes(ph + tcp0)
    field = struct.unpack('>H', tcp[16:18])[0]
    field_bytes_le = struct.unpack('<H', tcp[16:18])[0]
    print(f"  tcp_cksum field(bytes-wire)={field:#06x} field(LE-read)={field_bytes_le:#06x} BE-calc={c_be:#06x} LE-byte-calc={c_le:#06x}")
    print(f"  match: BE={c_be == field}  LEbyte-wireLE={c_le == field_bytes_le}")
