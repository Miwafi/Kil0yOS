#!/bin/bash
# Phase 4 acceptance: build a tiny gzip-based Debian-style repo in
# $HOME/ktest/p4repo (Packages index + .debs with control.tar.gz /
# data.tar.gz, both gzip - real Debian uses xz, which the kernel does
# not inflate; the roadmap mitigation is a re-packed repo).
# Usage (WSL): ./tools/make_test_repo.sh   (run AFTER make)
set -e
cd "$(dirname "$0")/.."

T="$HOME/ktest"
REPO="$T/p4repo"
rm -rf "$REPO" "$T/debwork"
mkdir -p "$REPO"

command -v ar >/dev/null || { echo "ar not found (install binutils)"; exit 1; }

HELLO_ELF=build/user/hello-lnx
if [ ! -f "$HELLO_ELF" ]; then
    echo "missing $HELLO_ELF - run 'make' first (needs musl-gcc)" >&2
    exit 1
fi

# make_deb <pkg> <version> <depends> <stage-dir> <out.deb>
make_deb() {
    local pkg="$1" ver="$2" depends="$3" stage="$4" out="$5"
    local W="$T/debwork"
    rm -rf "$W"; mkdir -p "$W/ctl" "$W/data"
    {
        echo "Package: $pkg"
        echo "Version: $ver"
        echo "Architecture: all"
        echo "Maintainer: Kil0yOS test <root@kil0yos.local>"
        [ -n "$depends" ] && echo "Depends: $depends"
        echo "Description: Kil0yOS Phase 4 acceptance payload"
    } > "$W/ctl/control"
    cp -r "$stage/." "$W/data/"
    tar czf "$W/control.tar.gz" -C "$W/ctl" .
    tar czf "$W/data.tar.gz" -C "$W/data" .
    echo "2.0" > "$W/debian-binary"
    rm -f "$out"
    ar rc "$out" "$W/debian-binary" "$W/control.tar.gz" "$W/data.tar.gz"
}

# --- hello-kit: ships /bin/hello-kit (the musl-static hello ELF) -------
STAGE="$T/stage1"; rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/share/doc/hello-kit"
cp "$HELLO_ELF" "$STAGE/bin/hello-kit"
echo "hello-kit readme" > "$STAGE/share/doc/hello-kit/README"
make_deb hello-kit 1.0 "" "$STAGE" "$REPO/hello-kit_1.0_all.deb"

# --- hello-kit-data: depends on hello-kit (topo-order exercise) --------
STAGE="$T/stage2"; rm -rf "$STAGE"; mkdir -p "$STAGE/share/hello-kit"
echo "payload-data-v1" > "$STAGE/share/hello-kit/data.txt"
make_deb hello-kit-data 1.0 "hello-kit" "$STAGE" "$REPO/hello-kit-data_1.0_all.deb"

# --- Packages index (gzip-only repo) ------------------------------------
: > "$REPO/Packages"
gen_para() { # file pkg version depends
    local f="$1" pkg="$2" ver="$3" dep="$4"
    {
        echo "Package: $pkg"
        echo "Version: $ver"
        echo "Architecture: all"
        [ -n "$dep" ] && echo "Depends: $dep"
        echo "Filename: $(basename "$f")"
        echo "SHA256: $(sha256sum "$f" | cut -d' ' -f1)"
        echo "Size: $(stat -c%s "$f")"
        echo
    } >> "$REPO/Packages"
}
gen_para "$REPO/hello-kit_1.0_all.deb" hello-kit 1.0 ""
gen_para "$REPO/hello-kit-data_1.0_all.deb" hello-kit-data 1.0 "hello-kit"

# short name for the TFTP-based dpkg -i test
cp "$REPO/hello-kit_1.0_all.deb" "$T/hk.deb"

echo "repo built at $REPO:"
ls -l "$REPO"
echo "--- Packages ---"
cat "$REPO/Packages"
