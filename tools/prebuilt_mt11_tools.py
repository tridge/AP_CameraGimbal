#!/usr/bin/env python3
"""Verify/unpack MT11 support tools, or refresh their checked-in compressed copies."""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = ROOT / 'packaging/mt11/tools'
NAMES = ('rsync', 'strace', 'tcpdump', 'ltrace', 'dropbear', 'dropbearkey')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def check_elf(data, name):
    if (len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or
            struct.unpack_from('<H', data, 18)[0] != 183):
        raise ValueError(f'{name}: expected a little-endian AArch64 ELF executable')
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if size < 56 or offset + size * count > len(data):
        raise ValueError(f'{name}: invalid ELF program headers')
    for index in range(count):
        if struct.unpack_from('<I', data, offset + size * index)[0] == 3:
            raise ValueError(f'{name}: unexpected dynamic interpreter')


def write_changed(path, data, mode=0o644):
    if path.is_file() and not path.is_symlink() and path.read_bytes() == data:
        path.chmod(mode)
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as out:
            out.write(data)
            os.fchmod(out.fileno(), mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def install(bundle, output):
    manifest_bytes = (bundle / 'manifest.json').read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest['format'] != 1 or set(manifest['binaries']) != set(NAMES):
        raise ValueError('Unexpected MT11 tools manifest')
    # Verify the complete bundle before replacing any installed files.
    binaries = {}
    for name in NAMES:
        expected = manifest['binaries'][name]
        compressed = (bundle / (name + '.xz')).read_bytes()
        if digest(compressed) != expected['xz_sha256']:
            raise ValueError(f'{name}: compressed checksum mismatch')
        data = lzma.decompress(compressed)
        if len(data) != expected['size'] or digest(data) != expected['sha256']:
            raise ValueError(f'{name}: executable checksum mismatch')
        check_elf(data, name)
        binaries[name] = data
    for name, data in binaries.items():
        write_changed(output / 'bin' / name, data, 0o755)
    write_changed(output / 'versions', manifest['versions'].encode())
    write_changed(output / '.built', (digest(manifest_bytes) + '\n').encode())
    print(f'MT11 prebuilt support tools verified in {output / "bin"}')


def refresh(built, bundle, cross_compile):
    manifest = {'format': 1, 'versions': (built / 'versions').read_text(),
                'build_script_sha256': digest((ROOT / 'tools/build_mt11_tools.sh').read_bytes()),
                'binaries': {}}
    with tempfile.TemporaryDirectory(prefix='mt11-tools-strip-') as directory:
        for name in NAMES:
            binary = Path(directory) / name
            shutil.copyfile(built / 'bin' / name, binary)
            subprocess.run([cross_compile + 'strip', str(binary)], check=True)
            dynamic = subprocess.check_output([cross_compile + 'readelf', '-d', str(binary)])
            if b'(NEEDED)' in dynamic:
                raise ValueError(f'{name}: unexpected shared-library dependency')
            data = binary.read_bytes()
            check_elf(data, name)
            compressed = lzma.compress(data, preset=9)
            write_changed(bundle / (name + '.xz'), compressed)
            manifest['binaries'][name] = {'size': len(data), 'sha256': digest(data),
                                          'xz_sha256': digest(compressed)}
    write_changed(bundle / 'manifest.json', (json.dumps(manifest, indent=2) + '\n').encode())
    print(f'MT11 prebuilt support tools refreshed in {bundle}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, default=BUNDLE)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/deps/mt11-tools')
    parser.add_argument('--refresh-from', type=Path, help='refresh the bundle from a source build')
    parser.add_argument('--cross-compile', default='aarch64-linux-gnu-')
    args = parser.parse_args()
    try:
        if args.refresh_from:
            refresh(args.refresh_from, args.bundle, args.cross_compile)
        else:
            install(args.bundle, args.output)
    except (OSError, ValueError, KeyError, lzma.LZMAError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'MT11 support tools: {error}\n')


if __name__ == '__main__':
    main()
