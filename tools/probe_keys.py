#!/usr/bin/env python3
"""Probe which HMP sendkey names QEMU accepts. Starts a paused (-S) QEMU,
sends sendkey for each candidate name, prints replies."""
import subprocess, socket, os, time, sys

SOCK = '/tmp/qprobe'
try: os.unlink(SOCK)
except FileNotFoundError: pass

p = subprocess.Popen([
    'qemu-system-x86_64', '-S', '-m', '64M', '-display', 'none',
    '-monitor', 'unix:' + SOCK + ',server,nowait',
], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.5)

s = socket.socket(socket.AF_UNIX)
s.connect(SOCK)
s.settimeout(0.5)
time.sleep(0.3)
try:
    while True: s.recv(4096)
except socket.timeout:
    pass

names = sys.argv[1:] or ['sp', 'spc', 'space', '0', '1', '9', 'dot', 'minus',
                         'slash', 'ret', 't', 'b', 'shift']
for n in names:
    s.sendall(('sendkey %s\n' % n).encode())
    reply = b''
    deadline = time.time() + 2.0
    while time.time() < deadline:
        s.settimeout(0.3)
        try:
            reply += s.recv(4096)
        except socket.timeout:
            if b'(qemu)' in reply:
                break
        if b'(qemu)' in reply and b'sendkey' in reply:
            # prompt redraw complete
            if not reply.endswith(b'(qemu)'):
                pass
    txt = reply.decode(errors='replace')
    rej = ('unknown key' in txt) or ('parse error' in txt) or ('Expected' in txt)
    print('%-8s %s' % (n, 'REJECTED' if rej else 'OK'))
    sys.stdout.flush()

p.terminate()
s.close()
