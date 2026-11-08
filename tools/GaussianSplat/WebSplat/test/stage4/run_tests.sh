#!/bin/bash
# Stage 4 acceptance: pairs, quantisation, containers, matcher edge cases, full reference run.
#   WEBSPLAT_DATA=... [TAG=name] test/stage4/run_tests.sh [run_match.mjs options]
# e.g. local laptop GPU: no options; win11: --cdp 9334 --port 8790 (see notes/stage4.md).
# Needs $WEBSPLAT_DATA/stage4/native from export_native.py and native_raw.bin from
# native_rematch.py (CUDA, ideally the reference GPU model). Any failing step fails the run.
set -euo pipefail
cd "$(dirname "$0")"
: "${WEBSPLAT_DATA:?set WEBSPLAT_DATA}"
PY=../../../.venv/bin/python
NATIVE="$WEBSPLAT_DATA/stage4/native"
TAG=${TAG:-accept}
OUT="$WEBSPLAT_DATA/stage4/out/$TAG.bin"

[ -f "$NATIVE/index.json" ] || $PY export_native.py "$WEBSPLAT_DATA/ref/work" "$NATIVE"
[ -f "$NATIVE/native_raw.bin" ] || { echo "missing $NATIVE/native_raw.bin: run native_rematch.py on a CUDA machine"; exit 1; }

step() { echo "== $1"; }
step pairs;        node test_pairs.mjs | tail -1
step quantise;     node test_quantize.mjs | tail -1
step containers;   node test_format.mjs
step edge;         node run_match.mjs "$@" "mode=edge" | grep -E '"pass"|FAIL|loop-var|error'
step full
rm -f "$OUT"
node run_match.mjs "$@" "upload=1&tag=$TAG" | grep -E '"(vendor|architecture|msPerPair|pairsPerS|matchMs|pass)"|error'
[ -f "$OUT" ] || { echo "FAIL no fresh output $OUT"; exit 1; }
step compare;      $PY compare_matches.py "$NATIVE" "$OUT" | tail -1
step selftest;     $PY compare_matches.py "$NATIVE" "$OUT" --selftest | tail -1
echo "PASS stage 4 ($TAG)"
