#!/bin/sh
# Build a SIYI A8 mini SD-card update that replaces the vendor application
# partition ("customer", JFFS2) with camera-app while keeping U-Boot, the
# kernel and the vendor root filesystem. The kernel modules, ISP tuning file
# and network configuration are the checked-in, checksum-verified platform
# files. No vendor firmware image is needed during a build.
set -eu

usage()
{
    echo "Usage: $0 PLATFORM_DIR OUTPUT CAMERA_APP A8_UUID WEB_APP APP_INIT CAMERA_INI UPGRADE_SCRIPT VERSION" >&2
    exit 2
}

[ "$#" -eq 9 ] || usage

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
platform=$1
output=$2
camera_app=$3
a8_uuid=$4
web_app=$5
app_init=$6
camera_ini=$7
upgrade_script=$8
version=$9
web_password=${A8_WEB_PASSWORD-ardupilot}

customer_size=$((0x600000))
erase_size=$((0x10000))
script_size=$((0x4000))
strip=${A8_STRIP:-${CROSS_COMPILE:-arm-linux-}strip}

[ "${#web_password}" -ge 8 ] && [ "${#web_password}" -le 128 ] || {
    echo "A8_WEB_PASSWORD must contain 8-128 characters" >&2
    exit 1
}
if printf '%s' "$web_password" | LC_ALL=C grep -q '[[:cntrl:]]'; then
    echo "A8_WEB_PASSWORD must not contain control characters" >&2
    exit 1
fi

for command in mkfs.jffs2 sha256sum; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Missing build tool: $command" >&2
        exit 1
    }
done
for input in "$platform/SHA256SUMS" "$camera_app" "$a8_uuid" "$web_app" \
             "$app_init" "$camera_ini" "$upgrade_script"; do
    [ -f "$input" ] || {
        echo "Missing input: $input" >&2
        exit 1
    }
done
(cd "$platform" && sha256sum -c SHA256SUMS)

output_dir=$(dirname "$output")
mkdir -p "$output_dir"
work=$(mktemp -d -p "$output_dir" a8-package-XXXXXX)
trap 'rm -rf "$work"' EXIT

tree=$work/customer
mkdir -p "$tree/bin" "$tree/camera-app" "$tree/config"
cp -R "$root/web/webroot" "$tree/camera-app/webroot"
# The retained boot script loads these modules from the customer partition.
cp -a "$platform/modules" "$tree/modules"
install -m 0644 "$platform/8836_imx678_v6.bin" "$tree/8836_imx678_v6.bin"
install -m 0644 "$platform/network_config.ini" "$tree/network_config.ini"
: >"$tree/.sys_upgrade_finish_flag"
# ours
install -m 0755 "$app_init" "$tree/bin/cardv"
install -m 0644 "$camera_ini" "$tree/camera-app/camera.ini.default"
printf '%s\n' "$version" >"$tree/camera-app/VERSION"
# seeded to /config/camera-app/web.pass on first boot
(umask 077 && printf '%s\n' "$web_password" >"$tree/camera-app/web.pass.default")
if command -v "$strip" >/dev/null 2>&1; then
    "$strip" -o "$tree/camera-app/camera-app" "$camera_app"
    "$strip" -o "$tree/camera-app/a8-uuid" "$a8_uuid"
    "$strip" -o "$tree/camera-app/a8-web" "$web_app"
else
    echo "warning: $strip not found; packaging unstripped binaries" >&2
    install "$camera_app" "$tree/camera-app/camera-app"
    install "$a8_uuid" "$tree/camera-app/a8-uuid"
    install "$web_app" "$tree/camera-app/a8-web"
fi
chmod 0755 "$tree/camera-app/camera-app" "$tree/camera-app/a8-uuid" \
    "$tree/camera-app/a8-web"

# an unpadded image gives the real occupancy; the flashed one is padded
mkfs.jffs2 -U -r "$tree" -o "$work/unpadded.jffs2" -e "$erase_size" -l -q
used=$(wc -c <"$work/unpadded.jffs2")
[ "$used" -le "$customer_size" ] || {
    echo "customer tree needs $used bytes, more than the $customer_size partition" >&2
    exit 1
}
mkfs.jffs2 -U -r "$tree" -o "$work/customer.jffs2" -e "$erase_size" -l \
    --pad="$customer_size" -q
[ "$(wc -c <"$work/customer.jffs2")" -eq "$customer_size" ] || {
    echo "customer image is not $customer_size bytes" >&2
    exit 1
}

grep -q '^% <- this is end of script symbol' "$upgrade_script" || {
    echo "upgrade script lacks the end-of-script marker" >&2
    exit 1
}
[ "$(wc -c <"$upgrade_script")" -lt "$script_size" ] || {
    echo "upgrade script is too long" >&2
    exit 1
}

# Generate only the required padding. An unbounded producer feeding head
# caused harmless but misleading "Broken pipe" errors during valid builds.
script_bytes=$(wc -c <"$upgrade_script")
{
    cat "$upgrade_script"
    head -c "$((script_size - script_bytes))" /dev/zero | tr '\0' '\377'
} >"$work/script.bin"
[ "$(wc -c <"$work/script.bin")" -eq "$script_size" ] || {
    echo "failed to pad the upgrade script" >&2
    exit 1
}

cat "$work/script.bin" "$work/customer.jffs2" >"$work/package.bin"
mv "$work/package.bin" "$output"
sha256sum "$output" | cut -d' ' -f1 >"$output.sha256"
echo "Built $output ($(wc -c <"$output") bytes; customer partition uses $((used / 1024)) of $((customer_size / 1024)) KiB)"
