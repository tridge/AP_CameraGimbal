#!/bin/sh
# Application-only SD package; no vendor binaries or flash image.
set -eu
[ "$#" = 5 ] || { echo "Usage: $0 OUTPUT CAMERA_APP WEB_APP VERSION UUID_HELPER" >&2;exit 2; }
output=$1
camera_app=$2
web_app=$3
version=$4
uuid_helper=$5
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
strip=${ZR10_STRIP:-${ZR10_CROSS_COMPILE:-arm-linux-}strip}
password=${ZR10_WEB_PASSWORD:-ardupilot}
[ "${#password}" -ge 8 ] && [ "${#password}" -le 128 ] || { echo 'Web password must be 8..128 characters' >&2;exit 1; }
if printf '%s' "$password" | LC_ALL=C grep -q '[[:cntrl:]]'; then exit 1;fi
mkdir -p "$(dirname "$output")"
work=$(mktemp -d "$(dirname "$output")/zr10-package.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/zr10"
tree=$work/zr10
cp -R "$root/web/webroot" "$tree/webroot"
"$strip" -o "$tree/camera-app" "$camera_app"
"$strip" -o "$tree/zr10-web" "$web_app"
"$strip" -o "$tree/zr10-uuid" "$uuid_helper"
cp "$root/packaging/zr10/"*.sh "$root/packaging/zr10/SDK_SHA256SUMS" "$root/packaging/zr10/README.md" "$tree/"
cp "$root/packaging/zr10/camera.ini" "$tree/camera.ini.default"
cp "$root/COPYING.txt" "$root/THIRD_PARTY.md" "$tree/"
cp "$root/camera_app/src/backends/zr10/mi/LICENSE" "$tree/OpenIPC-LICENSE.txt"
printf '%s\n' "$version" >"$tree/VERSION"
(umask 077;printf '%s\n' "$password" >"$tree/web.pass.default")
chmod 755 "$tree/camera-app" "$tree/zr10-web" "$tree/zr10-uuid" "$tree/"*.sh
(cd "$tree" && {
    sha256sum camera-app zr10-web zr10-uuid *.sh *.default VERSION SDK_SHA256SUMS README.md COPYING.txt THIRD_PARTY.md OpenIPC-LICENSE.txt
    find webroot -type f | LC_ALL=C sort | while IFS= read -r asset; do sha256sum "$asset"; done
} >SHA256SUMS)
tar -czf "$output" -C "$work" zr10
sha256sum "$output" >"$output.sha256"
echo "Built $output ($(wc -c <"$output") bytes)"
