#!/bin/sh
set -eu

usage()
{
    echo "Usage: $0 KERNEL ROOTFS CONFIG OUTPUT CAMERA_APP WEB_APP THERMAL_SOCKET TIMESYNC APP_INIT APP_SELECTION CAMERA_INI RSYNC STRACE TCPDUMP LTRACE DROPBEAR DROPBEARKEY DROPBEAR_START" >&2
    exit 2
}

[ "$#" -eq 18 ] || usage

kernel=$1
rootfs=$2
config=$3
output=$4
camera_app=$5
web_app=$6
thermal_socket=$7
timesync=$8
app_init=$9
shift 9
app_selection=$1
camera_ini=$2
rsync_bin=$3
strace_bin=$4
tcpdump_bin=$5
ltrace_bin=$6
dropbear_bin=$7
dropbearkey_bin=$8
dropbear_start=$9

# These hashes identify the reviewed platform images stored in the repository.
# Replacing either image requires a complete review and hardware test.
expected_kernel=b00e04f161cca1040768601f81c74b51a5965206cd93a8df0f6135c7f9a20046
expected_rootfs=09f386e45e1e29b29c8c46b8576e7bcb678142df590e11a49e386f6f652528a7
root_password_hash_file=${MT11_ROOT_PASSWORD_HASH_FILE:-}
root_password=${MT11_ROOT_PASSWORD-ardupilot}
web_password=${MT11_WEB_PASSWORD-ardupilot}

validate_password()
{
    variable=$1
    value=$2
    [ "${#value}" -ge 8 ] && [ "${#value}" -le 128 ] || {
        echo "$variable must contain 8-128 characters" >&2
        exit 1
    }
    if printf '%s' "$value" | LC_ALL=C grep -q '[[:cntrl:]]'; then
        echo "$variable must not contain control characters" >&2
        exit 1
    fi
}

if [ -n "$root_password_hash_file" ]; then
    [ -f "$root_password_hash_file" ] || {
        echo "MT11_ROOT_PASSWORD_HASH_FILE does not exist: $root_password_hash_file" >&2
        exit 1
    }
else
    validate_password MT11_ROOT_PASSWORD "$root_password"
fi
validate_password MT11_WEB_PASSWORD "$web_password"

script_dir=$(CDPATH='' cd "$(dirname "$0")" && pwd)
password_repacker=$script_dir/repack-root-password.sh

for command in awk grep install mktemp sed sha256sum tar unzip wc zip; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing build tool: $command" >&2
        exit 1
    }
done
for input in "$kernel" "$rootfs" "$config" "$camera_app" "$web_app" "$thermal_socket" \
             "$timesync" "$app_init" "$app_selection" "$camera_ini" \
             "$rsync_bin" "$strace_bin" "$tcpdump_bin" "$ltrace_bin" \
             "$dropbear_bin" "$dropbearkey_bin" "$dropbear_start" \
             "$password_repacker"; do
    [ -f "$input" ] || {
        echo "Missing input: $input" >&2
        exit 1
    }
done

case $(basename "$output") in
    MT11_FW_*.bin) ;;
    *)
        echo "Output must be named MT11_FW_*.bin so the camera discovers it" >&2
        exit 1
        ;;
esac

actual_kernel=$(sha256sum "$kernel" | awk '{print $1}')
if [ "$actual_kernel" != "$expected_kernel" ]; then
    echo "Refusing unreviewed kernel image: $kernel" >&2
    echo "  expected $expected_kernel" >&2
    echo "  found    $actual_kernel" >&2
    exit 1
fi
actual_rootfs=$(sha256sum "$rootfs" | awk '{print $1}')
if [ "$actual_rootfs" != "$expected_rootfs" ]; then
    echo "Refusing unreviewed rootfs image: $rootfs" >&2
    echo "  expected $expected_rootfs" >&2
    echo "  found    $actual_rootfs" >&2
    exit 1
fi

output_dir=$(dirname "$output")
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
output=$output_dir/$(basename "$output")
work=$(mktemp -d "$output_dir/mt11-package.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

outer=$work/outer
system=$work/system
overlay=$work/overlay
mkdir -p "$outer" "$system" "$overlay/bin" "$overlay/dropbear"
chmod 0755 "$overlay/bin" "$overlay/dropbear"

install -m 0644 "$config" "$outer/config.json"
install -m 0644 "$kernel" "$system/kernel"
if [ -n "$root_password_hash_file" ]; then
    "$password_repacker" "$rootfs" "$system/rootfs" \
        "$root_password_hash_file"
else
    MT11_ROOT_PASSWORD="$root_password" \
        "$password_repacker" "$rootfs" "$system/rootfs"
fi

# The updater's parser deliberately skips the first fileName and uses the
# second.  It has no NULL check, so reject any changed config shape.
filename_count=$(grep -o '"fileName"[[:space:]]*:' "$outer/config.json" | wc -l)
[ "$filename_count" -eq 2 ] || {
    echo "Expected exactly two fileName entries in config.json" >&2
    exit 1
}
second_filename=$(grep -o '"fileName"[[:space:]]*:[[:space:]]*"[^"]*"' \
    "$outer/config.json" | sed -n '2s/.*"\([^"]*\)"$/\1/p')
[ "$second_filename" = mini4k_mcu_firmware.bin ] || {
    echo "Second config fileName is not mini4k_mcu_firmware.bin" >&2
    exit 1
}

repacked_rootfs=$(sha256sum "$system/rootfs" | awk '{print $1}')
[ "$repacked_rootfs" != "$expected_rootfs" ] || {
    echo "Root password repack unexpectedly left the vendor rootfs unchanged" >&2
    exit 1
}

install -m 0755 "$camera_app" "$overlay/bin/camera-app"
install -m 0755 "$web_app" "$overlay/bin/mt11-web"
cp -R "$script_dir/../web/webroot" "$overlay/webroot"
install -m 0755 "$thermal_socket" "$overlay/bin/thermal_socket"
install -m 0755 "$timesync" "$overlay/bin/mt11-timesync.sh"
install -m 0755 "$rsync_bin" "$overlay/bin/rsync"
install -m 0755 "$strace_bin" "$overlay/bin/strace"
install -m 0755 "$tcpdump_bin" "$overlay/bin/tcpdump"
install -m 0755 "$ltrace_bin" "$overlay/bin/ltrace"
install -m 0755 "$dropbear_bin" "$overlay/bin/dropbear"
install -m 0755 "$dropbearkey_bin" "$overlay/bin/dropbearkey"
install -m 0755 "$dropbear_start" "$overlay/dropbear/start-dropbear.sh"
install -m 0755 "$app_init" "$overlay/app_init.sh"
install -m 0755 "$app_selection" "$overlay/app_selection.sh"
install -m 0644 "$camera_ini" "$overlay/camera.ini.default"
umask 077
printf '%s\n' "$web_password" >"$overlay/web.pass.default"

# This app archive is intentionally only an overlay.  In particular it does
# not contain siyi_camera_app_tmp: product_upgrade's final mv then fails
# harmlessly and leaves the already-installed vendor application intact.
tar --owner=0 --group=0 -czf "$system/app.tar.gz" -C "$overlay" \
    app_init.sh app_selection.sh bin dropbear webroot camera.ini.default web.pass.default
tar -tzf "$system/app.tar.gz" | grep -qx 'camera.ini.default' || {
    echo "Application overlay is missing camera.ini.default" >&2
    exit 1
}
if tar -tzf "$system/app.tar.gz" | grep -qx 'camera.ini'; then
    echo "Refusing package which would overwrite persistent camera.ini" >&2
    exit 1
fi
if tar -tzf "$system/app.tar.gz" | grep -Eq \
    '(^|/)(siyi_camera_app(_tmp)?|libs|cfg|boa)(/|$)'; then
    echo "Refusing package containing vendor application files" >&2
    exit 1
fi
for utility in rsync strace tcpdump ltrace dropbear dropbearkey; do
    tar -tzf "$system/app.tar.gz" | grep -qx "bin/$utility" || {
        echo "Application overlay is missing bin/$utility" >&2
        exit 1
    }
done
tar -tzf "$system/app.tar.gz" | \
    grep -qx 'dropbear/start-dropbear.sh' || {
    echo "Application overlay is missing the Dropbear launcher" >&2
    exit 1
}
tar -czf "$outer/sysimg.tar.gz" -C "$system" app.tar.gz kernel rootfs

# Do not omit this file.  The vendor updater has an uninitialized-value bug on
# fopen failure.  A successfully opened, exactly-zero-byte file takes its
# deterministic no-MCU-update branch before any UART command is transmitted.
: >"$outer/mini4k_mcu_firmware.bin"
[ ! -s "$outer/mini4k_mcu_firmware.bin" ]

rm -f "$output"
(
    cd "$outer"
    zip -q -9 "$output" sysimg.tar.gz mini4k_mcu_firmware.bin config.json
)

[ "$(unzip -p "$output" mini4k_mcu_firmware.bin | wc -c)" -eq 0 ]
[ "$(unzip -Z1 "$output" | wc -l)" -eq 3 ]
packaged_rootfs=$(unzip -p "$output" sysimg.tar.gz |
    tar -xzO rootfs | sha256sum | awk '{print $1}')
[ "$packaged_rootfs" = "$repacked_rootfs" ] || {
    echo "Final package rootfs differs from the password-repacked rootfs" >&2
    exit 1
}

echo "Built $output"
sha256sum "$output"
echo "Kernel is byte-identical to the reviewed repository input."
echo "Rootfs is derived from the reviewed repository input with the configured root password hash."
echo "MCU firmware is present and exactly zero bytes; no MCU UART update is sent."
