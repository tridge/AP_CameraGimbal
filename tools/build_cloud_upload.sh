#!/bin/sh
# Small HTTPS-only uploader dependencies, isolated per camera toolchain.
# Usage: build_cloud_upload.sh TARGET [CROSS_PREFIX]; host uses CC/CXX or cc/c++.
set -eu
target=${1:-host}
cross=${2:-}
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
root=${APCAM_UPLOAD_DEPS_ROOT:-$repo/build/deps/cloud-upload}
prefix=$root/$target
for tool in cmake make python3 curl tar sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done
build() {
    if [ -n "${MAKEFLAGS:-}" ]; then "${MAKE:-make}" -C "$1";
    else make -C "$1" -j"${UPLOAD_BUILD_JOBS:-4}"; fi
}
mkdir -p "$root/downloads" "$prefix"
exec 9>"$prefix/.lock"
python3 -c 'import fcntl; fcntl.flock(9, fcntl.LOCK_EX)'
cc=${cross}gcc
cxx=${cross}g++
[ "$target" != host ] || { cc=${CC:-cc}; cxx=${CXX:-c++}; }
cc=$(command -v "$cc")
cxx=$(command -v "$cxx")
fetch() {
    name=$1 url=$2 hash=$3
    if [ ! -f "$root/downloads/$name" ]; then
        temporary=$(mktemp "$root/downloads/.download.XXXXXX")
        trap 'rm -f "$temporary"' EXIT HUP INT TERM
        curl --fail --location --silent --show-error --retry 3 "$url" -o "$temporary"
        printf '%s  %s\n' "$hash" "$temporary" | sha256sum -c - >/dev/null
        mv "$temporary" "$root/downloads/$name"
        trap - EXIT HUP INT TERM
    fi
    printf '%s  %s\n' "$hash" "$root/downloads/$name" | sha256sum -c - >/dev/null
}
fetch mbedtls-3.6.7.tar.bz2 https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
fetch curl-8.23.0.tar.xz https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz 8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
fingerprint=$(cat "$repo/tools/build_cloud_upload.sh" | sha256sum | cut -d' ' -f1)
configuration="$fingerprint $cc $($cc -dumpmachine) $($cc -dumpversion)"
if [ ! -f "$prefix/configuration" ] || [ "$(cat "$prefix/configuration")" != "$configuration" ]; then
    # CMake can discard all command-line options after detecting a changed
    # compiler in an existing cache (including our install prefix/TLS flags).
    # Object files must also never cross compiler/configuration boundaries.
    rm -rf -- "$prefix/build-mbedtls" "$prefix/build-curl"
    src=$prefix/source
    mkdir -p "$src"
    [ -d "$src/mbedtls-3.6.7" ] || tar -xf "$root/downloads/mbedtls-3.6.7.tar.bz2" -C "$src"
    [ -d "$src/curl-8.23.0" ] || tar -xf "$root/downloads/curl-8.23.0.tar.xz" -C "$src"
    set -- -G 'Unix Makefiles' -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_C_FLAGS='-Os -ffunction-sections -fdata-sections'
    if [ "$target" != host ] && [ -n "$cross" ]; then
        set -- "$@" -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    fi
    cmake -S "$src/mbedtls-3.6.7" -B "$prefix/build-mbedtls" "$@" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
    build "$prefix/build-mbedtls"
    cmake --install "$prefix/build-mbedtls"
    for protocol in DICT FILE FTP GOPHER IMAP MQTT POP3 RTSP SMTP TELNET TFTP; do
        set -- "$@" "-DCURL_DISABLE_$protocol=ON"
    done
    cmake -S "$src/curl-8.23.0" -B "$prefix/build-curl" "$@" \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
        -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
        -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DUSE_NGHTTP2=OFF -DCURL_USE_GSSAPI=OFF -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
        -DMBEDTLS_INCLUDE_DIR="$prefix/include" -DMBEDTLS_LIBRARY="$prefix/lib/libmbedtls.a" \
        -DMBEDX509_LIBRARY="$prefix/lib/libmbedx509.a" -DMBEDCRYPTO_LIBRARY="$prefix/lib/libmbedcrypto.a" \
        -DENABLE_THREADED_RESOLVER=OFF -DENABLE_ARES=OFF -DENABLE_UNIX_SOCKETS=OFF \
        -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF \
        -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
        -DCURL_DISABLE_ALTSVC=ON -DCURL_DISABLE_HSTS=ON
    build "$prefix/build-curl"
    cmake --install "$prefix/build-curl"
    printf '%s\n' "$configuration" > "$prefix/configuration"
fi
printf '%s\n' "Native upload HTTPS libraries ready: $prefix"
