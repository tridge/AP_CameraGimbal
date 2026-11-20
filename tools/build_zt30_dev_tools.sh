#!/bin/sh
# Build only the modern SSH tools needed for incremental ZT30 RAM tests.
set -eu
[ "$#" = 3 ] || { echo "Usage: $0 OUTPUT_ROOT CROSS_COMPILE DROPBEAR_ARCHIVE" >&2; exit 2; }
output=$1
cross=$2
archive=$3
printf '%s  %s\n' e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81 "$archive" | sha256sum -c -
mkdir -p "$output"
output=$(cd "$output" && pwd)
tar -xjf "$archive" -C "$output"
cd "$output/dropbear-2026.94"
# Authentication uses the user's public keys in the RAM directory only.
printf '%s\n' '#define DROPBEAR_SVR_PASSWORD_AUTH 0' '#define DROPBEAR_SVR_PAM_AUTH 0' > localoptions.h
CC=${cross}gcc AR=${cross}ar RANLIB=${cross}ranlib CFLAGS='-O2 -march=armv7-a -mfloat-abi=hard' LDFLAGS='-Wl,--as-needed' \
    ./configure --host=arm-linux-gnueabihf --disable-zlib --disable-lastlog \
    --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --enable-bundled-libtom
# Bound foreground server logs on this temporary development filesystem.
python3 - <<'CAP_LOG'
from pathlib import Path
p = Path('src/svr-session.c')
s = p.read_text()
needle = '\t\tfprintf(stderr, "[%d] %s %s\\n", getpid(), datestr, printbuf);'
assert s.count(needle) == 1
prefix = """        struct stat log_stat;
        if (fstat(STDERR_FILENO, &log_stat) == 0 && S_ISREG(log_stat.st_mode) &&
            log_stat.st_size >= 1024 * 1024 && ftruncate(STDERR_FILENO, 0) == 0) {
            (void)lseek(STDERR_FILENO, 0, SEEK_SET);
        }
"""
p.write_text(s.replace(needle, prefix + needle))
CAP_LOG
make -j8 PROGRAMS='dropbear dropbearkey'
mkdir -p "$output/bin"
install -m 0755 dropbear dropbearkey "$output/bin/"
