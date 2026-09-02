#!/usr/bin/env bash
# Build the kernel zstd decoder with ZSTD_KTEST debug output and run one case.
set -e
cd "$(dirname "$0")/.."
mkdir -p /tmp/ztest/fakeinc/lib /tmp/ztest/fakeinc/mm /tmp/ztest/fakeinc/drivers
printf '#include <string.h>\n' > /tmp/ztest/fakeinc/lib/string.h
printf '#include <stdlib.h>\nstatic inline void* kmalloc(unsigned long long n) { return malloc(n); }\nstatic inline void kfree(void* p) { free(p); }\n' > /tmp/ztest/fakeinc/mm/memory.h
printf '#include <stdio.h>\nstatic inline void klog(const char* s) { (void)s; }\n' > /tmp/ztest/fakeinc/drivers/vga.h
gcc -O2 -g -DZSTD_KTEST -Wall -Wno-unused-parameter -I/tmp/ztest/fakeinc -Iinclude \
    tools/test_zstd_host.c src/kernel/pkg/zstd.c -o /tmp/ztest/zdecdbg
CASE="${1:-rle}" python3 - <<'EOF'
import os
import zstandard as zstd
case = os.environ.get('CASE', 'rle')
if case == 'rle':
    payload = b'A' * 200000
elif case == 'pat':
    payload = bytes(range(256)) * 400
else:
    payload = open(case, 'rb').read()
open('/tmp/ztest/dbg.zst', 'wb').write(zstd.ZstdCompressor(level=3).compress(payload))
print(f"compressed {len(payload)} bytes")
EOF
/tmp/ztest/zdecdbg /tmp/ztest/dbg.zst /tmp/ztest/dbg.out
