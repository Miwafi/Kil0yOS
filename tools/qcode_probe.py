#!/usr/bin/env python3
"""Probe QEMU's QKeyCode enum via QMP (find valid sendkey names)."""
import json
import socket
import subprocess
import time

q = subprocess.Popen(["qemu-system-x86_64", "-display", "none", "-serial", "null",
                      "-qmp", "tcp:127.0.0.1:4449,server,nowait"])
time.sleep(1)
s = socket.create_connection(("127.0.0.1", 4449), timeout=5)
f = s.makefile("rw")
f.readline()  # greeting
f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
f.write('{"execute":"query-qmp-schema"}\n'); f.flush()
schema = json.loads(f.readline())
for item in schema:
    if item.get("name") == "QKeyCode":
        print("QKeyCode values:")
        print(item.get("values"))
s.close()
q.terminate()
