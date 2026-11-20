#!/usr/bin/env python3
"""Exercise release layout and failed-build recovery without cross compilers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_release as release


class ReleaseTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='apcam-release-test-')
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name)
        shutil.copytree(ROOT / 'packaging/release', self.repo / 'packaging/release')
        def git(*args):
            return subprocess.check_output(['git', *args], cwd=self.repo, stderr=subprocess.DEVNULL)
        self.git = git
        git('init')
        git('add', 'packaging')
        git('-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-m', 'Fixture')
        self.revision = git('rev-parse', 'HEAD').decode().strip()
        self.output = self.repo / 'release'
        self.make = self.repo / 'fake-make'
        self.make.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
args = dict(arg.split('=', 1) for arg in sys.argv[2:])
out = Path(next(value for key, value in args.items() if key.endswith('_OUT')))
out.write_bytes(b'firmware-' + sys.argv[1].encode())
out.with_name(out.name + '.sha256').write_text('old sidecar format')
if Path('fail').exists():
    sys.exit(1)
''')
        self.make.chmod(0o755)

    def build(self, targets, version='v1.0'):
        release.build_release(self.repo, self.output, version, targets, str(self.make))

    def test_patch_version_packages(self):
        self.build(list(release.TARGETS), 'v1.0.1')
        for name, (vendor, _, _, pattern) in release.TARGETS.items():
            folder = self.output / 'v1.0.1' / f'{vendor}_{name}'
            firmware = pattern.format(version='v1.0.1', short_revision=self.revision[:6])
            self.assertTrue((folder / firmware).is_file())
            info = json.loads((folder / 'BUILD_INFO.json').read_text())
            self.assertEqual(info['version'], 'v1.0.1')
            self.assertEqual(info['package'], firmware)
            self.assertIn('v1.0.1', (folder / 'README.md').read_text())

    def test_make_selects_highest_reachable_version(self):
        for name in ('Makefile', 'web/Makefile', 'include/apcam/targets.mk',
                     'tools/build.mk', 'tools/zt30_stream.mk'):
            destination = self.repo / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / name, destination)
        env = os.environ.copy()
        for key in ('MT11_VERSION', 'MT11_GIT_HASH', 'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL'):
            env.pop(key, None)

        def check(expected):
            for directory in (self.repo, self.repo / 'web'):
                with self.subTest(directory=directory.name, expected=expected):
                    result = subprocess.check_output(
                        ['make', '--no-print-directory', '-s', '-f', 'Makefile', '-f', '-', 'print-version'],
                        input=".PHONY: print-version\nprint-version:\n\t@echo '$(MT11_VERSION)'\n",
                        cwd=directory, env=env, text=True)
                    self.assertEqual(result.strip(), expected)

        self.git('tag', 'post-refactor')
        self.git('tag', 'v9.0.0-rc1')
        check('')
        for version in ('v1.0', 'v1.0.1', 'v1.0.2', 'v1.0.10', 'v1.1'):
            self.git('tag', version)
            check(version)
        self.git('-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                 'commit', '--allow-empty', '-m', 'Unreachable version')
        self.git('tag', 'v99.0.0')
        self.git('checkout', '--detach', self.revision)
        check('v1.1')

    def test_make_default_release_targets(self):
        for name in ('Makefile', 'tools/build.mk', 'tools/zt30_stream.mk'):
            destination = self.repo / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(ROOT / name, destination)
        output = subprocess.check_output(
            ['make', '--no-print-directory', '-s', '-f', 'Makefile', '-f', '-', 'print-targets'],
            input=".PHONY: print-targets\nprint-targets:\n\t@echo '$(RELEASE_TARGETS)'\n",
            cwd=self.repo, text=True)
        self.assertEqual(set(output.split()), set(release.TARGETS))

    def test_layout_and_checksums(self):
        self.build(list(release.TARGETS))
        expected = {
            'SIYI_A8': 'SIYI_4K_MINI_UpgradeSD.bin',
            'SIYI_MT11': f'MT11_FW_ArduPilot_v1.0_{self.revision[:6]}.bin',
            'SIYI_ZR10': 'ZR10_UpgradeSD.bin',
            'SIYI_ZT30': f'ZT30_AP_v1.0_{self.revision[:6]}.apbundle',
            'XFRobot_Z1-Mini': f'Z1Mini_AP_native_v1.0_{self.revision[:6]}.gcu',
        }
        for name, firmware in expected.items():
            folder = self.output / 'v1.0' / name
            self.assertEqual({p.name for p in folder.iterdir()},
                             {firmware, 'README.md', 'BUILD_INFO.json', 'SHA256SUMS'})
            for line in (folder / 'SHA256SUMS').read_text().splitlines():
                digest, filename = line.split('  ')
                self.assertEqual(digest, hashlib.sha256((folder / filename).read_bytes()).hexdigest())
            info = json.loads((folder / 'BUILD_INFO.json').read_text())
            self.assertEqual(info['revision'], self.revision)
            self.assertFalse(info['dirty'])
            self.assertEqual(info['package'], firmware)
            self.assertIn(firmware, (folder / 'README.md').read_text())
            self.assertIn(self.revision, (folder / 'README.md').read_text())
        self.assertEqual(set(p.name for p in (self.output / 'v1.0').iterdir()), set(expected))

    def test_failed_rebuild_preserves_complete_package(self):
        self.build(['A8'])
        folder = self.output / 'v1.0/SIYI_A8'
        original = {p.name: p.read_bytes() for p in folder.iterdir()}
        template = self.repo / 'packaging/release/A8.md'
        template.write_text(template.read_text() + '\nUpdated instructions\n')
        (self.repo / 'fail').touch()
        with self.assertRaises(subprocess.CalledProcessError):
            self.build(['A8'])
        self.assertEqual(original, {p.name: p.read_bytes() for p in folder.iterdir()})
        self.assertEqual([p.name for p in folder.parent.iterdir()], ['SIYI_A8'])
        (self.repo / 'fail').unlink()
        self.build(['A8'])
        self.assertIn('Updated instructions', (folder / 'README.md').read_text())
        self.assertTrue(json.loads((folder / 'BUILD_INFO.json').read_text())['dirty'])
        self.assertEqual([p.name for p in folder.parent.iterdir()], ['SIYI_A8'])

    def test_target_aliases_and_version_validation(self):
        self.assertEqual(release.target_name('a8'), 'A8')
        self.assertEqual(release.target_name('z1mini'), 'Z1-Mini')
        self.assertEqual(release.target_name('zt30'), 'ZT30')
        with self.assertRaises(argparse.ArgumentTypeError):
            release.target_name('unknown')
        for version in ('post-refactor', 'v1', 'v1.0.', 'v1.0.1.2', 'v1.0.1-rc1', '1.0.1'):
            with self.subTest(version=version), self.assertRaises(ValueError):
                release.build_release(self.repo, self.output, version, ['A8'], str(self.make))
        self.assertFalse(self.output.exists())


if __name__ == '__main__':
    unittest.main()
