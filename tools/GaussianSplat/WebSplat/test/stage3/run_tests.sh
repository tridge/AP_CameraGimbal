#!/usr/bin/env bash
# Stage 3 acceptance (Node part; the browser chain runs in test/harness/thermal.mjs):
#  - the fork's SIFT against native COLMAP CPU SIFT (pycolmap, device=cpu) on the same CLAHE
#    images: like for like, must agree
#  - statistics against the reference database's GPU SIFT (not deterministic; reported)
# Needs WEBSPLAT_DATA, the fork in app/src/stages/map/wasm, the tools venv, a stage 2 output
# (features.u8; default $WEBSPLAT_DATA/stage12/orbit/select-node-single).
set -euo pipefail
: "${WEBSPLAT_DATA:?set WEBSPLAT_DATA}"
here=$(cd "$(dirname "$0")" && pwd)
gs=$(cd "$here/../../.." && pwd)
PY=$gs/.venv/bin/python
D=$WEBSPLAT_DATA
O=$D/stage3
U8=${FEATURES_U8:-$D/stage12/orbit/select-node-single/features.u8}
mkdir -p "$O"
rm -f "$O/node.bin" "$O/node.json"
cd "$here"
echo "== SIFT pool failure paths"
node test_pool.mjs | tail -1
echo "== native COLMAP CPU SIFT"
[ -f "$O/native_cpu.bin" ] || $PY native_sift_cpu.py "$D/ref/work" "$O/native_cpu.bin"
echo "== fork SIFT (Node, one instance)"
node run_sift_node.mjs "$U8" "$D/ref/work/frames.json" "$O/node.bin"
echo "== like for like: fork vs native CPU SIFT"
$PY compare_features.py "$O/native_cpu.bin" "$O/node.bin" --mode exact --selftest --json "$O/node_vs_cpu.json" | tail -1
echo "== statistics vs the reference database (GPU SIFT)"
$PY compare_features.py "$D/g2/native/features.bin" "$O/node.bin" --mode stats --json "$O/node_vs_refgpu.json" | tail -1
echo "ALL PASS"
