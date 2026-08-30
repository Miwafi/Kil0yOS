#!/usr/bin/env bash
# Phase 1.3: build a musl-static busybox for Kil0yOS.
# Usage (WSL):  ./tools/build_busybox.sh
#
# - builds in ~/busybox-1.36.1 (native FS; drvfs is too slow to compile on)
# - CONFIG_STATIC=y            -> single self-contained ELF
# - EXTRA_LDFLAGS              -> linked at 0x10000000 (kernel requires user
#                                 images at/above UVM_ELF_BASE, see uvm.h)
set -euo pipefail

MUSL_GCC="${MUSL_GCC:-$HOME/musl/bin/musl-gcc}"
[ -x "$MUSL_GCC" ] || { echo "musl-gcc not found at $MUSL_GCC" >&2; exit 1; }

# busybox includes <linux/*.h> (vt.h, kd.h, ...). musl-gcc's specs do not
# expose the host kernel headers by default - patch them in (idempotent).
# Constraints:
#  - musl headers MUST win over glibc: glibc headers emit __isoc23_* /
#    __printf_chk symbols musl's libc.a does not provide.
#  - the stock "include%s" path is cwd-relative (breaks from any dir
#    except the musl build tree), so rewrite it to an absolute path.
SPECS="$HOME/musl/lib/musl-gcc.specs"
MUSL_INC="$HOME/musl/include"
sed -i "s#-isystem [^ ]*include%s#-isystem $MUSL_INC#g" "$SPECS"
grep -q 'isystem /usr/include' "$SPECS" || \
    sed -i "s#-isystem $MUSL_INC#-isystem $MUSL_INC -isystem /usr/include#g" "$SPECS"

VER=1.36.1
SRC="$HOME/busybox-$VER"
[ -d "$SRC" ] || {
    curl -sO "https://busybox.net/downloads/busybox-$VER.tar.bz2"
    tar xf "busybox-$VER.tar.bz2"
    mv "busybox-$VER" "$SRC"
    rm "busybox-$VER.tar.bz2"
}
cd "$SRC"

[ -f .config ] || make defconfig >/dev/null
# static + link address; sed is idempotent
sed -i 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
grep -q '^CONFIG_EXTRA_LDFLAGS=' .config || \
    sed -i 's/^CONFIG_STATIC=y/CONFIG_STATIC=y\nCONFIG_EXTRA_LDFLAGS="-Wl,-Ttext-segment=0x10000000"/' .config
# tc needs TCA_CBQ_* which modern kernel headers (>= 6.8) removed
sed -i 's/^CONFIG_TC=y/# CONFIG_TC is not set/' .config
yes '' | make oldconfig >/dev/null 2>&1 || true

make -j"$(nproc)" CC="$MUSL_GCC" CONFIG_STATIC=y \
    CONFIG_EXTRA_LDFLAGS="-Wl,-Ttext-segment=0x10000000" "$@"

file busybox
echo "OK: $SRC/busybox"
