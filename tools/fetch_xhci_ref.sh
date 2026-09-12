#!/bin/bash
set -e
WS=/mnt/c/Users/19423/Desktop/Programs/Kil0yOS/build/xhci_ref
mkdir -p "$WS" && cd "$WS"
for u in \
  "https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/usb/host/xhci.h?h=v6.6" \
  "https://cdn.jsdelivr.net/gh/torvalds/linux@v6.6/drivers/usb/host/xhci.h" ; do
  curl -sfL -m 40 -o xhci.h "$u" && { echo "OK xhci.h ($(wc -c < xhci.h))"; break; }
done
wc -c xhci.h 2>/dev/null || echo FAIL
