#!/usr/bin/env python3
"""Type a string into the guest via a QEMU HMP monitor unix socket.
Appends Enter automatically. Usage: drive_keys.py <text> [monitor-socket]

Sends all sendkey commands first and reads monitor replies only at the
end - per-key draining makes each key take seconds and blows up timeouts.
"""
import socket, sys, time, os

MON = sys.argv[2] if len(sys.argv) > 2 else os.environ.get('QMON', '/tmp/qmon')

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3.0)
s.connect(MON)
time.sleep(0.3)

KEYMAP = {' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', '_': 'shift-minus',
          ':': 'shift-semicolon', ';': 'semicolon', '=': 'equal',
          ',': 'comma', "'": 'apostrophe', '"': 'shift-apostrophe',
          '<': 'shift-comma', '>': 'shift-dot',
          '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '$': 'shift-4',
          '%': 'shift-5', '&': 'shift-7', '*': 'shift-8',
          '(': 'shift-9', ')': 'shift-0', '?': 'shift-slash'}
# Uppercase letters are shifted lowercase in QEMU sendkey names.
for _up in 'ABCDEFGHIJKLMNOPQRSTUVWXYZ':
    KEYMAP[_up] = 'shift-' + _up.lower()

for ch in sys.argv[1]:
    s.sendall(('sendkey ' + KEYMAP.get(ch, ch) + '\n').encode())
    time.sleep(0.12)

# Enter, then wait for the guest to chew through the queue before
# the next typing session opens a new connection.
s.sendall(b'sendkey ret\n')
time.sleep(0.12)

try:
    s.settimeout(1.0)
    while True:
        s.recv(4096)
except (socket.timeout, ConnectionError, OSError):
    # ConnectionError/OSError: the guest may power off mid-session (e.g.
    # an earlier command triggered shutdown) - that is not a typing
    # failure; all keys were already delivered above.
    pass
s.close()
print('typing done: %r' % sys.argv[1], flush=True)
