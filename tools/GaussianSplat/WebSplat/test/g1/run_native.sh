#!/bin/bash
# Train the G1 dataset with the native Brush fork, sampling GPU memory.
# usage: BRUSH=path/to/brush-cli [ITERS=n] run_native.sh DATASET OUT [extra brush options]
set -e
dataset=$(realpath "$1"); out=$(realpath -m "$2"); shift 2
mkdir -p "$out"
comment="apcg_splat $(python3 -c 'import json,sys; print(json.dumps(json.load(open(sys.argv[1]))["apcg_splat"], separators=(",", ":")))' "$dataset/g1.json")"
nvidia-smi --query-gpu=memory.used,utilization.gpu --format=csv,noheader,nounits -lms 2000 > "$out/gpu.csv" &
smi=$!
trap 'kill $smi 2>/dev/null' EXIT
base=$(nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits)
opts=$(cat "$dataset/brush_args.txt")
[ -n "$ITERS" ] && opts=$(echo "$opts" | sed "s/--total-train-iters [0-9]*/--total-train-iters $ITERS/")
start=$(date +%s.%N)
# shellcheck disable=SC2046
"$BRUSH" "$dataset" $opts --eval-every "${EVAL_EVERY:-5000}" --eval-save-to-disk \
    --export-every "${ITERS:-30000}" --export-path "$out" --export-name 'export_{iter}.ply' \
    --export-comment "$comment" "$@" > "$out/log.txt" 2>&1
end=$(date +%s.%N)
kill $smi
peak=$(sort -t, -k1 -n "$out/gpu.csv" | tail -1 | cut -d, -f1)
python3 - "$out" "$start" "$end" "$base" "$peak" <<'EOF'
import json, sys
out, start, end, base, peak = sys.argv[1], *map(float, sys.argv[2:])
r = dict(wall_s=end - start, gpu_mem_before_mib=base, gpu_mem_peak_mib=peak, gpu_mem_peak_delta_mib=peak - base)
open(out + '/run.json', 'w').write(json.dumps(r, indent=1))
print(r)
EOF
