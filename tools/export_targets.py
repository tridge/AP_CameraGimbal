#!/usr/bin/env python3
"""Export target properties to JSON using the host C++ compiler."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HEADERS = ROOT / 'include/apcam'


def export(cc):
    registry = re.findall(r'APCAM_TARGET == (APCAM_TARGET_\w+)\n#include "(target_\w+\.h)"',
                          (HEADERS / 'target.h').read_text())
    targets = {}
    with tempfile.TemporaryDirectory(prefix='apcam-targets-') as temp:
        source = Path(temp) / 'export.cpp'
        binary = Path(temp) / ('export.exe' if os.name == 'nt' or 'cygwin' in os.sys.platform else 'export')
        for target_id, header in registry:
            properties = re.findall(r'^#define (APCAM_\w+) (.+)$', (HEADERS / header).read_text(), re.M)
            lines = ['#include <stdio.h>', '#include "apcam/target.h"',
                     'static void string(const char *s) { putchar(34); for (; *s; s++) { '
                     'if (*s == 34 || *s == 92) putchar(92); putchar(*s); } putchar(34); }',
                     'int main(void) { puts("{");']
            for i, (name, value) in enumerate(properties):
                key = name.removeprefix('APCAM_').lower()
                lines.append('printf(' + json.dumps((',' if i else '') + '"' + key + '":') + ');')
                if value.startswith('"'):
                    lines.append(f'string({name});')
                elif value.startswith('{'):
                    lines.append('{ double v[] = ' + name + '; putchar(91); '
                                 'for (unsigned j=0; j<sizeof(v)/sizeof(v[0]); j++) '
                                 'printf("%s%.9g", j ? "," : "", v[j]); putchar(93); }')
                else:
                    lines.append(f'printf("%.9g", (double)({name}));')
            lines.append('puts("}"); return 0; }')
            source.write_text('\n'.join(lines))
            subprocess.run(shlex.split(cc) + ['-std=gnu++17', '-I' + str(ROOT / 'include'),
                           '-DAPCAM_TARGET=' + target_id, str(source), '-o', str(binary)], check=True)
            data = json.loads(subprocess.check_output([str(binary)], text=True))
            data['target_id'] = target_id
            targets[data['name']] = data
    return targets


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('HOST_CXX', 'c++'))
    parser.add_argument('--output', type=Path, default=ROOT / 'build/targets/targets.json')
    args = parser.parse_args()
    data = json.dumps(export(args.cc), indent=2, sort_keys=True) + '\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != data:
        args.output.write_text(data)


if __name__ == '__main__':
    main()
