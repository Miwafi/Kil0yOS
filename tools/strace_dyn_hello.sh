#!/usr/bin/env bash
# Phase 3.0 probe: build a musl DYNAMIC hello and strace its syscall list.
set -euo pipefail
MUSL_GCC="$HOME/musl/bin/musl-gcc"
cat > /tmp/hd.c <<'EOF'
#include <stdio.h>
#include <unistd.h>
int main(void) {
    printf("hello dyn\n");
    fflush(stdout);
    return 42;
}
EOF
"$MUSL_GCC" -O2 /tmp/hd.c -o /tmp/hd
file /tmp/hd
readelf -l /tmp/hd | grep -E 'INTERP|Entry'
echo "=== strace ==="
strace -c /tmp/hd
echo "=== raw trace ==="
strace -f /tmp/hd 2>&1 | head -80
