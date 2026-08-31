#!/bin/bash
# Phase 4.4 dists-layout acceptance: sources.list in REAL Debian format
# ("deb http://host[:port]/base SUITE COMP..."), index fetched from
# dists/<suite>/<comp>/binary-amd64/Packages.gz with kernel-side gunzip
# (plain-Packages fallback exercised by the universe component), and
# pool/<...> Filenames resolved against the repo base.
# Usage (WSL): ./tools/accept_p4_dists.sh   (run tools/make_test_repo.sh-style
# repo build is done inline; needs a prior `make` for the hello ELF)
TDIR="$HOME/ktest"
REPO="$TDIR/p4drepo"
WORK="$TDIR/p4dwork"
STAGE_ROOT="$TDIR/p4dstage"
rm -rf "$REPO" "$WORK" "$STAGE_ROOT"
mkdir -p "$REPO"
rm -f "$TDIR/serial_p4d.log" "$TDIR/qmon_p4d"
cd "/mnt/c/Users/19423/Desktop/Programs/Kil0yOS" || exit 1

command -v ar >/dev/null || { echo "ar not found (install binutils)"; exit 1; }
HELLO_ELF=build/user/hello-lnx
[ -f "$HELLO_ELF" ] || { echo "missing $HELLO_ELF - run 'make' first" >&2; exit 1; }

make_deb() { # <pkg> <version> <depends> <stage-dir> <out.deb>
    local pkg="$1" ver="$2" depends="$3" stage="$4" out="$5"
    local W="$WORK"
    rm -rf "$W"; mkdir -p "$W/ctl" "$W/data"
    {
        echo "Package: $pkg"
        echo "Version: $ver"
        echo "Architecture: all"
        [ -n "$depends" ] && echo "Depends: $depends"
        echo "Description: Kil0yOS dists-layout acceptance payload"
    } > "$W/ctl/control"
    cp -r "$stage/." "$W/data/"
    tar czf "$W/control.tar.gz" -C "$W/ctl" .
    tar czf "$W/data.tar.gz" -C "$W/data" .
    echo "2.0" > "$W/debian-binary"
    rm -f "$out"
    ar rc "$out" "$W/debian-binary" "$W/control.tar.gz" "$W/data.tar.gz"
}

gen_para() { # <file> <pkg> <version> <depends>
    local f="$1" pkg="$2" ver="$3" dep="$4"
    {
        echo "Package: $pkg"
        echo "Version: $ver"
        echo "Architecture: all"
        [ -n "$dep" ] && echo "Depends: $dep"
        echo "Filename: ${f#$REPO/}"
        echo "SHA256: $(sha256sum "$f" | cut -d' ' -f1)"
        echo "Size: $(stat -c%s "$f")"
        echo
    }
}

# --- main component: hello-kit + hello-kit-data (dep-ordered pair) ------
S1="$STAGE_ROOT/s1"; mkdir -p "$S1/bin" "$S1/share/doc/hello-kit"
cp "$HELLO_ELF" "$S1/bin/hello-kit"
echo "hello-kit readme" > "$S1/share/doc/hello-kit/README"
D1="$REPO/pool/main/h/hello-kit"; mkdir -p "$D1"
make_deb hello-kit 1.0 "" "$S1" "$D1/hello-kit_1.0_all.deb"

S2="$STAGE_ROOT/s2"; mkdir -p "$S2/share/hello-kit"
echo "payload-data-v1" > "$S2/share/hello-kit/data.txt"
make_deb hello-kit-data 1.0 "hello-kit" "$S2" "$D1/hello-kit-data_1.0_all.deb"

IDX="$REPO/dists/jammy/main/binary-amd64"; mkdir -p "$IDX"
{ gen_para "$D1/hello-kit_1.0_all.deb" hello-kit 1.0 ""
  gen_para "$D1/hello-kit-data_1.0_all.deb" hello-kit-data 1.0 "hello-kit"
} > "$IDX/Packages"
gzip -c "$IDX/Packages" > "$IDX/Packages.gz"      # main: gz path

# --- universe component: PLAIN index (fallback path) + hello-extra ------
S3="$STAGE_ROOT/s3"; mkdir -p "$S3/share/hello-extra"
echo "extra-v1" > "$S3/share/hello-extra/extra.txt"
D3="$REPO/pool/universe/h/hello-extra"; mkdir -p "$D3"
make_deb hello-extra 1.0 "" "$S3" "$D3/hello-extra_1.0_all.deb"

IDX3="$REPO/dists/jammy/universe/binary-amd64"; mkdir -p "$IDX3"
gen_para "$D3/hello-extra_1.0_all.deb" hello-extra 1.0 "" > "$IDX3/Packages"
# (no Packages.gz here on purpose: exercises the plain-Packages fallback)

# --- host HTTP server (slirp: 10.0.2.2:8001 -> host:8001) ----------------
python3 -m http.server 8001 --directory "$REPO" >/dev/null 2>&1 &
HPID=$!
sleep 1

qemu-system-x86_64 -cdrom build/kil0yos.iso -m 512M -display none \
  -serial file:"$TDIR/serial_p4d.log" \
  -monitor unix:"$TDIR/qmon_p4d",server,nowait \
  -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -no-reboot &
QPID=$!

sleep 16
type_cmd() {
    timeout 20 python3 tools/drive_keys.py "$1" "$TDIR/qmon_p4d" || echo "TYPING FAILED: $1"
}

type_cmd "mkdir /etc/kilget";                                              sleep 3
type_cmd "echo deb http://10.0.2.2:8001 jammy main universe > /etc/kilget/sources.list"; sleep 3
type_cmd "kilget update";                                                  sleep 15
type_cmd "kilget install hello-kit-data";                                  sleep 30
type_cmd "kilget install hello-extra";                                     sleep 20
type_cmd "exec /bin/hello-kit";                                            sleep 5
type_cmd "dpkg -l";                                                        sleep 3
type_cmd "echo P4DISTS_ALL_DONE";                                          sleep 4

kill "$QPID" 2>/dev/null
kill "$HPID" 2>/dev/null
sleep 1
echo "=== dists-layout acceptance output ==="
python3 tools/dump_serial.py "$TDIR/serial_p4d.log" 600000 | \
  grep -aE 'kilget|dpkg|hello|P4DISTS_ALL_DONE|PANIC|panic|failed|mismatch|not implemented' | head -50
