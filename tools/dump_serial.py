#!/usr/bin/env python3
"""Dump a QEMU serial log with interleaved [  12.345678] timestamps stripped."""
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/home/aiden/ktest/serial_dbg.log"
tail = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
start_marker = sys.argv[3] if len(sys.argv) > 3 else None
data = open(path, "rb").read().decode("utf-8", "replace")
clean = re.sub(r"\[\s*[0-9]+\.[0-9]+\]", "", data)
if start_marker:
    i = clean.find(start_marker)
    print(clean[i:i + tail] if i >= 0 else f"marker not found: {start_marker}")
else:
    print(clean[-tail:])
