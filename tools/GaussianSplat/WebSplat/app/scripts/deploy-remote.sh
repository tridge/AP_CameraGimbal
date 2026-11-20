#!/usr/bin/env bash
# The target side of deploy.sh --upload, run under flock(1) on BASE/.deploy.lock (held by this
# process; the kernel releases it if the process dies):
#   deploy-remote.sh BASE RELEASE KEEP
# BASE/releases/RELEASE.partial/ is a complete upload; its root-index.html (the page with <base> at
# releases/RELEASE/) is the site's index.html while it is live. Name it, switch BASE/index.html to
# that (temporary file + rename) and BASE/current (the live release, a
# symlink, + rename), then remove uploads abandoned for a day and all but the newest KEEP releases,
# never the live release or the one it replaced.
# Rollback: under the same lock, copy releases/R/root-index.html over index.html (temporary + rename) and
# point current at releases/R.
set -euo pipefail
base=$1 release=$2 keep=$3
cd "$base"
new=releases/$release.partial
[[ -d $new && -f $new/root-index.html ]] || { echo "no complete upload at $base/$new" >&2; exit 1; }
live() { if [[ -e current ]]; then basename "$(readlink -f current)"; fi; }
# the release index.html serves; differs from current only if a switch died between the renames
served() { if [[ -f index.html ]]; then sed -n 's#.*<base href="\./releases/\([^/"]*\)/".*#\1#p' index.html | head -1; fi; }
prev=$(live) prev_served=$(served)
mv -T "$new" "releases/$release"
cp "releases/$release/root-index.html" "index.html.$release"
mv -f "index.html.$release" index.html
ln -sfn "releases/$release" "current.$release"
mv -T "current.$release" current
find releases -mindepth 1 -maxdepth 1 -name '*.partial' -mmin +1440 -exec rm -rf -- {} +
now=$(live)
find releases -mindepth 1 -maxdepth 1 -type d ! -name '*.partial' -printf '%f\n' | sort | head -n "-$keep" |
  while IFS= read -r old; do
    [[ $old == "$now" || $old == "$prev" || $old == "$prev_served" ]] || rm -rf -- "releases/$old"
  done
echo "current -> releases/$now (replaced ${prev:-nothing})"
