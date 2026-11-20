#!/usr/bin/env python3
"""Check offline support-tool installation, integrity and cache recovery."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('prebuilt_tools', ROOT / 'tools/prebuilt_mt11_tools.py')
tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tools)


class PrebuiltTools(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='prebuilt-tools-test-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.output = self.root / 'installed'

    def test_install_and_repair(self):
        tools.install(tools.BUNDLE, self.output)
        manifest = json.loads((tools.BUNDLE / 'manifest.json').read_text())
        before = {}
        for name in tools.NAMES:
            path = self.output / 'bin' / name
            self.assertEqual(tools.digest(path.read_bytes()), manifest['binaries'][name]['sha256'])
            self.assertEqual(path.stat().st_mode & 0o777, 0o755)
            before[name] = path.stat().st_mtime_ns
        tools.install(tools.BUNDLE, self.output)
        self.assertEqual(before, {n: (self.output / 'bin' / n).stat().st_mtime_ns for n in tools.NAMES})
        (self.output / 'bin/rsync').unlink()
        (self.output / 'bin/strace').write_bytes(b'broken cache')
        tools.install(tools.BUNDLE, self.output)
        for name in tools.NAMES:
            self.assertEqual(tools.digest((self.output / 'bin' / name).read_bytes()),
                             manifest['binaries'][name]['sha256'])

    def test_damaged_archive_keeps_installed_tools(self):
        tools.install(tools.BUNDLE, self.output)
        bundle = self.root / 'bundle'
        shutil.copytree(tools.BUNDLE, bundle)
        (bundle / 'tcpdump.xz').write_bytes(b'bad archive')
        before = {n: (self.output / 'bin' / n).stat().st_mtime_ns for n in tools.NAMES}
        with self.assertRaisesRegex(ValueError, 'compressed checksum mismatch'):
            tools.install(bundle, self.output)
        self.assertEqual(before, {n: (self.output / 'bin' / n).stat().st_mtime_ns for n in tools.NAMES})

    def test_decompressed_checksum(self):
        bundle = self.root / 'bundle'
        shutil.copytree(tools.BUNDLE, bundle)
        path = bundle / 'manifest.json'
        manifest = json.loads(path.read_text())
        manifest['binaries']['rsync']['sha256'] = '0' * 64
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, 'executable checksum mismatch'):
            tools.install(bundle, self.output)
        self.assertFalse(self.output.exists())

    def test_make_without_environment_or_compiler(self):
        # Run the actual Makefile in an otherwise empty checkout, with only
        # Make and Python on PATH. A network/compiler/build-script call fails.
        repo = self.root / 'checkout'
        repo.mkdir()
        shutil.copyfile(ROOT / 'Makefile', repo / 'Makefile')
        (repo / 'tools').mkdir(exist_ok=True)
        shutil.copyfile(ROOT / 'tools/build.mk', repo / 'tools/build.mk')
        shutil.copyfile(ROOT / 'tools/zt30_stream.mk', repo / 'tools/zt30_stream.mk')
        shutil.copyfile(ROOT / 'tools/prebuilt_mt11_tools.py', repo / 'tools/prebuilt_mt11_tools.py')
        shutil.copytree(tools.BUNDLE, repo / 'packaging/mt11/tools')
        commands = self.root / 'commands'
        commands.mkdir()
        for name in ('make', 'python3'):
            (commands / name).symlink_to(shutil.which(name))
        env = os.environ.copy()
        for name in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'DEPS_ROOT', 'MT11_TOOLS_ROOT'):
            env.pop(name, None)
        env['PATH'] = str(commands)
        subprocess.run(['make', '-j4', 'mt11_tools', 'MT11_VERSION=v1.0',
                        'MT11_GIT_HASH=123456', 'ZR10_BUILD_HASH=123456',
                        'Z1MINI_BUILD_HASH=123456', 'CROSS_COMPILE=missing-compiler-'],
                       cwd=repo, env=env, check=True)
        self.assertTrue((repo / 'build/deps/mt11-tools/bin/tcpdump').is_file())


if __name__ == '__main__':
    unittest.main()
