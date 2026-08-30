#!/bin/bash
# Symbolize a kernel address: tools/sym.sh <hex-addr>
cd "$(dirname "$0")/.."
A=$(( $1 ))
nm -n build/kernel.bin | gawk -v A="$A" '{ a=strtonum("0x" $1); if (a <= A) last=$0; else { print last; exit } }'
