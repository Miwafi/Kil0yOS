#!/usr/bin/env python3
"""Byte-compare kernel decoder output against the reference zstandard module."""
import sys
import zstandard as zstd

ok = True
for name in ("g12c", "g12d"):
    zpath = f"/tmp/ztest/{'g12_control' if name == 'g12c' else 'g12_data'}.tar.zst"
    out = open(f"/tmp/ztest/{name}.out", "rb").read()
    ref = zstd.ZstdDecompressor().decompress(
        open(zpath, "rb").read(), max_output_size=1 << 28)
    if out == ref:
        print(f"MATCH {zpath} ({len(ref)} bytes)")
    else:
        print(f"MISMATCH {zpath}: got {len(out)}, want {len(ref)}")
        ok = False
sys.exit(0 if ok else 1)
