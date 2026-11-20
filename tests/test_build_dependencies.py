#!/usr/bin/env python3
"""Check source bootstrap ordering and recursive Make's exported roots."""
import os
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildDependenciesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='apcam-dependencies-')
        self.addCleanup(temporary.cleanup)
        self.repo = Path(temporary.name)
        subprocess.run(['git', 'init', '-q', str(self.repo)], check=True)
        subprocess.run(['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                        'commit', '-q', '--allow-empty', '-m', 'Fixture'],
                       cwd=self.repo, check=True)
        shutil.copyfile(ROOT / 'Makefile', self.repo / 'Makefile')
        (self.repo / 'build').mkdir()
        (self.repo / 'build/environment.mk').write_text('# Test environment\n')
        # Stand in for downloaded sources; exercise the real Make entry points.
        tools = self.repo / 'tools'
        tools.mkdir()
        shutil.copyfile(ROOT / 'tools/build.mk', tools / 'build.mk')
        shutil.copyfile(ROOT / 'tools/zt30_stream.mk', tools / 'zt30_stream.mk')
        bootstrap = tools / 'bootstrap_dependencies.sh'
        bootstrap.write_text('''#!/bin/sh
set -eu
mkdir -p "$1/ss928-mpp" "$1/minimp4" "$1/dlib/dlib/image_processing"
touch "$1/minimp4/minimp4.h"
touch "$1/dlib/dlib/image_processing/correlation_tracker.h"
echo bootstrap >> calls
''')
        bootstrap.chmod(0o755)
        for path in ['modules/mavlink/message_definitions/v1.0/all.xml',
                     'modules/mavlink/pymavlink/tools/mavgen.py']:
            file = self.repo / path
            file.parent.mkdir(parents=True, exist_ok=True)
            file.touch()
        for component in ['camera_app', 'web']:
            directory = self.repo / component
            directory.mkdir()
            (directory / 'Makefile').write_text('''.DEFAULT_GOAL := all
.PHONY: generated
generated:
\t@test -f "$(MINIMP4_ROOT)/minimp4.h"
\t@test -f "$(DLIB_ROOT)/dlib/image_processing/correlation_tracker.h"
all:
	@test -f "$(MINIMP4_ROOT)/minimp4.h"
	@test -f "$(DLIB_ROOT)/dlib/image_processing/correlation_tracker.h"
	@echo compile >> ../calls
''')
        # Like build_release.py, this child receives exported dependency roots.
        (self.repo / 'recursive.mk').write_text('''include Makefile
.PHONY: recursive
recursive:
	$(MAKE) all
''')

    def make(self, *args, extra_env=None, expect_success=True):
        env = os.environ.copy()
        for key in ['SS928_MPP_ROOT', 'MINIMP4_ROOT', 'DLIB_ROOT', 'DEPS_ROOT',
                    'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL']:
            env.pop(key, None)
        env.update(extra_env or {})
        result = subprocess.run(['make', '-j10', '-f', 'recursive.mk', *args,
                                 'MT11_VERSION=v1.0', 'MT11_GIT_HASH=123456',
                                 'ZR10_BUILD_HASH=123456', 'Z1MINI_BUILD_HASH=123456'],
                                cwd=self.repo, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not expect_success:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            return result.stdout
        self.assertEqual(result.returncode, 0, result.stdout)
        calls = self.repo / 'calls'
        return calls.read_text().splitlines() if calls.exists() else []

    def test_plain_make_bootstraps_before_compiling(self):
        self.assertEqual(self.make(), ['bootstrap', 'compile', 'compile'])

    def test_hardware_builds_require_environment_before_any_work(self):
        (self.repo / 'build/environment.mk').unlink()
        for goal in [None, 'all', 'release', 'mt11_package', 'a8_package',
                     'zr10_firmware', 'z1mini_native_package', 'recursive']:
            with self.subTest(goal=goal):
                output = self.make(*([goal] if goal else []), expect_success=False)
                self.assertIn('python3 tools/install_build_environment.py', output)
                self.assertFalse((self.repo / 'calls').exists())

    def test_source_setup_does_not_require_hardware_environment(self):
        (self.repo / 'build/environment.mk').unlink()
        self.assertEqual(self.make('build-dependencies'), ['bootstrap'])

    def test_recursive_make_bootstraps_inherited_default_paths(self):
        self.assertEqual(self.make('recursive'), ['bootstrap', 'compile', 'compile'])

    def test_custom_roots_are_not_downloaded(self):
        self.assertEqual(self.make('build-dependencies', extra_env={
            'SS928_MPP_ROOT': '/custom/mpp', 'MINIMP4_ROOT': '/custom/minimp4'}), [])

    def test_one_default_root_still_bootstraps(self):
        self.assertEqual(self.make('build-dependencies', 'SS928_MPP_ROOT=/custom/mpp'),
                         ['bootstrap'])

    def test_custom_dependency_directory_survives_recursion(self):
        self.assertEqual(self.make('recursive', 'DEPS_ROOT=custom-deps'),
                         ['bootstrap', 'compile', 'compile'])
        self.assertTrue((self.repo / 'custom-deps/minimp4/minimp4.h').is_file())
        self.assertTrue((self.repo / 'custom-deps/dlib/dlib/image_processing/correlation_tracker.h').is_file())

    def prepare_sitl(self, fail=False):
        # The real top-level graph runs four backend submakes. Delay the shared
        # generator to expose any backend that starts before preparation ends.
        for name in ('sitl', 'a8-sitl', 'zr10-sitl', 'z1mini-sitl'):
            directory = self.repo / 'build' / name
            directory.mkdir()
            for filename in ('rgb.h264', 'thermal.h264', 'main.h264', 'sub.h264', 'photo.jpg'):
                (directory / filename).touch()
        (self.repo / 'sitl').mkdir()
        (self.repo / 'sitl/prepare_runtime.py').write_text('')
        (self.repo / 'tools/export_targets.py').write_text('')
        for component in ('camera_app', 'web'):
            (self.repo / component / 'Makefile').write_text(""".DEFAULT_GOAL := all
.PHONY: generated sitl all
generated:
\t@test -f "$(MINIMP4_ROOT)/minimp4.h"
\t@echo generating-%s >> ../calls
\t@sleep 0.05
\t@%s
\t@touch ../%s-ready
sitl all:
\t@test -f ../camera_app-ready -a -f ../web-ready
\t@echo compile-%s >> ../calls
""" % (component, 'false' if fail and component == 'camera_app' else 'true',
       component, component))

    def test_parallel_camera_goals_generate_shared_inputs_once(self):
        self.prepare_sitl()
        calls = self.make('dependencies', 'sitl', 'a8_sitl', 'zr10_sitl', 'z1mini_sitl')
        self.assertEqual(calls[:3], ['bootstrap', 'generating-camera_app', 'generating-web'])
        self.assertEqual(calls.count('bootstrap'), 1)
        self.assertEqual(calls.count('generating-camera_app'), 1)
        self.assertEqual(calls.count('generating-web'), 1)
        self.assertEqual(calls.count('compile-camera_app'), 4)
        self.assertEqual(calls.count('compile-web'), 4)

    def test_shared_generation_failure_stops_every_backend(self):
        self.prepare_sitl(fail=True)
        self.make('sitl', 'a8_sitl', 'zr10_sitl', 'z1mini_sitl', expect_success=False)
        self.assertEqual((self.repo / 'calls').read_text().splitlines(),
                         ['bootstrap', 'generating-camera_app'])

    def test_sitl_clean_preserves_build_environment_and_dependency_cache(self):
        cache = self.repo / 'build/deps/cached-source'
        cache.parent.mkdir()
        cache.touch()
        for name in ('sitl', 'a8-sitl', 'zr10-sitl', 'z1mini-sitl'):
            (self.repo / 'build' / name).mkdir()
            (self.repo / 'camera_app/build' / name).mkdir(parents=True)
        self.make('sitl-clean')
        self.assertTrue(cache.exists())
        self.assertTrue((self.repo / 'build/environment.mk').exists())
        for name in ('sitl', 'a8-sitl', 'zr10-sitl', 'z1mini-sitl'):
            self.assertFalse((self.repo / 'build' / name).exists())
            self.assertFalse((self.repo / 'camera_app/build' / name).exists())


class CachedRTSPSourcesTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='apcam-rtsp-dependency-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.mpp = self.root / 'deps/ss928-mpp'
        self.mpp.mkdir(parents=True)
        self.files = ['net/EventLoop.h', 'net/EventLoop.cpp', 'xop/RtspServer.h']
        for name in self.files:
            path = self.mpp / 'src/rtspserver/src' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('// pinned fixture\n')
        self.git('init', '-q')
        self.git('add', '.')
        self.git('-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                 'commit', '-qm', 'Pinned SDK fixture')
        revision = self.git('rev-parse', 'HEAD').strip()
        header = self.root / 'deps/minimp4/minimp4.h'
        header.parent.mkdir()
        header.write_text('// cached minimp4 fixture\n')
        script = (ROOT / 'tools/bootstrap_dependencies.sh').read_text()
        script = re.sub(r'^mpp_commit=.*$', 'mpp_commit=' + revision, script, flags=re.M)
        script = re.sub(r'^minimp4_sha256=.*$', 'minimp4_sha256=' +
                        hashlib.sha256(header.read_bytes()).hexdigest(), script, flags=re.M)
        self.script = self.root / 'bootstrap.sh'
        self.script.write_text(script)
        shutil.copyfile(ROOT / 'tools/bootstrap_dlib.sh', self.root / 'bootstrap_dlib.sh')
        dlib = self.root / 'deps/dlib'
        tracker = dlib / 'dlib/image_processing/correlation_tracker.h'
        tracker.parent.mkdir(parents=True)
        tracker.write_text('// cached dlib fixture\n')
        (dlib / '.apcam-version').write_text('20.0.1\n')

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.mpp), *args], text=True)

    def bootstrap(self):
        result = subprocess.run(['sh', str(self.script), str(self.root / 'deps')],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def test_missing_header_is_restored_without_overwriting_local_edits(self):
        source = self.mpp / 'src/rtspserver/src'
        (source / 'net/EventLoop.h').unlink()
        (source / 'net/EventLoop.cpp').write_text('// local change\n')
        self.assertIn('Restoring missing RTSP source', self.bootstrap())
        self.assertEqual((source / 'net/EventLoop.h').read_text(), '// pinned fixture\n')
        self.assertEqual((source / 'net/EventLoop.cpp').read_text(), '// local change\n')
        self.assertNotIn('Restoring', self.bootstrap())

    def test_missing_source_tree_is_restored(self):
        source = self.mpp / 'src/rtspserver/src'
        shutil.rmtree(source)
        self.bootstrap()
        for name in self.files:
            self.assertTrue((source / name).is_file())

    def test_sparse_checkout_does_not_hide_required_sources(self):
        self.git('sparse-checkout', 'set', '--cone', 'include')
        source = self.mpp / 'src/rtspserver/src'
        self.assertFalse(source.exists())
        self.bootstrap()
        for name in self.files:
            self.assertTrue((source / name).is_file())

    def download_with_failures(self, stage, failures):
        """Use real local Git with injected transport errors; never use the network."""
        upstream = self.root / 'upstream'
        self.mpp.rename(upstream)
        # The remote's tip is deliberately newer than the pinned revision.
        subprocess.run(['git', '-C', str(upstream), '-c', 'user.name=Test',
                        '-c', 'user.email=test@example.com', 'commit', '-qm',
                        'New upstream tip', '--allow-empty'], check=True)
        script = self.script.read_text()
        script = re.sub(r'^mpp_url=.*$', 'mpp_url=' + str(upstream), script, flags=re.M)
        self.script.write_text(script)
        wrappers = self.root / 'bin'
        wrappers.mkdir()
        self.attempts = self.root / 'attempts'
        wrapper = wrappers / 'git'
        wrapper.write_text(f'''#!{sys.executable}
import os
from pathlib import Path
import sys
args = sys.argv[1:]
if {stage!r} in args:
    counter = Path({str(self.attempts)!r})
    count = int(counter.read_text()) + 1 if counter.exists() else 1
    counter.write_text(str(count))
    if count <= {failures}:
        if {stage!r} == 'clone':
            # Leave debris as a failed or interrupted download might do.
            (Path(args[-1]) / '.git').mkdir(parents=True)
        print('simulated connection reset', file=sys.stderr)
        sys.exit(128)
os.execv({shutil.which('git')!r}, ['git'] + args)
''')
        wrapper.chmod(0o755)
        # Do not spend wall-clock time on the production retry delay.
        delay = wrappers / 'sleep'
        delay.write_text('#!/bin/sh\nexit 0\n')
        delay.chmod(0o755)
        return dict(os.environ, PATH=str(wrappers) + os.pathsep + os.environ['PATH'])

    def run_download(self, env):
        return subprocess.run(['sh', str(self.script), str(self.root / 'deps')],
                              env=env, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=10)

    def test_transient_clone_failure_retries_without_publishing_partial_cache(self):
        result = self.run_download(self.download_with_failures('clone', 1))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.attempts.read_text(), '2')
        self.assertFalse(list((self.root / 'deps').glob('.ss928-mpp.*')))
        self.bootstrap()  # The completed checkout is reusable at its pinned revision.

    def test_checkout_blob_fetch_failure_is_retried(self):
        result = self.run_download(self.download_with_failures('checkout', 1))
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(self.attempts.read_text(), '2')
        self.bootstrap()

    def test_permanent_failure_is_bounded_and_leaves_no_cache(self):
        env = self.download_with_failures('checkout', 3)
        result = self.run_download(env)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn('after 3 attempts', result.stdout)
        self.assertEqual(self.attempts.read_text(), '3')
        self.assertFalse(self.mpp.exists())
        self.assertFalse(list((self.root / 'deps').glob('.ss928-mpp.*')))
        # A subsequent invocation can recover when the network comes back.
        recovered = self.run_download(env)
        self.assertEqual(recovered.returncode, 0, recovered.stdout)
        self.bootstrap()



class DlibBootstrapTest(unittest.TestCase):
    def setUp(self):
        import tarfile
        temporary = tempfile.TemporaryDirectory(prefix='apcam-dlib-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        source = self.root / 'source/dlib/image_processing/correlation_tracker.h'
        source.parent.mkdir(parents=True)
        source.write_text('// pinned tracker fixture\n')
        self.archive = self.root / 'dlib.tar.gz'
        with tarfile.open(self.archive, 'w:gz') as archive:
            archive.add(self.root / 'source', arcname='dlib-20.0.1')
        script = (ROOT / 'tools/bootstrap_dlib.sh').read_text()
        script = re.sub(r'^sha=.*$', 'sha=' + hashlib.sha256(self.archive.read_bytes()).hexdigest(), script, flags=re.M)
        self.script = self.root / 'bootstrap.sh'
        self.script.write_text(script)
        self.cache = self.root / 'deps/dlib'
        wrappers = self.root / 'bin'
        wrappers.mkdir()
        curl = wrappers / 'curl'
        curl.write_text(f'''#!{sys.executable}
import os, shutil, sys, time
from pathlib import Path
with open(os.environ['DOWNLOADS'], 'a') as log: log.write('download\\n')
time.sleep(.2)
if os.environ.get('FAIL_DOWNLOAD'): sys.exit(22)
shutil.copyfile(os.environ['ARCHIVE'], sys.argv[-1])
''')
        curl.chmod(0o755)
        self.env = dict(os.environ, PATH=str(wrappers)+os.pathsep+os.environ['PATH'],
                        ARCHIVE=str(self.archive), DOWNLOADS=str(self.root/'downloads'))

    def test_parallel_bootstrap_publishes_once(self):
        processes = [subprocess.Popen(['sh', str(self.script), str(self.cache)], env=self.env,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                     for _ in range(4)]
        for process in processes:
            output = process.communicate(timeout=15)[0]
            self.assertEqual(process.returncode, 0, output)
        self.assertEqual((self.root/'downloads').read_text().splitlines(), ['download'])
        self.assertTrue((self.cache/'dlib/image_processing/correlation_tracker.h').is_file())
        self.assertFalse((self.cache/'source').exists())
        self.assertEqual((self.cache/'.apcam-version').read_text().strip(), '20.0.1')

    def test_failed_download_can_retry_without_partial_cache(self):
        command = ['sh', str(self.script), str(self.cache)]
        failed = subprocess.run(command, env=self.env | {'FAIL_DOWNLOAD': '1'}, capture_output=True)
        self.assertNotEqual(failed.returncode, 0)
        self.assertFalse(self.cache.exists())
        self.assertFalse(list(self.cache.parent.glob('.dlib.*')))
        subprocess.run(command, env=self.env, check=True, capture_output=True)
        self.assertTrue((self.cache/'dlib/image_processing/correlation_tracker.h').is_file())


if __name__ == '__main__':
    unittest.main()
