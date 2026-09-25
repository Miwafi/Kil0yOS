#!/usr/bin/env python3
"""Panic-path visual test: types 'painme' at the shell, waits for the
panic screen, then grabs a QEMU screendump and converts it to PNG.
Usage: python3 tools/panic_screenshot.py out.png"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/panic_shot.png"
LOG = "/tmp/panic_shot.log"
QLOG = "/tmp/panic_shot.qemu"
SOCK = "/tmp/panic_shot.sock"
PPM = "/tmp/panic_shot.ppm"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(REPO)

ISO = "/tmp/kil0yos_test.iso"
shutil.copyfile("build/kil0yos.iso", ISO)

for f in (LOG, QLOG, SOCK, PPM, OUT):
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


def ppm_to_png(ppm_path: str, png_path: str) -> None:
    with open(ppm_path, "rb") as f:
        data = f.read()
    # P6 header: magic, whitespace/comment separated tokens
    toks, pos = [], 2
    while len(toks) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        toks.append(data[start:pos])
    pos += 1  # single whitespace after maxval
    w, h = int(toks[0]), int(toks[1])
    pix = data[pos:pos + w * h * 3]

    raw = b"".join(b"\x00" + pix[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t: bytes, d: bytes) -> bytes:
        return (struct.pack(">I", len(d)) + t + d +
                struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) +
           chunk(b"IEND", b""))
    with open(png_path, "wb") as f:
        f.write(png)
    print("PNG written:", png_path, "(%dx%d)" % (w, h))


try:
    n = -1
    for i in range(8):               # adaptive boot wait (cold-start warmup)
        time.sleep(2)
        n = os.path.getsize(LOG)
        print("t=%2ds serial=%d" % ((i + 1) * 2, n), flush=True)
        if n > 0:
            break
    if n <= 0:
        print("FAIL: guest produced no serial output during boot")
        sys.exit(1)

    time.sleep(2)                    # let the shell prompt settle
    type_cmd("painme")
    time.sleep(2)                    # panic prints + dump starts
    # wait for the memory dump to finish (TCG: ~0.25 MB/s polled UART)
    for _ in range(40):
        with open(LOG) as f:
            if "MEMORY DUMP END" in f.read():
                break
        time.sleep(1)
    mon("screendump " + PPM)
    time.sleep(1)
    with open(LOG) as f:
        log = f.read()
    print("panic in serial:", "KERNEL PANIC" in log)
    ppm_to_png(PPM, OUT)
finally:
    qemu.terminate()
    try:
        qemu.wait(5)
    except subprocess.TimeoutExpired:
        qemu.kill()

sys.exit(0 if os.path.exists(OUT) and os.path.getsize(OUT) > 0 else 1)
