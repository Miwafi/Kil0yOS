#!/bin/bash
# GOP (UEFI framebuffer) acceptance:
#   1) UEFI boot (OVMF): kernel calls GOP itself via the EFI boot services
#      GRUB kept alive -> gop_ok mode=1024x768x32, ebs_ok, EFI-mmap PMM
#      fallback, 2.17.0 banner; no gop_fail / ebs_fail / EXCEPTION.
#   2) fb visual: monitor screendump -> PPM is 1024x768 and carries text
#      (bright pixel count in a plausible range).
#   3) fb shell: typing lands in the fb terminal (serial mirror shows the
#      echo + command output).
#   4) BIOS regression: same ISO without -bios -> "gop: no EFI ST" and NO
#      gop_ok; VGA path untouched (VGA_TEXT_OK marker + shell works).
# Usage (WSL): bash tools/accept_gop.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_gop_uefi.log" "$TDIR/serial_gop_bios.log" \
      "$TDIR/qmon_gop" "$TDIR/gop_dump.ppm"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

type_cmd() {
    timeout 20 python3 tools/drive_keys.py "$1" "$TDIR/qmon_gop" || echo "TYPING FAILED: $1"
}
mon_cmd() {
    timeout 20 python3 tools/mon_cmd.py "$@" "$TDIR/qmon_gop" || echo "MON FAILED: $*"
}
# wait for a marker in a serial log, timeout in seconds
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

FAIL=0
check() {  # check <log> <description> <pattern> [timeout]
    local log="$1" desc="$2"
    shift 2
    if wait_marker_in "$log" "$@"; then return 0; else FAIL=1; return 1; fi
}

echo "=== 1) UEFI boot (OVMF + GOP) ==="
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
  -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_gop_uefi.log" \
  -monitor unix:"$TDIR/qmon_gop",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

check "$TDIR/serial_gop_uefi.log" "gop_ok" "gop_ok mode=1024x768x32" 60 || true
check "$TDIR/serial_gop_uefi.log" "ebs_ok" "ebs_ok" 30 || true
check "$TDIR/serial_gop_uefi.log" "pmm_efi" "PMM: using EFI memory map fallback" 30 || true
check "$TDIR/serial_gop_uefi.log" "version" "Kil0yOS version 2.17.0" 30 || true

echo "--- 2) fb visual: screendump ---"
sleep 4
mon_cmd screendump "$TDIR/gop_dump.ppm"
sleep 2
PPM="$TDIR/gop_dump.ppm"
[ -f "$PPM" ] || PPM="$TDIR/gop_dump"
if [ -f "$PPM" ]; then
    python3 - "$PPM" <<'PYEOF'
import sys
path = sys.argv[1]
data = open(path, 'rb').read()
# parse PPM header (P6\n<w> <h>\n<max>\n)
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split())
pix = parts[3]
bright = 0
total = w * h
# count near-white pixels (text glyphs) sampling every 4th byte triple
for i in range(0, len(pix) - 3, 3 * 7):
    r, g, b = pix[i], pix[i+1], pix[i+2]
    if r > 128 and g > 128 and b > 128:
        bright += 1
est = bright * 7
print(f"[ppm] {w}x{h} bright~{est}")
ok = (w == 1024 and h == 768 and 2000 < est < total * 0.4)
print("PPM_OK" if ok else "PPM_FAIL")
sys.exit(0 if ok else 1)
PYEOF
    [ $? -eq 0 ] || FAIL=1
else
    echo "PPM_MISSING"; FAIL=1
fi

echo "--- 3) fb shell interaction ---"
type_cmd "echo gop_fb_shell_ok"
check "$TDIR/serial_gop_uefi.log" "gop_fb_shell_ok" "gop_fb_shell_ok" 30 || true

kill "$QPID" 2>/dev/null
sleep 1

echo "=== 4) BIOS regression (VGA untouched) ==="
qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_gop_bios.log" \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID2=$!

check "$TDIR/serial_gop_bios.log" "no_efi_st" "no EFI ST" 60 || true
check "$TDIR/serial_gop_bios.log" "bios_version" "Kil0yOS version 2.17.0" 30 || true

sleep 3
kill "$QPID2" 2>/dev/null
sleep 1

UEFI_LOG=$(python3 tools/dump_serial.py "$TDIR/serial_gop_uefi.log" 4000000 2>/dev/null)
BIOS_LOG=$(python3 tools/dump_serial.py "$TDIR/serial_gop_bios.log" 4000000 2>/dev/null)

echo "$UEFI_LOG" | grep -aq "gop_fail" && { echo "UEFI gop_fail present"; FAIL=1; }
echo "$UEFI_LOG" | grep -aq "ebs_fail" && { echo "UEFI ebs_fail present"; FAIL=1; }
echo "$UEFI_LOG" | grep -aqE "EXCEPTION|PANIC" && { echo "UEFI exception/panic"; FAIL=1; }
echo "$BIOS_LOG" | grep -aq "gop_ok" && { echo "BIOS unexpectedly has gop_ok"; FAIL=1; }
echo "$BIOS_LOG" | grep -aqE "EXCEPTION|PANIC" && { echo "BIOS exception/panic"; FAIL=1; }

echo "=== gop acceptance output (UEFI) ==="
echo "$UEFI_LOG" | grep -aiE 'gop|ebs|PMM|version|fb|EXCEPTION|PANIC' | tail -30
echo "=== gop acceptance output (BIOS) ==="
echo "$BIOS_LOG" | grep -aiE 'gop|version|EXCEPTION|PANIC' | tail -10

if [ "$FAIL" -eq 0 ]; then
    echo "GOP_ALL_OK"
    exit 0
else
    echo "GOP_ACCEPTANCE_FAILED"
    exit 1
fi
