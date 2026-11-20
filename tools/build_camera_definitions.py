#!/usr/bin/env python3
"""Export each camera's built-in definition XML using the host C++ compiler."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def build(output, cc='c++'):
    output.mkdir(parents=True, exist_ok=True)
    targets = re.findall(r'APCAM_TARGET == (APCAM_TARGET_\w+)\n#include "(target_\w+\.h)"',
                         (ROOT / 'include/apcam/target.h').read_text())
    with tempfile.TemporaryDirectory(prefix='apcam-definitions-') as work:
        source = Path(work) / 'export.cpp'
        source.write_text('''#include "camera_app/camera_definition.h"
#include <stdlib.h>
int main(void) {
    size_t length;
    char *xml = ca_camera_definition(&length);
    if (!xml) return 1;
    int failed = fwrite(xml, 1, length, stdout) != length;
    free(xml);
    return failed || fflush(stdout) != 0;
}
''')
        binary = Path(work) / 'export'
        for target, header in targets:
            subprocess.run(shlex.split(cc) + [
                '-O2', '-Wall', '-Wextra', '-Werror', '-std=gnu++17', '-Wno-missing-field-initializers',
                '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'camera_app/include'),
                '-DAPCAM_TARGET=' + target, str(source),
                str(ROOT / 'camera_app/src/protocol/camera_definition.cpp'),
                str(ROOT / 'camera_app/src/config.cpp'), '-lm', '-o', str(binary)], check=True)
            xml = subprocess.check_output([str(binary)])
            path = output / (header.removeprefix('target_').removesuffix('.h') + '.xml')
            path.write_bytes(xml)
            print(path)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/camera-definitions')
    parser.add_argument('--cc', default=os.environ.get('HOST_CXX', 'c++'))
    args = parser.parse_args()
    build(args.output, args.cc)
