#!/usr/bin/env python3
"""Strip interleaved serial timestamps from a QEMU serial log and show
context around each [heap] CORRUPT event."""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/serial_ext2.log'
max_events = int(sys.argv[2]) if len(sys.argv) > 2 else 4

log = open(path, 'rb').read().decode(errors='replace')
# serial console echoes "[    0.064667]" style stamps into the stream
clean = re.sub(r'\[\s+\d+\.\d+\]', '', log)
clean = clean.replace('\r', '')

events = list(re.finditer(r'CORRUPT|NON-ADJACENT', clean))
for i, m in enumerate(events[:max_events]):
    start = max(0, m.start() - 500)
    print('===== event %d =====' % i)
    print(clean[start:m.end() + 200])
    print()
