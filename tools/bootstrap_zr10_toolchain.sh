#!/bin/sh
# Install local build inputs; no system installation and no camera access.
set -eu
cd "$(dirname "$0")/.."
deps="$PWD/build/zr10-deps"
name=armv7-eabihf--uclibc--stable-2018.11-1
url="https://toolchains.bootlin.com/downloads/releases/toolchains/armv7-eabihf/tarballs/$name.tar.bz2"
mkdir -p "$deps"
if [ ! -f "$deps/toolchain.tar.bz2" ]; then
    curl -fL --retry 2 "$url" -o "$deps/toolchain.tar.bz2.part"
    mv "$deps/toolchain.tar.bz2.part" "$deps/toolchain.tar.bz2"
fi
python3 - "$deps" "$PWD" <<'PY'
import hashlib
import os
from pathlib import Path
import shutil
import sys
import tempfile
root = Path(sys.argv[1])
repo = Path(sys.argv[2])
archive = root / "toolchain.tar.bz2"
expected = "a0300cf5765436607e50d010abbe88a71b2447c40cd9ccd1a733a6e43608f081"
if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
    raise SystemExit("toolchain SHA-256 mismatch")
target = root / "armv7-eabihf--uclibc--stable-2018.11-1"
stamp = target / ".apcam-sha256"
def remove_path(path):
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        path.unlink()
    elif path.exists():
        shutil.rmtree(path)
if not target.is_dir() or not stamp.is_file() or stamp.read_text().strip() != expected:
    sys.path.insert(0, str(repo / "tools"))
    from safe_tar import safe_extract
    backup = root / (target.name + ".old")
    remove_path(backup)
    with tempfile.TemporaryDirectory(prefix="zr10-toolchain-", dir=root) as temporary:
        temporary = Path(temporary)
        safe_extract(archive, temporary)
        staged = temporary / target.name
        if not staged.is_dir():
            raise SystemExit("toolchain archive has no expected top-level directory")
        (staged / ".apcam-sha256").write_text(expected + "\n")
        had_target = target.exists()
        if had_target:
            os.replace(target, backup)
        try:
            os.replace(staged, target)
        except Exception:
            if had_target and backup.exists():
                os.replace(backup, target)
            raise
    remove_path(backup)
print(target)
PY
