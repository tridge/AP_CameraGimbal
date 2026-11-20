#!/usr/bin/env python3
"""Check real Make dependencies and atomic publication of patched RTSP inputs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCES = Path(os.environ.get('SS928_MPP_ROOT', ROOT / 'build/deps/ss928-mpp')) / 'src/rtspserver/src'


@unittest.skipUnless((SOURCES / 'net/BufferWriter.cpp').exists(), 'run make dependencies first')
class XopPreparationTest(unittest.TestCase):
    def test_changed_headers_missing_files_and_failed_preparation(self):
        with tempfile.TemporaryDirectory(prefix='apcam-xop-build-') as directory:
            root = Path(directory)
            for name in ('camera_app/Makefile', 'tools/build.mk', 'tools/prepare_xop.py',
                         'tools/xop_warnings.patch', 'include/apcam/targets.mk'):
                output = root / name
                output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, output)
            source = root / 'sdk/src/rtspserver/src'
            shutil.copytree(SOURCES, source)
            env = {key: value for key, value in os.environ.items()
                   if key not in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL')}
            def make(success=True):
                result = subprocess.run(['make', '-j10', '-C', str(root / 'camera_app'),
                                         'build/xop_buffer_writer.o', 'CROSS_COMPILE=',
                                         'SS928_MPP_ROOT=' + str(root / 'sdk')], env=env,
                                        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode == 0, success, result.stdout)
                return result.stdout
            make()
            obj = root / 'camera_app/build/xop_buffer_writer.o'
            built = obj.stat().st_mtime_ns
            self.assertNotIn('Compiling', make())
            self.assertEqual(obj.stat().st_mtime_ns, built)
            # A vendor header which this object does not include must not
            # force recompilation just because the preparation stamp changed.
            unrelated = source / 'xop/rtsp.h'
            unrelated.write_text(unrelated.read_text() + '\n// unrelated edit\n')
            self.assertNotIn('Compiling', make())
            self.assertEqual(obj.stat().st_mtime_ns, built)
            header = source / 'net/Socket.h'
            header.write_text(header.read_text() + '\n// included edit\n')
            self.assertIn('Compiling', make())
            self.assertGreater(obj.stat().st_mtime_ns, built)
            prepared = root / 'camera_app/build/xop'
            expected_header = (prepared / 'net/Socket.h').read_bytes()
            (prepared / 'net/Socket.h').unlink()
            self.assertIn('Compiling', make())
            self.assertEqual((prepared / 'net/Socket.h').read_bytes(), expected_header)
            original_output = (prepared / 'net/BufferWriter.cpp').read_bytes()
            incompatible = source / 'net/BufferWriter.cpp'
            incompatible.write_text(incompatible.read_text().replace('pkt.data.reset(new char[size+512]);',
                                                                    'unexpected upstream version;'))
            make(success=False)
            self.assertEqual((prepared / 'net/BufferWriter.cpp').read_bytes(), original_output)
            self.assertFalse(list(prepared.parent.glob('.xop-*')))


if __name__ == '__main__':
    unittest.main()
