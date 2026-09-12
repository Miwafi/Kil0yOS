#!/usr/bin/env python3
"""Headless desktop-layout acceptance test.

Boots build/kil0yos.iso in QEMU (BIOS path), drives the shell over the
telnet monitor with sendkey, takes screendumps of the new desktop layout
(main / Win menu popup / function switch / klog pane) and renders them as
ASCII so the layout can be verified without a display.

Usage: python3 tools/gui_screentest.py [iso]
Outputs: /tmp/dt_*.ppm + ASCII rendering on stdout.
"""
import re
import socket
import subprocess
import sys
import time

ISO = sys.argv[1] if len(sys.argv) > 1 else "build/kil0yos.iso"
QMON = ("127.0.0.1", 4445)
SER = "/tmp/kil0y_ser.log"


def wait_serial(pat, timeout=120):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(SER, "r", errors="replace") as f:
                if pat in f.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False


class Monitor:
    def __init__(self, addr):
        self.s = socket.create_connection(addr, timeout=10)
        time.sleep(0.5)
        self.s.recv(65536)

    def cmd(self, c, delay=0.2):
        self.s.sendall((c + "\n").encode())
        time.sleep(delay)
        try:
            return self.s.recv(65536).decode(errors="replace")
        except socket.timeout:
            return ""

    def type(self, text, delay=0.12):
        for ch in text:
            name = {" ": "spc", "\n": "ret", "-": "minus"}.get(ch, ch)
            self.cmd("sendkey " + name, delay)

    def dump(self, path, delay=0.4):
        self.cmd("screendump " + path, delay)

    def close(self):
        try:
            self.cmd("quit", 0.3)
        except Exception:
            pass
        self.s.close()


def ppm_ascii(path, cols=60):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    if not m:
        return "(not a PPM: %s)" % path
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    blk = max(1, w // cols)
    rows = []
    for by in range(0, h - blk + 1, blk):
        line = []
        for bx in range(0, w - blk + 1, blk):
            tot = 0
            n = 0
            for y in range(by, by + blk, 2):
                base = (y * w + bx) * 3
                for x in range(0, blk, 2):
                    o = base + x * 3
                    if o + 2 < len(px):
                        tot += (px[o] + px[o + 1] + px[o + 2]) // 3
                    n += 1
            v = tot // max(1, n)
            line.append(" .:-=+*#%@"[min(9, v * 10 // 256)])
        rows.append("".join(line))
    return "\n".join(rows)


def blue_pixels(path):
    """Count blue-ish pixels (popup border / selection bar / hints)."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    if not m:
        return -1
    w, h = int(m.group(1)), int(m.group(2))
    px = data[m.end():]
    n = 0
    for o in range(0, len(px) - 2, 3):
        r, g, b = px[o], px[o + 1], px[o + 2]
        if b > 120 and b > r + 40 and b > g + 40:
            n += 1
    return n


def main():
    subprocess.run(["rm", "-f", SER], check=False)
    qemu = subprocess.Popen([
        "qemu-system-x86_64", "-cdrom", ISO, "-m", "512M",
        "-display", "none", "-serial", "file:" + SER,
        "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % QMON[1],
    ])
    try:
        print("waiting for shell...", flush=True)
        ok = wait_serial("Welcome to Kil0yOS!", 150)
        print("shell up:", ok, flush=True)
        time.sleep(2)

        m = Monitor(QMON)
        m.type("gui\n")
        print("desktop:", wait_serial("desktop] loop enter", 30), flush=True)
        time.sleep(1.5)
        m.dump("/tmp/dt_main.ppm")

        # Win-key menu popup (QEMU cannot inject the real Win/menu E0 codes;
        # F12 is special-cased in the driver to the same KEY_WIN code)
        m.cmd("sendkey f12", 0.5)
        m.dump("/tmp/dt_menu.ppm")

        # navigate to System and apply
        m.cmd("sendkey down", 0.3)
        m.cmd("sendkey down", 0.3)
        m.type("\n")
        print("switch:", wait_serial("func -> ", 15), flush=True)
        time.sleep(1)
        m.dump("/tmp/dt_system.ppm")

        # shell still focused in the right-top pane: run ls
        m.type("echo hi-from-shell\n")
        print("shell echo:", wait_serial("hi-from-shell", 15), flush=True)
        time.sleep(0.5)
        m.dump("/tmp/dt_final.ppm")

        m.close()
    finally:
        time.sleep(0.5)
        qemu.terminate()

    for name in ("dt_main", "dt_menu", "dt_system", "dt_final"):
        print("\n===== %s (blue=%d) =====" % (name, blue_pixels("/tmp/%s.ppm" % name)),
              flush=True)
        print(ppm_ascii("/tmp/%s.ppm" % name, cols=80))

    print("\n===== serial tail =====", flush=True)
    try:
        with open(SER, "r", errors="replace") as f:
            lines = f.read().splitlines()
        for ln in lines[-40:]:
            print(ln)
    except FileNotFoundError:
        print("(no serial log)")


if __name__ == "__main__":
    main()
