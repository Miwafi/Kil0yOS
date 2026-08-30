#!/bin/bash
cd "$(dirname "$0")/.."
L="$HOME/ktest/qemu.log"
echo "=== all #PF / #DF line numbers with EFER context ==="
grep -n 'INT=0x0e\|INT=0x08\|INT=0x21' "$L" | cut -d: -f1 | while read n; do
  efer=$(sed -n "$((n-1))p" "$L" | grep -o 'EFER=[0-9a-f]*')
  vec=$(sed -n "${n}p" "$L" | grep -o 'INT=0x[0-9a-f]*')
  echo "line $n  $vec  $efer"
done | tail -40
