#!/usr/bin/env python3
"""Dump phdr0 bytes 64..128 of an ELF for guest/host comparison."""
import sys

path = sys.argv[1] if len(sys.argv) > 1 else 'build/user/hello-lnx'
d = open(path, 'rb').read()
print(path, 'size', len(d))
print('bytes 64..128:')
print(' '.join('%02x' % b for b in d[64:128]))
print('p_offset0', hex(int.from_bytes(d[72:80], 'little')))
print('p_vaddr0 ', hex(int.from_bytes(d[80:88], 'little')))
print('p_filesz0', hex(int.from_bytes(d[96:104], 'little')))
print('p_memsz0 ', hex(int.from_bytes(d[104:112], 'little')))
