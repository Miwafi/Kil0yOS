#!/usr/bin/env python3
"""QEMU acceptance test for P1.4 (AF_UNIX): runs /bin/hello-unix, a musl
static program exercising socketpair, SCM_RIGHTS fd passing, pathname
bind/listen/connect/accept with SO_PEERCRED, and abstract sockets.
Must print 'ux: OK'. See tools/threads_qemu_test.py for the harness."""
import os
import socket
import subprocess
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon_unix.sock'
SERIAL = '/tmp/serial_unix.log'

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
        stdout=open('/tmp/qemu_unix.log', 'w'), stderr=subprocess.STDOUT)
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

    print('>>> hello-unix')
    type_keys(s, 'hello-unix')
    mon(s, 'sendkey ret')

    # fork + two servers + usleeps; generous slack
    time.sleep(20)
    mon(s, 'quit')
    time.sleep(1)
    try:
        q.kill()
    except Exception:
        pass

    with open(SERIAL, 'r', errors='replace') as f:
        log = f.read()
    print('===== serial log (tail) =====')
    print(log[-3000:])

    ok = True
    if 'ux: OK' not in log:
        print('FAIL: hello-unix did not report success')
        ok = False
    if 'FAIL' in log.split('ux: OK')[0] if 'ux: OK' in log else 'FAIL' in log:
        print('FAIL: ux FAIL markers present')
        ok = False
    if 'panic' in log.lower():
        print('FAIL: kernel panic in log')
        ok = False
    print('PASS' if ok else 'FAIL')
    import sys as _s
    _s.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
