#!/bin/sh
# Installs application files on SD. --enable-boot also installs a tiny wrapper.
set -eu
case "${1-}" in ''|--enable-boot) ;; *) echo "Usage: $0 [--enable-boot]" >&2;exit 2;; esac
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
dest=/mnt/AP_CameraGimbal/zr10
[ "$(id -u)" = 0 ] || { echo 'Run on the ZR10 as root' >&2;exit 1; }
grep -q INFINITY6B0 /sys/devices/soc0/machine || { echo 'This package requires a ZR10 Infinity6B0' >&2;exit 1; }
grep -qs '^/dev/mmcblk0p1 /mnt ' /proc/mounts || { echo 'Mount the microSD card at /mnt first' >&2;exit 1; }
(cd "$src" && sha256sum -c SHA256SUMS)
# The media ABI was validated against these installed SDK libraries.
sha256sum -c "$src/SDK_SHA256SUMS"
if pidof camera-app zr10-web >/dev/null || [ -e /tmp/zr10-service.pid ]; then
    echo 'Stop the ZR10 service before updating it' >&2;exit 1
fi
# FAT mount masks cannot be changed by remount on this kernel.
[ -x "$src/camera-app" ] || {
    echo 'SD execution is masked. Stop the vendor app, then unmount and mount /mnt with fmask=0077,dmask=0077; see README.md.' >&2
    exit 1
}
mkdir -p "$dest/config"
if [ "$src" != "$dest" ]; then
    for file in camera-app zr10-web zr10-uuid demo.sh service.sh boot.sh install.sh uninstall.sh camera.ini.default web.pass.default VERSION SDK_SHA256SUMS README.md COPYING.txt THIRD_PARTY.md OpenIPC-LICENSE.txt; do
        cp "$src/$file" "$dest/$file.new"
        mv "$dest/$file.new" "$dest/$file"
    done
    # The service is stopped, so install the matching complete asset set and
    # remove assets which disappeared since the previous version.
    rm -rf "$dest/webroot.new"
    cp -R "$src/webroot" "$dest/webroot.new"
    rm -rf "$dest/webroot"
    mv "$dest/webroot.new" "$dest/webroot"
    cp "$src/SHA256SUMS" "$dest/SHA256SUMS"
fi
chmod 755 "$dest/camera-app" "$dest/zr10-web" "$dest/zr10-uuid" "$dest/"*.sh
[ -e "$dest/config/camera.ini" ] || cp "$dest/camera.ini.default" "$dest/config/camera.ini"
[ -e "$dest/config/web.pass" ] || cp "$dest/web.pass.default" "$dest/config/web.pass"
if [ "${1-}" = --enable-boot ]; then
    cp "$dest/zr10-uuid" /customer/zr10-uuid.new
    chmod 755 /customer/zr10-uuid.new
    mv /customer/zr10-uuid.new /customer/zr10-uuid
    if [ ! -e /customer/sycamera.vendor ]; then
        [ -f /customer/sycamera ] && [ -x /customer/sycamera ]
        # Copy first; rename only once the replacement is on persistent storage.
        cp "$dest/boot.sh" /customer/sycamera.launcher.new
        chmod 755 /customer/sycamera.launcher.new
        sync
        # A minimal flash installation already has our launcher, without a
        # vendor backup. Never label a copy of our own launcher as vendor code.
        if ! grep -q 'Small /customer/sycamera wrapper' /customer/sycamera; then
            ln /customer/sycamera /customer/sycamera.vendor
        fi
        mv /customer/sycamera.launcher.new /customer/sycamera
    else
        grep -q 'Small /customer/sycamera wrapper' /customer/sycamera || {
            echo 'Existing sycamera.vendor with an unknown active launcher; refusing overwrite' >&2;exit 1;
        }
        cp "$dest/boot.sh" /customer/sycamera.launcher.new
        chmod 755 /customer/sycamera.launcher.new
        mv /customer/sycamera.launcher.new /customer/sycamera
    fi
fi
sync
printf 'Installed in %s\nStart a test with: %s/service.sh\n' "$dest" "$dest"
