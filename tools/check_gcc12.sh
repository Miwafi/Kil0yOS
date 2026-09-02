#!/bin/bash
# Verify kernel zstd decoder against gcc-12-base control/data tarballs.
cd /mnt/c/Users/19423/Desktop/Programs/Kil0yOS || exit 1
ZDEC=/tmp/ztest/zdec
for f in /tmp/ztest/g12_control.tar.zst /tmp/ztest/g12_data.tar.zst; do
    [ -f "$f" ] || continue
    if $ZDEC "$f" "$f.out" 2>&1; then
        zstd -d -c "$f" > "$f.ref" 2>/dev/null
        if cmp -s "$f.out" "$f.ref"; then
            echo "MATCH $f"
        else
            echo "MISMATCH $f"
        fi
    else
        echo "DECFAIL $f"
    fi
done
