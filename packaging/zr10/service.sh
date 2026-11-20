#!/bin/sh
# SD/flash supervisor. Vendor demo.sh has already loaded kernel modules.
set -u
APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONFIG_ROOT=/mnt/AP_CameraGimbal/zr10/config
sd_available=false
REQUEST=/tmp/camera-app.request
STARTED=/tmp/camera-app.started
READY=/tmp/camera-app.ready
APP_LOG=/tmp/camera-app.log
WEB_LOG=/tmp/zr10-web.log
app_pid=
web_pid=
log_pid=

# The lock is a kernel flock, so stale files do not prevent recovery.
exec 9>/tmp/zr10-service.lock
flock -n 9 || { echo 'ZR10 service already running' >&2; exit 1; }
echo $$ >/tmp/zr10-service.pid

stop_child()
{
    [ -n "$app_pid" ] || return 0
    kill -TERM "$app_pid" 2>/dev/null || true
    for _ in $(seq 1 15); do
        kill -0 "$app_pid" 2>/dev/null || break
        sleep 1
    done
    kill -KILL "$app_pid" 2>/dev/null || true
    wait "$app_pid" 2>/dev/null || true
    app_pid=
    rm -f "$READY"
}
wait_for_restart()
{
    echo 'Camera stopped; web UI remains available for diagnosis and restart' >&2
    while [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; do
        check_web
        sleep 1
    done
}
stop()
{
    trap '' INT TERM
    stop_child
    for child in "$web_pid" "$log_pid"; do
        [ -n "$child" ] && kill "$child" 2>/dev/null || true
    done
    rm -f /tmp/zr10-service.pid "$STARTED"
    # Restore signal dispositions before exiting.
    trap - INT TERM
    exit 0
}
trap stop INT TERM

# Flash installs keep the web UI and AP controls available without SD.
# Replacement media output is enabled only with a real card, never into RAM.
if grep -qs '^/dev/mmcblk0p1 /mnt ' /proc/mounts; then
    sd_available=true
else
    CONFIG_ROOT=/tmp/zr10-config-nosd
fi
mkdir -p "$CONFIG_ROOT"
ln -sfn "$APP_ROOT" /tmp/zr10-app
ln -sfn "$CONFIG_ROOT" /tmp/zr10-config
# Exclude the stock app, then let its per-client MI cleanup finish.
for pid in $(pidof sycamera sycamera.vendor 2>/dev/null); do
    [ "$pid" = "$$" ] || kill -TERM "$pid"
done
for _ in $(seq 1 15); do
    pidof sycamera sycamera.vendor >/dev/null || break
    sleep 1
done
if pidof sycamera sycamera.vendor >/dev/null; then
    echo 'Vendor application did not stop; refusing shared hardware' >&2
    rm -f /tmp/zr10-service.pid
    exit 1
fi
# Application-only SD installs may be taking over from the stock HTTP server.
killall boa 2>/dev/null || true
# The vendor leaves loopback down on some boots; local web/API/video uses it.
ifconfig lo 127.0.0.1 up
# Apply the SIYI multicast receive-filter workaround before discovery starts.
# The A8 vendor driver drops group traffic without ALLMULTI; keep ZR10's
# retained driver configured the same way. This is not promiscuous mode.
ifconfig eth0 allmulti || echo 'warning: could not enable Ethernet multicast reception' >&2
[ "$sd_available" = false ] || mkdir -p /mnt/DCIM/record /mnt/DCIM/capture
[ -e "$CONFIG_ROOT/camera.ini" ] || cp "$APP_ROOT/camera.ini.default" "$CONFIG_ROOT/camera.ini"
[ -e "$CONFIG_ROOT/web.pass" ] || cp "$APP_ROOT/web.pass.default" "$CONFIG_ROOT/web.pass"
export CAMERA_APP_CONFIG=$CONFIG_ROOT/camera.ini
export CAMERA_APP_RECORD_ROOT=/mnt/DCIM/record
export CAMERA_APP_CAPTURE_ROOT=/mnt/DCIM/capture
if [ "$sd_available" = false ]; then
    export CAMERA_APP_RECORD_ROOT=/proc/camera-app/record
    export CAMERA_APP_CAPTURE_ROOT=/proc/camera-app/capture
fi
export CAMERA_APP_READY_PATH=$READY
export CAMERA_APP_BACKEND=zr10
"$APP_ROOT/zr10-web" -p 80 >>"$WEB_LOG" 2>&1 9>&- &
web_pid=$!
(
    while sleep 5; do
        for file in "$APP_LOG" "$WEB_LOG" /tmp/sycamera.log; do
            [ -f "$file" ] || continue
            if [ "$(wc -c <"$file")" -gt 524288 ]; then
                tail -c 131072 "$file" >"$file.1"
                : >"$file"
            fi
        done
    done
) 9>&- &
log_pid=$!

check_web()
{
    if ! kill -0 "$web_pid" 2>/dev/null; then
        "$APP_ROOT/zr10-web" -p 80 >>"$WEB_LOG" 2>&1 9>&- &
        web_pid=$!
    fi
}

while true; do
    check_web
    exec 8>/tmp/camera-app.request.lock
    flock -x 8 || exit 1
    generation=$(cat "$REQUEST" 2>/dev/null || echo 0)
    printf '%s\n' "$generation" >"$STARTED.new"
    mv "$STARTED.new" "$STARTED"
    flock -u 8
    exec 8>&-
        rm -f "$READY"
        "$APP_ROOT/camera-app" --backend zr10 --config "$CAMERA_APP_CONFIG" >>"$APP_LOG" 2>&1 9>&- &
        app_pid=$!
        ok=false
        for _ in $(seq 1 45); do
            [ -f "$READY" ] && ok=true && break
            kill -0 "$app_pid" 2>/dev/null || break
            [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" != "$generation" ] && break
            sleep 1
        done
        if [ "$ok" = false ]; then
            stop_child
            if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; then
                echo 'ZR10 startup failed; leaving the web UI available' >&2
                wait_for_restart
            fi
            continue
        fi
    # A web request can race the fork after its PID scan. Observe the request
    # ourselves as well, so a missed old instance cannot block a restart.
    while kill -0 "$app_pid" 2>/dev/null; do
        check_web
        if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" != "$generation" ]; then
            stop_child
            break
        fi
        sleep 1
    done
    [ -z "$app_pid" ] || wait "$app_pid" || true
    app_pid=
    rm -f "$READY"
    # An intentional web restart increments REQUEST; other exits await recovery.
    if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; then
        wait_for_restart
    fi
    sleep 1
done
