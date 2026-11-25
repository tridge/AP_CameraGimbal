#!/usr/bin/env python3
"""Build the ZT30 customer-only microSD image with an embedded web bundle."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

from build_zt30_app_bundle import asset, token
from build_zt30_dev_package import MIN_IO, LEB_BYTES, CUSTOMER_FS_MAX_LEBS
from check_zt30_elf import check, captured_path
import zt30_firmware

ROOT = Path(__file__).resolve().parents[1]


def populate(customer, args, data=None):
    if data is None:
        data = args.bundle.read_bytes()
    if data[:8] != b'APCZT301' or len(data) < 12 or len(data) > 32*1024*1024:
        raise ValueError('invalid application bundle')
    length = struct.unpack_from('<I', data, 8)[0]
    if not 0 < length <= 65536 or 12+length > len(data):
        raise ValueError('invalid application manifest length')
    text = data[12:12+length]
    manifest = json.loads(text)
    if (set(manifest) != {'schema', 'board', 'launcher', 'version', 'runtime', 'files'} or
            manifest.get('schema') != 'apcg.zt30.app.v1' or manifest.get('board') != 'SIYI_ZT30' or
            not token(manifest.get('version', ''), 80) or
            manifest.get('launcher') != 1 or json.dumps(manifest, separators=(',', ':'), ensure_ascii=True).encode() != text):
        raise ValueError('invalid application identity or canonical manifest')
    if not 1 <= len(manifest['runtime']) <= 64 or not 1 <= len(manifest['files']) <= 128:
        raise ValueError('invalid application entry count')
    previous = ''
    for entry in manifest['runtime']:
        name, path = entry['name'], entry['path']
        if (not token(name) or name <= previous or
                not any(path.startswith(prefix) and token(path[len(prefix):])
                        for prefix in ('/lib/', '/usr/lib/', '/config/lib/'))):
            raise ValueError('invalid runtime dependency')
        library = captured_path(args.runtime_root.resolve(), path).read_bytes()
        if len(library) != entry['size'] or hashlib.sha256(library).hexdigest() != entry['sha256']:
            raise ValueError('runtime checksum mismatch: '+path)
        previous = name
    app = customer/'camera-app'
    slot_id = hashlib.sha256(text).hexdigest()
    slot = app/'slots'/slot_id
    slot.mkdir(parents=True)
    offset = 12+length
    previous = ''
    for entry in manifest['files']:
        name, size, mode = entry['path'], entry['size'], entry['mode']
        binary = name in ('camera-app', 'zt30-web', 'encoder-sdk.so', 'apcam-upload')
        if (name <= previous or (not binary and name != 'upload-ca.pem' and not asset(name)) or
                mode != (0o755 if binary else 0o644) or not 0 < size <= (16 if binary else 2)*1024*1024):
            raise ValueError('invalid application file')
        payload = data[offset:offset+size]
        if len(payload) != size or hashlib.sha256(payload).hexdigest() != entry['sha256']:
            raise ValueError('application checksum mismatch')
        target = slot/name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload); target.chmod(mode)
        offset += size; previous = name
    required = ('camera-app', 'zt30-web', 'encoder-sdk.so', 'webroot/live.js',
                'webroot/status-actions.html', 'webroot/parameters-form.html')
    if offset != len(data) or not all((slot/name).is_file() for name in required):
        raise ValueError('incomplete application bundle')
    # The upload worker is optional, but needs its CA store beside it.
    worker = (slot/'apcam-upload').is_file()
    if worker and not (slot/'upload-ca.pem').is_file():
        raise ValueError('incomplete native upload payload')
    (slot/'manifest.json').write_bytes(text); (slot/'manifest.json').chmod(0o644)
    for name in ('camera-app', 'zt30-web', 'encoder-sdk.so') + (('apcam-upload',) if worker else ()):
        check(slot/name, args.readelf, args.runtime_root.resolve(), ('/config/lib', '/lib', '/usr/lib'),
              shared_library=name == 'encoder-sdk.so')
    (app/'active').symlink_to('slots/'+slot_id)
    for name in ('camera-app', 'zt30-web', 'encoder-sdk.so', 'webroot'):
        (app/name).symlink_to('active/'+name)
    stable = {name: ROOT/'packaging/zt30'/name for name in (
        'app-init.sh', 'app-supervisor.sh', 'web-supervisor.sh', 'start-dropbear.sh')}
    stable.update({'zt30-slot': args.slot_tool, 'dropbear': args.dropbear, 'dropbearkey': args.dropbearkey})
    for name, source in stable.items():
        if source.is_symlink() or not source.is_file():
            raise ValueError('missing or unsafe bootstrap input: '+str(source))
        if not name.endswith('.sh'):
            check(source, args.readelf, args.runtime_root.resolve(), ('/config/lib', '/lib', '/usr/lib'))
        shutil.copyfile(source, app/name); (app/name).chmod(0o755)
    shutil.copyfile(ROOT/'packaging/zt30/camera.ini.dev', app/'camera.ini.default')
    (app/'camera.ini.default').chmod(0o644)
    (app/'web.pass.default').write_text('ardupilot\n'); (app/'web.pass.default').chmod(0o600)
    shutil.copyfile(ROOT/'packaging/zt30/platform-init.sh', customer/'demo.sh')
    (customer/'demo.sh').chmod(0o755)
    (customer/'.sys_upgrade_finish_flag').touch()
    for directory in (customer, app, app/'slots', slot, *[p for p in slot.rglob('*') if p.is_dir()]):
        directory.chmod(0o755)
    # Do not copy a captured customer root: that could distribute another
    # camera's settings, SSH identities, licence data or vendor application.
    return slot_id


def build(args):
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='zt30-firmware-', dir=args.output.parent) as work:
        work = Path(work); customer = work/'customer'; customer.mkdir()
        # The SD slot and web bundle must come from the same immutable read,
        # even if another build replaces the input while mkfs is running.
        bundle = args.bundle.read_bytes()
        populate(customer, args, bundle)
        payload = work/'customer.ubifs'
        subprocess.run([args.mkfs, '-r', str(customer), '-m', str(MIN_IO), '-e', str(LEB_BYTES),
                        '-c', str(CUSTOMER_FS_MAX_LEBS), '-U', '-o', str(payload)], check=True)
        image = zt30_firmware.pack(payload.read_bytes(), bundle)
        output = work/'firmware.bin'; output.write_bytes(image); output.chmod(0o644)
        os.replace(output, args.output)
        print(json.dumps(zt30_firmware.inspect(image), indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('bundle', 'slot-tool', 'dropbear', 'dropbearkey', 'runtime-root', 'output'):
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--readelf', default='readelf')
    p.add_argument('--mkfs', default='mkfs.ubifs')
    args = p.parse_args()
    try:
        build(args)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        p.exit(1, str(error)+'\n')


if __name__ == '__main__':
    main()
