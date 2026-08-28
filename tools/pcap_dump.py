import struct
import sys

data = open(sys.argv[1] if len(sys.argv) > 1 else '/tmp/net8.pcap', 'rb').read()
off = 24
n = 0
while off + 16 <= len(data):
    ts, tu, incl, orig = struct.unpack('<IIII', data[off:off+16])
    off += 16
    pkt = data[off:off+incl]
    off += incl
    n += 1
    print('frame %d: len=%d dst=%s src=%s type=%s' % (
        n, incl, pkt[0:6].hex(), pkt[6:12].hex(), pkt[12:14].hex()))
    if pkt[12:14] == b'\x08\x00':
        iplen = (pkt[14] & 0xf) * 4
        proto = pkt[23]
        if proto == 17:
            sp, dp = struct.unpack('>HH', pkt[14+iplen:18+iplen])
            ulen = struct.unpack('>H', pkt[14+iplen+4:16+iplen+4])[0]
            print('   UDP %d->%d len=%d' % (sp, dp, ulen))
            # DHCP cookie check
            payload = pkt[14+iplen+8:]
            if len(payload) >= 240:
                print('   DHCP magic=%s op=%d' % (payload[236:240].hex(), payload[0]))
