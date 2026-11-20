#!/bin/sh
# CPU correlation tracker only: no DNN weights, BLAS, image codecs or GUI.
set -eu
root=${1:-build/deps/dlib}
version=20.0.1
sha=dab5b4ec4b68bd7dc128a1fb7900723f89d2da107e44cd5def7d38fc57252a9d
mkdir -p "$(dirname "$root")"
# Separate recursive makes can reach this rule together. Keep the lock outside
# the published tree and recheck the cache after acquiring it.
exec 9>"$root.lock"
python3 -c 'import fcntl; fcntl.flock(9, fcntl.LOCK_EX)'
if [ -f "$root/.apcam-version" ] && [ "$(cat "$root/.apcam-version")" = "$version" ] &&
   [ -f "$root/dlib/image_processing/correlation_tracker.h" ]; then exit 0; fi
if [ -e "$root" ]; then
    echo "Unverified dlib directory: $root (move it aside before bootstrapping)" >&2
    exit 1
fi
tmp=$(mktemp -d "$(dirname "$root")/.dlib.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
trap 'exit 1' HUP INT TERM
curl -fLsS --retry 3 "https://codeload.github.com/davisking/dlib/tar.gz/refs/tags/v$version" -o "$tmp/source.tar.gz"
printf '%s  %s\n' "$sha" "$tmp/source.tar.gz" | sha256sum -c -
mkdir "$tmp/source"
tar -xzf "$tmp/source.tar.gz" -C "$tmp/source" --strip-components=1
printf '%s\n' "$version" > "$tmp/source/.apcam-version"
mv "$tmp/source" "$root"
