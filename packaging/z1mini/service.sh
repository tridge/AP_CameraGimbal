#!/bin/sh
# Z1-Mini overlay supervisor. Stock rcS has loaded AX drivers/network.
# main owns the sensor/ISP; gb_control and camera-app are mutually exclusive.
set -u
umask 077
APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CONFIG_ROOT=/opt/ap_cameragimbal
ISP=/opt/bin/gcu/ipc/main
ISP_CONFIG=/opt/bin/gcu/ipc/imx415.json
REQUEST=/tmp/camera-app.request
STARTED=/tmp/camera-app.started
READY=/tmp/camera-app.ready
APP_LOG=/tmp/camera-app.log
WEB_LOG=/tmp/z1mini-web.log
app_pid=
web_pid=
log_pid=
native=false
[ ! -x "$APP_ROOT/ax-capture" ] || native=true
WEB_PORT=8080
[ "$native" = false ] || WEB_PORT=80
unset CAMERA_APP_Z1_NATIVE_HELPER

# The lock is a kernel flock, so stale files do not prevent recovery.
exec 9>/tmp/z1mini-service.lock
flock -n 9 || { echo 'Z1-Mini service already running' >&2; exit 1; }
echo $$ >/tmp/z1mini-service.pid

process_alive()
{
    [ -n "$1" ] && [ -r "/proc/$1/status" ] || return 1
    # A zombie thread-group leader can still have live SDK worker threads.
    awk '/^State:/ {zombie=($2 == "Z")} /^Threads:/ {threads=$2}
         END {exit zombie && threads <= 1}' "/proc/$1/status"
}
controller_running()
{
    for pid in $(pidof gb_control camera-app 2>/dev/null); do
        process_alive "$pid" && return 0
    done
    return 1
}
stop_child()
{
    [ -n "$app_pid" ] || return 0
    kill -TERM "$app_pid" 2>/dev/null || true
    for _ in $(seq 1 15); do
        process_alive "$app_pid" || break
        sleep 1
    done
    kill -KILL "$app_pid" 2>/dev/null || true
    # Target BusyBox ash can stall in wait after SIGCHLD already reaped a job.
    # The bounded liveness check above is sufficient; do not explicitly wait.
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
isp_running()
{
    for pid in $(pidof main 2>/dev/null); do
        process_alive "$pid" && return 0
    done
    return 1
}
start_isp()
{
    if ! isp_running; then
        (cd /opt/bin/gcu/ipc && exec "$ISP" -c "$ISP_CONFIG") </dev/null >>/tmp/z1mini-isp.log 2>&1 9>&- &
    fi
}
stop_isp()
{
    # pidof can stop listing a zombie leader before its workers have exited.
    # Retain the original PIDs throughout teardown rather than re-running pidof.
    isp_pids=$(pidof main 2>/dev/null || true)
    for pid in $isp_pids; do kill -TERM "$pid" 2>/dev/null || true; done
    for _ in $(seq 1 15); do
        isp_busy=false
        for pid in $isp_pids; do process_alive "$pid" && isp_busy=true; done
        [ "$isp_busy" = true ] || return 0
        sleep 1
    done
    echo 'ISP did not stop; refusing native hardware ownership' >&2
    return 1
}

stop()
{
    trap '' INT TERM
    stop_child
    for child in "$web_pid" "$log_pid"; do
        [ -n "$child" ] && kill "$child" 2>/dev/null || true
    done
    rm -f /tmp/z1mini-service.pid "$STARTED"
    # Do not pass the cleanup handler's ignored SIGTERM to the vendor app.
    trap - INT TERM
    exit 0
}
trap stop INT TERM

# Never start with another process owning the gimbal UART.
if controller_running; then
    echo 'A controller is already running; refusing concurrent UART ownership' >&2
    exit 1
fi
mkdir -p "$CONFIG_ROOT"
ifconfig lo 127.0.0.1 up
# The vendor mounts its card at /mnt/mmc. Do not create output paths on rootfs.
if awk '$1 ~ /^\/dev\/mmcblk/ && $2 == "/mnt/mmc" {found=1} END {exit !found}' /proc/mounts; then
    mkdir -p /mnt/mmc/DCIM/record /mnt/mmc/DCIM/capture
fi
if [ "$native" = true ]; then
    stop_isp || exit 1
else
    start_isp
fi
[ -e "$CONFIG_ROOT/camera.ini" ] || cp "$APP_ROOT/camera.ini.default" "$CONFIG_ROOT/camera.ini"
[ -e "$CONFIG_ROOT/web.pass" ] || cp "$APP_ROOT/web.pass.default" "$CONFIG_ROOT/web.pass"
export CAMERA_APP_CONFIG=$CONFIG_ROOT/camera.ini
export CAMERA_APP_RECORD_ROOT=/mnt/mmc/DCIM/record
export CAMERA_APP_CAPTURE_ROOT=/mnt/mmc/DCIM/capture
export CAMERA_APP_READY_PATH=$READY
export CAMERA_APP_BACKEND=z1mini
export CAMERA_APP_UART=/dev/ttyS3
"$APP_ROOT/z1mini-web" -p "$WEB_PORT" >>"$WEB_LOG" 2>&1 9>&- &
web_pid=$!
(
    while sleep 5; do
        for file in "$APP_LOG" "$WEB_LOG" /tmp/gb_control.log /tmp/z1mini-isp.log /tmp/z1mini-service.log; do
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
    if ! process_alive "$web_pid"; then
        "$APP_ROOT/z1mini-web" -p "$WEB_PORT" >>"$WEB_LOG" 2>&1 9>&- &
        web_pid=$!
    fi
}
while true; do
    check_web
    if [ "$native" = false ] && ! isp_running; then
        # ISP recovery owns AX global state; reboot is safer than repeated global
        # teardown/reinitialization until native AX lifecycle is validated.
        echo 'Vendor ISP exited; leaving web UI available for recovery' >&2
        stop_child
        sleep 2
        continue
    fi
    exec 8>/tmp/camera-app.request.lock
    flock -x 8 || exit 1
    generation=$(cat "$REQUEST" 2>/dev/null || echo 0)
    printf '%s\n' "$generation" >"$STARTED.new"
    mv "$STARTED.new" "$STARTED"
    flock -u 8
    exec 8>&-
    if [ "$native" = true ]; then
        if ! stop_isp; then wait_for_restart; continue; fi
        export CAMERA_APP_Z1_NATIVE_HELPER=$APP_ROOT/ax-capture
    fi
        rm -f "$READY"
        "$APP_ROOT/camera-app" --backend z1mini --config "$CAMERA_APP_CONFIG" >>"$APP_LOG" 2>&1 9>&- &
        app_pid=$!
        ok=false
        for _ in $(seq 1 45); do
            [ -f "$READY" ] && ok=true && break
            process_alive "$app_pid" || break
            [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" != "$generation" ] && break
            sleep 1
        done
        if [ "$ok" = false ]; then
            stop_child
            if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; then
                echo 'Z1-Mini startup failed; entering recovery' >&2
                wait_for_restart
            fi
            continue
        fi
    # A web request can race the fork after its PID scan. Observe the request
    # ourselves as well, so a missed old instance cannot block a switch.
    while process_alive "$app_pid"; do
        check_web
        if [ "$native" = false ] && ! isp_running; then
            stop_child
            break
        fi
        if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" != "$generation" ]; then
            stop_child
            break
        fi
        sleep 1
    done
    # BusyBox ash reaps exited children through SIGCHLD.
    app_pid=
    rm -f "$READY"
    # An intentional web restart increments REQUEST; other exits await recovery.
    if [ "$(cat "$REQUEST" 2>/dev/null || echo 0)" = "$generation" ]; then
        wait_for_restart
    fi
    sleep 1
done
