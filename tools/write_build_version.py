#!/usr/bin/env python3
"""Generate firmware build identity, updating the header only when values change."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[1]
version = os.environ.get('MT11_VERSION') or 'unknown'
git_hash = os.environ.get('MT11_GIT_HASH') or 'unknown'
try:
    dirty = subprocess.run(['git', 'diff', '--quiet', 'HEAD', '--'], cwd=repo,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 1
except OSError:
    dirty = False
if dirty:
    git_hash += '-dirty'
# SIYI/UniGCS camera versions carry three numeric bytes, not a string.
# Unknown or unrepresentable versions must not wrap or impersonate stock FW.
numeric = (0, 0, 0)
match = re.fullmatch(r'v?([0-9]+)\.([0-9]+)(?:\.([0-9]+))?', version)
if match:
    parts = tuple(int(part or 0) for part in match.groups())
    if all(part <= 255 for part in parts):
        numeric = parts
text = ('/* Generated at build time; do not edit. */\n'
        '#define CA_FIRMWARE_VERSION ' + json.dumps(version) + '\n'
        '#define CA_FIRMWARE_GIT_HASH ' + json.dumps(git_hash) + '\n'
        '#define CA_FIRMWARE_VERSION_MAJOR ' + str(numeric[0]) + '\n'
        '#define CA_FIRMWARE_VERSION_MINOR ' + str(numeric[1]) + '\n'
        '#define CA_FIRMWARE_VERSION_PATCH ' + str(numeric[2]) + '\n')
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text() != text:
    with tempfile.NamedTemporaryFile(mode='w', dir=output.parent, delete=False) as temporary:
        temporary.write(text)
    os.replace(temporary.name, output)
