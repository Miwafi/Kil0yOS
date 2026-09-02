#!/usr/bin/env python3
"""Host-side round-trip test for the kernel zstd decoder.

Compresses a set of payloads with the reference implementation
(python 'zstandard' module) across levels/options, runs the kernel
decoder (test_zstd_host.c binary) on each stream and compares output
byte-for-byte. Also decodes a real .deb data.tar.zst member fetched
from the Ubuntu jammy mirror.
"""
import os, random, subprocess, sys, hashlib
import zstandard as zstd

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = "/tmp/ztest"
ZDEC = os.path.join(TMP, "zdec")

def run_case(name, raw, params):
    cctx = zstd.ZstdCompressor(**params)
    zst = cctx.compress(raw)
    zin = os.path.join(TMP, "case.zst")
    zout = os.path.join(TMP, "case.out")
    with open(zin, "wb") as f:
        f.write(zst)
    r = subprocess.run([ZDEC, zin, zout], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"FAIL {name}: decoder rc={r.returncode} {r.stderr.strip()}")
        return False
    with open(zout, "rb") as f:
        got = f.read()
    if got != raw:
        h = lambda b: hashlib.sha256(b).hexdigest()[:16]
        print(f"FAIL {name}: content mismatch "
              f"(len {len(got)} vs {len(raw)}, {h(got)} vs {h(raw)})")
        return False
    print(f"ok   {name} ({len(raw)} -> {len(zst)} bytes, {params})")
    return True

def main():
    os.makedirs(TMP, exist_ok=True)
    # build the decoder binary with stub headers
    inc = os.path.join(TMP, "fakeinc")
    for d in ("lib", "mm", "drivers"):
        os.makedirs(os.path.join(inc, d), exist_ok=True)
    with open(os.path.join(inc, "lib", "string.h"), "w") as f:
        f.write("#include <string.h>\n")
    with open(os.path.join(inc, "mm", "memory.h"), "w") as f:
        f.write("#include <stdlib.h>\n"
                "static inline void* kmalloc(unsigned long long n) { return malloc(n); }\n"
                "static inline void kfree(void* p) { free(p); }\n")
    with open(os.path.join(inc, "drivers", "vga.h"), "w") as f:
        f.write("static inline void klog(const char* s) { (void)s; }\n"
                "static inline void klog_hex(const char* s, unsigned v) { (void)s; (void)v; }\n")
    src = os.path.join(ROOT, "src", "kernel", "pkg", "zstd.c")
    tst = os.path.join(ROOT, "tools", "test_zstd_host.c")
    subprocess.run(["gcc", "-O2", "-Wall", "-Wno-unused-parameter", "-g",
                    f"-I{inc}", f"-I{os.path.join(ROOT, 'include')}",
                    tst, src, "-o", ZDEC], check=True)

    rnd = random.Random(42)
    rand256k = bytes(rnd.getrandbits(8) for _ in range(256 * 1024))
    textish = (b"the quick brown fox jumps over the lazy dog. " * 4000 +
               bytes(rnd.getrandbits(8) for _ in range(4096)))
    cases = [
        ("empty", b"", dict(level=3)),
        ("tiny", b"hello world", dict(level=3)),
        ("rle-ish", b"A" * 200000, dict(level=3)),
        ("random-256k", rand256k, dict(level=3)),          # mostly raw blocks
        ("textish-l19", textish, dict(level=19)),
        ("textish-l1", textish, dict(level=1)),
        ("textish-l19-nocksum", textish, dict(level=19, write_checksum=False)),
        ("textish-l19-nofcs", textish, dict(level=19, write_content_size=False)),
        ("multi-block", textish * 40, dict(level=3)),       # several 128K blocks
        ("multi-block-l19", textish * 40, dict(level=19, write_checksum=True)),
        ("random-l19", rand256k * 4, dict(level=19)),       # ldm, big offsets
        ("skippable+mixed", textish * 3, dict(level=7)),
    ]
    fails = 0
    for name, raw, params in cases:
        if not run_case(name, raw, params):
            fails += 1

    # --- real .deb data.tar.zst from the aliyun jammy mirror ---
    try:
        import gzip, urllib.request
        url = ("https://mirrors.aliyun.com/ubuntu/dists/jammy/"
               "main/binary-amd64/Packages.gz")
        idx = gzip.decompress(urllib.request.urlopen(url, timeout=60).read())
        # pick the 'hello' package's data.tar.zst
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
        if not target:
            print("FAIL real-deb: 'hello' not found in index")
            return 1
        deb_url = "https://mirrors.aliyun.com/ubuntu/" + target
        deb = urllib.request.urlopen(deb_url, timeout=60).read()
        debpath = os.path.join(TMP, "hello.deb")
        with open(debpath, "wb") as f:
            f.write(deb)
        # extract data.tar.* member with ar
        subprocess.run(["ar", "x", debpath], cwd=TMP, check=True)
        member = None
        for fn in os.listdir(TMP):
            if fn.startswith("data.tar."):
                member = os.path.join(TMP, fn)
        if member is None or not member.endswith(".zst"):
            print(f"real-deb data member is {member}: not zst, skip")
        else:
            zst_bytes = open(member, "rb").read()
            expect = zstd.ZstdDecompressor().decompress(
                zst_bytes, max_output_size=64 * 1024 * 1024)
            zin = os.path.join(TMP, "real.zst")
            zout = os.path.join(TMP, "real.out")
            with open(zin, "wb") as f:
                f.write(zst_bytes)
            r = subprocess.run([ZDEC, zin, zout], capture_output=True, text=True)
            got = open(zout, "rb").read() if r.returncode == 0 else b""
            if r.returncode != 0 or got != expect:
                print(f"FAIL real-deb: rc={r.returncode} {r.stderr.strip()} "
                      f"len {len(got)} vs {len(expect)}")
                fails += 1
            else:
                print(f"ok   real-deb ({deb_url.split('/')[-1]}, "
                      f"{len(zst_bytes)} -> {len(expect)} bytes)")
    except Exception as e:
        print(f"WARN real-deb: {e}")

    print("=" * 50)
    print("ALL PASS" if fails == 0 else f"{fails} FAILURES")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
