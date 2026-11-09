#!/usr/bin/env bash
# Build the WebSplat app as a static site and (only with --upload) publish it to the test server.
#   deploy.sh            build and check a release under $WEBSPLAT_DATA/deploy/<release>/ (a site
#                        root: index.html and releases/<release>/; serve it to test), print the upload
#   deploy.sh --upload   also publish it to $WEBSPLAT_DEPLOY_TARGET ([user@host:]/path, the
#                        directory served as the app's URL; a plain static directory, no special
#                        headers): the release uploads to /path/releases/<release>.partial/, then,
#                        under flock on /path/.deploy.lock, deploy-remote.sh switches /path/index.html
#                        to it and prunes old releases (keeping the newest KEEP and the one replaced).
# The site's index.html is the release's with <base href="./releases/<release>/">: everything a page
# loads, workers, wasm and vendor files included, resolves inside that release's directory, so a page
# opened on an earlier release keeps working while that release is kept.
# WEBSPLAT_DATA, WEBSPLAT_BRUSH_PKG (the Brush fork's wasm-bindgen output, built with path remapping:
# test/g1/README) and WEBSPLAT_DEPLOY_TARGET come from the environment or WebSplat/local.env
# (untracked). Needs node >= 24, rsync, flock on the target, and the app's, viewer's and splat
# viewer's node_modules.
set -euo pipefail
app=$(cd "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ws=$(cd "$app/.." && pwd)
KEEP=3
upload=0
case "${1:-}" in
  --upload) upload=1 ;;
  '') ;;
  *) echo "usage: $0 [--upload]" >&2; exit 2 ;;
esac
from_env() { # NAME: the environment's value, else local.env's
  local v=${!1:-}
  if [[ -z $v && -f $ws/local.env ]]; then v=$(sed -n "s/^$1=//p" "$ws/local.env" | tail -1); fi
  printf '%s' "$v"
}
data=$(from_env WEBSPLAT_DATA)
[[ -n $data ]] || { echo "WEBSPLAT_DATA is not set (environment or local.env)" >&2; exit 2; }
export WEBSPLAT_BRUSH_PKG=$(from_env WEBSPLAT_BRUSH_PKG)
node -e 'if (+process.versions.node.split(".")[0] < 24) { console.error("node 24 or newer is required"); process.exit(1) }'

commit=$(git -C "$ws" rev-parse --short=12 HEAD)
dirty=$(git -C "$ws" status --porcelain -- . | grep -c . || true)
suffix=''
if [[ $dirty != 0 ]]; then suffix=-dirty; fi
release="$(date -u +%Y%m%dT%H%M%SZ)-$commit$suffix"
out="$data/deploy/$release"
rel="$out/releases/$release"
mkdir -p "$out"
echo "building $release (commit $commit, $dirty changed files in the working tree) into $out"
(cd "$app" && node scripts/prepare-assets.mjs &&
  node node_modules/vite/bin/vite.js build --outDir "$rel" --emptyOutDir --logLevel warn)
# the site's index.html: the release's, with <base> at releases/<release>/ (the switch installs it)
sed "0,/<head>/s||<head>\n    <base href=\"./releases/$release/\" />|" "$rel/index.html" > "$rel/root-index.html"
cp "$rel/root-index.html" "$out/index.html"

# what the page needs at run time
fail=0
for f in index.html vendor/brush/brush_js.js vendor/brush/brush_js_bg.wasm vendor/splat-viewer/viewer.js; do
  [[ -s $rel/$f ]] || { echo "missing $f" >&2; fail=1; }
done
ls "$rel"/vendor/ffmpeg/*/ffmpeg-core.wasm >/dev/null 2>&1 || { echo "missing the FFmpeg core" >&2; fail=1; }
ls "$rel"/assets/train.stage.worker-*.js "$rel"/assets/brush-worker-*.js >/dev/null 2>&1 || { echo "missing the training workers" >&2; fail=1; }
grep -q "<base href=\"./releases/$release/\"" "$out/index.html" || { echo "index.html does not load from releases/$release/" >&2; fail=1; }
# every wasm module valid; no local directory named anywhere (the wasm builds remap their source
# paths: -ffile-prefix-map for COLMAP, --remap-path-prefix for Brush; a remaining one fails here)
node -e '
const fs = require("fs"), path = require("path");
const [dir, ...prefixes] = process.argv.slice(1);
const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).flatMap((e) =>
  e.isDirectory() ? walk(path.join(d, e.name)) : [path.join(d, e.name)]);
let bad = 0;
for (const f of walk(dir)) {
  const b = fs.readFileSync(f);
  if (f.endsWith(".wasm") && !WebAssembly.validate(b)) { console.error(`invalid wasm: ${path.relative(dir, f)}`); bad++; }
  for (const p of prefixes) {
    const n = b.indexOf(Buffer.from(p));
    if (n >= 0) { console.error(`${path.relative(dir, f)} names a local path (${p}) at byte ${n}`); bad++; }
  }
}
process.exit(bad ? 1 : 0);' "$out" "$data" "$HOME" || fail=1
[[ $fail == 0 ]] || { echo "release incomplete or unfit: not uploading" >&2; exit 1; }
wasm=$(sha256sum "$rel/vendor/brush/brush_js_bg.wasm" | cut -c1-16)
cat > "$rel/BUILD.json" <<EOF
{"release":"$release","commit":"$commit","dirty_files":$dirty,"brush_wasm_sha256":"$wasm","built_utc":"$(date -u +%FT%TZ)"}
EOF
echo "release ready: $out ($(du -sh "$out" | cut -f1), Brush wasm $wasm)"

target=$(from_env WEBSPLAT_DEPLOY_TARGET)
if [[ $upload == 0 ]]; then
  echo "not uploaded. To upload: $0 --upload   (target: ${target:-WEBSPLAT_DEPLOY_TARGET not set})"
  exit 0
fi
[[ -n $target ]] || { echo "WEBSPLAT_DEPLOY_TARGET is not set ([user@host:]/path)" >&2; exit 2; }
[[ $dirty == 0 ]] || { echo "refusing to upload a build from a working tree with uncommitted changes" >&2; exit 1; }
if [[ $target == *:* ]]; then host=${target%%:*}; base=${target#*:}; else host=''; base=$target; fi
base=${base%/}
run() { # COMMAND...: on the target (ssh) or here
  if [[ -n $host ]]; then ssh -o BatchMode=yes "$host" "$(printf '%q ' "$@")"; else "$@"; fi
}
run mkdir -p "$base/releases"
# unchanged files hard-linked from the live release (by content: --checksum, and times are not
# preserved since link-dest needs every preserved attribute to match; relative to the destination)
live=$(run bash -c 'test -e "$1/current" && basename "$(readlink -f "$1/current")"' _ "$base" || true)
link=()
if [[ -n $live ]]; then link=(--link-dest="../$live/"); fi
echo "uploading $release to ${host:+$host:}$base/releases/$release.partial/"
rsync -rl --checksum --chmod=D755,F644 "${link[@]}" "$rel/" "${host:+$host:}$base/releases/$release.partial/"
# switch and prune, serialized by flock held by the target-side process
if [[ -n $host ]]; then
  ssh -o BatchMode=yes "$host" "$(printf '%q ' flock "$base/.deploy.lock" bash -s -- "$base" "$release" "$KEEP")" \
    < "$app/scripts/deploy-remote.sh"
else
  flock "$base/.deploy.lock" bash -s -- "$base" "$release" "$KEEP" < "$app/scripts/deploy-remote.sh"
fi
echo "published: ${host:+$host:}$base/index.html -> releases/$release"
