#!/bin/bash
# Window-manager desktop smoke test (BIOS / mode12h 640x480 `gui` path):
#   1) boot the ISO headless, wait for the 3.8.0 version banner
#   2) type `gui` through the QEMU monitor -> window-manager desktop
#   3) wait for "[desktop] loop enter" in the serial log
#   4) screendump -> PPM pixel assertions: wallpaper band, white window
#      bodies, blue chrome (focused title bar + Start button), red close
#      buttons, dark taskbar; serial log must be free of EXCEPTION/PANIC.
# Usage (WSL): bash tools/smoke_wm.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_wm.log" "$TDIR/qmon_wm" "$TDIR/wm_dump.ppm"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1
cp build/kil0yos.iso /tmp/kil0yos_wm.iso || { echo "ISO_COPY_FAILED"; exit 1; }

FAIL=0
wait_marker_in() {  # wait_marker_in <log> <pattern> [timeout-sec]
    local log="$1" pat="$2" tmo="${3:-60}"
    local i=0
    while [ "$i" -lt "$tmo" ]; do
        if python3 tools/dump_serial.py "$log" 4000000 2>/dev/null | grep -aq "$pat"; then
            echo "[marker] $pat (${i}s)"; return 0
        fi
        sleep 1; i=$((i+1))
    done
    echo "TIMEOUT waiting for: $pat"; FAIL=1; return 1
}

echo "=== 1) boot (BIOS) ==="
qemu-system-x86_64 -cdrom /tmp/kil0yos_wm.iso -m 512M -display none \
  -serial file:"$TDIR/serial_wm.log" \
  -monitor unix:"$TDIR/qmon_wm",server,nowait \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

wait_marker_in "$TDIR/serial_wm.log" "Kil0yOS version 3.9.0" 90
sleep 6

echo "=== 2) enter desktop (gui) ==="
timeout 20 python3 tools/drive_keys.py "gui" "$TDIR/qmon_wm" \
  || { echo "TYPING FAILED"; FAIL=1; }
wait_marker_in "$TDIR/serial_wm.log" "\[desktop\] loop enter" 30

echo "=== 3) screendump + pixel assertions ==="
sleep 3
timeout 20 python3 tools/mon_cmd.py screendump "$TDIR/wm_dump.ppm" "$TDIR/qmon_wm" \
  || { echo "MON FAILED"; FAIL=1; }
sleep 2
PPM="$TDIR/wm_dump.ppm"
[ -f "$PPM" ] || PPM="$TDIR/wm_dump"
if [ -f "$PPM" ]; then
    python3 - "$PPM" <<'PYEOF'
import sys
path = sys.argv[1]
data = open(path, 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
pix = parts[3]
total = w * h
white = blue = red = cyan = 0
tb_area = dark_tb = 0
tb_top = h - 14
for i in range(0, len(pix) - 2, 3):
    r, g, b = pix[i], pix[i+1], pix[i+2]
    y = (i // 3) // w
    if r > 200 and g > 200 and b > 200:
        white += 1
    elif b > r + 40 and b > g + 20 and b > 80:
        blue += 1
    elif r > g + 50 and r > b + 50 and r > 100:
        red += 1
    elif r < 90 and 90 < g < 210 and 90 < b < 210 and abs(g - b) < 45:
        cyan += 1
    if r < 110 and g < 110 and b < 110:
        if y >= tb_top:
            dark_tb += 1
            tb_area += 1
print(f"[ppm] {w}x{h} white={white} blue={blue} red={red} cyan={cyan} "
      f"tb_dark={dark_tb}/{14 * w}")
# mode12h (640x480, EGA-16): three white floating windows over a cyan
# wallpaper, blue focused title bar + Start button, red close buttons,
# dark taskbar (14 rows).
ok = (w == 640 and h == 480
      and white > total * 0.25
      and blue > 200
      and red > 50
      and cyan > 100
      and dark_tb > (14 * w) * 0.4)
print("PPM_OK" if ok else "PPM_FAIL")
sys.exit(0 if ok else 1)
PYEOF
    [ $? -eq 0 ] || FAIL=1
else
    echo "PPM_MISSING"; FAIL=1
fi

kill "$QPID" 2>/dev/null
sleep 1

echo "=== 4) serial log sanity ==="
python3 tools/dump_serial.py "$TDIR/serial_wm.log" 4000000 2>/dev/null | \
  grep -aqE "EXCEPTION|PANIC" && { echo "exception/panic in log"; FAIL=1; }
python3 tools/dump_serial.py "$TDIR/serial_wm.log" 4000000 2>/dev/null | \
  grep -a "desktop" | tail -5

if [ "$FAIL" -eq 0 ]; then
    echo "WM_SMOKE_OK"
    exit 0
else
    echo "WM_SMOKE_FAILED"
    exit 1
fi
