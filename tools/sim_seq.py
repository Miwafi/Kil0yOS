#!/usr/bin/env python3
"""Independent Python decode of the rle-ish first block, using the
reference zstd algorithm, to cross-check the C decoder."""

LL = [4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1,-1,-1,-1,-1]
ML = [1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1]
OF = [1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1]

LL_base = [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,22,24,28,32,40,48,64,0x80,0x100,0x200,0x400,0x800,0x1000,0x2000,0x4000,0x8000,0x10000]
LL_bits = [0]*16+[1,1,1,1,2,2,3,3,4,6,7,8,9,10,11,12,13,14,15,16]
ML_base = [3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,37,39,41,43,47,51,59,67,83,99,0x83,0x103,0x203,0x403,0x803,0x1003,0x2003,0x4003,0x8003,0x10003]
ML_bits = [0]*32+[1,1,1,1,2,2,3,3,4,4,5,7,8,9,10,11,12,13,14,15,16]
OF_base = [0,1,1,5,0xD,0x1D,0x3D,0x7D,0xFD,0x1FD,0x3FD,0x7FD,0xFFD,0x1FFD,0x3FFD,0x7FFD,0xFFFD,0x1FFFD,0x3FFFD,0x7FFFD,0xFFFFD,0x1FFFFD,0x3FFFFD,0x7FFFFD,0xFFFFFD,0x1FFFFFD,0x3FFFFFD,0x7FFFFFD,0xFFFFFFD,0x1FFFFFFD,0x3FFFFFFD,0x7FFFFFFD]
OF_bits = list(range(32))

def build(norm, tableLog, base, bits):
    tableSize = 1 << tableLog
    tab = [None] * tableSize
    high = tableSize - 1
    symnext = {}
    for s, c in enumerate(norm):
        if c == -1:
            tab[high] = s
            high -= 1
            symnext[s] = 1
        else:
            symnext[s] = c
    step = (tableSize >> 1) + (tableSize >> 3) + 3
    pos = 0
    for s, c in enumerate(norm):
        for _ in range(c):
            tab[pos] = s
            pos = (pos + step) & (tableSize - 1)
            while pos > high:
                pos = (pos + step) & (tableSize - 1)
    assert pos == 0
    entries = []
    for u in range(tableSize):
        sym = tab[u]
        ns = symnext[sym]
        symnext[sym] += 1
        nb = tableLog - ns.bit_length() + 1  # tableLog - highbit(ns)
        entries.append({'sym': sym, 'nbBits': nb,
                        'next': (ns << nb) - tableSize,
                        'base': base[sym], 'add': bits[sym]})
    return entries

def highbit32(v):
    return v.bit_length() - 1

class BR:
    def __init__(self, data):
        self.data = data
        self.bc = int.from_bytes(data, 'little') & ((1 << 64) - 1)
        last = data[-1]
        assert last != 0
        self.cons = 8 - highbit32(last) + (8 - len(data)) * 8
    def look(self, nb):
        reg = 63
        v = ((self.bc << (self.cons & reg)) & ((1 << 64) - 1))
        return (v >> 1) >> ((reg - nb) & reg)
    def skip(self, nb):
        self.cons += nb
    def read(self, nb):
        v = self.look(nb)
        self.skip(nb)
        return v
    def reload(self):
        # sub-8 stream: no container reload possible
        return

llT = build(LL, 6, LL_base, LL_bits)
ofT = build(OF, 5, OF_base, OF_bits)
mlT = build(ML, 6, ML_base, ML_bits)

bitstream = bytes.fromhex('fbff39c002')
br = BR(bitstream)
print(f"initial consumed={br.cons} (avail {8*len(bitstream)-br.cons} bits)")

stLL = br.read(6); br.reload()
stOF = br.read(5); br.reload()
stML = br.read(6); br.reload()
print(f"states ll={stLL} of={stOF} ml={stML}")
print(f"  llT[{stLL}] = {llT[stLL]}")
print(f"  ofT[{stOF}] = {ofT[stOF]}")
print(f"  mlT[{stML}] = {mlT[stML]}")

# sequence 1 (also last): no state update
ll = llT[stLL]; of = ofT[stOF]; ml = mlT[stML]
litLength = ll['base']
matchLength = ml['base']
ofBase = of['base']
ll0 = 1 if litLength == 0 else 0
if of['add'] > 1:
    offset = ofBase + br.read(of['add'])
elif of['add'] == 0:
    offset = None
    print("offset code 0 path")
else:
    offset = ofBase + ll0 + br.read(1)
    print(f"offset code {offset} rep-path")
if ml['add']: matchLength += br.read(ml['add'])
if ll['add']: litLength += br.read(ll['add'])
print(f"remaining consumed={br.cons} (should be 64 for exact end: ptr==start, consumed==64)")
print(f"seq: ll={litLength} ml={matchLength} off={offset}")
print(f"2 lit + ml = {2 + matchLength} (block 1 output), +68928 RLE = {2 + matchLength + 68928}")
