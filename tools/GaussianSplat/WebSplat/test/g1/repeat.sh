#!/bin/bash
# Repeat policy runner (notes/g1.md): N valid 30k-iteration browser runs, replacing invalid ones
# (at most 3). Each run goes to OUT/runN; invalid runs keep their logs with rejected artifacts.
# usage: WEBSPLAT_DATA=... repeat.sh N PKG DATASET OUT [run_browser.mjs options, e.g. remote ones]
n=$1 pkg=$2 dataset=$3 out=$4; shift 4
here=$(dirname "$0")
mkdir -p "$out"
valid=0 invalid=0 i=0
while [ "$valid" -lt "$n" ] && [ "$invalid" -le 3 ]; do
  i=$((i + 1))
  run="$out/run$i"
  rm -rf "$run"
  if node "$here/run_browser.mjs" --dataset "$dataset" --pkg "$pkg" --out "$run" --steps 200 "$@" > "$run.log" 2>&1; then
    valid=$((valid + 1)); echo "run$i valid"
  else
    invalid=$((invalid + 1)); echo "run$i INVALID: $(python3 -c 'import json,sys; print((json.load(open(sys.argv[1]))["reason"] or "").splitlines()[0][:200])' "$run/run.json" 2>/dev/null)"
  fi
done
echo "valid $valid, invalid $invalid"
[ "$valid" -eq "$n" ]
