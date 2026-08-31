#!/usr/bin/env python3
"""Reference values for the guest exec diagnostic print."""
d = open('build/user/hello-lnx', 'rb').read()

s128 = sum(d[:128]) & 0xFFFFFFFF
print('sum(d[0:128]) =', hex(s128))
print('d[72:80] =', ' '.join('%02x' % b for b in d[72:80]))
print('d[64:72] =', ' '.join('%02x' % b for b in d[64:72]))
# where do bytes 93 17 appear?
hits = [i for i in range(len(d) - 1) if d[i] == 0x93 and d[i + 1] == 0x17][:10]
print('occurrences of 93 17:', hits)
