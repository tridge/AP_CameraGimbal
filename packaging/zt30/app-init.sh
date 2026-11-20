#!/bin/sh
# AP-only startup, adapted from MT11's app_init/app_selection layout.
# Platform module setup is performed by /customer/demo.sh before this script.
set -eu
app_dir=${APCG_ZT30_APP_DIR:-/customer/camera-app}
run_dir=${APCG_ZT30_RUN_DIR:-/tmp/apcg-zt30}
mkdir -p "$run_dir"
chmod 0700 "$run_dir"
export LD_LIBRARY_PATH="/config/lib:/lib:/usr/lib"
export CAMERA_APP_CONFIG="$app_dir/camera.ini"
export CAMERA_APP_READY_PATH="$run_dir/camera-app.ready"
export CAMERA_APP_RECORD_ROOT=/mnt/DCIM
export CAMERA_APP_CAPTURE_ROOT=/mnt/DCIM/capture
export CAMERA_APP_LOG_ROOT="$run_dir"
export TMP="$run_dir"
# Recovery SSH is independent of payload selection and configuration defaults.
"$app_dir/start-dropbear.sh" >"$run_dir/dropbear.log" 2>&1 &
# The vendor boot scripts leave loopback unconfigured; web video uses it.
ifconfig lo 127.0.0.1 up || echo "Loopback setup failed; web video unavailable" >&2

# Existing settings and credentials survive application updates.
if [ ! -e "$CAMERA_APP_CONFIG" ]; then
    cp "$app_dir/camera.ini.default" "$CAMERA_APP_CONFIG"
fi
if [ ! -s "$app_dir/web.pass" ]; then
    # Publish complete credentials atomically; recover an empty file left by
    # an interrupted older installation without replacing an existing password.
    ( umask 077
      temporary=$(mktemp "$app_dir/.web.pass.XXXXXX") || exit 1
      trap 'rm -f "$temporary"' EXIT HUP INT TERM
      cp "$app_dir/web.pass.default" "$temporary" &&
      [ -s "$temporary" ] &&
      chmod 0600 "$temporary" &&
      sync &&
      mv -f "$temporary" "$app_dir/web.pass" &&
      sync
    ) || echo 'Cannot seed web credentials; camera and recovery web supervisors will still start' >&2
fi
# A separate supervisor keeps the web listener recoverable while the camera
# supervisor is waiting on its foreground app/logger pipeline.
# Restore an interrupted, unconfirmed trial before starting media owners.
"$app_dir/zt30-slot" --root "$app_dir" --run-dir "$run_dir" maintain ||
    echo 'Trial boot recovery failed; SSH/web recovery remains available' >&2
if [ -x "$app_dir/web-supervisor.sh" ]; then
    "$app_dir/web-supervisor.sh" >"$run_dir/web-supervisor.log" 2>&1 &
else
    echo 'Web supervisor missing; recovery SSH remains available' >&2
fi

mkdir -p /mnt
sd_mounted=0
# The platform may have mounted the SD card before AP startup.
while read -r device mountpoint filesystem options rest; do
    case "$device:$mountpoint:$filesystem:$options" in
        /dev/mmcblk0*:/mnt:exfat:rw*|/dev/mmcblk0*:/mnt:vfat:rw*) sd_mounted=1;;
    esac
done < /proc/mounts
for sd_device in /dev/mmcblk0p1 /dev/mmcblk0; do
    [ "$sd_mounted" = 0 ] || break
    [ -b "$sd_device" ] || continue
    if mount -t exfat -o rw,noatime,umask=077 "$sd_device" /mnt 2>/dev/null ||
       mount -t vfat -o rw,noatime,umask=077 "$sd_device" /mnt; then
        sd_mounted=1
        break
    fi
done
if [ "$sd_mounted" = 1 ]; then
    # Prevent this package being reapplied on the next boot.
    rm -f /mnt/SIYI_ZT30_UpgradeSD.bin /mnt/upgrade_script.txt
fi

if pidof siyi_app_359 >/dev/null 2>&1; then
    echo 'Vendor app owns media; refusing concurrent AP startup' >&2
    exit 1
fi
if pidof camera-app >/dev/null 2>&1; then
    echo 'camera-app is already running' >&2
    exit 0
fi
# Reuse the common web logger, which bounds and rotates app logs on tmpfs.
"$app_dir/app-supervisor.sh" &
