#!/bin/bash
# GPU buffer lifetime regression test: a 3500-iteration browser run (laptop flags) must stay valid
# (no "used in submit while destroyed" or any other GPU error). As a negative control, a package
# built with lifetime-negative-control.patch applied to the CubeCL fork (destroy right after the
# next submit, no ownership) fails it at iteration 75 or 150 in about 2 of 3 runs, so the run is
# repeated REPEAT (default 3) times; all must be valid.
# usage: WEBSPLAT_DATA=... [REPEAT=n] lifetime_test.sh PKG DATASET OUT [extra run_browser.mjs options]
set -e
pkg=$1 dataset=$2 out=$3; shift 3
here=$(dirname "$0")
for i in $(seq "${REPEAT:-3}"); do
  run="$out/run$i"
  rm -rf "$run"; mkdir -p "$out"
  if ! node "$here/run_browser.mjs" --dataset "$dataset" --pkg "$pkg" --out "$run" --iters 3500 --steps 5 "$@" > "$run.log" 2>&1; then
    echo "lifetime test: FAIL in run $i ($(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["reason"].splitlines()[0])' "$run/run.json"))"
    exit 1
  fi
done
echo "lifetime test: PASS (${REPEAT:-3} runs)"
