#!/usr/bin/env python3
"""Score a machine's repeat series under the notes/g1.md repeat policy.

  score_repeats.py DATASET BASE1.json BASE2.json --runs ATTEMPT_DIR... [--baselines PLY...]
  score_repeats.py DATASET BASE1.json BASE2.json --ledger SERIES/ledger.jsonl  (repeat_preflight.sh)

--runs is the machine's complete attempt ledger: every attempt directory of the series, valid or
not, including aborted ones. An attempt without a valid run.json (missing, invalid, or stopped)
counts as invalid; more than MAX_INVALID invalid attempts fails the series. Exactly RUNS distinct
valid attempts are required (distinct attempt ids and PLY contents), each trained for the declared
iterations, with complete artifacts (frozen renders, PLY payload matching its header). Per run:
criteria 1-6 (metrics.py) and 9 (export). Decision: every criterion passes in at least NEED of the
RUNS runs and the median over runs of each statistic is within its bound. --baselines scores extra
train.py PLYs the same way, for comparison only. Exits nonzero unless the decision passes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import numpy as np

here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import metrics  # noqa: E402


RUNS, NEED, MAX_INVALID = 5, 4, 3  # the repeat policy; not options
MAX_PREFLIGHT = 10  # environment preflight failures allowed in a series (series 4 rule)


def ply_problem(path, meta):
    if not path.exists():
        return 'no export.ply'
    data = path.read_bytes()
    end = data.find(b'end_header\n')
    if end < 0:
        return 'PLY has no header'
    lines = data[:end].decode('latin1').split('\n')
    vertex = [ln for ln in lines if ln.startswith('element vertex ')]
    n = int(vertex[0].split()[2]) if vertex else 0
    props = sum(ln.startswith('property float ') for ln in lines)
    if n <= 0 or len(data) != end + len(b'end_header\n') + n * props * 4:
        return 'PLY payload %d bytes for %d x %d floats' % (len(data), n, props)
    apcg = [ln for ln in lines if ln.startswith('comment apcg_splat ')]
    info = json.loads(apcg[0][len('comment apcg_splat '):]) if apcg else {}
    if info.get('kind') != 'thermal' or info.get('temperature_scale') != meta['temperature_scale']:
        return 'PLY apcg_splat metadata'
    return None


def render(ply, dataset, out):
    if not out.exists():
        subprocess.run([sys.executable, str(here / 'render_ply.py'), str(ply), str(dataset), str(out)], check=True,
                       stdout=subprocess.DEVNULL)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dataset', type=Path)
    ap.add_argument('base', type=Path, nargs=2)
    ap.add_argument('--runs', type=Path, nargs='+')
    ap.add_argument('--ledger', type=Path, help='series ledger: attempts and preflight failures')
    ap.add_argument('--baselines', type=Path, nargs='*', default=[])
    a = ap.parse_args()
    preflight_failures = []
    if a.ledger:
        if a.runs:
            sys.exit('give --runs or --ledger, not both')
        entries = [json.loads(line) for line in a.ledger.open() if line.strip()]
        preflight_failures = [e for e in entries if e['kind'] == 'preflight']
        a.runs = [a.ledger.parent / e['run'] for e in entries if e['kind'] == 'attempt']
        listed = {e['run'] for e in entries if e['kind'] == 'attempt'}
        on_disk = {d.name for d in a.ledger.parent.iterdir() if d.is_dir() and d.name.startswith('run')}
        if listed != on_disk:
            sys.exit('ledger attempts %s do not match the attempt directories %s' % (sorted(listed), sorted(on_disk)))
        for e in preflight_failures:
            print('preflight failure at %s: %s' % (e['utc'], json.dumps(e['result'])))
        if not a.runs and not preflight_failures:
            sys.exit('empty ledger')
    elif not a.runs:
        sys.exit('give --runs or --ledger')
    bases = [json.loads(b.read_text()) for b in a.base]
    meta = json.loads((a.dataset / 'g1.json').read_text())
    args = (a.dataset / 'brush_args.txt').read_text().split()
    iters = int(args[args.index('--total-train-iters') + 1])
    rows, invalid, seen = [], [], {}
    if len({r.resolve() for r in a.runs}) != len(a.runs):
        sys.exit('the same attempt directory is listed more than once')
    for run in a.runs:
        try:
            info = json.loads((run / 'run.json').read_text())
        except (OSError, ValueError):
            info = {'valid': False, 'reason': 'no run.json (attempt stopped or incomplete)'}
        done = []
        if (run / 'log.jsonl').exists():
            done = [json.loads(line) for line in (run / 'log.jsonl').open() if '"type":"done"' in line]
        problem = None if info.get('valid') else (info.get('reason') or 'invalid').splitlines()[0][:120]
        if not problem and (not done or done[-1]['iter'] != iters):
            problem = 'trained %s of %d iterations' % (done[-1]['iter'] if done else 'no', iters)
        if not problem:
            problem = ply_problem(run / 'export.ply', meta)
        if problem:
            invalid.append(run)
            print('%s: INVALID attempt (%s)' % (run, problem))
            continue
        digest = hashlib.sha256((run / 'export.ply').read_bytes()).hexdigest()
        for key in filter(None, (info.get('attempt'), digest)):
            if key in seen:
                sys.exit('%s duplicates %s' % (run, seen[key]))
            seen[key] = run
        scored = metrics.score(run / 'eval', a.dataset)
        (run / 'metrics.json').write_text(json.dumps(scored, indent=1))
        crit = metrics.criteria(scored, bases)
        exp = metrics.export_check(run / 'eval', render(run / 'export.ply', a.dataset, run / 'ply_render.npz'), a.dataset)
        crit.append((9, 'export', [('min PSNR dB', exp['min_psnr'], 45.0, '>='),
                                   ('max peak diff C', exp['max_peak_diff_c'], 1.0, '<=')]))
        hot = {x['name']: x['abs_peak_error_c'] for x in scored['views']['hot']}
        rows.append(dict(run=run.name, crit=crit, hot=hot, train_s=done[-1]['elapsedMs'] / 1000,
                         splats=done[-1]['splats'], wall_s=info['wall_s'], peak=info['gpu_mem_peak_mib'],
                         before=info['gpu_mem_before_mib']))
    print('ledger: %d attempts, %d valid, %d invalid (allowed %d); %d preflight failures (allowed %d, not attempts)'
          % (len(a.runs), len(rows), len(invalid), MAX_INVALID, len(preflight_failures), MAX_PREFLIGHT))
    if len(preflight_failures) > MAX_PREFLIGHT:
        print('G1 on this series: ENVIRONMENT FAILURE (%d preflight failures)' % len(preflight_failures))
        sys.exit(1)
    if not rows:
        sys.exit('no valid runs')
    print('\nper run (statistic: value, PASS/FAIL):')
    for r in rows:
        cells = []
        for num, name, stats in r['crit']:
            ok = all(metrics.within(v, b, op) for _, v, b, op in stats)
            cells.append('%d %s %s' % (num, '/'.join('%.2f' % v for _, v, _, _ in stats), 'ok' if ok else 'FAIL'))
        print('%-6s %s | train %.0f s, wall %.0f s, GPU %d->%d MiB, %d splats' % (
            r['run'], '  '.join(cells), r['train_s'], r['wall_s'], r['before'], r['peak'], r['splats']))
    print('\nhot-view absolute peak error (C):')
    for r in rows:
        print('%-6s %s' % (r['run'], ' '.join('%s %5.1f' % (k[:5], v) for k, v in r['hot'].items())))
    for ply in a.baselines:
        out = ply.parent / 'g1_render.npz'
        scored = metrics.score(render(ply, a.dataset, out), a.dataset)
        crit = metrics.criteria(scored, bases)
        fails = [num for num, _, stats in crit if not all(metrics.within(v, b, op) for _, v, b, op in stats)]
        hot = {x['name']: x['abs_peak_error_c'] for x in scored['views']['hot']}
        print('baseline %s: hot peak median %.1f C, fails %s | %s' % (
            ply.parent.name, scored['summary']['hot']['abs_peak_error_median_c'], fails or 'none',
            ' '.join('%s %5.1f' % (k[:5], v) for k, v in hot.items())))
    print('\ndecision (pass in >= %d of %d runs, median within bound):' % (NEED, RUNS))
    ok_all = len(rows) == RUNS and len(invalid) <= MAX_INVALID
    for idx, (num, name, stats) in enumerate(rows[0]['crit']):
        passes = sum(all(metrics.within(v, b, op) for _, v, b, op in r['crit'][idx][2]) for r in rows)
        meds = []
        med_ok = True
        for k, (sname, _, bound, op) in enumerate(stats):
            m = float(np.median([r['crit'][idx][2][k][1] for r in rows]))
            meds.append('%s median %.2f (%s %.2f)' % (sname, m, op, bound))
            med_ok &= metrics.within(m, bound, op)
        ok = passes >= NEED and med_ok
        ok_all &= ok
        print('%d %-20s %d/%d runs, %s: %s' % (num, name, passes, len(rows), '; '.join(meds), 'PASS' if ok else 'FAIL'))
    why = []
    if len(rows) != RUNS:
        why.append('%d valid runs, the policy needs exactly %d' % (len(rows), RUNS))
    if len(invalid) > MAX_INVALID:
        why.append('%d invalid attempts, the policy allows %d' % (len(invalid), MAX_INVALID))
    print('G1 on this series: %s%s' % ('PASS' if ok_all else 'FAIL', (' (' + '; '.join(why) + ')') if why else ''))
    sys.exit(0 if ok_all else 1)


if __name__ == '__main__':
    main()
