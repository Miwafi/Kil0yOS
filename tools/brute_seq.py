#!/usr/bin/env python3
"""Brute-force the sequence bitstream layout for the rle-ish first block.

Data bits (33): 011000000001110011111111111111011
Truth: single seq, block output = 131072 = 2 lit + match, so
ml = 131070 (ML sym 52: base 65539, add 16 -> 65539+65531).
Try every init-state order permutation and extra-bits order; require
exact consumption of all 33 bits.
"""
from itertools import permutations

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
            tab[high] = s; high -= 1; symnext[s] = 1
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
    out = []
    for u in range(tableSize):
        sym = tab[u]; ns = symnext[sym]; symnext[sym] += 1
        nb = tableLog - (ns.bit_length() - 1)
        out.append({'sym': sym, 'base': base[sym], 'add': bits[sym], 'nb': nb})
    return out

llT = build(LL, 6, LL_base, LL_bits)
ofT = build(OF, 5, OF_base, OF_bits)
mlT = build(ML, 6, ML_base, ML_bits)

bits_str = "0" + format(0xc0, '08b') + format(0x39, '08b') + format(0xff, '08b') + format(0xfb, '08b')
# ^ bit32 then bytes 3,2,1,0 MSB-first
assert len(bits_str) == 33, len(bits_str)

tables = {'LL': llT, 'OF': ofT, 'ML': mlT}
sizes = {'LL': 6, 'OF': 5, 'ML': 6}

for order in permutations(['LL', 'OF', 'ML']):
    for exorder in permutations(['off', 'ml', 'll']):
        p = 0
        states = {}
        ok = True
        for t in order:
            v = int(bits_str[p:p+sizes[t]], 2); p += sizes[t]
            states[t] = v
            if v >= len(tables[t]):
                ok = False; break
        if not ok: continue
        ll = tables['LL'][states['LL']]
        of = tables['OF'][states['OF']]
        ml = tables['ML'][states['ML']]
        vals = {}
        for w in exorder:
            if w == 'off':
                if of['add'] > 0:
                    vals['off'] = of['base'] + int(bits_str[p:p+of['add']] or '0', 2); p += of['add']
                else:
                    vals['off'] = 'rep'
            elif w == 'ml':
                vals['ml'] = ml['base'] + (int(bits_str[p:p+ml['add']], 2) if ml['add'] else 0); p += ml['add']
            else:
                vals['ll'] = ll['base'] + (int(bits_str[p:p+ll['add']], 2) if ll['add'] else 0); p += ll['add']
        if p == 33 and vals.get('ml') == 131070:
            print(f"MATCH init={order} extra={exorder} states={states}")
            print(f"  ll={vals.get('ll')} ml={vals['ml']} off={vals.get('off')}")
        if p == 33:
            print(f"  (exact) init={order} extra={exorder} states={states} vals={vals}")
