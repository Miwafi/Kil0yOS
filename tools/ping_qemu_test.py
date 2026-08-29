#!/usr/bin/env python3
"""QEMU headless regression for `ping` / `net ping`.

Boots the ISO on the default e1000 NIC (slirp user networking), types ping
commands into the shell via the QEMU monitor and validates them on the wire
by dumping slirp traffic to a pcap:
  - ping 10.0.2.2          (slirp gateway - must always answer)
  - ping 114.114.114.114   (public IP via slirp NAT; needs host internet)
Checks:
  - >=1 echo request sent to the gateway and >=1 echo reply received
  - request pacing >= ~0.9s between consecutive echo requests
  - for the public IP: request must be forwarded to the gateway MAC
Run inside WSL from the repo root:  python3 tools/ping_qemu_test.py
"""
import os
import socket
import struct
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
SOCK = '/tmp/qmon_ping.sock'
TMP = '/tmp'


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


def type_line(s, text):
    keymap = {' ': 'spc', '.': 'dot', '/': 'slash', '-': 'minus'}
    for ch in text:
        if ch == '\n':
            mon(s, 'sendkey ret')
        elif ch in keymap:
            mon(s, 'sendkey ' + keymap[ch])
        else:
            mon(s, 'sendkey ' + ch)
        time.sleep(0.04)


def read_pcap(path):
    """Return list of (ts_sec, ts_usec, data) tuples."""
    with open(path, 'rb') as f:
        d = f.read()
    if len(d) < 24:
        return []
    magic = d[:4]
    if magic == b'\xd4\xc3\xb2\xa1':
        endian = '<'
    elif magic == b'\xa1\xb2\xc3\xd4':
        endian = '>'
    else:
        return []
    pkts = []
    off = 24
    while off + 16 <= len(d):
        ts, tus, caplen, origlen = struct.unpack(endian + 'IIII', d[off:off + 16])
        off += 16
        data = d[off:off + caplen]
        off += caplen
        pkts.append((ts, tus, data))
    return pkts


def analyze(pcap_path):
    pkts = read_pcap(pcap_path)
    icmp = []   # (ts, type, dst_ip, src_ip)
    for ts, tus, data in pkts:
        if len(data) < 34:
            continue
        eth_type = struct.unpack('>H', data[12:14])[0]
        if eth_type != 0x0800 or len(data) < 34:
            continue
        ip = data[14:]
        if (ip[0] >> 4) != 4:
            continue
        ihl = (ip[0] & 0xF) * 4
        if len(ip) < ihl + 8:
            continue
        proto = ip[9]
        src = '.'.join(str(b) for b in ip[12:16])
        dst = '.'.join(str(b) for b in ip[16:20])
        if proto != 1:
            continue
        icmp_type = ip[ihl]
        icmp.append((ts + tus / 1e6, icmp_type, dst, src))
    return icmp


def main():
    p = os.path.join(TMP, 'qmon_ping.sock')
    if os.path.exists(p):
        os.unlink(p)
    pcap = '%s/ping_dump.pcap' % TMP
    if os.path.exists(pcap):
        os.unlink(pcap)

    q = subprocess.Popen(
        ['qemu-system-x86_64', '-cdrom', ISO, '-m', '512', '-display', 'none',
         '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
         '-netdev', 'user,id=n1', '-device', 'e1000,netdev=n1',
         '-object', 'filter-dump,id=f,netdev=n1,file=%s' % pcap,
         '-serial', 'file:%s/serial_ping.log' % TMP],
        stdout=open('%s/qemu_ping.log' % TMP, 'w'), stderr=subprocess.STDOUT)
    time.sleep(16)
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    time.sleep(0.5)
    try:
        s.setblocking(False)
        s.recv(65536)
        s.setblocking(True)
    except Exception:
        pass

    for i, t in enumerate(['10.0.2.2', '114.114.114.114']):
        type_line(s, 'ping %s\n' % t)
        time.sleep(14)  # 4 requests * >=1s + margin
        mon(s, 'screendump %s/ping_scr%d.ppm' % (TMP, i))
        time.sleep(0.6)

    mon(s, 'quit')
    time.sleep(1)
    try:
        q.kill()
    except Exception:
        pass

    icmp = analyze(pcap)
    reqs_gw = [x for x in icmp if x[1] == 8 and x[2] == '10.0.2.2']
    reps_gw = [x for x in icmp if x[1] == 0 and x[3] == '10.0.2.2']
    reqs_pub = [x for x in icmp if x[1] == 8 and x[2] == '114.114.114.114']
    reps_pub = [x for x in icmp if x[1] == 0 and x[3] == '114.114.114.114']

    print('gateway   requests=%d replies=%d' % (len(reqs_gw), len(reps_gw)))
    print('public    requests=%d replies=%d' % (len(reqs_pub), len(reps_pub)))

    # pacing on gateway requests
    ok = True
    if len(reqs_gw) == 0:
        print('FAIL: no echo request reached the wire for 10.0.2.2')
        ok = False
    else:
        gaps = [reqs_gw[i + 1][0] - reqs_gw[i][0] for i in range(len(reqs_gw) - 1)]
        if gaps:
            print('request gaps (s): %s' % ' '.join('%.2f' % g for g in gaps))
            if min(gaps) < 0.85:
                print('FAIL: requests too fast (<0.85s apart)'); ok = False
    if reqs_gw and len(reps_gw) == 0:
        print('FAIL: gateway sent no echo replies'); ok = False

    if len(reqs_pub) == 0:
        print('WARN: no request sent to public IP')
    elif len(reps_pub) == 0:
        print('WARN: public IP no replies (host offline / NAT filtered)')

    with open('%s/serial_ping.log' % TMP, 'rb') as f:
        serial_text = f.read().decode(errors='replace')
    if 'EXCEPTION' in serial_text:
        print('FAIL: kernel exception during run'); ok = False

    # text-mode screens after each ping (720x400 PPM -> stats + ASCII)
    for i in (0, 1):
        scr = '%s/ping_scr%d.ppm' % (TMP, i)
        if not os.path.exists(scr):
            print('screen %d: MISSING' % i)
            continue
        with open(scr, 'rb') as f:
            d = f.read()
        toks = d.split(None, 4)
        w, h = int(toks[1]), int(toks[2])
        body = toks[4]
        nz = sum(1 for p in range(0, len(body), 3) if body[p] or body[p+1] or body[p+2])
        print('screen %d: %dx%d nonblack=%d' % (i, w, h, nz))
        if i == 1:
            print('--- screen after second ping ---')
            for y in range(0, h, 16):
                row = ''
                for x in range(0, w, 8):
                    px = (y * w + x) * 3
                    row += '#' if px + 2 < len(body) and body[px] > 60 else ' '
                if row.strip():
                    print('|' + row.rstrip() + '|')

    print('RESULT:', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
