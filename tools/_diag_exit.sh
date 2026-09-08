#!/bin/bash
cd /mnt/c/Users/19423/Desktop/Programs/Kil0yOS || exit 1
rm -f "$HOME/ktest/gopv8.log" "$HOME/ktest/qintv8.log"
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -cdrom build/kil0yos.iso \
  -m 512M -display none -serial file:"$HOME/ktest/gopv8.log" \
  -d int -D "$HOME/ktest/qintv8.log" -no-reboot >/dev/null 2>&1 &
Q=$!
START=$(date +%s)
CRASHED=no
for i in $(seq 1 55); do
  if ! kill -0 $Q 2>/dev/null; then
    CRASHED="yes (after $(( $(date +%s) - START ))s)"
    break
  fi
  sleep 1
done
ALIVE=yes
kill -0 $Q 2>/dev/null || ALIVE=no
kill $Q 2>/dev/null
wait $Q 2>/dev/null
echo "QEMU exited early: $CRASHED (still running at 55s: $ALIVE)"
echo "check_exception: $(grep -ac check_exception "$HOME/ktest/qintv8.log")"
echo "=== serial tail ==="
tail -c 700 "$HOME/ktest/gopv8.log"
