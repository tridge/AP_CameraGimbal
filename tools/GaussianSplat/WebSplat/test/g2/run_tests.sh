#!/usr/bin/env bash
# G2 acceptance, in order (about 15 min on the laptop). Needs WEBSPLAT_DATA, the fork built
# into app/src/stages/map/wasm (build-colmap.sh), the tools venv (pycolmap) and stage 4's
# exports ($WEBSPLAT_DATA/stage4/native, stage4/out/laptop-nv.bin).
#   ./run_tests.sh [--win11 CDP_PORT HTTP_PORT]   (a Chrome on the win11 machine, see notes/g2.md)
set -euo pipefail
: "${WEBSPLAT_DATA:?set WEBSPLAT_DATA}"
here=$(cd "$(dirname "$0")" && pwd)
gs=$(cd "$here/../../.." && pwd)
PY=$gs/.venv/bin/python
D=$WEBSPLAT_DATA
N=$D/g2/native
O=$D/g2/out
REF=$D/ref/work/colmap/sparse/final
cd "$here"
rm -rf "$O" "$D/g2/browser/laptop-g2" "$D/g2/browser/win11-g2"
mkdir -p "$O"

echo "== native exports"
[ -f "$N/features.bin" ] || $PY export_native.py "$D/ref/work" "$N"
[ -f "$N/verify_seed0/geometry.bin" ] ||
  $PY native_verify.py "$D/ref/work" "$D/stage4/native/native_raw.bin" "$N/verify_seed0" --seed 0

echo "== telemetry model"
node check_telemetry.mjs "$D/ref/work/frames.json" "$D/ref/work/colmap/sparse/telemetry/images.txt"

echo "== colmap.js heap handling"
node test_colmap_js.mjs
node test_pool.mjs | tail -1

echo "== stop points (Node callbacks)"
node test_cancel.mjs "$D/ref/work/frames.json" "$N/features.bin" "$D/stage4/native/native_raw.bin" \
  "$D/stage4/native/geometry.bin"

echo "== mapping resume from every round checkpoint (Node)"
node test_resume.mjs "$D/ref/work/frames.json" "$N/features.bin" "$D/stage4/native/geometry.bin" | tail -4

echo "== 1. verification of the native raw matches (Node)"
node run_node.mjs verify "$N/features.bin" "$D/stage4/native/native_raw.bin" "$N/camera.json" "$O/verify_native.bin" --seed 0
$PY compare_geometry.py "$N/verify_seed0/geometry.bin" "$O/verify_native.bin" --identical --allow 5 \
  --model-tol 1e-6 --json "$O/verify_native_vs_seed0.json" | tail -1
$PY check_models.py "$N/verify_seed0/geometry.bin" "$O/verify_native.bin" "$N/features.bin" \
  "$D/stage4/native/native_raw.bin" "$N/camera.json" --json "$O/verify_native_models.json" | tail -1
$PY compare_geometry.py "$D/stage4/native/geometry.bin" "$O/verify_native.bin" --min-ratio 0.98 \
  --json "$O/verify_native_vs_ref.json" | tail -1

echo "== 2. mapping from the native verified geometry (Node)"
node run_node.mjs map "$D/ref/work/frames.json" "$N/features.bin" "$D/stage4/native/geometry.bin" "$O/map_native"
$PY compare_models.py "$REF" "$O/map_native" --json "$O/map_native/compare.json" | tail -1

echo "== 3. browser-matched native descriptors -> verification -> mapping (Node)"
node run_node.mjs verify "$N/features.bin" "$D/stage4/out/laptop-nv.bin" "$N/camera.json" "$O/verify_browser.bin" --seed 0
$PY compare_geometry.py "$D/stage4/native/geometry.bin" "$O/verify_browser.bin" --min-ratio 0.9 | tail -1
$PY check_models.py "$O/verify_browser.bin" "$O/verify_browser.bin" "$N/features.bin" \
  "$D/stage4/out/laptop-nv.bin" "$N/camera.json" | tail -1
node run_node.mjs map "$D/ref/work/frames.json" "$N/features.bin" "$O/verify_browser.bin" "$O/map_browser"
$PY compare_models.py "$REF" "$O/map_browser" --json "$O/map_browser/compare.json" | tail -1

echo "== Stop path in Chrome workers (pool.js: abort -> terminate + AbortError within 3 s)"
node run_browser.mjs --tag laptop-stop "mode=stop&workers=4" | tail -1

echo "== 2-3 in module workers, local headless Chrome"
node run_browser.mjs --tag laptop-g2 "matches=laptop-nv&workers=${WORKERS:-6}" | tail -1
B=$D/g2/browser/laptop-g2
cmp "$B/geometry.bin" "$O/verify_browser.bin"
for f in cameras images points3D; do cmp "$B/model/$f.bin" "$O/map_browser/$f.bin"; done
echo "Chrome output identical to Node"

if [ "${1:-}" = "--win11" ]; then
  echo "== win11 Chrome"
  node run_browser.mjs --cdp "$2" --port "$3" --tag win11-stop "mode=stop&workers=4" | tail -1
  node run_browser.mjs --cdp "$2" --port "$3" --tag win11-g2 "matches=laptop-nv&workers=8" | tail -1
  W=$D/g2/browser/win11-g2
  cmp "$W/geometry.bin" "$O/verify_browser.bin"
  for f in cameras images points3D; do cmp "$W/model/$f.bin" "$O/map_browser/$f.bin"; done
  echo "win11 output identical to Node"
fi
echo "ALL PASS"
