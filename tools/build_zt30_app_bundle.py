#!/usr/bin/env python3
"""Build a deterministic ZT30 application-only bundle against a captured runtime.

No credentials, configuration, launchers, kernel or MCU firmware are included.
Unsigned SHA256 checks integrity, not authenticity. See packaging/zt30/APP_BUNDLE.md.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
from check_zt30_elf import check

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 32 * 1024 * 1024
TOKEN = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.+\-]{0,127}\Z')


def token(value, maximum=128):
    return len(value) <= maximum and TOKEN.fullmatch(value) and '..' not in value


def asset(path):
    parts = path.split('/')
    return parts[0] == 'webroot' and 2 <= len(parts) <= 5 and all(token(p, 80) for p in parts[1:]) and Path(path).suffix in {'.html', '.css', '.js', '.txt'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def build(args):
    if not token(args.version, 80):
        raise ValueError('version must be a bounded ASCII token')
    runtime_root = args.runtime_root.resolve()
    sources = {'camera-app': args.camera_app, 'zt30-web': args.web_app, 'encoder-sdk.so': args.sdk}
    runtime = {}
    for name, path in sources.items():
        if path.is_symlink() or not path.is_file():
            raise ValueError('binary input must be a regular file: ' + str(path))
        report = check(path, args.readelf, runtime_root, ('/config/lib', '/lib', '/usr/lib'), shared_library=name == 'encoder-sdk.so')
        for dep in report['runtime_dependencies']:
            dep_path = Path(dep['path'])
            device_path = '/' + dep_path.relative_to(runtime_root).as_posix()
            if not any(device_path.startswith(p) and token(device_path[len(p):]) for p in ('/lib/', '/usr/lib/', '/config/lib/')):
                raise ValueError('unsupported qualified runtime path: ' + device_path)
            data = dep_path.read_bytes()
            item = {'name': dep['name'], 'path': device_path, 'size': len(data), 'sha256': digest(data)}
            if not token(item['name']) or not 0 < len(data) <= LIMIT:
                raise ValueError('invalid runtime dependency')
            if item['name'] in runtime and runtime[item['name']] != item:
                raise ValueError('inconsistent runtime dependency')
            runtime[item['name']] = item
    if not 1 <= len(runtime) <= 64:
        raise ValueError('invalid runtime dependency count')
    if args.webroot.is_symlink() or not args.webroot.is_dir():
        raise ValueError('webroot must be a real directory')
    for path in sorted(args.webroot.rglob('*')):
        if path.is_symlink():
            raise ValueError('symlink asset input: ' + str(path))
        name = 'webroot/' + path.relative_to(args.webroot).as_posix()
        if path.is_dir():
            if len(name.split('/')) > 4 or not all(token(p, 80) for p in name.split('/')[1:]):
                raise ValueError('invalid asset directory')
            continue
        if not path.is_file() or not asset(name):
            raise ValueError('invalid asset input: ' + name)
        sources[name] = path
    required = {'webroot/live.js', 'webroot/status-actions.html', 'webroot/parameters-form.html'}
    if not required <= sources.keys() or len(sources) > 128:
        raise ValueError('incomplete or excessive asset tree')
    files, payload = [], []
    for name, path in sorted(sources.items()):
        data = path.read_bytes()
        binary = not name.startswith('webroot/')
        if not 0 < len(data) <= (16 if binary else 2) * 1024 * 1024:
            raise ValueError('invalid payload size: ' + name)
        files.append({'path': name, 'mode': 0o755 if binary else 0o644, 'size': len(data), 'sha256': digest(data)})
        payload.append(data)
    directories = {str(parent) for name in sources for parent in Path(name).parents if str(parent) != '.'}
    if len(directories) + len(files) + 1 > 256:
        raise ValueError('too many application tree entries')
    manifest = {'schema': 'apcg.zt30.app.v1', 'board': 'SIYI_ZT30', 'launcher': 1, 'version': args.version,
                'runtime': [runtime[n] for n in sorted(runtime)], 'files': files}
    text = json.dumps(manifest, separators=(',', ':'), ensure_ascii=True).encode('ascii')
    if len(text) > 65536 or 12 + len(text) + sum(map(len, payload)) > LIMIT:
        raise ValueError('bundle exceeds bounds')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix='.' + args.output.name + '.', dir=args.output.parent, delete=False) as f:
            temporary = f.name
            f.write(b'APCZT301' + struct.pack('<I', len(text)))
            f.write(text)
            for data in payload:
                f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(temporary, 0o644)
        os.replace(temporary, args.output)
        temporary = None
    finally:
        if temporary:
            os.unlink(temporary)
    return {'slot': digest(text), 'version': args.version, 'bytes': args.output.stat().st_size,
            'sha256': digest(args.output.read_bytes()), 'runtime_libraries': len(runtime), 'files': len(files)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--camera-app', type=Path, default=ROOT / 'camera_app/build/zt30-dev/camera-app')
    p.add_argument('--web-app', type=Path, default=ROOT / 'web/zt30-web')
    p.add_argument('--sdk', type=Path, default=ROOT / 'build/zt30/encoder-sdk.so')
    p.add_argument('--webroot', type=Path, default=ROOT / 'web/webroot')
    p.add_argument('--runtime-root', type=Path, required=True)
    p.add_argument('--readelf', default='readelf')
    p.add_argument('--version', required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    try:
        print(json.dumps(build(args), indent=2))
    except (ValueError, OSError) as e:
        p.exit(1, str(e) + '\n')


if __name__ == '__main__':
    main()
