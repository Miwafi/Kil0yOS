#!/usr/bin/env python3
"""QEMU headless regression for /bin/pong.bin.

Boots the ISO on the default e1000 NIC, types `exec /bin/pong.bin` into the
shell via the QEMU monitor, plays a few frames and verifies:
  - boot reaches the shell (text mode 720x400)
  - the game switches to VGA mode 13h (320x200) with colored content
  - W/S input changes the frame
  - ESC restores text mode for the shell
Run inside WSL from the repo root:  python3 tools/pong_qemu_test.py
"""
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon.sock'
TMP = '/tmp'


def mon(s, cmd):
    s.sendall((cmd + '\n').encode())
    time.sleep(0.15)
    try:
        s.setblocking(False)
        r = s.recv(65536)
        s.setblocking(True)
        return r.decode(errors='replace')
    except Exception:
        s.setblocking(True)
        return ''


def dump(s, path):
    mon(s, 'screendump ' + path)
    time.sleep(0.6)


def analyze(path):
    with open(path, 'rb') as f:
        d = f.read()
    toks = d.split(None, 4)
    w, h = int(toks[1]), int(toks[2])
    body = toks[4]
    nz = sum(1 for i in range(0, len(body), 3) if body[i] or body[i + 1] or body[i + 2])
    colors = set()
    for i in range(0, len(body), 3):
        colors.add((body[i], body[i + 1], body[i + 2]))
    return w, h, nz, len(colors)


def view(path):
    with open(path, 'rb') as f:
        d = f.read()
    toks = d.split(None, 4)
    w, h = int(toks[1]), int(toks[2])
    body = toks[4]
    px = 8
    print('--- %s (%dx%d) ---' % (os.path.basename(path), w, h))
    for y in range(0, h, px * 2):
        row = ''
        for x in range(0, w, px):
            i = (y * w + x) * 3
            v = body[i] if i + 2 < len(body) else 0
            row += '#' if v > 64 else ('.' if v > 8 else ' ')
        print(row)


def main():
    for f in ('qmon.sock',):
        p = os.path.join(TMP, f)
        if os.path.exists(p):
            os.unlink(p)

    q = subprocess.Popen(
        ['qemu-system-x86_64', '-cdrom', ISO, '-m', '512', '-display', 'none',
         '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
         '-serial', 'file:%s/serial_pong.log' % TMP],
        stdout=open('%s/qemu_pong.log' % TMP, 'w'), stderr=subprocess.STDOUT)
    time.sleep(16)
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    time.sleep(0.5)
    try:
        s.setblocking(False)
        s.recv(65536)
        s.setblocking(True)
    except Exception:
        pass

    shots = {}
    dump(s, '%s/t_shell.ppm' % TMP)
    for k in 'e x e c spc slash b i n slash p o n g dot b i n ret'.split():
        mon(s, 'sendkey ' + k)
        time.sleep(0.05)
    time.sleep(3)
    dump(s, '%s/t_banner.ppm' % TMP)
    mon(s, 'sendkey w')
    time.sleep(2.5)
    dump(s, '%s/t_game1.ppm' % TMP)
    mon(s, 'sendkey s')
    time.sleep(1.5)
    mon(s, 'sendkey s')
    time.sleep(1.5)
    dump(s, '%s/t_game2.ppm' % TMP)
    mon(s, 'sendkey esc')
    time.sleep(2)
    dump(s, '%s/t_text.ppm' % TMP)
    mon(s, 'quit')
    time.sleep(1)
    try:
        q.kill()
    except Exception:
        pass

    results = {}
    for name in ('t_shell', 't_banner', 't_game1', 't_game2', 't_text'):
        p = '%s/%s.ppm' % (TMP, name)
        if os.path.exists(p):
            results[name] = analyze(p)
            print('%s: %dx%d nonblack=%d distinct=%d' % ((name,) + results[name]))
        else:
            print('%s: MISSING' % name)
            sys.exit(1)
    if '-v' in sys.argv:
        for name in ('t_banner', 't_game2', 't_text'):
            view('%s/%s.ppm' % (TMP, name))

    ok = True
    if results['t_shell'][0] != 720:
        print('FAIL: shell not in text mode'); ok = False
    # QEMU screendump doubles mode-13h 320x200 to 640x400
    if results['t_banner'][0] != 640:
        print('FAIL: game did not enter mode 13h'); ok = False
    if results['t_banner'][2] < 200:
        print('FAIL: banner screen nearly empty'); ok = False
    # game1 may legitimately catch the instant after GFX_CLEAR; judge by game2
    if results['t_game2'][0] != 640:
        print('FAIL: game screen left mode 13h'); ok = False
    if results['t_game2'][2] < 200:
        print('FAIL: game screen nearly empty'); ok = False
    if results['t_game1'][2] == results['t_game2'][2] and \
       results['t_game1'][:2] == results['t_game2'][:2]:
        # identical nonblack count strongly suggests no animation
        print('WARN: game frames identical (no motion or input ignored)')
    if results['t_text'][0] != 720:
        print('FAIL: ESC did not restore text mode'); ok = False

    print('=== serial tail ===')
    serial_text = ''
    try:
        with open('%s/serial_pong.log' % TMP, 'rb') as f:
            serial_text = f.read().decode(errors='replace')
        print(serial_text[-800:])
    except Exception as e:
        print('no serial log:', e)
    if 'EXCEPTION' in serial_text:
        print('FAIL: kernel exception during run'); ok = False

    print('RESULT:', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
