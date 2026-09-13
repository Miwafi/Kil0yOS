#!/bin/bash
# MP3 playback acceptance (AC97 bus-master DMA):
#   1) UEFI boot -> "desktop"; shell "cd /home/user/art" so the Files panel
#      lands in the art directory.
#   2) F12 (menu key) -> Down -> Enter selects the Files panel; Down -> Enter
#      opens 103.mp3: serial must show "[audio] playing 103.mp3" and the
#      AC97 codec must come up ("[ac97] codec ready").
#   3) screendump of the player window (title/progress bar drawn).
#   4) the captured wav (QEMU wav audiodev) must carry non-silent 16-bit
#      stereo PCM at the MP3's own sample rate - i.e. the decoder really fed
#      the DMA ring, not just silence.
#   5) no EXCEPTION/PANIC on the serial mirror.
# Usage (WSL): bash tools/accept_audio.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_mp3.log" "$TDIR/qmon_mp3" "$TDIR/mp3_capture.wav" \
      "$TDIR/mp3_player.ppm"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

type_cmd() {
    timeout 30 python3 tools/drive_keys.py "$1" "$TDIR/qmon_mp3" || echo "TYPING FAILED: $1"
}
mon_cmd() {
    timeout 20 python3 tools/mon_cmd.py "$@" "$TDIR/qmon_mp3" || echo "MON FAILED: $*"
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

# wav capture by default; AUDIO_BACKEND=none,id=snd0 switches to the dummy
# backend (realtime paced, no file) for driver-only runs
WAV_OUT="$TDIR/mp3_capture.wav"
if [ -z "${AUDIO_BACKEND:-}" ]; then
    ADEV="wav,id=snd0,path=$WAV_OUT"
else
    ADEV="$AUDIO_BACKEND"
    WAV_OUT=""
fi
echo "audiodev: $ADEV"

echo "=== 1) UEFI boot + desktop ==="
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
  -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_mp3.log" \
  -monitor unix:"$TDIR/qmon_mp3",server,nowait \
  -audiodev "$ADEV" \
  -device AC97,audiodev=snd0 \
  -no-reboot &
QPID=$!

check "$TDIR/serial_mp3.log" "shell up" "Kil0yOS version" 60 || true
sleep 3
type_cmd "desktop"
check "$TDIR/serial_mp3.log" "desktop launch" "Launching GOP desktop" 30 || true
sleep 3

echo "--- 2) cd into the art directory, switch to the Files panel ---"
type_cmd "cd /home/user/art"
sleep 2
mon_cmd sendkey f12      # Win/Menu key: opens the start menu
sleep 1
mon_cmd sendkey down     # Editor -> Files
sleep 1
mon_cmd sendkey ret
check "$TDIR/serial_mp3.log" "files panel" "func ->" 20 || true
sleep 2

echo "--- 3) open 103.mp3 (row 0 = \"..\", row 1 = 103.mp3) ---"
mon_cmd sendkey down
sleep 1
mon_cmd sendkey ret
check "$TDIR/serial_mp3.log" "codec ready" "codec ready" 20 || true
check "$TDIR/serial_mp3.log" "playing" "playing.*103.mp3" 20 || true

echo "--- 4) let it play: two screendumps must show the progress bar grow ---"
sleep 3
mon_cmd screendump "$TDIR/mp3_player_a.ppm"
sleep 6
mon_cmd screendump "$TDIR/mp3_player_b.ppm"
sleep 2
PPMA="$TDIR/mp3_player_a.ppm"; [ -f "$PPMA" ] || PPMA="$TDIR/mp3_player_a"
PPMB="$TDIR/mp3_player_b.ppm"; [ -f "$PPMB" ] || PPMB="$TDIR/mp3_player_b"
if [ -f "$PPMA" ] && [ -f "$PPMB" ]; then
    python3 - "$PPMA" "$PPMB" <<'PYEOF'
import sys

def load(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def bar_strip(path):
    """pixels of the player window's progress bar (fill area only)"""
    w, h, pix = load(path)
    x = (w - 340) // 2 + 11          # window is 340x92, bar starts 10px in
    y = (h - 92) // 2 + 53
    out = bytearray()
    for row in range(y, y + 8):
        off = (row * w + x) * 3
        out += pix[off:off + 320 * 3]
    return w, h, bytes(out)

wa, ha, a = bar_strip(sys.argv[1])
wb, hb, b = bar_strip(sys.argv[2])
changed = sum(1 for i in range(0, min(len(a), len(b)), 3)
              if a[i:i+3] != b[i:i+3])
print(f"[ppm] {wa}x{ha} progress-bar pixels changed={changed}")
ok = wa == 1024 and ha == 768 and changed > 20
print("MP3_PPM_OK" if ok else "MP3_PPM_FAIL")
sys.exit(0 if ok else 1)
PYEOF
    [ $? -eq 0 ] || FAIL=1
else
    echo "MP3_PPM_MISSING"; FAIL=1
fi

kill "$QPID" 2>/dev/null
sleep 2

echo "--- 5) wav capture analysis ---"
if [ -z "$WAV_OUT" ]; then
    echo "[skip] dummy backend: no capture to analyse"
elif [ ! -f "$WAV_OUT" ]; then
    echo "NO_WAV_CAPTURE"; FAIL=1
else
python3 - "$WAV_OUT" <<'PYEOF'
import struct, sys
raw = open(sys.argv[1], 'rb').read()
if len(raw) < 44 or raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
    print("BAD_WAV_HEADER"); sys.exit(1)
pos = 12
fmt = None
data = b''
while pos + 8 <= len(raw):
    cid = raw[pos:pos+4]; size = struct.unpack('<I', raw[pos+4:pos+8])[0]
    body = raw[pos+8:pos+8+size]
    if cid == b'fmt ':
        fmt = struct.unpack('<HHIIHH', body[:16])
    elif cid == b'data':
        data = body
    pos += 8 + size + (size & 1)
if fmt is None:
    print("NO_FMT_CHUNK"); sys.exit(1)
channels, rate, block, bits = fmt[1], fmt[2], fmt[4], fmt[5]
n = len(data) // 2
samples = struct.unpack('<%dh' % n, data[:n*2]) if n else ()
peak = max((abs(s) for s in samples), default=0)
mean = sum(abs(s) for s in samples) / n if n else 0
# the tail must be loud too: a driver that stalls after the first ring-full
# would leave the captured file silent from then on
tail = samples[n * 3 // 4:]
tail_mean = sum(abs(s) for s in tail) / len(tail) if tail else 0
secs = (n / max(channels, 1)) / rate if rate else 0
print(f"[wav] fmt={fmt[0]} {channels}ch {rate}Hz {bits}bit frames={n//max(channels,1)} "
      f"({secs:.2f}s) peak={peak} mean|x|={mean:.1f} tail_mean={tail_mean:.1f}")
ok = (bits == 16 and channels >= 2 and 8000 <= rate <= 48000
      and peak > 500 and mean > 50 and secs > 3.0 and tail_mean > 50)
print("MP3_WAV_OK" if ok else "MP3_WAV_FAIL")
sys.exit(0 if ok else 1)
PYEOF
    [ $? -eq 0 ] || FAIL=1
fi

LOG=$(python3 tools/dump_serial.py "$TDIR/serial_mp3.log" 4000000 2>/dev/null)
echo "$LOG" | grep -aqE "EXCEPTION|PANIC" && { echo "exception/panic"; FAIL=1; }

echo "=== mp3 acceptance output ==="
echo "$LOG" | grep -aiE 'ac97|audio|mp3|files|EXCEPTION|PANIC' | tail -20

if [ "$FAIL" -eq 0 ]; then
    echo "AUDIO_ALL_OK"
    exit 0
else
    echo "AUDIO_ACCEPTANCE_FAILED"
    exit 1
fi
