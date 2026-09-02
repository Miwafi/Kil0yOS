#!/bin/bash
# Phase 4.x acceptance: zstd-compressed data.tar members end-to-end.
#  1) kilget update + install libc6  (real Ubuntu jammy deb, data.tar.zst,
#     multi-window HTTP download + SHA256 + dpkg unpack)
#  2) dpkg -i of hello.deb fetched via TFTP (data.tar.zst)
#  3) exec /usr/bin/hello, dpkg -l/-L/-r bookkeeping
# Usage (WSL): bash tools/accept_zst.sh
TDIR="$HOME/ktest"
mkdir -p "$TDIR"
rm -f "$TDIR/serial_zst.log" "$TDIR/qmon_zst"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

# fetch hello.deb (data.tar.zst) if not already present
if [ ! -f "$TDIR/hello.deb" ]; then
    python3 - <<'EOF'
import gzip, os, urllib.request
url = ("https://mirrors.aliyun.com/ubuntu/dists/jammy/"
       "main/binary-amd64/Packages.gz")
idx = gzip.decompress(urllib.request.urlopen(url, timeout=60).read())
target, para = None, {}
for line in idx.decode("utf-8", "replace").splitlines():
    if not line:
        if para.get("Package") == "hello":
            target = para.get("Filename"); break
        para = {}
    elif ": " in line:
        k, v = line.split(": ", 1); para[k] = v
assert target, "hello not found"
deb = urllib.request.urlopen("https://mirrors.aliyun.com/ubuntu/" + target,
                             timeout=60).read()
tdir = os.path.expanduser("~/ktest")
open(os.path.join(tdir, "hello.deb"), "wb").write(deb)
print("fetched", len(deb), "bytes")
EOF
fi
python3 - <<'EOF'
# sanity: confirm the data member really is .zst
import subprocess, os, tempfile
tdir = os.path.expanduser("~/ktest")
with tempfile.TemporaryDirectory() as d:
    subprocess.run(["cp", os.path.join(tdir, "hello.deb"), d], check=True)
    subprocess.run(["ar", "x", "hello.deb"], cwd=d, check=True)
    members = [f for f in os.listdir(d) if f.startswith("data.tar")]
    print("data members:", members)
    assert any(f.endswith(".zst") for f in members), "data.tar.zst expected"
EOF

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_zst.log" \
  -monitor unix:"$TDIR/qmon_zst",server,nowait \
  -netdev user,id=net0,tftp="$TDIR" -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

type_cmd() {
    timeout 20 python3 tools/drive_keys.py "$1" "$TDIR/qmon_zst" || echo "TYPING FAILED: $1"
}
# wait for a marker in the serial log (tail window), timeout in seconds
wait_marker() {
    local pat="$1" tmo="${2:-120}"
    local i=0
    while [ "$i" -lt "$tmo" ]; do
        if python3 tools/dump_serial.py "$TDIR/serial_zst.log" 2000000 2>/dev/null \
           | grep -aq "$pat"; then
            echo "[marker] $pat (${i}s)"; return 0
        fi
        sleep 1; i=$((i+1))
    done
    echo "TIMEOUT waiting for: $pat"; return 1
}

sleep 16
type_cmd "kilget update";                   wait_marker "index updated" 240
type_cmd "kilget install libc6";            wait_marker "install complete" 300
type_cmd "tftp 10.0.2.2 hello.deb /tmp/hello.deb"; wait_marker "tftp: installed" 60
type_cmd "dpkg -i /tmp/hello.deb";          wait_marker "installed hello" 60
type_cmd "dpkg -l"
sleep 3
type_cmd "dpkg -L hello"
sleep 3
type_cmd "exec /usr/bin/hello";             wait_marker "Hello, world" 30
type_cmd "dpkg -r hello"
sleep 6
type_cmd "exec /usr/bin/hello"
sleep 4
type_cmd "echo ZST_ALL_DONE"
sleep 4

kill "$QPID" 2>/dev/null
sleep 1
echo "=== zstd deb acceptance output ==="
python3 tools/dump_serial.py "$TDIR/serial_zst.log" 2000000 | \
  grep -aE 'dpkg|kilget|\[deb\]|\[tar\]|hello|Hello|ZST_ALL_DONE|PANIC|panic|failed|not implemented|decompress|TIMEOUT|TYPING' | head -80
