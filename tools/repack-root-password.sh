#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage: repack-root-password.sh SOURCE_ROOTFS OUTPUT_ROOTFS [HASH_FILE]

Rebuild an MT11 UBI root filesystem with a replacement root password hash.

SOURCE_ROOTFS must be the reviewed MT11 base image. OUTPUT_ROOTFS must not
already exist. If HASH_FILE is given, its only line must be a crypt(3)
SHA-512 hash beginning with $6$. Without HASH_FILE, the script uses
MT11_ROOT_PASSWORD when set, or securely prompts twice otherwise.
EOF
}

die() {
    echo "error: $*" >&2
    exit 1
}

[[ $# -ge 2 && $# -le 3 ]] || {
    usage
    exit 2
}

source_rootfs=$(realpath "$1")
output_parent=$(realpath "$(dirname "$2")")
output_rootfs="$output_parent/$(basename "$2")"
hash_file=${3:-}
script_dir=$(realpath "$(dirname "${BASH_SOURCE[0]}")")
ubifs_root_mode_reader="$script_dir/ubifs-root-mode.py"

[[ -f "$source_rootfs" ]] || die "source rootfs does not exist: $source_rootfs"
[[ ! -e "$output_rootfs" ]] || die "refusing to overwrite: $output_rootfs"
[[ -f "$ubifs_root_mode_reader" ]] || die "root mode reader not found: $ubifs_root_mode_reader"

for tool in awk fakeroot find getent mkfs.ubifs od openssl python3 realpath sha256sum stat tr ubinize; do
    command -v "$tool" >/dev/null || die "required tool not found: $tool"
done

ubireader_extract=$(command -v ubireader_extract_files || true)
[[ -n "$ubireader_extract" ]] || die "ubireader_extract_files is required (package: ubi-reader)"

# Some development hosts have ubi_reader installed in a user site directory
# belonging to a different minor Python version. Locate it explicitly if the
# current Python does not see it by default.
ubireader_pythonpath=""
if ! python3 -c 'import ubireader' >/dev/null 2>&1; then
    mt11_user_home=$(getent passwd "$(id -u)" | cut -d: -f6)
    for candidate in "$mt11_user_home"/.local/lib/python*/site-packages/ubireader/__init__.py; do
        [[ -f "$candidate" ]] || continue
        ubireader_pythonpath=$(dirname "$(dirname "$candidate")")
        break
    done
    [[ -n "$ubireader_pythonpath" ]] || \
        die "Python cannot import ubireader; install ubi-reader for $(python3 --version 2>&1)"
fi

if [[ -n "$hash_file" ]]; then
    [[ -f "$hash_file" ]] || die "hash file does not exist: $hash_file"
    mapfile -t hash_lines < "$hash_file"
    [[ ${#hash_lines[@]} -eq 1 ]] || die "hash file must contain exactly one line"
    new_hash=${hash_lines[0]}
elif [[ -n ${MT11_ROOT_PASSWORD:-} ]]; then
    new_hash=$(printf '%s' "$MT11_ROOT_PASSWORD" | openssl passwd -6 -stdin)
else
    read -r -s -p 'New MT11 root password: ' password_one
    echo >&2
    read -r -s -p 'Confirm MT11 root password: ' password_two
    echo >&2
    [[ "$password_one" == "$password_two" ]] || die "passwords do not match"
    [[ -n "$password_one" ]] || die "password must not be empty"
    new_hash=$(printf '%s' "$password_one" | openssl passwd -6 -stdin)
    unset password_one password_two
fi

[[ ${new_hash:0:3} == "\$6\$" ]] || die "expected a SHA-512 crypt hash beginning with \$6\$"
[[ "$new_hash" != *:* ]] || die "password hash must not contain ':'"
[[ "$new_hash" != *$'\n'* ]] || die "password hash must be a single line"

mt11_work_dir=$(mktemp -d "$output_parent/.mt11-rootfs-password.XXXXXX")
readonly mt11_work_dir
cleanup() {
    if [[ -d "$mt11_work_dir" && $(basename "$mt11_work_dir") == .mt11-rootfs-password.* ]]; then
        rm -rf -- "$mt11_work_dir"
    fi
}
trap cleanup EXIT

root_extract_dir="$mt11_work_dir/root-extract"
verify_extract_dir="$mt11_work_dir/verify-extract"
fakeroot_state="$mt11_work_dir/fakeroot.state"

# Abort on an unexpected image rather than producing a plausible-looking but
# unreviewed filesystem.
expected_rootfs_sha=09f386e45e1e29b29c8c46b8576e7bcb678142df590e11a49e386f6f652528a7
expected_rootfs_size=17039360
[[ $(sha256sum "$source_rootfs" | awk '{print $1}') == "$expected_rootfs_sha" ]] || \
    die "source rootfs does not match the reviewed repository image"
[[ $(stat -c %s "$source_rootfs") -eq $expected_rootfs_size ]] || \
    die "unrecognised rootfs geometry (expected $expected_rootfs_size bytes)"

ubireader_env=(env)
if [[ -n "$ubireader_pythonpath" ]]; then
    ubireader_env+=("PYTHONPATH=$ubireader_pythonpath")
fi

fakeroot -s "$fakeroot_state" -- "${ubireader_env[@]}" python3 "$ubireader_extract" \
    -k -o "$root_extract_dir" "$source_rootfs"

root_tree=$(find "$root_extract_dir" -mindepth 2 -maxdepth 2 -type d -name ubifs -print -quit)
[[ -n "$root_tree" && -f "$root_tree/etc/shadow" ]] || \
    die "could not locate etc/shadow in the rootfs"

shadow_tmp="$root_tree/etc/.shadow.mt11-new"
awk -F: -v OFS=: -v replacement="$new_hash" '
    $1 == "root" { $2 = replacement; found = 1 }
    { print }
    END { if (!found) exit 42 }
' "$root_tree/etc/shadow" > "$shadow_tmp" || die "root entry not found in etc/shadow"
mv "$shadow_tmp" "$root_tree/etc/shadow"
chmod 0644 "$root_tree/etc/shadow"
fakeroot -i "$fakeroot_state" -s "$fakeroot_state" -- \
    chown 1000:1000 "$root_tree/etc/shadow"

# The vendor image leaves / group-writable. Current Dropbear rejects every
# authorized_keys path in that case because all absolute paths pass through /.
# Preserve the vendor ownership while removing only the unsafe group-write bit.
fakeroot -i "$fakeroot_state" -s "$fakeroot_state" -- \
    chmod 0755 "$root_tree"

# The vendor image fills its 128 LEBs to within a few KiB, and how tightly
# mkfs.ubifs packs depends on the host's directory order, so a rebuild can
# need a 129th. The vendor tree holds several byte-identical copies of its
# multi-call binaries: store each once, as hard links, for room to spare.
fakeroot -i "$fakeroot_state" -s "$fakeroot_state" -- python3 - "$root_tree" <<'PY'
import hashlib, os, stat, sys
first = {}
for folder, _, names in os.walk(sys.argv[1]):
    times = os.lstat(folder)
    for name in sorted(names):
        path = os.path.join(folder, name)
        info = os.lstat(path)
        if not stat.S_ISREG(info.st_mode) or info.st_size == 0 or info.st_nlink != 1:
            continue
        with open(path, 'rb') as stream:
            digest = hashlib.sha256(stream.read()).digest()
        key = (digest, info.st_mode, info.st_uid, info.st_gid, info.st_mtime_ns)
        if key in first:
            os.unlink(path)
            os.link(first[key], path)
        else:
            first[key] = path
    os.utime(folder, ns=(times.st_atime_ns, times.st_mtime_ns))
PY

rebuilt_ubifs="$mt11_work_dir/rootfs.ubifs"
rebuilt_rootfs="$mt11_work_dir/rootfs"
fakeroot -i "$fakeroot_state" -s "$fakeroot_state" -- \
    mkfs.ubifs -m 2048 -e 126976 -c 256 -x zlib -F \
    -r "$root_tree" -o "$rebuilt_ubifs"

# Keep the vendor image size: empty LEBs up to the volume's 128.
ubifs_bytes=$(stat -c %s "$rebuilt_ubifs")
[[ $ubifs_bytes -le 16252928 ]] || die "rebuilt UBIFS is $ubifs_bytes bytes, over the 16252928 byte volume"
head -c $((16252928 - ubifs_bytes)) /dev/zero | tr '\0' '\377' >> "$rebuilt_ubifs"

image_seq_hex=$(od -An -tx1 -j24 -N4 "$source_rootfs" | tr -d ' \n')
[[ "$image_seq_hex" =~ ^[0-9a-fA-F]{8}$ ]] || die "could not read UBI image sequence"
image_seq=$((16#$image_seq_hex))

printf '[ubifs]\nmode=ubi\nimage=%s\nvol_id=0\nvol_type=dynamic\nvol_name=ubifs\nvol_flags=autoresize\nvol_size=16252928\n' \
    "$rebuilt_ubifs" |
    ubinize -o "$rebuilt_rootfs" -m 2048 -p 131072 -s 2048 -O 2048 \
        -Q "$image_seq" /dev/stdin

[[ $(stat -c %s "$rebuilt_rootfs") -eq $expected_rootfs_size ]] || \
    die "rebuilt rootfs has the wrong size"

"${ubireader_env[@]}" python3 "$ubireader_extract" \
    -o "$verify_extract_dir" "$rebuilt_rootfs"
verified_tree=$(find "$verify_extract_dir" -mindepth 2 -maxdepth 2 -type d -name ubifs -print -quit)
verified_hash=$(awk -F: '$1 == "root" { print $2 }' "$verified_tree/etc/shadow")
[[ "$verified_hash" == "$new_hash" ]] || die "rebuilt rootfs password verification failed"
verified_root_mode=$("${ubireader_env[@]}" python3 "$ubifs_root_mode_reader" "$rebuilt_rootfs")
[[ "$verified_root_mode" == 755 ]] || \
    die "rebuilt rootfs root directory permissions are $verified_root_mode, not 0755"

mv "$rebuilt_rootfs" "$output_rootfs"
echo "Created $output_rootfs"
sha256sum "$output_rootfs"
