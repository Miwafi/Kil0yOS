#!/usr/bin/env python3
"""Download hello.deb, extract data.tar.zst, save to /tmp/ztest/real.zst."""
import gzip, os, urllib.request
url = ("https://mirrors.aliyun.com/ubuntu/dists/jammy/"
       "main/binary-amd64/Packages.gz")
idx = gzip.decompress(urllib.request.urlopen(url, timeout=60).read())
target = None
para = {}
for line in idx.decode("utf-8", "replace").splitlines():
    if not line:
        if para.get("Package") == "hello":
            target = para.get("Filename")
            break
        para = {}
    elif ": " in line:
        k, v = line.split(": ", 1)
        para[k] = v
assert target, "hello not found"
deb = urllib.request.urlopen("https://mirrors.aliyun.com/ubuntu/" + target,
                             timeout=60).read()
open("/tmp/ztest/hello.deb", "wb").write(deb)
os.system("cd /tmp/ztest && ar x hello.deb")
for fn in os.listdir("/tmp/ztest"):
    if fn.startswith("data.tar."):
        data = open("/tmp/ztest/" + fn, "rb").read()
        open("/tmp/ztest/real.zst", "wb").write(data)
        print(fn, len(data))
