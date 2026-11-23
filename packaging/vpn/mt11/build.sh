#!/bin/sh
# Rebuild the external TUN driver for the measured MT11 vendor kernel.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${1:-"$here/build"}
cross=${CROSS_COMPILE:-aarch64-linux-gnu-}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
archive="$work/linux-4.19.90.tar.xz"
expected=29d86c0a6daf169ec0b4b42a12f8d55dc894c52bd901f876f52a05906a5cf7fd
if ! printf '%s  %s\n' "$expected" "$archive" | sha256sum -c --status 2>/dev/null; then
    curl -fL --retry 2 -o "$archive.part" https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.19.90.tar.xz
    printf '%s  %s\n' "$expected" "$archive.part" | sha256sum -c --status
    mv "$archive.part" "$archive"
fi
kernel="$work/linux-4.19.90"
if [ ! -d "$kernel" ]; then tar -xf "$archive" -C "$work"; fi
# The measured vendor device tail is 24 bytes larger than upstream's. This
# keeps gso_max_size at 1864 and netdev_priv at 2048; abi_check.c enforces both.
python3 - "$kernel/include/linux/netdevice.h" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
if 'apcam_vendor_device_tail' not in s:
    needle='\tstruct device\t\tdev;'
    assert s.count(needle)==1
    p.write_text(s.replace(needle,needle+'\n\tunsigned char apcam_vendor_device_tail[24];'))
PY
cp "$here/mt11-4.19.90.defconfig" "$kernel/.config"
# -fcommon is needed for the old upstream DTC with current host compilers.
make -s -C "$kernel" ARCH=arm64 CROSS_COMPILE="$cross" HOSTCFLAGS=-fcommon olddefconfig modules_prepare
module="$work/module"
mkdir -p "$module"
cp "$kernel/drivers/net/tun.c" "$module/tun_main.c"
patch --batch "$module/tun_main.c" "$here/tun-vendor.patch"
cp "$here/abi_check.c" "$module/"
printf 'obj-m := tun.o abi_check.o\ntun-y := tun_main.o\n' > "$module/Makefile"
make -s -C "$kernel" ARCH=arm64 CROSS_COMPILE="$cross" HOSTCFLAGS=-fcommon M="$module" modules
python3 - "$here/kernel-exports.txt" "${cross}nm" "$module/tun.ko" <<'PY'
from pathlib import Path
import subprocess,sys
exports=set(Path(sys.argv[1]).read_text().splitlines())
imports={s.split()[-1] for s in subprocess.check_output([sys.argv[2],'-u',sys.argv[3]],text=True).splitlines()}
if imports-exports: raise SystemExit('Unqualified target imports: '+', '.join(sorted(imports-exports)))
print('PASS measured ABI and target kernel exports')
PY
cp "$module/tun.ko" "$work/tun.ko"
sha256sum "$work/tun.ko"
