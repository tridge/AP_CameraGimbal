#!/usr/bin/env bash

set -euo pipefail

repo_root=$(cd "$(dirname "$0")/.." && pwd)
ardupilot=${1:-"$repo_root/build/ardupilot"}

if [[ ! -x "$ardupilot/waf" ]]; then
    echo "ArduPilot checkout not found at $ardupilot" >&2
    echo "Run tests/clone_ardupilot.sh '$ardupilot' first." >&2
    exit 1
fi

# ArduPilot's Ubuntu prerequisite installer uses this venv on current releases.
if [[ -f "$HOME/venv-ardupilot/bin/activate" ]]; then
    # shellcheck disable=SC1091
    source "$HOME/venv-ardupilot/bin/activate"
fi

make -C "$repo_root" sitl CAMERA=mt11
(
    cd "$ardupilot"
    ./waf configure --board sitl
    ./waf copter
)

export MAVLINK20=1
python3 "$repo_root/tests/test_ardupilot_siyi.py" \
    "$repo_root/build/sitl/camera-app" \
    "$repo_root/sitl/gimbal_sim.py" \
    "$ardupilot/build/sitl/bin/arducopter" \
    "$repo_root/build/sitl/ardupilot-test"
