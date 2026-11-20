#!/bin/sh
# Installed as /customer/bin/cardv on the SIYI A8 mini.
#
# The vendor root filesystem is kept as-is. Its demo.sh loads the MI kernel
# modules and then runs "/customer/bin/cardv /customer/bin/default.ini &",
# and mac.sh runs "/customer/bin/cardv Uuid" to derive the Ethernet MAC, so
# this script stands in for both uses of the vendor application. The
# original executable may remain in older installations but is never selected.
#
# /customer is rewritten by every application update, so the live
# configuration lives on the small vendor settings partition at /config.

APP_ROOT=/customer/camera-app
CONFIG_ROOT=/config/camera-app
# The web interface bumps this counter when asking for a restart.
REQUEST_FILE=/tmp/camera-app.request
# the request the application being started (or running) was started for;
# written before the fork so the web interface never stops that instance
STARTED_FILE=/tmp/camera-app.started
SD_DEVICE=/dev/mmcblk0p1
SD_MOUNT=/mnt/mmc
LOG=/tmp/camera-app.log
LOG_LIMIT=524288
LOG_KEEP=131072
# md5 of an empty "cardv Uuid" answer; mac.sh would persist this shared MAC
EMPTY_UUID_MAC=7c:d4:1d:8c:d9:8f

if [ "$1" = Uuid ]; then
    # mac.sh hashes whatever comes back; never answer with junk
    uuid=$("$APP_ROOT/a8-uuid") || exit 1
    case "$uuid" in
    ''|*[!0-9]*) exit 1 ;;
    esac
    printf '%s' "$uuid"
    exit 0
fi

log()
{
    printf '%s\n' "app_init: $*" >&2
}

# The retained A8 Ethernet driver drops UniGCS 224.0.0.1 discovery packets
# with only the normal multicast group filter installed. ALLMULTI restores
# reception; the kernel still delivers packets only to matching sockets.
ifconfig eth0 allmulti || log "warning: could not enable Ethernet multicast reception"

sd_mounted()
{
    grep -qs "^$SD_DEVICE $SD_MOUNT " /proc/mounts
}

mount_microsd()
{
    [ -b "$SD_DEVICE" ] || return 1
    if mount -t exfat -o rw,noatime "$SD_DEVICE" "$SD_MOUNT" 2>/dev/null ||
       mount -t vfat -o rw,noatime "$SD_DEVICE" "$SD_MOUNT" 2>/dev/null; then
        log "mounted $SD_DEVICE at $SD_MOUNT"
        # U-Boot reflashes on every boot while this file exists; demo.sh only
        # removes it from FAT cards.
        for stale in SIYI_4K_MINI_UpgradeSD.bin upgrade_script.txt; do
            [ -e "$SD_MOUNT/$stale" ] || continue
            rm -f "$SD_MOUNT/$stale" 2>/dev/null
            if [ -e "$SD_MOUNT/$stale" ]; then
                mount -o remount,rw "$SD_MOUNT" 2>/dev/null
                rm -f "$SD_MOUNT/$stale" 2>/dev/null
            fi
            [ -e "$SD_MOUNT/$stale" ] &&
                log "warning: could not remove $SD_MOUNT/$stale; the card will reflash on the next boot"
        done
        sync
        return 0
    fi
    log "could not mount $SD_DEVICE as exFAT or FAT"
    return 1
}

# Retry the card in the background so a late or replaced card still works.
sd_watcher()
{
    while true; do
        # never stack a second mount on one still in progress
        if ! pidof mount >/dev/null 2>&1 && ! sd_mounted; then
            mount_microsd
        fi
        sleep 5
    done
}

start_sd_watcher()
{
    [ -n "$sd_pid" ] && kill -0 "$sd_pid" 2>/dev/null && return 0
    sd_watcher &
    sd_pid=$!
}

# Bound the logs in tmpfs; the applications append, so truncation is safe.
log_watcher()
{
    while true; do
        sleep 5
        for file in "$LOG" /tmp/a8-web.log; do
            size=$(wc -c <"$file" 2>/dev/null || echo 0)
            if [ "$size" -gt "$LOG_LIMIT" ]; then
                tail -c "$LOG_KEEP" "$file" >"$file.1" 2>/dev/null
                : >"$file"
            fi
        done
    done
}

request_generation()
{
    cat "$REQUEST_FILE" 2>/dev/null || echo 0
}

# tell the web interface which request the next instance belongs to
mark_started()
{
    printf '%s\n' "$generation" >"$STARTED_FILE.new" &&
        mv -f "$STARTED_FILE.new" "$STARTED_FILE"
}

# Allow recording shutdown before forcing a stuck process to exit.
kill_app()
{
    [ -n "$app_pid" ] || return 0
    kill "$app_pid" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        kill -0 "$app_pid" 2>/dev/null || break
        sleep 1
    done
    kill -9 "$app_pid" 2>/dev/null
    wait "$app_pid" 2>/dev/null
    app_pid=
}

stop()
{
    log "stopping"
    [ -n "$sd_pid" ] && kill "$sd_pid" 2>/dev/null
    [ -n "$log_pid" ] && kill "$log_pid" 2>/dev/null
    [ -n "$web_pid" ] && kill "$web_pid" 2>/dev/null
    kill_app
    exit 0
}

app_pid=
sd_pid=
log_pid=
web_pid=
trap stop TERM INT

# A failed UUID query on first boot leaves mac.sh's fallback MAC behind;
# drop it so the next boot derives the real one.
if [ "$(cat /customer/mac_addr/mac.txt 2>/dev/null)" = "$EMPTY_UUID_MAC" ]; then
    log "warning: MAC file holds the empty-UUID fallback; removing it"
    rm -f /customer/mac_addr/mac.txt
fi

export CAMERA_APP_CONFIG=$CONFIG_ROOT/camera.ini
export CAMERA_APP_RECORD_ROOT=$SD_MOUNT/DCIM/record
export CAMERA_APP_CAPTURE_ROOT=$SD_MOUNT/DCIM/capture
export CAMERA_APP_READY_PATH=/tmp/camera-app.ready

# the live video relay binds to the loopback interface
ifconfig lo 127.0.0.1 up

# Firmware images carry only a default configuration; keep edits across updates.
mkdir -p "$CONFIG_ROOT"
if [ ! -f "$CONFIG_ROOT/camera.ini" ] && [ -r "$APP_ROOT/camera.ini.default" ]; then
    cp "$APP_ROOT/camera.ini.default" "$CONFIG_ROOT/camera.ini.new" &&
        mv "$CONFIG_ROOT/camera.ini.new" "$CONFIG_ROOT/camera.ini" && sync
fi
if [ ! -s "$CONFIG_ROOT/web.pass" ] && [ -r "$APP_ROOT/web.pass.default" ]; then
    umask 077
    cp "$APP_ROOT/web.pass.default" "$CONFIG_ROOT/web.pass.new" &&
        mv "$CONFIG_ROOT/web.pass.new" "$CONFIG_ROOT/web.pass" && sync
    umask 022
fi

# Without a card the output paths must not land in RAM: keep a tiny tmpfs
# under the mount point so writes fail quickly instead of filling memory.
mkdir -p "$SD_MOUNT"
if ! sd_mounted && ! mount -t tmpfs -o size=64k,mode=0555 tmpfs "$SD_MOUNT"; then
    log "warning: no tmpfs guard under $SD_MOUNT; disabling card output paths"
    export CAMERA_APP_RECORD_ROOT=/proc/camera-app/record
    export CAMERA_APP_CAPTURE_ROOT=/proc/camera-app/capture
    export CAMERA_APP_LOG_ROOT=/proc/camera-app/logs
fi
# demo.sh mounts the card briefly right after starting us; let it finish
sleep 2
sd_mounted || mount_microsd || log "continuing without a microSD card"
log_watcher &
log_pid=$!

# administrative web interface on port 80
if [ -x "$APP_ROOT/a8-web" ]; then
    "$APP_ROOT/a8-web" -p 80 >>/tmp/a8-web.log 2>&1 &
    web_pid=$!
fi

# AP CameraGimbal is the only application. Ignore legacy selection files.
# A startup failure leaves the web server running until an explicit restart.
check_web()
{
    if [ -z "$web_pid" ] || ! kill -0 "$web_pid" 2>/dev/null; then
        "$APP_ROOT/a8-web" -p 80 >>/tmp/a8-web.log 2>&1 &
        web_pid=$!
    fi
}
wait_for_restart()
{
    log "camera stopped; web UI remains available for diagnosis and restart"
    while [ "$(request_generation)" = "$generation" ]; do
        check_web
        sleep 1
    done
}
start_sd_watcher
while true; do
    check_web
    generation=$(request_generation)
    rm -f "$CAMERA_APP_READY_PATH"
    ok=false
    if mark_started; then
        "$APP_ROOT/camera-app" --backend a8 --config "$CAMERA_APP_CONFIG" >>"$LOG" 2>&1 &
        app_pid=$!
        for _ in $(seq 1 60); do
            [ -e "$CAMERA_APP_READY_PATH" ] && ok=true && break
            kill -0 "$app_pid" 2>/dev/null || break
            [ "$(request_generation)" != "$generation" ] && break
            sleep 1
        done
    fi
    if [ "$ok" = true ]; then
        while kill -0 "$app_pid" 2>/dev/null; do
            check_web
            [ "$(request_generation)" != "$generation" ] && break
            sleep 1
        done
    fi
    kill_app
    rm -f "$CAMERA_APP_READY_PATH"
    wait_for_restart
done
