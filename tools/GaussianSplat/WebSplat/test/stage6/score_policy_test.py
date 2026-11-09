#!/usr/bin/env python3
"""score_policy.py ledger handling (codex stage 6 round 2, findings 5 and 6): a truncated
report.json, a laptop report without GPU-idle evidence and one with a failed query (null) are
invalid attempts, counted against the allowance, and scoring continues with the valid runs.

  score_policy_test.py STAGE6_DIR [--build ID] RUN_DIR...

STAGE6_DIR: $WEBSPLAT_DATA/stage6 (baselines in tp/, the page dataset in the first laptop run).
RUN_DIRs: valid runs to place after the broken attempts (e.g. a scored series). Broken attempts
are made in STAGE6_DIR/policy-test/ (removed afterwards)."""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
ap = argparse.ArgumentParser()
ap.add_argument('stage6', type=Path)
ap.add_argument('runs', type=Path, nargs='+')
ap.add_argument('--build')
a = ap.parse_args()
d = a.stage6
work = d / 'policy-test'
shutil.rmtree(work, ignore_errors=True)
src = a.runs[0]
broken = []
for name, edit in [('truncated', lambda t: t[: len(t) // 2]),
                   ('no-gpu-evidence', lambda t: json.dumps({k: v for k, v in json.loads(t).items() if k != 'gpuBefore'})),
                   ('gpu-query-failed', lambda t: json.dumps({**json.loads(t), 'gpuBefore': None}))]:
    run = work / name
    run.mkdir(parents=True)
    for part in ('train', 'prepare', 'ply_render.npz'):
        (run / part).symlink_to((src / part).resolve())
    (run / 'report.json').write_text(edit((src / 'report.json').read_text()))
    broken.append(run)
base = d / 'runs' / 'laptop-2026-11-08T14-35-09'
cmd = [sys.executable, str(here / 'score_policy.py'), str(base / 'g1view'), str(d / 'tp/page_b1/metrics_page.json'),
       str(d / 'tp/page_b2/metrics_page.json'), '--prepare', str(base / 'prepare'),
       *(['--build', a.build] if a.build else []), '--runs', *map(str, broken + a.runs)]
r = subprocess.run(cmd, capture_output=True, text=True)
shutil.rmtree(work, ignore_errors=True)
out = r.stdout
failures = 0


def check(name, ok, detail=''):
    global failures
    print(('ok   ' if ok else 'FAIL ') + name + ('' if ok else ': ' + detail))
    failures += not ok


check('no traceback', 'Traceback' not in r.stderr, r.stderr[-300:])
check('truncated report is an invalid attempt', 'invalid truncated' in out and 'unreadable attempt' in out, out[:400])
check('missing GPU evidence is invalid', 'invalid no-gpu-evidence' in out and 'no evidence the laptop GPU' in out, out[:400])
check('failed GPU query is invalid', 'invalid gpu-query-failed' in out, out[:400])
n = len(a.runs)
check('the valid runs are still scored', ('%d attempts, %d valid' % (n + 3, n)) in out, out[-600:])
print('score_policy ledger: ' + ('PASS' if not failures else '%d check(s) failed' % failures))
sys.exit(1 if failures else 0)
