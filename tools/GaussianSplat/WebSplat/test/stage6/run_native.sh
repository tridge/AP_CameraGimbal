#!/bin/bash
# Native brush-cli training of a stage 6 Brush dataset (make_brush_dataset.mjs), sampling GPU
# memory; writes OUT/eval_N/images/*.png (16-bit renders), export_N.ply, run.json.
# usage: BRUSH=path/to/brush-cli [ARGS=args.txt] run_native.sh BRUSH_DATASET OUT [extra brush options]
#   ARGS: an edited copy of the dataset's brush_args.txt (default the dataset's own)
set -e
ds=$(realpath "$1"); out=$(realpath -m "$2"); shift 2
args=${ARGS:-$ds/brush_args.txt}
mkdir -p "$out"
nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits -lms 2000 > "$out/gpu.csv" &
smi=$!
trap 'kill $smi 2>/dev/null' EXIT
start=$(date +%s.%N)
# shellcheck disable=SC2046
"$BRUSH" "$ds" $(cat "$args") --eval-every 30000 --eval-save-to-disk --export-every 30000 \
    --export-path "$out" --export-name 'export_{iter}.ply' "$@" > "$out/log.txt" 2>&1
end=$(date +%s.%N)
kill $smi
python3 -c "
import json,sys
rows=[int(l) for l in open('$out/gpu.csv') if l.strip()]
json.dump(dict(wall_s=$end-$start, gpu_mem_peak_mib=max(rows), gpu_mem_before_mib=rows[0]), open('$out/run.json','w'), indent=1)"
cat "$out/run.json"
