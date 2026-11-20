#!/usr/bin/env python3
"""C++ runtime flags and the developer web installation contract."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CppBuildTest(unittest.TestCase):
    def test_user_flags_preserve_required_policy(self):
        user_flags = '-O0 -DUSER_CXX_FLAG=1 -std=c++11 -fexceptions -frtti'
        for component, target in [('camera_app', 'tests/test_protocol'),
                                  ('web', 'tests/test_string_buffer')]:
            for command_line in (False, True):
                with self.subTest(component=component, command_line=command_line):
                    env = os.environ.copy()
                    env.pop('MAKEFLAGS', None)
                    env.pop('MFLAGS', None)
                    args = ['make', '-nB', '-C', str(ROOT / component), target]
                    if command_line:
                        args += ['CXXFLAGS=' + user_flags, 'CFLAGS=-O1']
                    else:
                        env.update(CXXFLAGS=user_flags, CFLAGS='-O1')
                    output = subprocess.check_output(args, text=True, env=env)
                    line = next(line for line in output.splitlines()
                                if '-o ' + target in line)
                    tokens = shlex.split(line)
                    self.assertIn('-DUSER_CXX_FLAG=1', tokens)
                    for group, expected in [(('-std=',), '-std=gnu++17'),
                                            (('-fexceptions', '-fno-exceptions'), '-fno-exceptions'),
                                            (('-frtti', '-fno-rtti'), '-fno-rtti')]:
                        self.assertEqual([t for t in tokens if t.startswith(group)][-1], expected)
                    if component == 'web':
                        self.assertIn('-Wformat-security', tokens)
                        self.assertIn('-Werror', tokens)

    def test_install_web_deploys_assets_before_restart(self):
        real_rsync = shutil.which('rsync')
        self.assertIsNotNone(real_rsync, 'Install rsync (tools/install_build_environment.py)')
        with tempfile.TemporaryDirectory(prefix='apcam-web-install-') as directory:
            fixture = Path(directory)
            for name in ('camera_app', 'web', 'include/apcam', 'tools', 'remote/bin'):
                (fixture / name).mkdir(parents=True)
            shutil.copy(ROOT / 'camera_app/Makefile', fixture / 'camera_app/Makefile')
            shutil.copy(ROOT / 'tools/build.mk', fixture / 'tools/build.mk')
            shutil.copy(ROOT / 'include/apcam/targets.mk', fixture / 'include/apcam/targets.mk')
            shutil.copytree(ROOT / 'web/webroot', fixture / 'web/webroot')
            (fixture / 'web/mt11-web').write_text('new web binary')
            remote = fixture / 'remote'
            (remote / 'webroot').mkdir()
            (remote / 'webroot/stale.js').write_text('old asset')
            # Run rsync locally with the real production recipe/options. The
            # SSH double only checks readiness; it never executes remote text.
            rsync = fixture / 'rsync'
            rsync.write_text('''#!/usr/bin/env python3
import os, subprocess, sys
args = sys.argv[1:]
if os.environ.get('FAIL_ASSETS') and args[-1].endswith('/webroot/'):
    sys.exit(17)
assert args[-1].startswith('fixture:')
args[-1] = args[-1].split(':', 1)[1]
sys.exit(subprocess.call([os.environ['REAL_RSYNC'], *args]))
''')
            ssh = fixture / 'ssh'
            ssh.write_text('''#!/usr/bin/env python3
import os
from pathlib import Path
remote = Path(os.environ['TEST_REMOTE'])
source = Path(os.environ['TEST_SOURCE'])
assert (remote / 'bin/mt11-web.new').read_text() == 'new web binary'
assert not (remote / 'webroot/stale.js').exists()
for asset in source.rglob('*'):
    installed = remote / 'webroot' / asset.relative_to(source)
    if asset.is_dir():
        assert installed.is_dir()
        assert installed.stat().st_mode & 0o777 == 0o755
    else:
        assert installed.read_bytes() == asset.read_bytes()
        assert installed.stat().st_mode & 0o777 == 0o644
assert (remote / 'webroot').stat().st_mode & 0o777 == 0o755
(remote / 'restart').touch()
''')
            rsync.chmod(0o755)
            ssh.chmod(0o755)
            env = os.environ.copy()
            env.update(REAL_RSYNC=real_rsync, TEST_REMOTE=str(remote),
                       TEST_SOURCE=str(fixture / 'web/webroot'))
            command = ['make', '-C', str(fixture / 'camera_app'), 'install-web',
                       'MAKE=/bin/true', 'SSH_HOST=fixture', 'SSH=' + str(ssh),
                       'RSYNC=' + str(rsync), 'REMOTE_WEB=' + str(remote / 'bin/mt11-web'),
                       'REMOTE_WEBROOT=' + str(remote / 'webroot')]
            failed = subprocess.run(command, env=env | {'FAIL_ASSETS': '1'},
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertNotEqual(failed.returncode, 0)
            self.assertFalse((remote / 'restart').exists())
            self.assertFalse((remote / 'bin/mt11-web.new').exists())
            result = subprocess.run(command, env=env, stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertTrue((remote / 'restart').exists())


if __name__ == '__main__':
    unittest.main()
