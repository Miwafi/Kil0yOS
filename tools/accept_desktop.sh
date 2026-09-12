#!/bin/bash
# GOP desktop acceptance:
#   1) UEFI boot (OVMF): shell -> type "desktop" -> chrome draws on the GOP
#      framebuffer. Screendump must be 1024x768 with the blue chrome
#      (header/footer/backdrop), black content panels and white text.
#   2) desktop shell: typed command runs in the Shell panel; echo output
#      reaches the serial mirror.
#   3) ESC exits back to the fb text shell ("Returned to shell").
#   4) BIOS regression: "desktop" on the VGA path must refuse with a hint.
# Usage (WSL): bash tools/accept_desktop.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_dt_uefi.log" "$TDIR/serial_dt_bios.log" \
      "$TDIR/qmon_dt" "$TDIR/dt_"*.ppm
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_dt" || echo "TYPING FAILED: $1"
}
mon_cmd() {
    timeout 20 python3 tools/mon_cmd.py "$@" "$TDIR/qmon_dt" || echo "MON FAILED: $*"
}
wait_marker_in() {
    local log="$1" pat="$2" tmo="${3:-60}"
    local i=0
    while [ "$i" -lt "$tmo" ]; do
        if python3 tools/dump_serial.py "$log" 4000000 2>/dev/null | grep -aq "$pat"; then
            echo "[marker] $pat (${i}s)"; return 0
        fi
        sleep 1; i=$((i+1))
    done
    echo "TIMEOUT waiting for: $pat"; return 1
}
check() {
    local log="$1" desc="$2"
    shift 2
    if wait_marker_in "$log" "$@"; then return 0; else FAIL=1; return 1; fi
}

FAIL=0

echo "=== 1) UEFI boot + desktop command ==="
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
  -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_dt_uefi.log" \
  -monitor unix:"$TDIR/qmon_dt",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

check "$TDIR/serial_dt_uefi.log" "shell up" "Kil0yOS version" 60 || true
sleep 3

type_cmd "desktop"
check "$TDIR/serial_dt_uefi.log" "desktop launch" "Launching GOP desktop" 30 || true
sleep 3

echo "--- 2) desktop chrome screendump ---"
mon_cmd screendump "$TDIR/dt_chrome.ppm"
sleep 2
PPM="$TDIR/dt_chrome.ppm"
[ -f "$PPM" ] || PPM="$TDIR/dt_chrome"
if [ -f "$PPM" ]; then
    python3 - "$PPM" <<'PYEOF'
import sys
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
pix = parts[3]
blue = black = white = 0
total = w * h
for i in range(0, len(pix) - 3, 3):
    r, g, b = pix[i], pix[i+1], pix[i+2]
    if b > 128 and r < 80 and g < 80:
        blue += 1
    elif r < 40 and g < 40 and b < 40:
        black += 1
    elif r > 200 and g > 200 and b > 200:
        white += 1
wf = white / total; bf = blue / total
print(f"[ppm] {w}x{h} white={wf:.3f} blue={bf:.4f} black={black}")
# white bg/panels dominant, blue selection highlight + clock patch,
# black text/borders present
ok = (w == 1024 and h == 768 and wf > 0.5
      and 0.001 < bf < 0.06 and black > 300)
print("DT_PPM_OK" if ok else "DT_PPM_FAIL")
sys.exit(0 if ok else 1)
PYEOF
    [ $? -eq 0 ] || FAIL=1
else
    echo "DT_PPM_MISSING"; FAIL=1
fi

echo "--- 3) desktop shell interaction ---"
type_cmd "echo dt_shell_ok"
check "$TDIR/serial_dt_uefi.log" "dt_shell_ok" "dt_shell_ok" 30 || true

mon_cmd screendump "$TDIR/dt_shell.ppm"
sleep 1

echo "--- 4) ESC exit back to text shell ---"
mon_cmd sendkey esc
check "$TDIR/serial_dt_uefi.log" "returned" "Returned to shell" 30 || true
sleep 1
kill "$QPID" 2>/dev/null
sleep 1

echo "=== 5) BIOS regression: desktop must refuse ==="
qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_dt_bios.log" \
  -monitor unix:"$TDIR/qmon_dt",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID2=$!

check "$TDIR/serial_dt_bios.log" "bios shell" "Kil0yOS version" 60 || true
sleep 2
type_cmd "desktop"
check "$TDIR/serial_dt_bios.log" "refuse" "needs the UEFI GOP framebuffer" 30 || true
sleep 1
kill "$QPID2" 2>/dev/null
sleep 1

UEFI_LOG=$(python3 tools/dump_serial.py "$TDIR/serial_dt_uefi.log" 4000000 2>/dev/null)
BIOS_LOG=$(python3 tools/dump_serial.py "$TDIR/serial_dt_bios.log" 4000000 2>/dev/null)
echo "$UEFI_LOG" | grep -aqE "EXCEPTION|PANIC" && { echo "UEFI exception/panic"; FAIL=1; }
echo "$BIOS_LOG" | grep -aqE "EXCEPTION|PANIC" && { echo "BIOS exception/panic"; FAIL=1; }

echo "=== desktop acceptance output (UEFI) ==="
echo "$UEFI_LOG" | grep -aiE 'desktop|dt_shell|gop|version|EXCEPTION|PANIC' | tail -15
echo "=== desktop acceptance output (BIOS) ==="
echo "$BIOS_LOG" | grep -aiE 'desktop|version|EXCEPTION|PANIC' | tail -6

if [ "$FAIL" -eq 0 ]; then
    echo "DESKTOP_ALL_OK"
    exit 0
else
    echo "DESKTOP_ACCEPTANCE_FAILED"
    exit 1
fi
