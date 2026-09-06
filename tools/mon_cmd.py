#!/usr/bin/env python3
"""Send a single HMP command via a QEMU monitor unix socket.
Usage: mon_cmd.py <command...> [monitor-socket]
Examples:
  mon_cmd.py mouse_move 10 6
  mon_cmd.py device_add usb-kbd,id=kbd0
  mon_cmd.py device_del kbd0
Protocol matches drive_keys.py: connect -> settle 0.3s -> send -> drain
replies until the guest or socket closes.
"""
import socket, sys, time, os

MON = None
args = sys.argv[1:]
if len(args) >= 2 and (args[-1].startswith('/') or os.sep in args[-1]):
    MON = args[-1]; args = args[:-1]
if not MON:
    MON = os.environ.get('QMON', '/tmp/qmon')
cmd = ' '.join(args)
assert cmd, "no command given"

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(3.0)
s.connect(MON)
time.sleep(0.3)
s.sendall((cmd + '\n').encode())
time.sleep(0.3)
try:
    s.settimeout(1.0)
    data = b''
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
except (socket.timeout, ConnectionError, OSError):
    pass
s.close()
print('mon_cmd done: %r' % cmd, flush=True)
