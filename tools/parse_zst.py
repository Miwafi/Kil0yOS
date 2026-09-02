#!/usr/bin/env python3
"""Parse a zstd frame by hand: frame header, block headers, literals section."""
import sys

path = sys.argv[1]
d = open(path, "rb").read()
print(f"file {len(d)} bytes")

# frame header
magic = int.from_bytes(d[0:4], "little")
print(f"magic {magic:08x}")
fhd = d[4]
fcs_flag = (fhd >> 6) & 3
sds = (fhd >> 5) & 1
csz = (fhd >> 2) & 1
did = fhd & 3
print(f"FHD {fhd:02x}: fcs_flag={fcs_flag} single_seg={sds} checksum={csz} did={did}")
pos = 5
dict_id = 0
if did == 1:
    dict_id = d[pos]; pos += 1
elif did == 2:
    dict_id = int.from_bytes(d[pos:pos+2], "little"); pos += 2
elif did == 3:
    dict_id = int.from_bytes(d[pos:pos+4], "little"); pos += 4
if not sds:
    wlog = 10 + d[pos]; pos += 1
    print(f"windowLog {wlog}")
fcs_size = {0: 0, 1: 2, 2: 4, 3: 8}[fcs_flag]
if sds and fcs_flag == 0:
    fcs_size = 1
if fcs_size:
    fcs = int.from_bytes(d[pos:pos+fcs_size], "little")
    print(f"frame content size {fcs} (fcs bytes={fcs_size})")
    pos += fcs_size
print(f"frame header ends at {pos}")

# blocks
while pos < len(d):
    bh = int.from_bytes(d[pos:pos+3], "little")
    last = bh >> 24
    btype = (bh >> 30) & 3
    bsize = bh & 0x1FFFFF
    print(f"\nblock @ {pos}: last={last} type={btype} size={bsize}")
    pos += 3
    bp = pos
    if btype == 0:
        pos += bsize
        continue
    if btype == 1:
        pos += 1
        continue
    # compressed: parse literals section
    lsec = d[bp:bp+bsize]
    lt = lsec[0] & 3
    lhl = (lsec[0] >> 2) & 3
    print(f"  literals type={lt} lhl={lhl} first4={lsec[:4].hex()}")
    if lt in (0, 1):
        lh = 2 if lhl == 1 else (3 if lhl == 3 else 1)
        ls = int.from_bytes(lsec[:lh], "little") >> 4
        print(f"  raw/rle litSize={ls} lhSize={lh}")
    else:
        lhc = int.from_bytes(lsec[:4], "little")
        if lhl == 0:
            lh, single = 3, True
            ls = (lhc >> 4) & 0x3FF
            lcs = (lhc >> 14) & 0x3FF
        elif lhl == 1:
            lh, single = 3, False
            ls = (lhc >> 4) & 0x3FF
            lcs = (lhc >> 14) & 0x3FF
        elif lhl == 2:
            lh, single = 4, False
            ls = (lhc >> 4) & 0x3FFF
            lcs = lhc >> 18
        else:
            lh, single = 5, False
            ls = (lhc >> 4) & 0x3FFFF
            lcs = (lhc >> 22) + (lsec[4] << 10)
        print(f"  huff litSize={ls} litCSize={lcs} lhSize={lh} single={single} treeless={lt==3}")
        if lh + lcs > bsize:
            print(f"  !! litCSize overruns block (bsize={bsize})")
        # Huffman stats header
        hs = lsec[lh:lh+lcs]
        hstyle = hs[0] & 0xF
        print(f"  huf stats header byte {hs[0]:02x} style={hstyle}")
        if hstyle < 2:
            hsl = (hs[0] >> 4) & 3
            hsz = {0: (hs[0] >> 4), 1: int.from_bytes(hs[:2], "little") >> 4,
                   2: int.from_bytes(hs[:2], "little") >> 4,
                   3: int.from_bytes(hs[:3], "little") >> 4}[hsl]
            print(f"  direct weights: bytes={hsz+1} (hsl={hsl})")
        else:
            # FSE-compressed weights
            p = 1
            it = 0
            while True:
                b = hs[p]
                it = b if (b & 0x20) else (b & 0x1F) + 1
                p += 1
                if b & 0x20:
                    break
            rle_flag = (it == 0)
            nc = it if rle_flag else it + 1
            print(f"  fse weights: it={it} rle={rle_flag} remaining={nc}")
        pos += lh + lcs
    # sequences section
    sp = bp + (pos - bp) if btype == 2 else pos
    sp = pos
    if sp < bp + bsize:
        nb = d[sp]
        print(f"  sequences: first byte {nb:02x} (nbSeq={nb})")
    else:
        print("  no sequences section bytes left?!")
    if last:
        break
    pos += bsize
print(f"\nend pos {pos}, file len {len(d)}, checksum bytes left: {len(d)-pos}")
