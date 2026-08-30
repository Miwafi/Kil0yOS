#!/usr/bin/env python3
"""QEMU headless acceptance test for Phase 1 (busybox on Kil0yOS).

Boots the ISO and types a sequence of busybox invocations into the shell via
the QEMU monitor, then dumps the serial log.
Run inside WSL from the repo root:  python3 tools/bb_qemu_test.py
"""
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon_bb.sock'
SERIAL = '/tmp/serial_bb.log'

# command lines typed into the Kil0yOS shell (keep chars within the keymap)
COMMANDS = [
    'busybox',
    'busybox ls /',
    'busybox echo hello from busybox',
    'busybox mkdir /tmp',
    'busybox ls /',
]

KEYMAP = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}


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

    extra = os.environ.get('QEMU_EXTRA', '').split()
    q = subprocess.Popen(
        ['qemu-system-x86_64', '-cdrom', ISO, '-m', '512', '-display', 'none',
         '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
         '-serial', 'file:' + SERIAL] + extra,
        stdout=open('/tmp/qemu_bb.log', 'w'), stderr=subprocess.STDOUT)
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

    for cmd in COMMANDS:
        print('>>> ' + cmd)
        type_keys(s, cmd)
        mon(s, 'sendkey ret')
        time.sleep(8)

    mon(s, 'quit')
    time.sleep(1)
    try:
        q.kill()
    except Exception:
        pass

    print('===== serial log =====')
    with open(SERIAL, 'r', errors='replace') as f:
        print(f.read())


if __name__ == '__main__':
    main()
