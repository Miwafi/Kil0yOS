#!/usr/bin/env bash
# tools/make_ext2_disk.sh - build the Phase 2 ext2 root disk image (WSL).
#
# Produces build/ext2.img: a raw ext2 image (8 MiB, 1 KiB blocks) holding
# /bin/busybox. Attach to QEMU with -hda so the kernel mounts it at "/".
# Only free-standing userspace tools are used (no loop mounts / no root).
set -euo pipefail
cd "$(dirname "$0")/.."

IMG=build/ext2.img
ROOT=build/ext2_rootfs
SIZE_BLOCKS=8192   # 8192 x 1 KiB = 8 MiB = 16384 sectors (== DISK_MAX_SECTORS)

BB="$HOME/busybox-1.36.1/busybox"
if [ ! -f "$BB" ]; then
    echo "error: busybox binary not found at $BB (run tools/build_busybox.sh)" >&2
    exit 1
fi

rm -rf "$ROOT"
mkdir -p "$ROOT/bin"
cp "$BB" "$ROOT/bin/busybox"
chmod 755 "$ROOT/bin/busybox"

rm -f "$IMG"
# -d populates the fresh filesystem from a directory (e2fsprogs >= 1.43).
mke2fs -q -F -t ext2 -b 1024 -I 128 -d "$ROOT" "$IMG" "$SIZE_BLOCKS"

echo "ext2 image ready: $IMG ($(stat -c %s "$IMG") bytes)"
