#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
ardupilot=${1:-"$repo_root/build/ardupilot"}
if [[ -f "$HOME/venv-ardupilot/bin/activate" ]]; then
    # shellcheck disable=SC1091
    source "$HOME/venv-ardupilot/bin/activate"
fi
if [[ ! -x "$ardupilot/build/sitl/bin/arducopter" ]]; then
    echo "Build ArduPilot SITL first (tests/run_ardupilot_mavlink_test.sh)." >&2
    exit 1
fi
make -C "$repo_root" sitl CAMERA="mt11 a8"
export MAVLINK20=1
python3 "$repo_root/tests/test_ardupilot_rc_mount.py" \
    --arducopter "$ardupilot/build/sitl/bin/arducopter" \
    --output "$repo_root/build/rc-mount-test"
