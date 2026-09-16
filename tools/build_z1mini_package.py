#!/usr/bin/env python3
"""Build the XFRobot application's ordinary ZIP overlay, without contacting hardware."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import struct
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def arm_binary(path):
    data = path.read_bytes()
    if (data[:7] != b'\x7fELF\x01\x01\x01' or len(data) < 52 or
            struct.unpack_from('<H', data, 18)[0] != 40 or
            not struct.unpack_from('<I', data, 36)[0] & 0x400):
        raise ValueError(f'{path}: expected ARM ELF32 little-endian hard-float executable')
    versions = subprocess.check_output(['readelf', '--version-info', str(path)], text=True)
    required = [tuple(map(int, v.split('.'))) for v in re.findall(r'Name: GLIBC_([0-9.]+)', versions)]
    if not required or max(required) > (2, 25):
        raise ValueError(f'{path}: symbol requirements exceed camera glibc 2.25: {required}')
    program = subprocess.check_output(['readelf', '-l', str(path)], text=True)
    if '/lib/ld-linux-armhf.so.3' not in program:
        raise ValueError(f'{path}: wrong ARM runtime interpreter')
    return data


def build(output, native_capture=None):
    payload = {
        'camera-app': arm_binary(ROOT / 'camera_app/build/z1mini/camera-app'),
        'z1mini-web': arm_binary(ROOT / 'web/z1mini-web'),
        'service.sh': (ROOT / 'packaging/z1mini/service.sh').read_bytes(),
        'camera.ini.default': (ROOT / 'packaging/z1mini/camera.ini').read_bytes(),
        'web.pass.default': b'ardupilot\n',
        'README.md': (ROOT / 'packaging/z1mini/README.md').read_bytes(),
    }
    if native_capture is not None:
        payload['ax-capture'] = arm_binary(native_capture)
        payload['camera.ini.default'] = payload['camera.ini.default'].replace(
            b'[recording]\nautorecord = false\nresolution = 1920x1080',
            b'[recording]\nautorecord = false\nresolution = 3840x2160')
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--'], cwd=ROOT).returncode != 0
    payload['manifest.json'] = (json.dumps({
        'format': 1, 'target': 'xfrobot-z1mini', 'architecture': 'armv7-hardfloat-glibc',
        'revision': revision, 'dirty': dirty, 'experimental': True,
        'update_type': 'application-overlay', 'vendor_isp_required': native_capture is None,
        'vendor_isp_sha256': 'c50eb60f7d7250262d353adaa59211cb2826ca92a6981abf3fae41da580a9f04',
        'native_vin_isp': native_capture is not None, 'public_xfrobot_sdk': True,
        'protocols': ['MAVLink2', 'XFRobot'], 'web_port': 80 if native_capture is not None else 8080,
    }, indent=2) + '\n').encode()
    payload['SHA256SUMS'] = ''.join(
        f'{hashlib.sha256(data).hexdigest()}  {name}\n' for name, data in sorted(payload.items())
    ).encode()
    members = {'gcu/ap/' + name: data for name, data in payload.items()}
    # Write the startup hook last. Checksums reject partially overlaid payloads
    # on boot, but extraction itself is not power-loss atomic.
    # The vendor updater replaces all of /opt/bin/gcu, so rcS's ipc/run.sh must be supplied.
    members['gcu/ipc/run.sh'] = (ROOT / 'packaging/z1mini/run.sh').read_bytes()
    members['gcu/ipc/camera_gcu.sh'] = (ROOT / 'packaging/z1mini/camera_gcu.sh').read_bytes()
    if native_capture is not None:
        members['gcu/ipc/camera_gcu.sh'] = members['gcu/ipc/camera_gcu.sh'].replace(b'NATIVE=false', b'NATIVE=true')
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + '.tmp')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in members.items():
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            executable = name.endswith(('.sh', '/camera-app', '/z1mini-web', '/ax-capture'))
            info.external_attr = (0o100755 if executable else 0o100644) << 16
            archive.writestr(info, data)
    with zipfile.ZipFile(temporary) as archive:
        if archive.testzip() is not None:
            raise ValueError('ZIP verification failed')
    temporary.replace(output)
    checksum = hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix(output.suffix + '.sha256').write_text(f'{checksum}  {output.name}\n')
    print(f'{output}: {output.stat().st_size} bytes, sha256={checksum}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--native-capture', type=Path, help='include the optional AX capture executable')
    args = parser.parse_args()
    build(args.output, args.native_capture)
