#!/usr/bin/env python3
"""Fetch gcc-12-base deb, extract control.tar.zst + data.tar.zst to /tmp/ztest."""
import os, subprocess, tempfile, urllib.request
url = ("https://mirrors.aliyun.com/ubuntu/pool/main/g/gcc-12/"
       "gcc-12-base_12-20220319-1ubuntu1_amd64.deb")
deb = urllib.request.urlopen(url, timeout=60).read()
os.makedirs("/tmp/ztest", exist_ok=True)
open("/tmp/ztest/gcc12base.deb", "wb").write(deb)
with tempfile.TemporaryDirectory() as d:
    open(os.path.join(d, "g.deb"), "wb").write(deb)
    subprocess.run(["ar", "x", "g.deb"], cwd=d, check=True)
    for fn in os.listdir(d):
        if fn.startswith(("control.tar", "data.tar")):
            data = open(os.path.join(d, fn), "rb").read()
            open("/tmp/ztest/g12_" + fn, "wb").write(data)
            print(fn, len(data))
