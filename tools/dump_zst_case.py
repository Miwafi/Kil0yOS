#!/usr/bin/env python3
"""Dump a minimal failing zstd case for manual inspection."""
import zstandard as zstd

c = zstd.ZstdCompressor(level=3)
z = c.compress(b"A" * 200000)
open("/tmp/zst_case.zst", "wb").write(z)
print(" ".join("%02x" % b for b in z))
print("len:", len(z))
