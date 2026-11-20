#!/usr/bin/env python3
"""Survey producer and web reader must honour the camera's writable TMP."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SurveyRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='survey-runtime-')
        root = Path(cls.temporary.name)
        source = root / 'path.cpp'
        source.write_text('''#include "apcam/survey_runtime.h"
int main() {
    char path[4096];
    if (!apcam_survey_status_path("/app/camera.ini", path, sizeof(path))) return 1;
    puts(path);
}
''')
        cls.binary = root / 'path'
        subprocess.run(['c++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'include'), str(source), '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def path(self, **variables):
        environment = {k: v for k, v in os.environ.items() if k not in ('TMPDIR', 'TMP', 'TEMP')}
        environment.update(variables)
        return Path(subprocess.check_output([str(self.binary)], env=environment, text=True).strip())

    def test_mt11_ram_directory(self):
        default = self.path()
        path = self.path(TMP='/run')
        self.assertEqual(path.parent, Path('/run'))
        self.assertEqual(path.name, default.name)

    def test_tmpdir_overrides_camera_tmp(self):
        self.assertEqual(self.path(TMP='/run', TMPDIR='/custom').parent, Path('/custom'))

    def test_empty_tmpdir_uses_camera_tmp(self):
        self.assertEqual(self.path(TMP='/run', TMPDIR='').parent, Path('/run'))

    def test_default_without_environment(self):
        self.assertEqual(self.path(TMP='', TMPDIR='').parent, Path('/tmp'))


if __name__ == '__main__':
    unittest.main()
