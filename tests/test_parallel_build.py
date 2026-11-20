#!/usr/bin/env python3
"""Exercise generated-input ordering, diagnostics and failed-build recovery."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ParallelBuildTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='apcam-parallel-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in ('camera_app/Makefile', 'include/apcam/targets.mk',
                     'tools/build.mk', 'tools/write_build_version.py', 'tools/patch_source.py'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, path)
        self.source = self.root / 'camera_app/src/recording/binlog.cpp'
        self.source.parent.mkdir(parents=True)
        self.source.write_text('#include "binlog_version.h"\n'
                               'const char *identity = CA_FIRMWARE_VERSION;\n')
        self.target = self.root / 'camera_app/src/recording/binlog.o'
        self.env = {key: value for key, value in os.environ.items()
                    if key not in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL')}

    def make(self, *args, success=True):
        result = subprocess.run(['make', '-j10', '-C', str(self.root / 'camera_app'),
                                 'src/recording/binlog.o', 'CROSS_COMPILE=',
                                 'MT11_GIT_HASH=abcdef', *args], env=self.env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode == 0, success, result.stdout)
        return result.stdout

    def test_first_build_and_changed_identity_rebuild_its_consumer(self):
        output = self.make('MT11_VERSION=v1.0')
        self.assertIn('Compiling binlog.cpp', output)
        first = self.target.stat().st_mtime_ns
        self.assertNotIn('Compiling', self.make('MT11_VERSION=v1.0'))
        self.assertEqual(first, self.target.stat().st_mtime_ns)
        # Keep the checked-in source older than the object, and remove .d to
        # verify the explicit generated-header dependency rather than luck.
        self.target.with_suffix('.d').unlink()
        self.assertIn('Compiling', self.make('MT11_VERSION=v1.1'))
        self.assertGreater(self.target.stat().st_mtime_ns, first)
        self.assertIn(b'v1.1', self.target.read_bytes())

    def test_verbose_commands_and_quiet_diagnostics(self):
        self.source.write_text('#warning build diagnostic remains visible\nint value;\n')
        output = self.make('CFLAGS=-O0')
        self.assertIn('Compiling binlog.cpp', output)
        self.assertIn('warning: #warning build diagnostic remains visible', output)
        self.assertNotIn('g++ -MMD', output)
        self.target.unlink()
        verbose = self.make('CFLAGS=-O0', 'VERBOSE=1')
        self.assertIn('g++ -MMD', verbose)
        self.assertIn(' -c -o src/recording/binlog.o', verbose)

    def test_failed_compiler_does_not_leave_an_up_to_date_object(self):
        compiler = self.root / 'bad-compiler'
        compiler.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
Path(sys.argv[sys.argv.index('-o') + 1]).write_text('partial object')
print('deliberate compiler failure', file=sys.stderr)
sys.exit(17)
''')
        compiler.chmod(0o755)
        output = self.make('CXX=' + str(compiler), success=False)
        self.assertIn('deliberate compiler failure', output)
        self.assertFalse(self.target.exists())
        self.make()
        self.assertTrue(self.target.exists())

    def test_thermal_submake_shares_the_outer_job_limit(self):
        self.assertGreater(self.thermal_submake(10), 1)

    def test_sitl_tracker_objects_are_owned_by_each_variant(self):
        for name in ('src/media/tracking_dlib.cpp', 'include/camera_app/image_tracker.h'):
            path = self.root/'camera_app'/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('int tracker_fixture;\n' if name.endswith('.cpp') else '')
        dlib = self.root/'custom-deps/dlib'
        header = dlib/'dlib/image_processing/correlation_tracker.h'
        header.parent.mkdir(parents=True)
        header.touch()
        processes = []
        for backend in ('mt11','a8','zr10','z1mini'):
            objdir = 'build/'+backend+'-sitl'
            target = objdir+'/tracking_dlib.o'
            processes.append((target, subprocess.Popen(
                ['make', '-C', str(self.root/'camera_app'), target,
                 'CAMERA_BACKEND='+backend, 'SITL_OBJDIR='+objdir, 'DLIB_ROOT='+str(dlib)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)))
        for target, process in processes:
            output = process.communicate(timeout=15)[0]
            self.assertEqual(process.returncode, 0, output)
            self.assertTrue((self.root/'camera_app'/target).is_file())
        self.assertFalse((self.root/'camera_app/build/host/tracking_dlib.o').exists())

    def test_serial_parent_keeps_thermal_submake_serial(self):
        self.assertEqual(self.thermal_submake(1), 1)

    def test_thermal_download_recovers_from_connection_reset(self):
        payload = b'complete source fixture after connection reset'

        class Handler(http.server.BaseHTTPRequestHandler):
            requests = 0

            def do_GET(self):
                Handler.requests += 1
                self.send_response(200)
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                if Handler.requests == 1:
                    self.wfile.write(payload[:3])
                    self.wfile.flush()
                    self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                               struct.pack('ii', 1, 0))
                    self.close_connection = True
                else:
                    self.wfile.write(payload)

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        root = self.root / 'build/deps/thermal-codecs'
        source = root / 'ffmpeg-8.0.1'
        source.mkdir(parents=True)
        (source / 'COPYING.LGPLv2.1').write_text('fixture')
        configure = source / 'configure'
        configure.write_text('''#!/usr/bin/env python3
from pathlib import Path
import sys
prefix = next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('--prefix='))
Path('Makefile').write_text('.PHONY: all install\\nall:\\ninstall:\\n\\tmkdir -p ' + prefix + '/lib\\n\\ttouch ' + prefix + '/lib/libavcodec.a ' + prefix + '/lib/libavutil.a\\n')
''')
        configure.chmod(0o755)
        script = (ROOT / 'tools/build_thermal_codecs.sh').read_text()
        script = script.replace('05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41',
                                hashlib.sha256(payload).hexdigest())
        script = script.replace('https://ffmpeg.org/releases/ffmpeg-$version.tar.xz',
                                f'http://127.0.0.1:{server.server_port}/ffmpeg.tar.xz')
        target_script = self.root / 'tools/build_thermal_codecs.sh'
        target_script.write_text(script)
        result = subprocess.run(['sh', str(target_script)], env=self.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(Handler.requests, 2)
        archive = root / 'downloads/ffmpeg-8.0.1.tar.xz'
        self.assertEqual(archive.read_bytes(), payload)
        self.assertFalse(archive.with_suffix('.xz.tmp').exists())
        self.assertTrue((root / 'host/lib/libavcodec.a').is_file())

    def thermal_submake(self, jobs):
        # Eight outer workers plus four nested workers must share ten slots.
        root = self.root / 'build/deps/thermal-codecs'
        source = root / 'ffmpeg-8.0.1'
        source.mkdir(parents=True)
        (source / 'COPYING.LGPLv2.1').write_text('fixture')
        archive = root / 'downloads/ffmpeg-8.0.1.tar.xz'
        archive.parent.mkdir()
        archive.write_bytes(b'cached source fixture')
        script = (ROOT / 'tools/build_thermal_codecs.sh').read_text()
        script = script.replace('05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41',
                                hashlib.sha256(archive.read_bytes()).hexdigest())
        target_script = self.root / 'tools/build_thermal_codecs.sh'
        target_script.write_text(script)
        target_script.chmod(0o755)
        state = self.root / 'workers.json'
        worker = self.root / 'worker.py'
        worker.write_text(f'''import fcntl, json, pathlib, time
path = pathlib.Path({str(state)!r})
def update(delta):
    with path.open('a+') as output:
        fcntl.flock(output, fcntl.LOCK_EX)
        output.seek(0)
        state = json.loads(output.read() or '{{"active": 0, "peak": 0}}')
        state['active'] += delta
        state['peak'] = max(state['peak'], state['active'])
        output.seek(0); output.truncate(); json.dump(state, output)
update(1)
time.sleep(0.3)
update(-1)
''')
        configure = source / 'configure'
        configure.write_text(f'''#!/usr/bin/env python3
from pathlib import Path
import sys
prefix = next(arg.split('=', 1)[1] for arg in sys.argv if arg.startswith('--prefix='))
Path('Makefile').write_text('.PHONY: all install a b c d\\nall: a b c d\\na b c d:\\n\\tpython3 {worker}\\ninstall:\\n\\tmkdir -p ' + prefix + '/lib\\n\\ttouch ' + prefix + '/lib/libavcodec.a ' + prefix + '/lib/libavutil.a\\n')
''')
        configure.chmod(0o755)
        (self.root / 'Makefile').write_text(f'''.PHONY: all codec $(addprefix worker-,1 2 3 4 5 6 7 8)
all: codec $(addprefix worker-,1 2 3 4 5 6 7 8)
codec:
\t$(MAKE) -C camera_app {root}/host/lib/libavcodec.a
$(addprefix worker-,1 2 3 4 5 6 7 8):
\tpython3 {worker}
''')
        result = subprocess.run(['make', '-j' + str(jobs)], cwd=self.root, env=self.env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertNotIn('jobserver unavailable', result.stdout)
        self.assertNotIn('forced in submake', result.stdout)
        workers = json.loads(state.read_text())
        self.assertEqual(workers['active'], 0)
        self.assertLessEqual(workers['peak'], jobs)
        return workers['peak']

    def test_failed_patch_preserves_previous_complete_output(self):
        source, patch, output = [self.root / name for name in ('source', 'patch', 'output')]
        source.write_text('first\nsecond\n')
        output.write_text('previous complete source\n')
        patch.write_text('--- a\n+++ b\n@@ -1 +1 @@\n-first\n+changed\n'
                         '@@ -4 +4 @@\n-absent\n+replacement\n')
        result = subprocess.run(['python3', str(ROOT / 'tools/patch_source.py'),
                                 str(source), str(patch), str(output)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertEqual(output.read_text(), 'previous complete source\n')
        self.assertFalse(list(self.root.glob('.patch-*')))


if __name__ == '__main__':
    unittest.main()
