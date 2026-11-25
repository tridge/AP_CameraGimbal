#!/bin/sh
# Pinned userspace client; no controller, SSO or dynamic C++ runtime dependency.
set -eu
[ "$#" = 3 ] || { echo "Usage: $0 mt11|zt30 OUTPUT CROSS_COMPILE" >&2; exit 2; }
platform=$1
output=$2
cross=$3
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
case "$platform" in
    zt30) arch='-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard';;
    mt11) arch='-march=armv8-a+crypto';;
    *) echo 'Unsupported VPN platform' >&2; exit 2;;
esac
archive="$output/zerotier-1.16.2.tar.gz"
expected=2c607f573c6e38815433af289d364a689a203b18b51125f06c4472014d0657f0
if ! printf '%s  %s\n' "$expected" "$archive" | sha256sum -c --status 2>/dev/null; then
    curl -fL --retry 2 -o "$archive.part" https://codeload.github.com/zerotier/ZeroTierOne/tar.gz/refs/tags/1.16.2
    printf '%s  %s\n' "$expected" "$archive.part" | sha256sum -c --status
    mv "$archive.part" "$archive"
fi
source="$output/ZeroTierOne-1.16.2"
if [ ! -d "$source" ]; then tar -xf "$archive" -C "$output"; fi
# Never detect host NAT libraries while cross compiling.
python3 - "$source/make-linux.mk" <<'PYCODE'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text()
s=s.replace('ifeq ($(wildcard /usr/include/natpmp.h),)', 'ifeq (1,1)')
p.write_text(s)
PYCODE
flags="-Os $arch -ffunction-sections -fdata-sections -fstack-protector-strong"
make -C "$source" -j"${JOBS:-6}" CC="${cross}gcc" CXX="${cross}g++" AS="${cross}as" STRIP="${cross}strip" \
    ZT_EMBEDDED=1 ZT_CONTROLLER=0 ZT_STATIC=0 ZT_SSO_SUPPORTED=0 MINIUPNPC_IS_NEW_ENOUGH=0 \
    CFLAGS="$flags" CXXFLAGS="$flags" \
    LDFLAGS='-Wl,--gc-sections,-z,relro,-z,now -static-libstdc++ -static-libgcc' one
mkdir -p "$output/runtime"
cp "$source/zerotier-one" "$output/runtime/zerotier-one"
"${cross}strip" "$output/runtime/zerotier-one"
cp "$source/LICENSE.txt" "$source/LICENSE-MPL.txt" "$output/runtime/"
sha256sum "$output/runtime/zerotier-one"
