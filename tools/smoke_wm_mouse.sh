#!/bin/bash
# Window-manager DIRTY-REPAINT interaction test (BIOS / mode12h 640x480):
# drives the desktop with real pointer clicks through the QEMU monitor and
# asserts the pixels after each interaction. Exercises the wm_flush() paths:
#   A) click Shell title      -> focus colors move (win_dirty)
#   B) click KLOG close (x)   -> window hides, wallpaper exposed (damage)
#   C) taskbar "Log" button   -> window returns, focused (win_dirty)
#   D) drag KLOG title up 60  -> damage = old+new extent, cascade repaint
# Window layout at 640x480 (desktop_layout_init):
#   APPS  x 2..131  y 2..463   SHELL x 136..637 y 2..255   KLOG x 136..637 y 260..463
#   taskbar y 466..479; task buttons: Start 2..59, Apps 68..107, Shell 115..162, Log 170..201
# Usage (WSL): bash tools/smoke_wm_mouse.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR"/serial_wmm.log "$TDIR"/qmon_wmm "$TDIR"/wm_*.ppm
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1
cp build/kil0yos.iso /tmp/kil0yos_wmm.iso || { echo "ISO_COPY_FAILED"; exit 1; }

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
qemu-system-x86_64 -cdrom /tmp/kil0yos_wmm.iso -m 512M -display none \
  -serial file:"$TDIR/serial_wmm.log" \
  -monitor unix:"$TDIR/qmon_wmm",server,nowait \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

wait_marker_in "$TDIR/serial_wmm.log" "Kil0yOS version 3.10.0" 90
sleep 6

echo "=== 2) enter desktop (gui) ==="
timeout 20 python3 tools/drive_keys.py "gui" "$TDIR/qmon_wmm" \
  || { echo "TYPING FAILED"; FAIL=1; }
wait_marker_in "$TDIR/serial_wmm.log" "\[desktop\] loop enter" 30
sleep 3

echo "=== 3) pointer interactions ==="
python3 - "$TDIR" <<'PYEOF'
import socket, sys, time
TDIR = sys.argv[1]
MON = TDIR + '/qmon_wmm'
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3.0)
s.connect(MON)
time.sleep(0.5)

def cmd(c, gap=0.15):
    s.sendall((c + '\n').encode())
    time.sleep(gap)

def shot(name):
    cmd('screendump %s/%s.ppm' % (TDIR, name), 0.8)

def move(dx, dy):
    # HMP mouse_move is RELATIVE; PS/2 packets clamp ~255, so chunk <=200
    xs, ys = [], []
    while dx:
        c = max(-200, min(200, dx)); xs.append(c); dx -= c
    while dy:
        c = max(-200, min(200, dy)); ys.append(c); dy -= c
    n = max(len(xs), len(ys), 1)
    xs += [0] * (n - len(xs)); ys += [0] * (n - len(ys))
    for x, y in zip(xs, ys):
        cmd('mouse_move %d %d' % (x, y))

def goto(x, y):
    for _ in range(12):        # pin pointer to (0,0) regardless of start
        move(-300, -300)
    time.sleep(0.3)
    move(x, y)
    time.sleep(0.3)

def click(x, y):
    goto(x, y)
    cmd('mouse_button 1', 0.6)
    cmd('mouse_button 0', 0.6)

shot('wm_base')            # A0: baseline, Shell focused by design
click(60, 8)               # A: Apps title bar -> focus moves to Apps
shot('wm_focus')
click(630, 266)            # B: KLOG close button -> hides
shot('wm_close')
click(185, 473)            # C: taskbar "Log" button -> returns focused
shot('wm_taskbar')
goto(300, 266)             # D: drag KLOG title up by 60
cmd('mouse_button 1', 0.6)
move(0, -60)
cmd('mouse_button 0', 0.6)
shot('wm_drag')
s.close()
print("INTERACT_DONE")
PYEOF
[ $? -eq 0 ] || { echo "INTERACT_FAILED"; FAIL=1; }
sleep 2

echo "=== 4) pixel assertions ==="
python3 - "$TDIR" <<'PYEOF'
import sys
TDIR = sys.argv[1]
fail = 0

def load(name):
    data = open(TDIR + '/' + name + '.ppm', 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

def row_class(pix, w, y, x0, x1):
    cnt = {'blue': 0, 'cyan': 0, 'white': 0, 'n': 0}
    base = (y * w + x0) * 3
    for x in range(x0, x1):
        i = base + (x - x0) * 3
        r, g, b = pix[i], pix[i+1], pix[i+2]
        cnt['n'] += 1
        if r > 200 and g > 200 and b > 200:
            cnt['white'] += 1
        elif b > r + 40 and b > g + 20 and b > 80:
            cnt['blue'] += 1
        elif r < 90 and 90 < g < 210 and 90 < b < 210 and abs(g - b) < 45:
            cnt['cyan'] += 1
    return cnt

def check(name, cond, desc):
    global fail
    print(("  OK  " if cond else "  FAIL") + " %s: %s" % (name, desc))
    if not cond:
        fail = 1

# A0 baseline: Shell title focused(blue) by design, Apps title gray
w, h, pix = load('wm_base')
c = row_class(pix, w, 8, 10, 112)
check('base', c['blue'] < 20, "Apps title gray=%d blue" % c['blue'])
c = row_class(pix, w, 8, 140, 600)
check('base', c['blue'] > 200, "Shell title focused blue=%d/%d" % (c['blue'], c['n']))

# A focus click on Apps title: colors flipped
w, h, pix = load('wm_focus')
c = row_class(pix, w, 8, 10, 112)
check('focus', c['blue'] > 60, "Apps title focused blue=%d/%d" % (c['blue'], c['n']))
c = row_class(pix, w, 8, 140, 600)
check('focus', c['blue'] < 20, "Shell title lost blue=%d blue" % c['blue'])

# B close click: KLOG area = wallpaper(cyan), Shell intact(white)
w, h, pix = load('wm_close')
c = row_class(pix, w, 350, 200, 560)
check('close', c['cyan'] > 300, "KLOG area cyan=%d/%d" % (c['cyan'], c['n']))
c = row_class(pix, w, 100, 200, 560)
check('close', c['white'] > 200, "Shell intact white=%d/%d" % (c['white'], c['n']))

# C taskbar Log: KLOG back, focused title, white content
w, h, pix = load('wm_taskbar')
c = row_class(pix, w, 266, 140, 610)
check('taskbar', c['blue'] > 250, "KLOG title blue=%d/%d" % (c['blue'], c['n']))
c = row_class(pix, w, 350, 200, 560)
check('taskbar', c['white'] > 200, "KLOG content white=%d/%d" % (c['white'], c['n']))

# D drag up 60: new title row blue, old bottom band exposed cyan
w, h, pix = load('wm_drag')
c = row_class(pix, w, 206, 140, 610)
check('drag', c['blue'] > 250, "KLOG title@206 blue=%d/%d" % (c['blue'], c['n']))
c = row_class(pix, w, 430, 140, 610)
check('drag', c['cyan'] > 300, "old band@430 cyan=%d/%d" % (c['cyan'], c['n']))
c = row_class(pix, w, 100, 200, 560)
check('drag', c['white'] > 200, "Shell intact white=%d/%d" % (c['white'], c['n']))

print("PIXELS_OK" if not fail else "PIXELS_FAIL")
sys.exit(0 if not fail else 1)
PYEOF
[ $? -eq 0 ] || FAIL=1

kill "$QPID" 2>/dev/null
sleep 1

echo "=== 5) serial log sanity ==="
python3 tools/dump_serial.py "$TDIR/serial_wmm.log" 4000000 2>/dev/null | \
  grep -aqE "EXCEPTION|PANIC" && { echo "exception/panic in log"; FAIL=1; }

if [ "$FAIL" -eq 0 ]; then
    echo "WM_MOUSE_OK"
    exit 0
else
    echo "WM_MOUSE_FAILED"
    exit 1
fi
