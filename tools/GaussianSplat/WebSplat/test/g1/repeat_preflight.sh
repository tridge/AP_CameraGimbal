#!/bin/bash
# Remote repeat series with an environment preflight per attempt (notes/g1.md, win11 series 4):
# relaunch the test Chrome, require the NVIDIA adapter in a page and a worker (preflight.mjs), then
# run the attempt. Failed preflights are logged as 'preflight' (not attempts), at most 10.
# Stops at N valid runs or more than 3 invalid attempts. Writes OUT/ledger.jsonl and OUT/runK.
# usage: WEBSPLAT_DATA=... repeat_preflight.sh N PKG DATASET OUT HOST [CDP_PORT [APP_PORT]]
# Expects the ssh tunnels (-L CDP_PORT, -R APP_PORT) and win11/start.ps1, stop.ps1 copied to
# the host's /tmp/g1-start.ps1, /tmp/g1-stop.ps1 (win11/README).
n=$1 pkg=$2 dataset=$3 out=$4 host=$5 cdp=${6:-9353} app=${7:-8782}
here=$(dirname "$0")
mkdir -p "$out"
ledger="$out/ledger.jsonl"
ps1() { ssh -o LogLevel=ERROR "$host" "powershell -NoProfile -ExecutionPolicy Bypass -File \$(cygpath -w /tmp/g1-$1.ps1) $2" > /dev/null; }
entry() { python3 -c 'import json,sys,time; d=json.loads(sys.argv[1]); d["utc"]=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()); print(json.dumps(d))' "$1" >> "$ledger"; }
valid=0 invalid=0 pre=0 k=0
while [ "$valid" -lt "$n" ] && [ "$invalid" -le 3 ] && [ "$pre" -le 10 ]; do
  ps1 stop; ps1 start "-port $cdp"
  for i in $(seq 30); do curl -s "http://127.0.0.1:$cdp/json/version" > /dev/null && break; sleep 1; done
  if ! res=$(node "$here/preflight.mjs" --cdp "http://127.0.0.1:$cdp" --port "$app"); then
    pre=$((pre + 1))
    entry "{\"kind\": \"preflight\", \"ok\": false, \"result\": ${res:-null}}"
    echo "preflight $pre failed: $res"
    continue
  fi
  k=$((k + 1))
  run="run$k"
  entry "{\"kind\": \"attempt\", \"run\": \"$run\", \"preflight\": $res}"
  if node "$here/run_browser.mjs" --dataset "$dataset" --pkg "$pkg" --out "$out/$run" --steps 200 \
      --port "$app" --cdp "http://127.0.0.1:$cdp" --gpu-ssh "$host" > "$out/$run.log" 2>&1; then
    valid=$((valid + 1)); echo "$run valid"
  else
    invalid=$((invalid + 1)); echo "$run INVALID"
  fi
done
ps1 stop
echo "valid $valid, invalid $invalid, preflight failures $pre"
[ "$valid" -eq "$n" ] && [ "$invalid" -le 3 ] && [ "$pre" -le 10 ]
