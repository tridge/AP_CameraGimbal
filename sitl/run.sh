#!/bin/sh
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
backend=${CAMERA_GIMBAL_SITL_BACKEND:-mt11}
case "$backend" in
    mt11)
        build=${CAMERA_GIMBAL_SITL_BUILD:-$repo/build/sitl}
        web_binary=mt11-web
        label=MT11
        camera_backend_args=
        config_template="$repo/camera_app/camera.ini"
        gimbal_port=${MT11_SITL_GIMBAL_PORT:-0}
        camera_port=${MT11_SITL_CAMERA_PORT:-37260}
        rtsp_port=${MT11_SITL_RTSP_PORT:-8554}
        web_port=${MT11_SITL_WEB_PORT:-8081}
        orientation=${MT11_SITL_ORIENTATION:-upright}
        video1="$build/rgb.h264"
        video2="$build/thermal.h264"
        build_hint="make sitl"
        ;;
    a8)
        build=${CAMERA_GIMBAL_SITL_BUILD:-$repo/build/a8-sitl}
        web_binary=a8-web
        label=A8
        camera_backend_args="--backend a8"
        config_template="$repo/packaging/a8/camera.ini"
        gimbal_port=${A8_SITL_GIMBAL_PORT:-0}
        camera_port=${A8_SITL_CAMERA_PORT:-37260}
        rtsp_port=${A8_SITL_RTSP_PORT:-8554}
        web_port=${A8_SITL_WEB_PORT:-8081}
        orientation=${A8_SITL_ORIENTATION:-upright}
        video1="$build/main.h264"
        video2="$build/sub.h264"
        build_hint="make a8_sitl"
        ;;
    zr10)
        build=${CAMERA_GIMBAL_SITL_BUILD:-$repo/build/zr10-sitl}
        web_binary=zr10-web
        label=ZR10
        camera_backend_args="--backend zr10"
        config_template="$repo/sitl/zr10.ini"
        gimbal_port=${ZR10_SITL_GIMBAL_PORT:-0}
        camera_port=${ZR10_SITL_CAMERA_PORT:-37260}
        rtsp_port=${ZR10_SITL_RTSP_PORT:-8554}
        web_port=${ZR10_SITL_WEB_PORT:-8081}
        orientation=${ZR10_SITL_ORIENTATION:-upright}
        video1="$build/main.h264"
        video2="$build/sub.h264"
        build_hint="make zr10_sitl"
        ;;
    z1mini)
        build=${CAMERA_GIMBAL_SITL_BUILD:-$repo/build/z1mini-sitl}
        web_binary=z1mini-web
        label=Z1MINI
        camera_backend_args="--backend z1mini"
        config_template="$repo/packaging/z1mini/camera.ini"
        gimbal_port=${Z1MINI_SITL_GIMBAL_PORT:-0}
        camera_port=${Z1MINI_SITL_CAMERA_PORT:-2337}
        rtsp_port=${Z1MINI_SITL_RTSP_PORT:-8554}
        web_port=${Z1MINI_SITL_WEB_PORT:-8081}
        orientation=${Z1MINI_SITL_ORIENTATION:-upright}
        video1="$build/main.h264"
        video2="$build/sub.h264"
        build_hint="make z1mini_sitl"
        ;;
    *)
        echo "unsupported SITL backend: $backend" >&2
        exit 2
        ;;
esac
runtime="$build/runtime"
gimbal_ready="$runtime/run/gimbal.ready"
camera_ready="$runtime/run/camera-app.ready"
launcher_lock="$runtime/run/launcher.lock"
launcher_pid_file="$runtime/run/launcher.pid"
gimbal_pid=
camera_pid=
web_pid=

# Inherited by both camera-app and the web service's camera restart child.
if [ -z "${CAMERA_GIMBAL_SITL_PYTHON:-}" ] && [ -x "$repo/build/terrain-venv/bin/python" ]; then
    CAMERA_GIMBAL_SITL_PYTHON="$repo/build/terrain-venv/bin/python"
    export CAMERA_GIMBAL_SITL_PYTHON
fi
CAMERA_APP_SITL_RENDERER="$repo/sitl/terrain_video.py"
export CAMERA_APP_SITL_RENDERER
case "${CAMERA_GIMBAL_SITL_VIDEO:-simple}" in
    simple)
        unset CAMERA_APP_SITL_TERRAIN
        "${CAMERA_GIMBAL_SITL_PYTHON:-python3}" "$CAMERA_APP_SITL_RENDERER" --check --simple
        ;;
    terrain)
        CAMERA_APP_SITL_TERRAIN="$repo/sitl/terrain_video.py"
        export CAMERA_APP_SITL_TERRAIN
        "${CAMERA_GIMBAL_SITL_PYTHON:-python3}" "$CAMERA_APP_SITL_TERRAIN" --check
        ;;
    *) echo "CAMERA_GIMBAL_SITL_VIDEO must be simple or terrain" >&2; exit 2 ;;
esac

# camera.ini owns the normal MAVLink listener configuration, including changes
# made through the web UI.  Only turn the launcher variables into process-wide
# overrides when the caller explicitly supplied them; exporting default values
# here would make every web-triggered camera restart ignore the saved INI.
unset CAMERA_APP_MAVLINK_TCP_PORT CAMERA_APP_MAVLINK_UDP_PORT
for transport in TCP UDP; do
    key="${label}_SITL_MAVLINK_${transport}_PORT"
    # Only these fixed backend/transport names are evaluated, never values.
    eval "present=\${${key}+set}"
    if [ "$present" = set ]; then
        eval "port=\${${key}}"
        export "CAMERA_APP_MAVLINK_${transport}_PORT=$port"
    elif [ "$backend" = a8 ] && [ -z "${CAMERA_GIMBAL_SITL_INSTANCE:-}" ]; then
        # Preserve standalone A8 isolation; GUI slots seed their own ports.
        export "CAMERA_APP_MAVLINK_${transport}_PORT=0"
    fi
done

stop_processes()
{
    trap - EXIT INT TERM
    [ -z "$web_pid" ] || kill "$web_pid" 2>/dev/null || true
    [ -z "$camera_pid" ] || kill "$camera_pid" 2>/dev/null || true
    [ -z "$gimbal_pid" ] || kill "$gimbal_pid" 2>/dev/null || true
    [ -z "$web_pid" ] || wait "$web_pid" 2>/dev/null || true
    [ -z "$camera_pid" ] || wait "$camera_pid" 2>/dev/null || true
    [ -z "$gimbal_pid" ] || wait "$gimbal_pid" 2>/dev/null || true
    rm -f "$launcher_pid_file"
}
trap stop_processes EXIT INT TERM

if [ ! -x "$build/camera-app" ] || [ ! -x "$build/$web_binary" ]; then
    echo "SITL is not built; run '$build_hint' first" >&2
    exit 1
fi

python3 "$repo/sitl/prepare_runtime.py" "$runtime" "$config_template"
if python3 "$repo/sitl/check_running.py" "$web_port" \
        "$runtime/app/web.pass"; then
    echo "$label SITL is already running at http://127.0.0.1:$web_port/" >&2
    echo "Stop its original make sitl-run with Ctrl-C before starting another." >&2
    exit 1
fi
exec 9>"$launcher_lock"
if ! flock -n 9; then
    echo "$label SITL is already running; stop its original launcher with Ctrl-C" >&2
    exit 1
fi
case "${CAMERA_GIMBAL_SITL_RESET_PARAMETERS:-0}" in
    0) ;;
    1) python3 "$repo/sitl/prepare_runtime.py" "$runtime" "$config_template" --reset-parameters ;;
    *) echo "CAMERA_GIMBAL_SITL_RESET_PARAMETERS must be 0 or 1" >&2; exit 2 ;;
esac
printf '%s\n' "$$" >"$launcher_pid_file"
rm -f "$gimbal_ready" "$camera_ready"
python3 "$repo/sitl/gimbal_sim.py" --backend "$backend" --port "$gimbal_port" \
    --orientation "$orientation" --ready-file "$gimbal_ready" \
    >"$runtime/run/gimbal.log" 2>&1 9>&- &
gimbal_pid=$!

attempt=0
while [ ! -f "$gimbal_ready" ]; do
    if ! kill -0 "$gimbal_pid" 2>/dev/null; then
        echo "gimbal simulator failed to start; see $runtime/run/gimbal.log" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo "gimbal simulator did not become ready; see $runtime/run/gimbal.log" >&2
        exit 1
    fi
    sleep 0.05
done
IFS=: read -r gimbal_host gimbal_port <"$gimbal_ready"

CAMERA_APP_BACKEND="$backend" \
CAMERA_APP_UART="udp://$gimbal_host:$gimbal_port" \
CAMERA_APP_PORT="$camera_port" \
CAMERA_APP_RTSP_PORT="$rtsp_port" \
CAMERA_APP_CONFIG="$runtime/app/camera.ini" \
CAMERA_APP_READY_PATH="$camera_ready" \
CAMERA_APP_RECORD_STATE="$runtime/run/recording.state" \
CAMERA_APP_RECORD_ROOT="$runtime/mnt/DCIM/record" \
CAMERA_APP_LOG_ROOT="$runtime/mnt/logs" \
CAMERA_APP_CAPTURE_ROOT="$runtime/mnt/DCIM/capture" \
CAMERA_APP_SITL_VIDEO1="$video1" \
CAMERA_APP_SITL_VIDEO2="$video2" \
CAMERA_APP_SITL_PHOTO="$build/photo.jpg" \
    "$build/camera-app" $camera_backend_args >"$runtime/run/camera_app.log" 2>&1 9>&- &
camera_pid=$!

attempt=0
while [ ! -f "$camera_ready" ]; do
    if ! kill -0 "$camera_pid" 2>/dev/null; then
        echo "camera-app failed to start; see $runtime/run/camera_app.log" >&2
        exit 1
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 100 ]; then
        echo "camera-app did not become ready; see $runtime/run/camera_app.log" >&2
        exit 1
    fi
    sleep 0.05
done

CAMERA_APP_BACKEND="$backend" \
CAMERA_APP_UART="udp://$gimbal_host:$gimbal_port" \
CAMERA_APP_PORT="$camera_port" \
CAMERA_APP_RTSP_PORT="$rtsp_port" \
CAMERA_APP_CONFIG="$runtime/app/camera.ini" \
CAMERA_APP_READY_PATH="$camera_ready" \
CAMERA_APP_RECORD_STATE="$runtime/run/recording.state" \
CAMERA_APP_RECORD_ROOT="$runtime/mnt/DCIM/record" \
CAMERA_APP_LOG_ROOT="$runtime/mnt/logs" \
CAMERA_APP_CAPTURE_ROOT="$runtime/mnt/DCIM/capture" \
CAMERA_APP_SITL_VIDEO1="$video1" \
CAMERA_APP_SITL_VIDEO2="$video2" \
CAMERA_APP_SITL_PHOTO="$build/photo.jpg" \
MT11_WEB_CAMERA_PORT="$camera_port" \
MT11_WEB_LIVE_PORT="$((rtsp_port + 1))" \
    "$build/$web_binary" -p "$web_port" >"$runtime/run/web.log" 2>&1 9>&- &
web_pid=$!

sleep 0.1
if ! kill -0 "$web_pid" 2>/dev/null; then
    echo "web service failed to start; see $runtime/run/web.log" >&2
    exit 1
fi

echo "$label SITL running"
echo "  Web UI:   http://127.0.0.1:$web_port/ (admin / ardupilot)"
echo "  SIYI API: 127.0.0.1:$camera_port (UDP and TCP)"
echo "  Gimbal:   udp://$gimbal_host:$gimbal_port ($orientation)"
if [ "$backend" = a8 ] && [ "${CAMERA_APP_MAVLINK_TCP_PORT:-0}" = 0 ] &&
        [ "${CAMERA_APP_MAVLINK_UDP_PORT:-0}" = 0 ]; then
    echo "  MAVLink:  disabled (set ${label}_SITL_MAVLINK_TCP_PORT and/or ${label}_SITL_MAVLINK_UDP_PORT)"
fi
echo "  Logs:     $runtime/run"
echo "Press Ctrl-C to stop."

wait "$web_pid"
