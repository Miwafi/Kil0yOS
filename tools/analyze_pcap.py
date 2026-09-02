#!/usr/bin/env python3
"""Summarize a QEMU filter-dump pcap: per-frame eth type, IP proto,
src/dst, TCP flags. Usage: analyze_pcap.py file.pcap [max_frames]"""
import struct
import sys

def mac(b):
    return ":".join(f"{x:02x}" for x in b)

def ip4(b):
    return ".".join(str(x) for x in b)

TCP_FLAGS = [(0x02, "SYN"), (0x10, "ACK"), (0x01, "FIN"), (0x04, "RST"),
             (0x08, "PSH"), (0x20, "URG")]

def main():
    path = sys.argv[1]
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    data = open(path, "rb").read()
    magic = data[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    elif magic == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    else:
        print("not a pcap file:", magic.hex())
        return
    off = 24
    n = 0
    while off + 16 <= len(data) and n < limit:
        ts_sec, ts_usec, incl_len, _orig = struct.unpack(endian + "IIII", data[off:off + 16])
        off += 16
        frame = data[off:off + incl_len]
        off += incl_len
        n += 1
        if len(frame) < 14:
            print(f"#{n} short frame {len(frame)}B")
            continue
        ethertype = struct.unpack(">H", frame[12:14])[0]
        line = f"#{n} {mac(frame[6:12])} -> {mac(frame[0:6])} type=0x{ethertype:04x}"
        if ethertype == 0x0800 and len(frame) >= 34:
            ihl = (frame[14] & 0x0F) * 4
            proto = frame[23]
            src = ip4(frame[26:30]); dst = ip4(frame[30:34])
            line += f" ip {src}->{dst} proto={proto}"
            if proto == 6 and len(frame) >= 14 + ihl + 14:
                t = frame[14 + ihl:]
                sport, dport = struct.unpack(">HH", t[:4])
                flags = t[13]
                fl = "".join(name for bit, name in TCP_FLAGS if flags & bit)
                seq = struct.unpack(">I", t[4:8])[0]
                line += f" tcp {sport}->{dport} flags={fl or hex(flags)} seq={seq}"
            elif proto == 17 and len(frame) >= 14 + ihl + 8:
                t = frame[14 + ihl:]
                sport, dport = struct.unpack(">HH", t[:4])
                line += f" udp {sport}->{dport}"
        print(line)

if __name__ == "__main__":
    main()
