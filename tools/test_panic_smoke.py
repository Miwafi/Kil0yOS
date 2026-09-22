#!/usr/bin/env python3
"""Panic-path smoke test: types 'painme' at the shell, which calls
panic() directly. PASS = serial log shows KERNEL PANIC with the painme
message. Run with no build flags - the command ships in every image.
"""
import os
import shutil
import socket
import subprocess
import sys
import time

LOG = "/tmp/panic_smoke.log"
QLOG = "/tmp/panic_smoke.qemu"
SOCK = "/tmp/panic_smoke.sock"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)

# Copy the ISO onto WSL-native ext4 first: a cold-started wsl.exe session
# may not have the /mnt/c 9p backend ready, and qemu then hangs reading
# the image (guest never runs, serial file: stays 0 bytes).
ISO = "/tmp/kil0yos_test.iso"
shutil.copyfile("build/kil0yos.iso", ISO)

for f in (LOG, QLOG, SOCK):
    try:
        os.unlink(f)
    except FileNotFoundError:
        pass

qemu = subprocess.Popen(
    [
        "qemu-system-x86_64", "-cdrom", ISO, "-m", "512M",
        "-display", "none", "-serial", "file:" + LOG,
        "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
        "-no-reboot", "-monitor", "unix:" + SOCK + ",server,nowait",
    ],
    stdin=subprocess.DEVNULL,
    stdout=open(QLOG, "w"),
    stderr=subprocess.STDOUT,
)


def mon(cmd: str) -> None:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.settimeout(2)
    try:
        s.sendall((cmd + "\n").encode())
        time.sleep(0.25)
        s.recv(4096)
    except socket.timeout:
        pass
    finally:
        s.close()


def type_cmd(text: str) -> None:
    for ch in text:
        mon("sendkey " + ch)
        time.sleep(0.15)
    mon("sendkey ret")


def serial_bytes() -> int:
    try:
        return os.path.getsize(LOG)
    except FileNotFoundError:
        return -1


try:
    n = -1
    for i in range(8):               # adaptive boot wait (cold-start warmup)
        time.sleep(2)
        n = serial_bytes()
        print("t=%2ds serial=%d" % ((i + 1) * 2, n), flush=True)
        if n > 0:
            break
    if n <= 0:
        print("FAIL: guest produced no serial output during boot")
        sys.exit(1)

    time.sleep(2)                    # let the shell prompt settle
    type_cmd("painme")
    time.sleep(5)                    # panic prints + halts immediately
    with open(LOG) as f:
        log = f.read()
    print("serial bytes:", os.path.getsize(LOG))

    if "KERNEL PANIC" in log and "painme: test panic requested" in log:
        print("PASS: painme triggered KERNEL PANIC")
        for line in log.splitlines():
            if any(k in line for k in ("painme", "PANIC", "Message")):
                print("  |", line.strip())
        rc = 0
    else:
        print("FAIL: no panic for painme")
        print("--- serial tail ---")
        print(log[-600:])
        rc = 1
finally:
    qemu.terminate()
    try:
        qemu.wait(5)
    except subprocess.TimeoutExpired:
        qemu.kill()
    print("--- qemu stderr ---")
    print(open(QLOG).read())

sys.exit(rc)
