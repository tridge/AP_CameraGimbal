#!/usr/bin/env bash
# Stage 7 acceptance: the export stage and the result view.
#  1. Node: the streaming export (stream.js) of the tiny model, G1's laptop browser run (Brush
#     PLY), run13 (train.py PLY) and a synthetic 2M-Gaussian Brush PLY; byte-identical with the
#     in-memory export (export.js, stage 7a) where both fit; peak memory at 2M
#  2. held-out evaluation (G1's metrics.py) of the training output, the exported PLY, the PLY
#     with SPZ's range limits and the SPZ, for G1's Brush run and run13 (lobe)
#  3. the page (laptop Chrome, a separate profile and port): the 'export-test' set on each case,
#     Stop/resume, result view, downloads = stage outputs = Node exports, the downloaded PLY in
#     the viewer's open-a-file page with temperatures, the COLMAP archive, export memory at 2M;
#     recovery: Stop/Resume, reload during the SPZ pass, a training file chosen in the page
#  4. WIN11=1: step 3 on the win11 machine (harness/win11, profile stage7-win11)
# Needs WEBSPLAT_DATA ($WEBSPLAT_DATA/g1, ref, stage5b/node-native, stage7a/tiny and the native
# sites (tiny, run13) from the stage 7a tests), the tools venv, node_modules of app/ and the viewer. Outputs in $WEBSPLAT_DATA/stage7.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ws=$(cd "$here/../.." && pwd)
gs=$(cd "$ws/.." && pwd)
if [ -z "${WEBSPLAT_DATA:-}" ] && [ -f "$ws/local.env" ]; then
  WEBSPLAT_DATA=$(sed -n 's/^WEBSPLAT_DATA=//p' "$ws/local.env")
fi
: "${WEBSPLAT_DATA:?set WEBSPLAT_DATA}"
export WEBSPLAT_DATA
PY=$gs/.venv/bin/python
D=$WEBSPLAT_DATA
O=$D/stage7
G1=$D/g1/final_laptop
DS=$D/stage5b/node-native/dataset.json
FR=$D/ref/work/frames.json
cd "$here"
for f in "$G1/export.ply" "$G1/eval" "$DS" "$FR" "$D/ref/splat_run13/splat.ply" "$D/stage7a/tiny/params.bin" \
  "$D/stage6/runs/laptop-2026-11-08T14-35-09/train/splat.ply" \
  "$D/stage6/runs/win11-stop10000-2026-11-08T15-13-55/train/splat.ply" \
  "$D/stage6/runs/win11-2026-11-08T16-34-57/train/splat.ply"; do
  [ -e "$f" ] || { echo "missing input $f" >&2; exit 1; }
done
mkdir -p "$O"
rm -rf "$O/page-laptop" "$O/page-win11" "$O/web"

echo "== Node: streaming export"
[ -f "$O/synth2m.ply" ] || node synth_ply.mjs 2000000 "$O/synth2m.ply"
node stream_export.mjs "$D/stage7a/tiny/params.bin" "$D/stage7a/tiny/dataset.json" \
  "$D/stage7a/tiny/frames.json" "$O/tiny" --title 'WebSplat tiny end_header' --compare
node --max-old-space-size=6000 stream_export.mjs "$G1/export.ply" "$DS" "$FR" "$O/g1_laptop" \
  --title 'WebSplat g1_laptop end_header' --compare
node stream_export.mjs "$D/ref/splat_run13/splat.ply" "$DS" "$FR" "$O/run13" \
  --title 'WebSplat run13 end_header'
node stream_export.mjs "$O/synth2m.ply" "$DS" "$FR" "$O/synth2m" --title 'WebSplat synth2m end_header'
# stage 6's trained splats: SPZ scale bytes that a float16 tie after Spark's exp/ln round trip
# puts a step further than the source (regression: these failed the SPZ check; win11 16-34-57:
# Gaussian 9664 ln(scale) -1.7192384 decodes to -1.6875, one float32 ulp past the f16-only bound)
for r in laptop-2026-11-08T14-35-09 win11-stop10000-2026-11-08T15-13-55 win11-2026-11-08T16-34-57; do
  node stream_export.mjs "$D/stage6/runs/$r/train/splat.ply" "$DS" "$FR" "$O/stage6-$r" --title "stage 6 $r"
done
node -e "
const r = JSON.parse(require('fs').readFileSync('$O/synth2m/result.json'));
const ok = r.maxRssMiB < 1024;
console.log((ok ? 'PASS' : 'FAIL') + ' Node streaming export at 2M: peak RSS ' + r.maxRssMiB.toFixed(0) + ' MiB (< 1024)');
process.exit(ok ? 0 : 1);"

echo "== held-out evaluation: training output, PLY, SPZ ranges, SPZ"
for c in g1_laptop run13; do
  node decode_spz.mjs "$O/$c/splat.spz" "$O/$c/spz_decoded.ply" "$O/$c/splat.ply" "$O/$c/clamped.ply"
done
$PY evaluate.py "$D/g1/dataset" "$O/eval_g1_laptop.json" --training "$G1/eval" --ply "$O/g1_laptop/splat.ply" \
  --clamped-ply "$O/g1_laptop/clamped.ply" --spz-ply "$O/g1_laptop/spz_decoded.ply" --require-export
$PY evaluate.py "$D/g1/dataset" "$O/eval_run13.json" --training "$D/ref/splat_run13/splat.pt" \
  --ply "$O/run13/splat.ply" --clamped-ply "$O/run13/clamped.ply" --spz-ply "$O/run13/spz_decoded.ply" \
  --require-training-ply 50 0.5

echo "== start view: the hottest hot view vs export_web.py's middle hot view"
$PY start_view.py "$O/g1_laptop/splat.ply" "$O/g1_laptop/native_camera.json" "$D/g1/dataset"

echo "== gsplat references for the viewer (start view; run13 and G1 at held-out view 00140)"
mkdir -p "$O/viewer-ref"
for c in tiny g1_laptop run13; do $PY viewer_reference.py "$O/$c/splat.ply" "$O/viewer-ref/$c.json"; done
for c in g1_laptop run13; do
  $PY viewer_reference.py "$O/$c/splat.ply" "$O/viewer-ref/$c-00140.json" --size 640 512 \
    --view "$D/g1/dataset" 00140.png
done

echo "== app build (viewer from source; into the test output, app/dist untouched)"
(cd "$gs/viewer" && npm run build >/dev/null)
(cd "$ws/app" && node scripts/prepare-assets.mjs >/dev/null &&
  node node_modules/vite/bin/vite.js build --configLoader native --outDir "$O/app-dist" --emptyOutDir \
    --logLevel error)
mkdir -p "$O/web/data"
cp -r "$O/app-dist/." "$O/web/"

echo "== the viewer against gsplat (as deployed: the app's copy of the built viewer)"
node view_check.mjs "$O/web/vendor/splat-viewer" "$O/view-check" --port 8862 \
  "$O/tiny/splat.ply:$O/viewer-ref/tiny.json" "$O/g1_laptop/splat.ply:$O/viewer-ref/g1_laptop.json" \
  "$O/run13/splat.ply:$O/viewer-ref/run13.json" "$O/run13/splat.ply:$O/viewer-ref/run13-00140.json" \
  "$O/g1_laptop/splat.ply:$O/viewer-ref/g1_laptop-00140.json"
ln -s "$FR" "$O/web/data/frames.json"
ln -s "$DS" "$O/web/data/dataset.json"
ln -s "$G1/export.ply" "$O/web/data/g1_laptop.ply"
ln -s "$D/ref/splat_run13/splat.ply" "$O/web/data/run13.ply"
ln -s "$O/synth2m.ply" "$O/web/data/synth2m.ply"
M=$D/ref/work/colmap/sparse/final
for f in cameras.bin images.bin points3D.bin; do ln -s "$M/$f" "$O/web/data/$f"; done
MAP='"map":{"files":{"final/cameras.bin":"cameras.bin","final/images.bin":"images.bin","final/points3D.bin":"points3D.bin"}}'
for c in g1_laptop run13 synth2m; do
  printf '{"select":{"files":{"frames.json":"frames.json"}},%s,"prepare":{"files":{"dataset.json":"dataset.json"}},"train":{"files":{"splat.ply":"%s.ply"}}}' \
    "$MAP" "$c" > "$O/web/data/$c.json"
done
ln -s "$D/stage7a/tiny/params.bin" "$O/web/data/tiny.bin"
ln -s "$D/stage7a/tiny/frames.json" "$O/web/data/tiny-frames.json"
ln -s "$D/stage7a/tiny/dataset.json" "$O/web/data/tiny-dataset.json"
printf '{"select":{"files":{"frames.json":"tiny-frames.json"}},%s,"prepare":{"files":{"dataset.json":"tiny-dataset.json"}},"train":{"files":{"params.bin":"tiny.bin"}}}' \
  "$MAP" > "$O/web/data/tiny.json"
# no trained splat: the page asks for one (recovery_check.mjs pick)
printf '{"select":{"files":{"frames.json":"tiny-frames.json"}},%s,"prepare":{"files":{"dataset.json":"tiny-dataset.json"}}}' \
  "$MAP" > "$O/web/data/pick.json"

# same bytes, or the run fails (cmp in an if: set -e does not stop at a failed `cmp && ...`)
same() {
  if cmp -s "$1" "$2"; then echo "PASS $3"; else echo "FAIL $3: $1 differs from $2"; exit 1; fi
}

echo "== the viewer's fallbacks: no float32 linear filtering (nearest, same result), no float32"
echo "   blending (half floats: notice and approximate readouts; the error is reported)"
VREF="$O/g1_laptop/splat.ply:$O/viewer-ref/g1_laptop.json $O/run13/splat.ply:$O/viewer-ref/run13-00140.json"
node view_check.mjs "$O/web/vendor/splat-viewer" "$O/view-check-nolinear" --port 8862 $VREF \
  --mask OES_texture_float_linear
node view_check.mjs "$O/web/vendor/splat-viewer" "$O/view-check-half" --port 8862 $VREF \
  --mask EXT_float_blend --degraded

echo "== migrating legacy exports (local copies only) and the viewer on a migrated PLY"
node migrate_check.mjs "$D/stage7a/full/native/site/2026-11-08/full/run13" \
  "$D/stage7a/native/site/2026-11-08/tiny/run1" "$O/migrate-test"
node view_check.mjs "$O/web/vendor/splat-viewer" "$O/view-check-migrated" --port 8862 \
  "$O/migrate-test/standalone.ply:$O/viewer-ref/run13-00140.json"

echo "== the page, laptop"
node page_check.mjs "$O/web" "$O/page-laptop" --cases tiny,g1_laptop,run13,synth2m --stop run13 \
  --no-download synth2m --max-export-mib 1024 --colmap "$M" --viewer-ref "$O/viewer-ref"
for c in tiny g1_laptop run13; do
  for f in splat.ply splat.spz meta.json; do same "$O/page-laptop/$c/$f" "$O/$c/$f" "page $c/$f = Node export"; done
done
echo "== the page, laptop: Stop/Resume, reload, a chosen training file"
node recovery_check.mjs "$O/web" "$O"

if [ "${WIN11:-0}" = 1 ]; then
  echo "== the page, win11"
  W=$ws/test/harness/win11/win11.sh
  CDP_PORT=9355 APP_PORT=8799 PROFILE=stage7-win11 "$W" start
  trap 'CDP_PORT=9355 APP_PORT=8799 PROFILE=stage7-win11 "$W" stop' EXIT
  node page_check.mjs "$O/web" "$O/page-win11" --machine win11 --profile stage7-win11 \
    --cdp http://127.0.0.1:9355 --port 8799 --cases tiny,g1_laptop,synth2m --stop g1_laptop \
    --no-download synth2m --max-export-mib 1024 --colmap "$M" --viewer-ref "$O/viewer-ref"
  for c in tiny g1_laptop; do
    for f in splat.ply splat.spz meta.json; do
      same "$O/page-win11/$c/$f" "$O/$c/$f" "win11 page $c/$f = Node export"
    done
  done
  node recovery_check.mjs "$O/web" "$O" --machine win11 --profile stage7-win11 \
    --cdp http://127.0.0.1:9355 --port 8799 --scenarios stop,reload
fi
echo "ALL PASS"
