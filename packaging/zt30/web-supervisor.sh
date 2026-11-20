#!/bin/sh
# Independent AP web recovery; camera pipeline waits never block this service.
set -eu
app_dir=${APCG_ZT30_APP_DIR:-/customer/camera-app}
run_dir=${APCG_ZT30_RUN_DIR:-/tmp/apcg-zt30}
mkdir -p "$run_dir"
exec 9>"$run_dir/web-supervisor.lock"
flock -n 9 || exit 0
trap 'exit 0' HUP INT TERM
payload_failed=0
web_retry_after=0
web_port=${APCG_ZT30_WEB_PORT:-80}
while :; do
    if "$app_dir/zt30-slot" --root "$app_dir" --run-dir "$run_dir" maintain "$web_port"; then
        :
    else
        result=$?
        if [ "$result" = 3 ]; then sleep 1; continue; fi
        echo 'Application trial maintenance failed; recovery SSH remains available' >&2
    fi
    if "$app_dir/zt30-slot" --root "$app_dir" web-listener "$web_port" >/dev/null; then
        sleep 3
        continue
    fi
    read -r uptime rest < /proc/uptime
    now=${uptime%%.*}
    if [ "$now" -lt "$web_retry_after" ]; then
        sleep 1
        continue
    fi
    if "$app_dir/zt30-slot" --root "$app_dir" --run-dir "$run_dir" note-web-launch "$web_port"; then
        :
    else
        result=$?
        if [ "$result" = 3 ]; then sleep 1; continue; fi
        echo 'Application trial launch accounting failed; starting recovery web service anyway' >&2
    fi
    if ! selection=$("$app_dir/zt30-slot" --root "$app_dir" resolve 2>&1); then
        if [ "$payload_failed" = 0 ]; then
            echo "No valid AP web payload; recovery SSH remains available; retrying: $selection" >&2
        fi
        payload_failed=1
        sleep 5
        continue
    fi
    payload_failed=0
    slot=$selection
    export CAMERA_APP_WEBROOT="$slot/webroot"
    web_retry_after=$((now + 10))
    "$slot/zt30-web" -p "$web_port" 9>&- >>"$run_dir/zt30-web.log" 2>&1 &
    sleep 1
done
