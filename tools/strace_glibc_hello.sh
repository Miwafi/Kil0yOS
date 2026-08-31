#!/usr/bin/env bash
# Phase 3.1 probe: glibc dynamic hello syscall surface (strace) + blob sizes.
set -u
cat > /tmp/hg.c <<'EOF'
#include <stdio.h>
int main(void) {
    printf("hello glibc\n");
    return 0;
}
EOF
gcc -O2 /tmp/hg.c -o /tmp/hg || { echo "NO_GCC"; exit 1; }
ls -la /lib64/ld-linux-x86_64.so.2 /lib/x86_64-linux-gnu/libc.so.6
ldd /tmp/hg
echo "=== strace summary ==="
strace -c /tmp/hg 2>&1 | tail -30
echo "=== raw trace (first 90) ==="
strace -f /tmp/hg 2>&1 | sed 's/^[0-9]* *//' | head -90
