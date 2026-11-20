#!/usr/bin/env python3
"""Exercise SD installation with and without an original camera-app backup."""
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='zr10-install-') as work:
    for existing in ('stock', 'ap-flash'):
        root = Path(work) / existing
        source = root / 'mnt/zr10'
        customer = root / 'customer'
        commands = root / 'commands'
        for path in (source, customer, commands):
            path.mkdir(parents=True)
        # All device checks and fixed installation paths stay inside the fixture.
        replacements = {'/mnt': str(root / 'mnt'), '/customer': str(customer),
                        '/sys/devices/soc0/machine': str(root / 'machine'),
                        '/proc/mounts': str(root / 'mounts'),
                        '/tmp/zr10-service.pid': str(root / 'service.pid'),
                        '/dev/console': str(root / 'console')}
        for script in (ROOT / 'packaging/zr10').glob('*.sh'):
            text = script.read_text()
            for before, after in replacements.items():
                text = text.replace(before, after)
            (source / script.name).write_text(text)
        for name in ('camera-app', 'zr10-web', 'zr10-uuid'):
            (source / name).write_text('#!/bin/sh\nexit 0\n')
        for name in ('camera.ini.default', 'web.pass.default', 'VERSION', 'README.md',
                     'COPYING.txt', 'THIRD_PARTY.md', 'OpenIPC-LICENSE.txt'):
            (source / name).write_text('fixture\n')
        (source / 'webroot').mkdir()
        (source / 'webroot/app.js').write_text('/* packaged web UI */\n')
        destination = root / 'mnt/AP_CameraGimbal/zr10'
        (destination / 'webroot').mkdir(parents=True)
        (destination / 'webroot/stale.js').write_text('old version')
        (root / 'machine').write_text('INFINITY6B0\n')
        (root / 'mounts').write_text(f'/dev/mmcblk0p1 {root}/mnt vfat rw 0 0\n')
        (root / 'sdk').write_bytes(b'SDK fixture')
        (source / 'SDK_SHA256SUMS').write_text(
            hashlib.sha256((root / 'sdk').read_bytes()).hexdigest() + f'  {root}/sdk\n')
        for path in source.iterdir():
            path.chmod(0o755)
        (source / 'SHA256SUMS').write_text(''.join(
            hashlib.sha256(path.read_bytes()).hexdigest() + '  ' + path.relative_to(source).as_posix() + '\n'
            for path in sorted(source.rglob('*')) if path.is_file()))
        for name, command in [('id', 'echo 0'), ('pidof', 'exit 1'), ('sync', 'exit 0')]:
            (commands / name).write_text('#!/bin/sh\n' + command + '\n')
            (commands / name).chmod(0o755)
        original = b'#!/bin/sh\n# Original camera fixture\nexit 0\n'
        (customer / 'sycamera').write_bytes(original if existing == 'stock' else
                                           (source / 'boot.sh').read_bytes())
        (customer / 'sycamera').chmod(0o755)
        env = dict(os.environ, PATH=str(commands) + ':' + os.environ['PATH'])
        subprocess.run(['sh', str(source / 'install.sh'), '--enable-boot'],
                       env=env, check=True, stdout=subprocess.DEVNULL)
        assert (destination / 'webroot/app.js').read_bytes() == (source / 'webroot/app.js').read_bytes()
        assert not (destination / 'webroot/stale.js').exists()
        subprocess.run(['sha256sum', '-c', 'SHA256SUMS'], cwd=destination,
                       check=True, stdout=subprocess.DEVNULL)
        assert (customer / 'zr10-uuid').read_bytes() == (source / 'zr10-uuid').read_bytes()
        assert (customer / 'sycamera').read_bytes() == (source / 'boot.sh').read_bytes()
        backup = customer / 'sycamera.vendor'
        assert backup.exists() == (existing == 'stock')
        result = subprocess.run(['sh', str(source / 'uninstall.sh')], env=env,
                                capture_output=True, text=True)
        if existing == 'stock':
            assert result.returncode == 0 and (customer / 'sycamera').read_bytes() == original
        else:
            assert result.returncode == 1 and 'original SIYI firmware' in result.stderr
print('PASS ZR10 SD install: UUID helper, stock rollback, no false vendor backup on AP flash installs')
