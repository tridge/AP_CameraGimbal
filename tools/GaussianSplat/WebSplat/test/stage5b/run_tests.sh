#!/usr/bin/env bash
# Stage 5b acceptance in Node: the browser stage's scene preparation (app/src/stages/prepare/scene.js)
# against train.py's own scene loading (dump_scene.py) on the same model, frames and options;
# compare_dataset.py checks views, holdouts, scale, undistorted images, targets, bounds, ceiling,
# initial points, hot spot and training settings. The page runs are in test/harness/thermal.mjs.
# Needs WEBSPLAT_DATA, the tools venv, a stage 2 thermal.u16 (default
# $WEBSPLAT_DATA/stage12/orbit/select-node-single) and a browser model (BROWSER_MODEL, default the
# newest harness run's map/final).
#   ./run_tests.sh
set -euo pipefail
: "${WEBSPLAT_DATA:?set WEBSPLAT_DATA}"
here=$(cd "$(dirname "$0")" && pwd)
gs=$(cd "$here/../../.." && pwd)
PY=$gs/.venv/bin/python
D=$WEBSPLAT_DATA
O=$D/stage5b/tests
REF=$D/ref/work
U16=${THERMAL_U16:-$D/stage12/orbit/select-node-single/thermal.u16}
BM=${BROWSER_MODEL:-$(ls -d "$D"/harness/runs/thermal-*/map/final 2>/dev/null | sort | tail -1)}
RUN13=$D/ref/splat_run13/scale.json
[ -f "$BM/images.bin" ] || { echo "no browser model (BROWSER_MODEL)" >&2; exit 2; }
rm -rf "$O"
mkdir -p "$O"
cd "$here"
fails=0

# case NAME MODEL FRAMES [--options scale.json | name=value ...] [--frozen]
case_() {
  local name=$1 model=$2 frames=$3
  shift 3
  local jsargs=() pyargs=() frozen=()
  while [ $# -gt 0 ]; do
    case $1 in
    --options) jsargs+=(--options "$2"); pyargs+=(--options "$2"); shift ;;
    --frozen) frozen=(--frozen) ;;
    *=*)
      local opt=${1%%=*}
      jsargs+=(--opt "$1")
      pyargs+=("--train-arg=--${opt//_/-}=${1#*=}")
      ;;
    esac
    shift
  done
  echo "== $name"
  $PY dump_scene.py "$REF" "$model" "$O/$name-native" "${pyargs[@]}" >"$O/$name-native.log" 2>&1 ||
    { tail -5 "$O/$name-native.log"; echo "FAIL $name: dump_scene.py"; fails=$((fails + 1)); return; }
  node run_node.mjs "$model" "$frames" "$U16" "$O/$name-browser" "${jsargs[@]}" --celsius \
    >"$O/$name-browser.log" 2>&1 ||
    { tail -5 "$O/$name-browser.log"; echo "FAIL $name: run_node.mjs"; fails=$((fails + 1)); return; }
  if $PY compare_dataset.py "$O/$name-native" "$O/$name-browser" "${frozen[@]}" \
    --json "$O/$name.json" >"$O/$name.txt"; then
    tail -1 "$O/$name.txt"
  else
    grep -v '^PASS' "$O/$name.txt"
    fails=$((fails + 1))
  fi
}

echo "== unit checks (float16 conversion, NumPy helpers)"
node test_units.mjs || fails=$((fails + 1))

# (a) the native final model, run13's options (all train.py options recorded in scale.json)
case_ native-run13 "$REF/colmap/sparse/final" "$REF/frames.json" --options "$RUN13" --frozen
# train.py defaults (the SH-only baseline's scene options)
case_ native-default "$REF/colmap/sparse/final" "$REF/frames.json" --frozen
# (b) the browser's model (stages 3-5 in the page)
case_ browser-run13 "$BM" "$REF/frames.json" --options "$RUN13" --frozen
# option variants: cold budget (subsampled cold views, points valued by projection), rejections by
# elevation and observations, explicit scale and bounds options, every view (no budget)
case_ budget "$REF/colmap/sparse/final" "$REF/frames.json" max_images=60
case_ reject "$REF/colmap/sparse/final" "$REF/frames.json" min_obs=1500 min_elevation=42
case_ explicit "$REF/colmap/sparse/final" "$REF/frames.json" t0=5 t1=100 k=4 hot_delta=5 holdout=7 \
  hot_holdout=3 radius=80 height_margin=5 ceiling_cell=3 ceiling_margin=2 iterations=12000 \
  grow_grad2d=0.0004 refine_every=200 max_scale=2 sh_degree=2
case_ all-views "$BM" "$REF/frames.json" max_images=0 holdout=0 hot_holdout=0
# telemetry far from the SfM pose on three frames: misregistration rejection
$PY - "$REF/frames.json" "$O/frames-offset.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
for f in d['frames']:
    if f['name'] in ('00020.png', '00100.png', '00180.png'):
        f['position'][0] += 60.0
json.dump(d, open(sys.argv[2], 'w'))
PY
mkdir -p "$O/work-offset"
ln -sfn "$REF/thermal" "$O/work-offset/thermal"
cp "$O/frames-offset.json" "$O/work-offset/frames.json"
REF_SAVED=$REF
REF=$O/work-offset
case_ pose-offset "$REF_SAVED/colmap/sparse/final" "$O/frames-offset.json"
REF=$REF_SAVED
# PINHOLE camera with unequal focal lengths (no undistortion): the native model's camera rewritten
$PY - "$REF/colmap/sparse/final" "$O/pinhole-model" <<'PY'
import struct, sys
from pathlib import Path
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
dst.mkdir(parents=True, exist_ok=True)
b = (src / 'cameras.bin').read_bytes()
n, cid, model, w, h = struct.unpack_from('<QIiQQ', b, 0)
assert n == 1 and model == 2, 'expected one SIMPLE_RADIAL camera'
f, cx, cy, k1 = struct.unpack_from('<4d', b, 32)
(dst / 'cameras.bin').write_bytes(struct.pack('<QIiQQ4d', 1, cid, 1, w, h, f, f * 1.0123, cx + 0.37, cy - 0.29))
for name in ('images.bin', 'points3D.bin'):
    (dst / name).unlink(missing_ok=True)
    (dst / name).symlink_to(src / name)
PY
case_ pinhole "$O/pinhole-model" "$REF/frames.json"

for c in budget reject pose-offset; do
  $PY - "$O/$c-browser/dataset.json" "$c" <<'PY' || fails=$((fails + 1))
import json, sys
d, c = json.load(open(sys.argv[1])), sys.argv[2]
s = d['skipped']
need = dict(budget=('budget', d['stats']['points_unseen']), reject=('min_obs', len(s['elevation'])),
            **{'pose-offset': ('pose_offset', len(s['pose_offset']))})[c]
ok = len(s[need[0]]) > 0 and need[1] > 0
print('%s %s exercises %s: skipped %s, unseen points %d' % ('PASS' if ok else 'FAIL', c, need[0],
      {k: len(v) for k, v in s.items() if isinstance(v, list)}, d['stats']['points_unseen']))
sys.exit(0 if ok else 1)
PY
done

echo "== negative controls (the comparator must fail on perturbed datasets)"
$PY negative.py "$O/native-default-native" "$O/native-default-browser" "$O/reject-native" "$O/reject-browser" \
  "$O/negative" || fails=$((fails + 1))

if [ "$fails" -ne 0 ]; then
  echo "FAILED: $fails"
  exit 1
fi
echo "ALL PASS"
