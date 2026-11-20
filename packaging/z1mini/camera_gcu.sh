#!/bin/sh
# Application-only .gcu overlay entry point, called by the overlay's ipc/run.sh.
# A vendor .gcu restores this file. No rootfs/bootloader/MCU flashing.
APP=/opt/bin/gcu/ap
NATIVE=false
export LD_LIBRARY_PATH=/soc/lib:/usr/local/lib:/opt/bin/gcu/ipc/lib
if command -v flock >/dev/null 2>&1 && [ -x "$APP/service.sh" ] && (cd "$APP" && sha256sum -c SHA256SUMS >/dev/null 2>&1); then
    "$APP/service.sh" </dev/null >>/tmp/z1mini-service.log 2>&1 &
else
    echo 'AP overlay incomplete; reinstall the application package' >&2
    exit 1
fi
