#!/usr/bin/env python3
"""Check the build identity used for numeric UniGCS camera-version replies."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildVersionTest(unittest.TestCase):
    def generate(self, directory, version):
        output = Path(directory)/'build_version.h'
        env = dict(os.environ, MT11_VERSION=version, MT11_GIT_HASH='123abc')
        subprocess.run([sys.executable, str(ROOT/'tools/write_build_version.py'), str(output)],
                       env=env, check=True)
        macros = dict(line.split(' ', 2)[1:] for line in output.read_text().splitlines()
                      if line.startswith('#define '))
        self.assertEqual(json.loads(macros['CA_FIRMWARE_VERSION']), version or 'unknown')
        numeric = tuple(int(macros['CA_FIRMWARE_VERSION_'+part])
                        for part in ('MAJOR', 'MINOR', 'PATCH'))
        return output, numeric

    def test_release_and_patch_versions(self):
        with tempfile.TemporaryDirectory() as directory:
            for version, expected in [('v1.0.1', (1, 0, 1)), ('v2.7.19', (2, 7, 19)),
                                      ('v1.0', (1, 0, 0)), ('3.4.5', (3, 4, 5)),
                                      ('v255.254.253', (255, 254, 253))]:
                with self.subTest(version=version):
                    self.assertEqual(self.generate(directory, version)[1], expected)

    def test_unknown_invalid_and_overflow_do_not_spoof_or_wrap(self):
        with tempfile.TemporaryDirectory() as directory:
            for version in ('', 'unknown', 'v1', 'v-1.0.1', 'v1.2.3-rc1',
                            'v256.0.1', 'v1.256.1', 'v1.0.256', 'AP-1.0.1'):
                with self.subTest(version=version):
                    self.assertEqual(self.generate(directory, version)[1], (0, 0, 0))

    def test_same_identity_preserves_header_mtime(self):
        with tempfile.TemporaryDirectory() as directory:
            output, _ = self.generate(directory, 'v1.0.1')
            os.utime(output, ns=(1_000_000_000, 1_000_000_000))
            self.generate(directory, 'v1.0.1')
            self.assertEqual(output.stat().st_mtime_ns, 1_000_000_000)
            _, numeric = self.generate(directory, 'v1.0.2')
            self.assertEqual(numeric, (1, 0, 2))
            self.assertNotEqual(output.stat().st_mtime_ns, 1_000_000_000)


if __name__ == '__main__':
    unittest.main()
