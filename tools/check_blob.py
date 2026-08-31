#!/usr/bin/env python3
"""Verify the embedded user-program blobs inside build/kernel.bin."""
import sys

data = open('build/kernel.bin', 'rb').read()
print('kernel.bin size:', len(data))
for name in ['hello-lnx', 'hello-dyn', 'hello-glibc', 'nettest']:
    try:
        ref = open('build/user/' + name, 'rb').read()
    except FileNotFoundError:
        print(name, ': no build file')
        continue
    i = data.find(ref[:256])
    if i < 0:
        print(name, ': HEAD NOT FOUND in kernel.bin, size', len(ref))
        continue
    full = data[i:i + len(ref)] == ref
    print(name, ': at', i, 'size', len(ref), 'full match:', full)
