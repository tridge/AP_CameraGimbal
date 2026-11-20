#!/bin/sh
# AP-only service restart. Vendor software is never started by this supervisor.
set -eu
app_dir=${APCG_ZT30_APP_DIR:-/customer/camera-app}
run_dir=${APCG_ZT30_RUN_DIR:-/tmp/apcg-zt30}
: "${CAMERA_APP_READY_PATH:=$run_dir/camera-app.ready}"
export CAMERA_APP_READY_PATH
mkdir -p "$run_dir"
# Serialize supervisors without stale persistent locks across reboots.
exec 9>"$run_dir/supervisor.lock"
flock -n 9 || exit 0
trap 'exit 0' HUP INT TERM
payload_failed=0
while :; do
    if ! selection=$("$app_dir/zt30-slot" --root "$app_dir" resolve 2>&1); then
        if [ "$payload_failed" = 0 ]; then
            echo "No valid AP payload; recovery SSH remains available; retrying: $selection" >&2
        fi
        payload_failed=1
        sleep 5
        continue
    fi
    payload_failed=0
    slot=$selection
    export CAMERA_APP_ZT30_SDK="$slot/encoder-sdk.so"
    export CAMERA_APP_WEBROOT="$slot/webroot"
    if pidof siyi_app_359 >/dev/null 2>&1; then
        echo 'Vendor owns media; AP supervisor stopping' >&2
        exit 1
    fi
    if pidof camera-app >/dev/null 2>&1; then
        sleep 3
        continue
    fi
    # The common web server publishes a restart generation and kills the old
    # app. Record this generation before starting, as its restart waiter expects.
    generation=$(cat "$run_dir/apcg-app-request" 2>/dev/null || echo 0)
    case "$generation" in ''|*[!0-9]*) generation=0;; esac
    printf '%s\n' "$generation" > "$run_dir/apcg-app-started.new"
    mv "$run_dir/apcg-app-started.new" "$run_dir/apcg-app-started"
    rm -f "$CAMERA_APP_READY_PATH"
    # Resolve once per launch. A pointer switch never mixes an app, SDK and
    # logger from different releases; old slots remain until processes exit.
    "$slot/camera-app" 9>&- 2>&1 | "$slot/zt30-web" --capture-app-log replacement 9>&- || true
    sleep 2
done
