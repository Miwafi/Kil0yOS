#!/bin/bash
# Phase 4.3 acceptance repo: REAL Debian/Ubuntu packages (libc6 chain).
# Ubuntu 24.04 .debs compress members with zstd, which the kernel cannot
# inflate - per the roadmap mitigation ("re-packed repo"), the ORIGINAL
# tar members are decompressed and re-compressed as gzip; the payload
# bytes stay identical, only the container compression changes.
# Chain: gcc-14-base <- libc6 <-> libgcc-s1 (includes a dependency cycle,
# exercising the topological planner's visited-set).
# Usage (WSL): ./tools/make_repo43.sh
set -e
cd "$(dirname "$0")/.."

T="$HOME/ktest"
REPO="$T/p43repo"
WORK="$T/p43work"
rm -rf "$REPO" "$WORK"
mkdir -p "$REPO" "$WORK"

command -v zstd >/dev/null || {
    [ -x /tmp/zst/usr/bin/zstd ] || {
        cd "$WORK"
        apt-get download zstd libzstd1 >/dev/null 2>&1
        for d in "$WORK"/zstd_*.deb; do dpkg -x "$d" /tmp/zst; done
    }
    ZSTD=/tmp/zst/usr/bin/zstd
}
ZSTD=${ZSTD:-zstd}

# --- fetch real packages -------------------------------------------------
cd "$WORK"
for p in libc6 libgcc-s1 gcc-14-base; do
    compgen -G "$p"_*.deb >/dev/null || apt-get download "$p" >/dev/null 2>&1
    src=$(ls "$p"_*.deb | head -1)
    dpkg-deb -f "$src" Package Version Depends > "$p.meta"
done
ls *.deb

# --- re-pack: zst members -> gz (payload tar bytes unchanged) ------------
# repack <src.deb> <out.deb>
repack() {
    local src="$1" out="$2" B="$WORK/x"
    rm -rf "$B"; mkdir -p "$B"
    (cd "$B" && ar x "$OLDPWD/$src")
    local m
    for m in control data; do
        if [ -f "$B/$m.tar.zst" ]; then
            "$ZSTD" -dc "$B/$m.tar.zst" | gzip -c > "$B/$m.tar.gz"
        elif [ -f "$B/$m.tar.xz" ]; then
            xz -dc "$B/$m.tar.xz" | gzip -c > "$B/$m.tar.gz"
        elif [ -f "$B/$m.tar.gz" ]; then
            : # already gzip, keep as-is
        else
            echo "no $m member in $src" >&2; exit 1
        fi
    done
    rm -f "$out"
    ar rc "$out" "$B/debian-binary" "$B/control.tar.gz" "$B/data.tar.gz"
}

for p in libc6 libgcc-s1 gcc-14-base; do
    src=$(ls "$p"_*.deb | head -1)
    repack "$src" "$REPO/$p.deb"
done

# --- Packages index with REAL sha256/size --------------------------------
: > "$REPO/Packages"
gen_para() { # package-name
    local p="$1" f="$REPO/$p.deb" meta="$WORK/$p.meta"
    local pkg ver dep
    pkg=$(grep '^Package:' "$meta" | awk '{print $2}')
    ver=$(grep '^Version:' "$meta" | awk '{print $2}')
    dep=$(grep '^Depends:' "$meta" | cut -d' ' -f2-)
    {
        echo "Package: $pkg"
        echo "Version: $ver"
        echo "Architecture: amd64"
        [ -n "$dep" ] && echo "Depends: $dep"
        echo "Filename: $(basename "$f")"
        echo "SHA256: $(sha256sum "$f" | cut -d' ' -f1)"
        echo "Size: $(stat -c%s "$f")"
        echo
    } >> "$REPO/Packages"
}
for p in libc6 libgcc-s1 gcc-14-base; do
    gen_para "$p"
done

echo "repo43 built at $REPO:"
ls -l "$REPO"
echo "--- Packages ---"
cat "$REPO/Packages"
