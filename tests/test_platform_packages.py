#!/usr/bin/env python3
"""Inspect built SD images without needing any vendor firmware bundle."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import zr10_firmware

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('release', type=Path)
args = parser.parse_args()
for target, filename, size in [('a8', 'SIYI_4K_MINI_UpgradeSD.bin', 0x600000),
                               ('zr10', 'ZR10_UpgradeSD.bin', 0x6b0000)]:
    image = (args.release / ('SIYI_' + target.upper()) / filename).read_bytes()
    script = (ROOT / f'packaging/{target}/upgrade_script.txt').read_bytes()
    assert image[:0x4000] == script.ljust(0x4000, b'\xff')
    assert len(image) == 0x4000 + size + (36 if target == 'zr10' else 0)
    if target == 'zr10':
        zr10_firmware.validate(image)
    with tempfile.TemporaryDirectory(prefix=target + '-package-') as work:
        work = Path(work)
        (work / 'customer.jffs2').write_bytes(image[0x4000:0x4000 + size])
        customer = work / 'customer'
        subprocess.run(['jefferson', str(work / 'customer.jffs2'), '-d', str(customer)],
                       stdout=subprocess.DEVNULL, check=True)
        platform = ROOT / f'packaging/{target}/platform'
        for line in (platform / 'SHA256SUMS').read_text().splitlines():
            digest, name = line.split('  ')
            assert hashlib.sha256((customer / name).read_bytes()).hexdigest() == digest, name
        forbidden = ['boa', 'audio', 'osdpic', 'OSD_Path', 'wifi', 'sycamera.vendor', 'bin/cardv.vendor']
        assert not any((customer / name).exists() for name in forbidden)
        app = customer / 'camera-app'
        for name in ('camera-app', target + '-web', target + '-uuid'):
            assert (app / name).read_bytes().startswith(b'\x7fELF'), name
        for asset in (ROOT / 'web/webroot').rglob('*'):
            if asset.is_file():
                assert (app / 'webroot' / asset.relative_to(ROOT / 'web/webroot')).read_bytes() == asset.read_bytes()
        assert (customer / '.sys_upgrade_finish_flag').is_file()
        if target == 'a8':
            assert (customer / 'bin/cardv').read_bytes() == (ROOT / 'packaging/a8/app_init.sh').read_bytes()
            assert not (app / 'camera.ini').exists()  # Defaults must not overwrite saved settings.
        else:
            assert (customer / 'zr10-uuid').read_bytes() == (app / 'zr10-uuid').read_bytes()
            assert (customer / 'sycamera').read_bytes() == (ROOT / 'packaging/zr10/boot.sh').read_bytes()
            assert b'boa' not in (customer / 'demo.sh').read_bytes()
    print('PASS', target, 'SD image: platform hashes, boot script, UUID helper, AP payload; no vendor application or UI assets')
