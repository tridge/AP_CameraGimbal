#!/usr/bin/env bash
# Stage 7a acceptance: export/viewer round trip with the tiny known model.
#  1. tiny model -> native exports (export_web.py --ply and site, train.py save_ply)
#  2. JS export (Node): byte comparisons with native, start camera, SPZ decode error; the SPZ
#     metadata in the gzip header (spz_meta.mjs)
#  3. native checks: plyfile / render_ply loaders on the JS PLYs, SH convention vs gsplat,
#     gsplat renders at the viewer cameras with negative controls
#  4. Chrome (laptop, WebGL viewer): the JS outputs in the built viewer by every route (an SPZ
#     alone with its embedded metadata too), and the export in a worker with the result in the
#     embedded viewer; screenshots and temperature readouts compared with the gsplat renders
#  5. optional full-size: run13's native export re-exported by JS (FULL=0 skips)
# Needs WEBSPLAT_DATA, the tools venv, the viewer's node_modules; outputs in $WEBSPLAT_DATA/stage7a.
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
O=$WEBSPLAT_DATA/stage7a
cd "$here"
rm -rf "$O/tiny" "$O/native" "$O/js" "$O/web" "$O/views" "$O/viewer-dist" "$O/spz-meta"
mkdir -p "$O"

# --configLoader native: no bundled-config temp files in the source tree (read-only checkouts)
echo "== viewer build from the current source (into the test output)"
(cd "$gs/viewer" && node node_modules/vite/bin/vite.js build --configLoader native --outDir "$O/viewer-dist" --emptyOutDir \
  --logLevel warn)
[ -f "$gs/viewer/dist/viewer.js" ] || (cd "$gs/viewer" && npm run build >/dev/null) # export_web.py site mode

echo "== tiny model and native exports"
node tiny_model.mjs "$O/tiny"
$PY native_export.py "$O/tiny" "$O/native" >/dev/null

echo "== JS export (Node) vs native"
node run_node.mjs "$O/tiny" "$O/native" "$O/js"

echo "== SPZ metadata (gzip FEXTRA): reader, exports, Spark (Node), gzip tools, embed-meta.mjs"
node spz_meta.mjs "$O/native" "$O/js" "$O/spz-meta"

echo "== native loaders, SH convention, gsplat renders"
$PY check_native.py "$O/tiny" "$O/native" "$O/js" "$O/gsplat.npz"

echo "== Chrome: the viewer"
node ../../app/node_modules/vite/bin/vite.js build --configLoader native --config vite.config.mjs --outDir "$O/web/embed" \
  --logLevel warn
cp -r "$O/viewer-dist" "$O/web/viewer"
mkdir -p "$O/web/files" "$O/web/site/2026-11-08/tiny/run1"
cp "$O/tiny/params.bin" "$O/tiny/dataset.json" "$O/tiny/frames.json" "$O/web/files/"
cp "$O/js/splat.ply" "$O/js/splat.spz" "$O/js/meta.json" "$O/web/files/"
cp "$O/js/splat.ply" "$O/js/splat.spz" "$O/js/meta.json" "$O/web/site/2026-11-08/tiny/run1/"
sed 's|@ROOT@|../../../|g' "$gs/viewer/splat.html" > "$O/web/site/2026-11-08/tiny/run1/index.html"
cp -r "$O/viewer-dist" "$O/web/site/viewer"
node viewer_check.mjs "$O/web" "$O/js" "$O/views" --port "${PORT:-8861}" # 8797 is often taken by other harness runs
$PY compare_views.py "$O/js" "$O/gsplat.npz" "$O/views"

if [ "${FULL:-1}" != 0 ] && [ -f "$WEBSPLAT_DATA/ref/splat_run13/splat.pt" ]; then
  echo "== full size: run13"
  node run_full.mjs "$WEBSPLAT_DATA/ref/splat_run13" "$O/full"
fi
echo "ALL PASS"
