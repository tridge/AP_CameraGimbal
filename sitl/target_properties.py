"""Firmware target properties exported from include/apcam/target_*.h.

Packaged builds include targets.json; source builds refresh it through make.
No compiler or header parser is required in an installed simulator.
"""
import json
import os
from pathlib import Path
import subprocess
import sys


def load_targets():
    override = os.environ.get('APCAM_TARGETS_JSON')
    if override:
        return json.loads(Path(override).read_text())
    if getattr(sys, 'frozen', False):
        base = Path(sys._MEIPASS)
        candidates = (base / 'payload/targets.json', base / 'targets.json')
    else:
        root = Path(__file__).resolve().parents[1]
        path = root / 'build/targets/targets.json'
        dependencies = list((root / 'include/apcam').glob('*.h')) + [root / 'tools/export_targets.py']
        if not path.exists() or any(p.stat().st_mtime > path.stat().st_mtime for p in dependencies):
            subprocess.run([sys.executable, str(root / 'tools/export_targets.py')], check=True)
        candidates = (path,)
    for path in candidates:
        if path.is_file():
            return json.loads(path.read_text())
    raise RuntimeError('Target properties are missing from the SITL package')


TARGETS = load_targets()
# Hardware-only targets (APCAM_HAVE_SITL 0) have no simulator build.
SITL_TARGETS = {name: target for name, target in TARGETS.items() if target.get('have_sitl', 1)}


def transform(target, channel, inverted, vector, *, rates=False, inverse=False):
    """Joint-coordinate affine mapping; inverse uses the orthogonal transpose."""
    prefix = 'gimbal_' + channel + ('_inverted' if inverted else '_upright')
    matrix, offset = target[prefix + '_matrix'], target[prefix + '_offset']
    if inverse:
        value = [v - (0 if rates else o) for v, o in zip(vector, offset)]
        result = [sum(matrix[3*j+i] * value[j] for j in range(3)) for i in range(3)]
    else:
        result = [sum(matrix[3*i+j] * vector[j] for j in range(3)) + (0 if rates else offset[i]) for i in range(3)]
    return result if rates else [(v + 180) % 360 - 180 for v in result]
