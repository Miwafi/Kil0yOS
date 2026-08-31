#!/bin/bash
# Host-side unit tests for the Phase 4 kernel modules (inflate, sha256).
# Usage (WSL): ./tools/pkg_host_test.sh
set -e
cd "$(dirname "$0")/.."

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

gcc -O1 -Wall -Wno-unused-function -I include \
    tools/pkg_host_test.c \
    src/kernel/pkg/inflate.c \
    src/kernel/pkg/sha256.c \
    src/kernel/lib/string.c \
    src/kernel/lib/stdlib.c \
    -o "$T/pkgtest"

# Case 1: small text
head -c 10000 /dev/urandom > "$T/small.bin"
gzip -c -n "$T/small.bin" > "$T/small.gz"
"$T/pkgtest" "$T/small.gz" "$T/small.bin"

# Case 2: multi-MB with repetitive sections (long LZ77 matches, dynamic blocks)
python3 - "$T/mid.bin" <<'EOF'
import sys, os
pat = os.urandom(4096)
data = bytearray()
while len(data) < 3 * 1024 * 1024:
    data += pat                       # long matches
    data += os.urandom(1024)          # random interleave
data += b"tail" * 100
open(sys.argv[1], "wb").write(bytes(data))
EOF
gzip -c -n -6 "$T/mid.bin" > "$T/mid.gz"
"$T/pkgtest" "$T/mid.gz" "$T/mid.bin"

# Case 3: highly compressible (stored-block style test isn't forced by gzip,
# but level-1 exercises different code paths)
gzip -c -n -1 "$T/mid.bin" > "$T/mid1.gz"
"$T/pkgtest" "$T/mid1.gz" "$T/mid.bin"

# Case 4: concatenated members (like .tar.gz joined streams)
cat "$T/small.gz" "$T/small.gz" > "$T/concat.gz"
python3 - "$T/small.bin" "$T/concat.bin" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
open(sys.argv[2], "wb").write(d + d)
EOF
"$T/pkgtest" "$T/concat.gz" "$T/concat.bin" multi

echo "HOST PKG TESTS DONE"
