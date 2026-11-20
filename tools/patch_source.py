#!/usr/bin/env python3
"""Patch a source in a temporary file, publishing only a complete result."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--minimp4', action='store_true')
parser.add_argument('source', type=Path)
parser.add_argument('patch', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=args.output.parent, prefix='.patch-') as directory:
    temporary = Path(directory) / args.output.name
    temporary.write_bytes(args.source.read_bytes().replace(b'\r\n', b'\n'))
    command = ['patch', '-t', '-N', str(temporary)]
    if os.environ.get('VERBOSE') != '1':
        command.append('-s')
    with args.patch.open('rb') as patch:
        subprocess.run(command, stdin=patch, check=True)
    if args.minimp4:
        content, count = re.subn(r'(?m)^#define MP4D_TFDT_SUPPORT\s+0',
                                '#define MP4D_TFDT_SUPPORT 1', temporary.read_text())
        if count != 1:
            raise SystemExit('minimp4: cannot enable MP4D_TFDT_SUPPORT')
        temporary.write_text(content)
    os.replace(temporary, args.output)
