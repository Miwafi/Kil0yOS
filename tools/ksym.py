#!/usr/bin/env python3
"""Resolve kernel addresses to symbols: ksym.py kernel.bin ADDR [ADDR...]"""
import bisect, subprocess, sys

kernel = sys.argv[1]
addrs = [int(a, 16) if a.startswith('0x') else int(a, 16) for a in sys.argv[2:]]

out = subprocess.run(['nm', kernel], capture_output=True, text=True).stdout
syms = []
for line in out.splitlines():
    p = line.split()
    if len(p) == 3 and p[1] in 'tTwWdDbBrR':
        syms.append((int(p[0], 16), p[2]))
syms.sort()
saddrs = [s[0] for s in syms]

for a in addrs:
    i = bisect.bisect_right(saddrs, a) - 1
    if i < 0:
        print(f'0x{a:x} -> ?')
    else:
        sa, sn = syms[i]
        print(f'0x{a:x} -> {sn}+0x{a-sa:x} ({sn})')
