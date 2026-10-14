#!/usr/bin/env bash
# List BSS/rodata symbols around a given address to find neighbors.
cd /mnt/c/Users/19423/Desktop/Programs/Kil0yOS || exit 1
nm -n build/kernel.bin | grep -E ' [bdDrR] ' | awk '{print}' | grep -B4 -A4 -i 'tasks' | head -20
echo "=== symbols just below 0x112e3c0 ==="
nm -n build/kernel.bin | awk 'strtonum("0x"$1) <= strtonum("0x112e3c0") {last=$0} strtonum("0x"$1) > strtonum("0x112e3c0") {print last; print $0; exit}'
echo "=== sizeof(process_t) estimate: (tasks - processes)/128 ==="
python3 -c "print((0x112e3c0-0x10274a0)/128)"
