#!/bin/bash
# usage: make_splat.sh THERMAL.mkv WORKDIR LAT LON [extract_frames.py options]
# Environment: SPLAT_PYTHON (default: .venv next to this script), TRAIN_ARGS
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
PY=${SPLAT_PYTHON:-$HERE/.venv/bin/python}
if [ $# -lt 4 ]; then
    echo "usage: $0 THERMAL.mkv WORKDIR LAT LON [extract_frames.py options]" >&2
    exit 1
fi
MKV=$1 WORK=$2 LAT=$3 LON=$4
shift 4

[ -f "$WORK/frames.json" ] || "$PY" "$HERE/extract_frames.py" "$MKV" "$WORK" --lat "$LAT" --lon "$LON" "$@"
[ -d "$WORK/colmap/sparse/final" ] || "$PY" "$HERE/sfm.py" "$WORK"
"$PY" "$HERE/train.py" "$WORK" ${TRAIN_ARGS:-}
echo "view with: $PY $HERE/view_splat.py $WORK"
