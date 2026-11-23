#!/bin/sh
# Reuse the pinned ARMv7 SSH build for the stable SD bootstrap.
set -eu
output=$1
cross=$2
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$output"
output=$(CDPATH= cd -- "$output" && pwd)
fingerprint=$(cat "$root/tools/build_zt30_dev_tools.sh" "$0" | sha256sum | cut -d' ' -f1)
configuration="$fingerprint $cross $(${cross}gcc -dumpversion)"
if [ -x "$output/bin/dropbear" ] && [ -x "$output/bin/dropbearkey" ] &&
    [ -f "$output/configuration" ] && [ "$(cat "$output/configuration")" = "$configuration" ]; then
    exit 0
fi
archive=$output/dropbear-2026.94.tar.bz2
hash=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
if [ ! -f "$archive" ]; then
    temporary=$(mktemp "$output/.download.XXXXXX")
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    curl --fail --location --retry 3 https://matt.ucc.asn.au/dropbear/releases/dropbear-2026.94.tar.bz2 -o "$temporary"
    printf '%s  %s\n' "$hash" "$temporary" | sha256sum -c -
    mv "$temporary" "$archive"
    trap - EXIT HUP INT TERM
fi
printf '%s  %s\n' "$hash" "$archive" | sha256sum -c -
# Do not reuse objects/configuration from another compiler or option set.
rm -rf -- "$output/dropbear-2026.94"
sh "$root/tools/build_zt30_dev_tools.sh" "$output" "$cross" "$archive"
printf '%s\n' "$configuration" > "$output/configuration"
