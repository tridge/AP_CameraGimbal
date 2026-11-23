#!/bin/sh
# Build only an external TUN module; never build or install a replacement kernel.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=${1:-"$here/build"}
cross=${CROSS_COMPILE:-/usr/bin/arm-linux-gnueabihf-}
mkdir -p "$work"
work=$(CDPATH= cd -- "$work" && pwd)
archive="$work/linux-4.9.227.tar.xz"
expected=38c485b411a14e825f8a66a5c2e64f1480397325a5f692b4929f68d228070c5f
if ! printf '%s  %s\n' "$expected" "$archive" | sha256sum -c --status 2>/dev/null; then
    curl -fL --retry 2 -o "$archive.part" https://cdn.kernel.org/pub/linux/kernel/v4.x/linux-4.9.227.tar.xz
    printf '%s  %s\n' "$expected" "$archive.part" | sha256sum -c --status
    mv "$archive.part" "$archive"
fi
kernel="$work/linux-4.9.227"
if [ ! -d "$kernel" ]; then tar -xf "$archive" -C "$work"; fi
cp "$here/zt30-4.9.227.defconfig" "$kernel/.config"
make -s -C "$kernel" ARCH=arm CROSS_COMPILE="$cross" KCFLAGS=-march=armv7-a olddefconfig modules_prepare
module="$work/module"
mkdir -p "$module"
cp "$kernel/drivers/net/tun.c" "$module/tun_main.c"
patch --batch "$module/tun_main.c" "$here/tun-vendor.patch"
cp "$here/compat.c" "$here/abi_check.c" "$module/"
printf 'obj-m := tun.o abi_check.o\ntun-y := tun_main.o compat.o\n' > "$module/Makefile"
make -s -C "$kernel" ARCH=arm CROSS_COMPILE="$cross" KCFLAGS=-march=armv7-a M="$module" modules
python3 - "$here/kernel-exports.txt" "${cross}nm" "$module/tun.ko" <<'PY'
import pathlib,subprocess,sys
exports=set(pathlib.Path(sys.argv[1]).read_text().splitlines())
raw=subprocess.check_output([sys.argv[2],'-u',sys.argv[3]],text=True)
missing=[s.split()[-1] for s in raw.splitlines() if s.split()[-1] not in exports]
if missing:raise SystemExit('Imports absent from target kernel: '+', '.join(missing))
print('PASS measured ABI checks and all target kernel exports')
PY
cp "$module/tun.ko" "$work/tun.ko"
sha256sum "$work/tun.ko"
