#!/usr/bin/env bash
# Builds the WebSplat colmap-wasm fork into wasm/ (websplat-colmap.mjs/.wasm, the licences and
# third-party notices, BUILD-INFO.txt): upstream obraia/colmap-wasm at the pinned commit +
# colmap-wasm-patches/, with the system emscripten (emcc/emcmake on PATH, tested with 3.1.69).
# Build tree under $WEBSPLAT_DATA/g2 (fetch.sh downloads the pinned COLMAP 4.2.1, Eigen, Ceres,
# glog, PoseLib and SQLite sources there).
#   WEBSPLAT_DATA=... ./build-colmap.sh
# An existing fork checkout must be exactly the pinned commit plus this patch series with a
# clean tree; anything else is refused (never overwritten).
set -euo pipefail
UPSTREAM=https://github.com/obraia/colmap-wasm.git
COMMIT=ec1f1bc2ecac44d7d110edef9eb297419be57cfb
here=$(cd "$(dirname "$0")" && pwd)
: "${WEBSPLAT_DATA:?set WEBSPLAT_DATA}"
fork=${COLMAP_FORK:-$WEBSPLAT_DATA/colmap-wasm-fork}
export COLMAP_WASM_DIR=${COLMAP_WASM_DIR:-$WEBSPLAT_DATA/g2/cw}
export TMPDIR=$WEBSPLAT_DATA/g2/tmp COLMAP_WASM_SYSTEM_TOOLCHAIN=1 BOOST_INC=${BOOST_INC:-/usr/include}
export JOBS=${JOBS:-8}
# -msimd128 (WASM_SIMD=0 for the scalar build): same outputs, 3-4% faster verification and mapping
export WASM_SIMD=${WASM_SIMD-1}
[ "$WASM_SIMD" = 0 ] && unset WASM_SIMD
build=$COLMAP_WASM_DIR/build${WASM_SIMD:+-simd}
mkdir -p "$TMPDIR"
patches=("$here"/colmap-wasm-patches/*.patch)

if [ ! -d "$fork/.git" ]; then
  git clone -q "$UPSTREAM" "$fork"
  git -C "$fork" checkout -q -b websplat "$COMMIT"
  git -C "$fork" am -q "${patches[@]}"
fi

# the checkout must be COMMIT + the patch series (compared by git patch-id), clean
n=${#patches[@]}
fail() { echo "build-colmap.sh: $fork: $*; refusing to build (nothing changed)" >&2; exit 1; }
[ -z "$(git -C "$fork" status --porcelain --untracked-files=no)" ] || fail "uncommitted changes"
[ "$(git -C "$fork" rev-parse "HEAD~$n" 2>/dev/null)" = "$COMMIT" ] ||
  fail "HEAD~$n is not the pinned upstream commit $COMMIT"
want=$(for p in "${patches[@]}"; do git -C "$fork" patch-id --stable < "$p" | cut -d' ' -f1; done)
have=$(for c in $(git -C "$fork" rev-list --reverse "$COMMIT..HEAD"); do
  git -C "$fork" show "$c" | git -C "$fork" patch-id --stable | cut -d' ' -f1; done)
[ "$want" = "$have" ] || fail "commits differ from colmap-wasm-patches/ (re-export or re-apply)"

bash "$fork/native/fetch.sh"
bash "$fork/native/build.sh"
cmake --build "$build/colmap" -j "$JOBS" --target colmap_websplat
cp "$build"/colmap/websplat-colmap.{mjs,wasm} "$here/wasm/"
cp "$fork/THIRD_PARTY_NOTICES.md" "$here/wasm/THIRD_PARTY_NOTICES.md"
cp "$fork/LICENSE" "$here/wasm/LICENSE-colmap-wasm.txt"
cp "$COLMAP_WASM_DIR/src/colmap/COPYING.txt" "$here/wasm/LICENSE-COLMAP.txt"
{
  echo "fork: $(git -C "$fork" rev-parse HEAD) = $COMMIT + ${n} patches"
  echo "patch-ids: $(echo $want)"
  echo "simd: ${WASM_SIMD:+-msimd128}"
  echo "emcc: $(emcc --version | head -1)"
  echo "cmake: $(cmake --version | head -1)"
  echo "ninja: $(ninja --version)"
  echo "boost: $(grep -m1 '#define BOOST_LIB_VERSION' "$BOOST_INC/boost/version.hpp" | cut -d'"' -f2)"
  echo "wasm sha256: $(sha256sum "$here/wasm/websplat-colmap.wasm" | cut -d' ' -f1)"
} > "$here/wasm/BUILD-INFO.txt"
cat "$here/wasm/BUILD-INFO.txt"
