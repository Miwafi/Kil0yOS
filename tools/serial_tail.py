#!/usr/bin/env python3
"""Dump tail of a QEMU serial log with timestamps stripped."""
import re
import sys

path = sys.argv[1]
tail = int(sys.argv[2]) if len(sys.argv) > 2 else 5200
log = open(path, 'rb').read().decode(errors='replace')
clean = re.sub(r'\[\s+\d+\.\d+\]', '', log)
clean = clean.replace('\r', '')
print(clean[-tail:])
