#!/usr/bin/env python3
"""QEMU headless acceptance test for Phase 2 (ext2 root + memfs /tmp).

Run inside WSL from the repo root:  python3 tools/ext2_qemu_test.py

Boot 1: -hda build/ext2.img (raw ext2 with /bin/busybox)
        - verifies the ext2 mount markers in the boot log
        - runs busybox from the ext2 disk (ELF read-through)
        - creates/reads a file under /tmp (memory fs) via builtins
Boot 2 (reboot, same disk image):
        - /bin content must still be there (persistence, M2)
        - /tmp content must be gone (overlay volatility)
"""
import os
import socket
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
DISK = os.path.join(REPO, 'build', 'ext2.img')
SOCK = '/tmp/qmon_ext2.sock'
SERIAL = '/tmp/serial_ext2.log'

BOOT_WAIT = 20
CMD_WAIT = 8

COMMANDS = [
    'ls /',            # builtin: bin + lost+found (ext2) + tmp (memfs)
    'ls /bin',         # busybox from the ext2 disk
    'echo hello-from-ext2',  # busybox echo: ELF exec read from ext2
    'touch /tmp/t1',   # builtin: MEM overlay write under memfs /tmp
    'ls /tmp',         # t1 (+ .selftest) must appear
]

MARKERS_BOOT1 = [
    '[ext2] probe ok',
    '[fs] ext2 mounted at /',
    '[fs] memfs mounted at /tmp',
    '[fs] memfs self-test ok',
]
MARKERS_BOOT2 = [
    '[fs] ext2 mounted at /',
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


def boot(commands, markers, serial_log, tag):
    for p in (SOCK, serial_log):
        if os.path.exists(p):
            os.unlink(p)

    q = subprocess.Popen(
        ['qemu-system-x86_64', '-cdrom', ISO, '-hda', DISK,
         '-m', '512', '-display', 'none',
         '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
         '-serial', 'file:' + serial_log],
        stdout=open('/tmp/qemu_ext2_%s.log' % tag, 'w'),
        stderr=subprocess.STDOUT)
    try:
        time.sleep(BOOT_WAIT)
        s = socket.socket(socket.AF_UNIX)
        s.connect(SOCK)
        time.sleep(0.5)
        try:
            s.setblocking(False)
            s.recv(65536)
            s.setblocking(True)
        except Exception:
            pass

        for cmd in commands:
            print('>>> ' + cmd)
            type_keys(s, cmd)
            mon(s, 'sendkey ret')
            time.sleep(CMD_WAIT)
        mon(s, 'quit')
        time.sleep(1)
    finally:
        try:
            q.kill()
        except Exception:
            pass

    with open(serial_log, 'r', errors='replace') as f:
        log = f.read()

    print('===== serial log (%s) =====' % tag)
    print(log)

    ok = True
    for m in markers:
        if m not in log:
            print('MISSING MARKER (%s): %s' % (tag, m))
            ok = False
    return ok, log


def main():
    if not os.path.exists(DISK):
        print('build/ext2.img missing - run tools/make_ext2_disk.sh first')
        return 1

    ok1, log1 = boot(COMMANDS, MARKERS_BOOT1, SERIAL, 'boot1')

    # boot-specific assertions
    if 'lost+found' not in log1:
        print('MISSING MARKER (boot1): lost+found in ls /')
        ok1 = False
    if 'hello-from-ext2' not in log1:
        print('MISSING MARKER (boot1): busybox echo output')
        ok1 = False
    if 't1' not in log1.split('>>> ls /tmp')[-1]:
        print('MISSING MARKER (boot1): t1 in ls /tmp')
        ok1 = False

    # reboot: persistence on disk, volatility of the memory layer
    ok2, log2 = boot(['ls /bin', 'ls /tmp'], MARKERS_BOOT2, SERIAL, 'boot2')
    if 'busybox' not in log2.split('>>> ls /bin')[-1]:
        print('MISSING MARKER (boot2): busybox still in /bin after reboot')
        ok2 = False
    tail = log2.split('>>> ls /tmp')[-1]
    if 't1' in tail:
        print('UNEXPECTED (boot2): t1 survived reboot in /tmp')
        ok2 = False

    print('===== result =====')
    print('boot1 (ext2 mount + memfs):', 'PASS' if ok1 else 'FAIL')
    print('boot2 (reboot persistence):', 'PASS' if ok2 else 'FAIL')
    return 0 if (ok1 and ok2) else 1


if __name__ == '__main__':
    sys.exit(main())
