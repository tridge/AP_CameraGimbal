#!/bin/sh
set -eu

destination=${1:-build/deps}
mpp_root=$destination/ss928-mpp
minimp4_root=$destination/minimp4
mpp_url=https://gitee.com/hieulerpi/SS928V100_SDK_V2.0.2.2_MPP_Sample.git
mpp_commit=eef3b001cbf3d55f44392878bb7f4817cdb45e6c
minimp4_url=https://raw.githubusercontent.com/lieff/minimp4/5a212a18dba7dca09543bbc7d65619274fd2931a/minimp4.h
minimp4_sha256=1fc8d29b8c29dbeead9710bd9e95f6aab4d2fec48a55c2cc54a59f0338190bf2

for command in curl git mkdir mktemp mv rm sha256sum sleep; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing dependency tool: $command" >&2
        exit 1
    }
done

mkdir -p "$destination"
if [ -d "$mpp_root/.git" ]; then
    actual=$(git -C "$mpp_root" rev-parse HEAD)
    [ "$actual" = "$mpp_commit" ] || {
        echo "Unexpected SS928 MPP revision in $mpp_root" >&2
        echo "  expected $mpp_commit" >&2
        echo "  found    $actual" >&2
        exit 1
    }
elif [ -e "$mpp_root" ]; then
    echo "Refusing non-Git SS928 MPP path: $mpp_root" >&2
    exit 1
else
    # Gitee occasionally resets or stalls connections. A partial clone may
    # also fetch blobs during checkout, so retry both operations together.
    # Publish only a complete pinned checkout; failed attempts must not leave
    # a .git directory which a later invocation mistakes for a usable cache.
    temporary=$(mktemp -d "$destination/.ss928-mpp.XXXXXX")
    trap 'rm -rf "$temporary"' EXIT
    trap 'exit 1' HUP INT TERM
    attempt=1
    while :; do
        if git -c http.lowSpeedLimit=1 -c http.lowSpeedTime=60 \
            clone --filter=blob:none --no-checkout "$mpp_url" "$temporary/checkout" &&
            git -c http.lowSpeedLimit=1 -c http.lowSpeedTime=60 \
                -C "$temporary/checkout" checkout --detach "$mpp_commit"; then
            break
        fi
        rm -rf "$temporary/checkout"
        if [ "$attempt" -ge 3 ]; then
            echo "Failed to fetch pinned SS928 MPP sources after $attempt attempts" >&2
            exit 1
        fi
        echo "SS928 MPP fetch failed (attempt $attempt/3); retrying" >&2
        sleep 5
        attempt=$((attempt + 1))
    done
    mv "$temporary/checkout" "$mpp_root"
    rm -rf "$temporary"
    trap - EXIT HUP INT TERM
fi

# A cached checkout can have the right HEAD but missing working-tree files
# (for example after an incomplete copy). SITL builds this library from source;
# checking only the revision otherwise lets compilation fail at EventLoop.h.
# Restore only absent files so local edits to existing SDK sources survive.
git -C "$mpp_root" ls-tree -r --name-only "$mpp_commit" -- src/rtspserver/src |
while IFS= read -r source; do
    if [ ! -e "$mpp_root/$source" ]; then
        echo "Restoring missing RTSP source: $source"
        git -C "$mpp_root" restore --source="$mpp_commit" --worktree \
            --ignore-skip-worktree-bits -- "$source"
    fi
done

mkdir -p "$minimp4_root"
if [ -f "$minimp4_root/minimp4.h" ]; then
    printf '%s  %s\n' "$minimp4_sha256" "$minimp4_root/minimp4.h" |
        sha256sum -c - >/dev/null
else
    temporary=$minimp4_root/minimp4.h.tmp.$$
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    curl -L --fail --silent --show-error "$minimp4_url" -o "$temporary"
    printf '%s  %s\n' "$minimp4_sha256" "$temporary" |
        sha256sum -c - >/dev/null
    mv "$temporary" "$minimp4_root/minimp4.h"
    trap - EXIT HUP INT TERM
fi

sh "$(dirname "$0")/bootstrap_dlib.sh" "$destination/dlib"
echo "Dependencies ready in $destination"
