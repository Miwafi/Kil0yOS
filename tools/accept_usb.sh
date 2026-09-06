#!/bin/bash
# USB (UHCI + HID) plug-and-play acceptance:
#  1) boot with a built-in usb-mouse -> controller init + enumeration +
#     mouse bind (usb_uhci_ok / usb_enumerated / usb_mouse_ok)
#  2) monitor mouse_move -> HID report path (usb_mouse_delta)
#  3) hot-plug a usb-kbd via monitor -> ~1s root-hub poll picks it up
#     (usb_kbd_plugged + usb_ps2_suppressed)
#  4) sendkey typing -> delivered ONLY through USB (exactly 2 occurrences
#     of the marker: echo + command output) + usb_kbd_ok
#  5) device_del -> PS/2 keyboard resumes (usb_kbd_gone + usb_ps2_resumed)
#  6) typing works again through PS/2
# Usage (WSL): bash tools/accept_usb.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_usb.log" "$TDIR/qmon_usb"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_usb.log" \
  -monitor unix:"$TDIR/qmon_usb",server,nowait \
  -device piix3-usb-uhci,id=usb0 \
  -device usb-mouse \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

type_cmd() {
    timeout 20 python3 tools/drive_keys.py "$1" "$TDIR/qmon_usb" || echo "TYPING FAILED: $1"
}
mon_cmd() {
    timeout 20 python3 tools/mon_cmd.py "$@" "$TDIR/qmon_usb" || echo "MON FAILED: $*"
}
# wait for a marker in the serial log (tail window), timeout in seconds
wait_marker() {
    local pat="$1" tmo="${2:-60}"
    local i=0
    while [ "$i" -lt "$tmo" ]; do
        if python3 tools/dump_serial.py "$TDIR/serial_usb.log" 2000000 2>/dev/null \
           | grep -aq "$pat"; then
            echo "[marker] $pat (${i}s)"; return 0
        fi
        sleep 1; i=$((i+1))
    done
    echo "TIMEOUT waiting for: $pat"; return 1
}

FAIL=0
check() {  # check <description> <wait_marker args...>
    if wait_marker "$@"; then return 0; else FAIL=1; return 1; fi
}

echo "--- 1) boot: built-in usb-mouse enumerates ---"
check "usb_uhci_ok" 60 || true
check "usb_enumerated" 30 || true
check "usb_mouse_ok" 30 || true

echo "--- 2) USB mouse report path ---"
mon_cmd mouse_move 10 6
check "usb_mouse_delta" 30 || true

echo "--- 3) hot-plug usb-kbd ---"
mon_cmd device_add usb-kbd,id=kbd0
check "usb_kbd_plugged" 15 || true
check "usb_ps2_suppressed" 15 || true

echo "--- 4) typing through USB keyboard only ---"
type_cmd "echo usb_kbd_once"
check "usb_kbd_ok" 30 || true
check "usb_kbd_once" 30 || true
sleep 4   # let the echo + output fully land
KBD_COUNT=$(python3 tools/dump_serial.py "$TDIR/serial_usb.log" 2000000 2>/dev/null \
            | grep -ao "usb_kbd_once" | wc -l)
echo "[count] usb_kbd_once occurrences: $KBD_COUNT (expect exactly 2)"
[ "$KBD_COUNT" -eq 2 ] || { echo "DOUBLE-DELIVERY or lost keys (expected 2)"; FAIL=1; }

echo "--- 5) unplug usb-kbd -> PS/2 resumes ---"
mon_cmd device_del kbd0
check "usb_kbd_gone" 15 || true
check "usb_ps2_resumed" 15 || true

echo "--- 6) typing through PS/2 again ---"
type_cmd "echo usb_all_done"
check "usb_all_done" 30 || true

kill "$QPID" 2>/dev/null
sleep 1
echo "=== usb acceptance output ==="
python3 tools/dump_serial.py "$TDIR/serial_usb.log" 2000000 2>/dev/null | \
  grep -aiE 'usb|PANIC|panic|EXCEPTION|exception|TIMEOUT|TYPING|MON FAILED' | \
  grep -av 'usb_mouse_delta' | head -60

if [ "$FAIL" -eq 0 ]; then
    echo "USB_ALL_OK"
    exit 0
else
    echo "USB_ACCEPTANCE_FAILED"
    exit 1
fi
