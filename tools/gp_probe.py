#!/usr/bin/env python3
"""One-shot busybox exec test with QEMU int logging to catch the #GP frame."""
import os, socket, subprocess, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon_gp.sock'

KEYMAP = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus'}

def mon(s, cmd):
    s.sendall((cmd + '\n').encode())
    time.sleep(0.15)
    try:
        s.setblocking(False); r = s.recv(65536); s.setblocking(True)
        return r.decode(errors='replace')
    except Exception:
        s.setblocking(True); return ''

def type_keys(s, text):
    for ch in text:
        mon(s, 'sendkey ' + KEYMAP.get(ch, ch)); time.sleep(0.04)

if os.path.exists(SOCK): os.unlink(SOCK)
q = subprocess.Popen(
    ['qemu-system-x86_64', '-cdrom', ISO, '-m', '512', '-display', 'none',
     '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
     '-serial', 'file:/tmp/serial_gp.log', '-d', 'int', '-D', '/tmp/int_gp.log'],
    stdout=open('/tmp/qemu_gp.log', 'w'), stderr=subprocess.STDOUT)
time.sleep(18)
s = socket.socket(socket.AF_UNIX); s.connect(SOCK); time.sleep(0.5)
try:
    s.setblocking(False); s.recv(65536); s.setblocking(True)
except Exception: pass

type_keys(s, 'busybox')
mon(s, 'sendkey ret')
time.sleep(10)
mon(s, 'quit'); time.sleep(1)
q.kill()

with open('/tmp/int_gp.log', errors='replace') as f:
    log = f.read()
idx = log.rfind('v=0d')
if idx < 0:
    idx = log.rfind('v=0e')
print(log[max(0, idx - 200): idx + 1200])
