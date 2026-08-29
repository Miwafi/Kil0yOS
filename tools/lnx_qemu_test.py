#!/usr/bin/env python3
"""QEMU headless smoke test for Linux-ABI user programs (Phase 0).

Boots the ISO, types `exec /bin/mini` (and optionally `exec /bin/hello-lnx`)
into the shell via the QEMU monitor and prints the serial log so syscall
traces / exceptions can be analyzed.
Run inside WSL from the repo root:  python3 tools/lnx_qemu_test.py
"""
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon_lnx.sock'
SERIAL = '/tmp/serial_lnx.log'


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
    keymap = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}
    for ch in text:
        k = keymap.get(ch, ch)
        mon(s, 'sendkey ' + k)
        time.sleep(0.04)


def main():
    progs = sys.argv[1:] or ['/bin/mini']
    for p in (SOCK, SERIAL):
        if os.path.exists(p):
            os.unlink(p)

    extra = os.environ.get('QEMU_EXTRA', '').split()
    q = subprocess.Popen(
        ['qemu-system-x86_64', '-cdrom', ISO, '-m', '512', '-display', 'none',
         '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
         '-serial', 'file:' + SERIAL] + extra,
        stdout=open('/tmp/qemu_lnx.log', 'w'), stderr=subprocess.STDOUT)
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

    for prog in progs:
        type_keys(s, 'exec ' + prog)
        mon(s, 'sendkey ret')
        time.sleep(6)
        type_keys(s, '\n')  # no-op
        mon(s, 'sendkey ret')
        time.sleep(2)

    mon(s, 'quit')
    time.sleep(1)
    try:
        q.kill()
    except Exception:
        pass

    if os.path.exists('/tmp/int.log'):
        import re
        with open('/tmp/int.log', 'r', errors='replace') as f:
            log = f.read()
        # Only the tail: last CPU exception entries with register dumps
        idx = log.rfind('    RIP=')
        if idx > 0:
            start = log.rfind('v=08', 0, idx)
            if start < 0:
                start = max(0, idx - 3000)
            print('===== qemu int.log (tail) =====')
            print(log[start:idx + 800])
    print('===== serial log =====')
    with open(SERIAL, 'r', errors='replace') as f:
        print(f.read())


if __name__ == '__main__':
    main()
