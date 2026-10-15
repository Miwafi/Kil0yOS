#!/usr/bin/env python3
"""QEMU acceptance test for P1.3 (clone/futex/TLS): runs /bin/hello-threads,
a musl-static program that spawns 4 REAL threads via pthread_create,
increments a mutex-guarded counter, waits on a condvar and joins them all.
Must print 'thr: OK' and exit cleanly - a broken clone, futex rewrite,
TLS switch or thread-exit path hangs or faults instead.
Run inside WSL from the repo root:  python3 tools/threads_qemu_test.py
"""
import os
import socket
import subprocess
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon_threads.sock'
SERIAL = '/tmp/serial_threads.log'

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
        stdout=open('/tmp/qemu_threads.log', 'w'), stderr=subprocess.STDOUT)
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

    print('>>> hello-threads')
    type_keys(s, 'hello-threads')
    mon(s, 'sendkey ret')

    # 4 workers x 25 rounds of (mutex + usleep 1ms) + joins; allow slack
    time.sleep(25)
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
    if 'thr: OK' not in log:
        print('FAIL: hello-threads did not report success')
        ok = False
    if 'counter=100' not in log:
        print('FAIL: mutex-protected counter wrong or missing')
        ok = False
    if 'joined 3' not in log:
        print('FAIL: pthread_join incomplete')
        ok = False
    if 'Kernel' in log and 'panic' in log:
        print('FAIL: kernel panic in log')
        ok = False
    print('PASS' if ok else 'FAIL')
    import sys as _s
    _s.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
