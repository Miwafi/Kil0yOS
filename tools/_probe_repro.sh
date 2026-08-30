#!/usr/bin/env python3
# temp: exact Popen clone of ext2_qemu_test.py boot(), but NO keystrokes.
import os, subprocess, time
REPO = '/mnt/c/Users/19423/Desktop/Programs/Kil0yOS'
ISO = os.path.join(REPO, 'build', 'kil0yos.iso')
DISK = os.path.join(REPO, 'build', 'ext2.img')
SOCK = '/tmp/qmon_bb2.sock'
SERIAL = '/tmp/serial_bb2.log'
for p in (SOCK, SERIAL):
    if os.path.exists(p): os.unlink(p)
q = subprocess.Popen(
    ['qemu-system-x86_64', '-cdrom', ISO, '-hda', DISK,
     '-m', '512', '-display', 'none',
     '-monitor', 'unix:%s,server,nowait' % SOCK, '-no-reboot',
     '-serial', 'file:' + SERIAL],
    stdout=open('/tmp/qemu_bb2.log', 'w'), stderr=subprocess.STDOUT)
time.sleep(40)
try: q.kill()
except Exception: pass
log = open(SERIAL, errors='replace').read()
print('EXCEPTION count:', log.count('EXCEPTION'))
idx = log.find('EXCEPTION')
print(log[max(0,idx-300):idx+1500] if idx >= 0 else log[-600:])
