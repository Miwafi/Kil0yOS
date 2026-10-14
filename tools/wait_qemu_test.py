#!/usr/bin/env python3
"""QEMU acceptance test for P1.1 (blocking syscalls + wait queues).

`busybox sleep 5` parks the process in nanosleep (wait queue + timer).
While parked, a second command is typed into the tty. Before P1.1 the
busy-wait ran with IF=0: ticks froze and PS/2 keystrokes were dropped.
After P1.1 the keystrokes must survive intact and be processed when the
parked sleep is woken by its timer.
Run inside WSL from the repo root:  python3 tools/wait_qemu_test.py
"""
import os
import socket
import subprocess
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon_wait.sock'
SERIAL = '/tmp/serial_wait.log'

KEYMAP = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus',
          '&': 'shift-7', "'": 'apostrophe', '=': 'equal'}


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


def type_keys(s, text):
    for ch in text:
        k = KEYMAP.get(ch, ch)
        mon(s, 'sendkey ' + k)
        time.sleep(0.04)


def main():
    for p in (SOCK, SERIAL):
        if os.path.exists(p):
            os.unlink(p)

    q = subprocess.Popen(
        ['qemu-system-x86_64', '-cdrom', ISO, '-m', '512', '-display', 'none',
         '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
         '-serial', 'file:' + SERIAL],
        stdout=open('/tmp/qemu_wait.log', 'w'), stderr=subprocess.STDOUT)
    time.sleep(18)
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    time.sleep(0.5)
    try:
        s.setblocking(False)
        s.recv(65536)
        s.setblocking(True)
    except Exception:
        pass

    # 1. Start a 5s sleep: the process parks (timer completes the frame)
    print('>>> busybox sleep 5')
    type_keys(s, 'busybox sleep 5')
    mon(s, 'sendkey ret')
    time.sleep(2)                       # it is parked now

    # 2. Type a command while parked (old code dropped these keystrokes)
    print('>>> echo alive-during-park')
    type_keys(s, 'echo alive-during-park')
    mon(s, 'sendkey ret')

    time.sleep(6)                       # sleep timer wakes, shell resumes
    mon(s, 'quit')
    time.sleep(1)
    try:
        q.kill()
    except Exception:
        pass

    with open(SERIAL, 'r', errors='replace') as f:
        log = f.read()
    print('===== serial log (tail) =====')
    print(log[-2500:])

    ok = True
    if 'alive-during-park' not in log:
        print('FAIL: keystrokes typed during park were lost / system frozen')
        ok = False
    if 'syntax error' in log or 'Kernel' in log and 'panic' in log:
        print('FAIL: shell error or kernel panic in log')
        ok = False
    print('PASS' if ok else 'FAIL')
    import sys as _s
    _s.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
