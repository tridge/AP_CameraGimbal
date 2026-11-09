#!/usr/bin/env python3
"""Score a machine's stage 6 series under the acceptance rule in notes/stage6.md: G1's v2 bounds
(metrics.criteria) against the worse of two baselines, criterion 9 as G1, and G1's repeat
decision (exactly 5 valid runs; every criterion passes in >= 4 and its median is within bound).

  score_policy.py DATASET BASE1.json BASE2.json --runs RUN_DIR... --prepare PREPARE_DIR
                  [--build ID] [--info NAME=METRICS.json ...]

--runs is the machine's whole attempt ledger in order (every e2e.mjs run directory of the series,
valid or not). A run is valid when its report is readable and has no harness error, a successful
nvidia-smi query showed no other compute process on the laptop GPU at its start (report
gpuBefore, a list; win11 runs exempt) and its train stage completed;
train.json follows the contract; splat.ply's payload matches its header; all held-out renders are
present at full size; its dataset is bit-identical to PREPARE_DIR's (points.f32, targets.f16);
its build id is --build (when given); and its trainer and Brush configuration equal the other
runs'. Invalid attempts are listed; more than MAX_INVALID fails the series. Exactly RUNS valid,
distinct (directory and splat.ply hash) runs are required: the first RUNS valid attempts are scored.
DATASET: a g1_view.py directory of PREPARE_DIR. --info: other scored runs, reported against the same
bounds for information. Exits nonzero unless the decision passes."""
import argparse
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'g1'))
import metrics  # noqa: E402

RUNS, NEED, MAX_INVALID = 5, 4, 3  # the repeat policy; not options


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def ply_problem(path):
    data = path.read_bytes()
    end = data.find(b'end_header\n')
    if end < 0:
        return 'splat.ply has no header'
    head = data[:end].decode('latin1').split('\n')
    n = next((int(l.split()[2]) for l in head if l.startswith('element vertex')), None)
    props = [l for l in head if l.startswith('property')]
    if n is None or any(p.split()[1] != 'float' for p in props):
        return 'splat.ply header is not float vertices'
    if len(data) - end - 11 != n * len(props) * 4:
        return 'splat.ply payload does not match its header'
    return None


def run_problem(run, ref, build, dataset):
    try:
        return check_run(run, ref, build, dataset)
    except Exception as e:  # malformed or truncated attempt metadata: an invalid attempt
        return 'unreadable attempt (%s: %s)' % (type(e).__name__, str(e)[:100])


def check_run(run, ref, build, dataset):
    rep = run / 'report.json'
    if not rep.exists():
        return 'no report.json'
    r = json.loads(rep.read_text())
    if not isinstance(r, dict):
        return 'report.json is not an object'
    if r.get('error'):
        return 'harness error: ' + str(r['error']).split('\n')[0][:120]
    if r.get('machine') != 'win11':
        g = r.get('gpuBefore')
        if not isinstance(g, list):
            return 'no evidence the laptop GPU was idle at start (gpuBefore %r)' % (g,)
        if g:
            return 'GPU not idle at start: ' + '; '.join(map(str, g))[:120]
    if not any(c['name'] == 'train done' and c['ok'] for c in r.get('checks', [])):
        return 'train stage did not complete'
    if build and (r.get('build') or {}).get('id') != build:
        return 'build %s, not %s' % ((r.get('build') or {}).get('id'), build)
    tj = run / 'train' / 'train.json'
    if not tj.exists():
        return 'no train.json'
    t = json.loads(tj.read_text())
    if t.get('format') != 'websplat.train' or t.get('version') != 1 or t.get('standin') is not False:
        return 'train.json does not follow the contract'
    if not (run / 'train' / 'splat.ply').exists():
        return 'no splat.ply'
    p = ply_problem(run / 'train' / 'splat.ply')
    if p:
        return p
    px = dataset['width'] * dataset['height'] * 4
    views = [im['name'] for im in dataset['images'] if im['split'] == 'test']
    if sorted(t['eval']['views']) != sorted(views):
        return 'train.json eval views differ from the dataset\'s holdouts'
    for v in views:
        f = run / 'train' / 'eval' / (v + '.f32')
        if not f.exists() or f.stat().st_size != px:
            return 'eval render %s missing or truncated' % v
    for name, h in ref.items():
        f = run / 'prepare' / name
        if not f.exists() or sha(f) != h:
            return 'dataset %s differs from the baselines\'' % name
    if not (run / 'ply_render.npz').exists():
        return 'no ply_render.npz (export check)'
    return None


def crit_for(scored, bases, export=None):
    crit = metrics.criteria(scored, bases)
    if export is not None:
        crit.append((9, 'export', [('min PSNR dB', export['min_psnr'], 45.0, '>='),
                                   ('max peak diff C', export['max_peak_diff_c'], 1.0, '<=')]))
    return crit


def fails(crit):
    return [num for num, _, stats in crit if not all(metrics.within(v, b, op) for _, v, b, op in stats)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dataset', type=Path)
    ap.add_argument('base1', type=Path)
    ap.add_argument('base2', type=Path)
    ap.add_argument('--runs', type=Path, nargs='+', required=True)
    ap.add_argument('--prepare', type=Path, required=True)
    ap.add_argument('--build')
    ap.add_argument('--info', nargs='*', default=[])
    a = ap.parse_args()
    bases = [json.loads(p.read_text()) for p in (a.base1, a.base2)]
    for b, p in zip(bases, (a.base1, a.base2)):
        h = b['summary']['hot']
        print('baseline %s: hot PSNR %.2f, hot peak median %.1f C, max %.1f C' % (
            p.parent.name, h['psnr_mean'], h['abs_peak_error_median_c'], h['abs_peak_error_max_c']))
    ref = {n: sha(a.prepare / n) for n in ('points.f32', 'targets.f16')}
    dataset = json.loads((a.prepare / 'dataset.json').read_text())
    valid, invalid, seen_dirs, seen_ply, config = [], [], set(), set(), None
    for run in a.runs:
        why = run_problem(run, ref, a.build, dataset)
        if not why:
            key = run.resolve()
            h = sha(run / 'train' / 'splat.ply')
            t = json.loads((run / 'train' / 'train.json').read_text())
            c = json.dumps([t.get('trainer'), t.get('brush_config')], sort_keys=True)
            if key in seen_dirs or h in seen_ply:
                why = 'duplicate of an earlier attempt'
            elif config is not None and c != config:
                why = 'trainer or Brush configuration differs from the series'
            else:
                seen_dirs.add(key)
                seen_ply.add(h)
                config = config or c
        if why:
            invalid.append((run.name, why))
            print('invalid %-38s %s' % (run.name, why))
        else:
            valid.append(run)
    scored_runs = valid[:RUNS]
    rows = []
    for run in scored_runs:
        scored = metrics.score(run / 'train' / 'eval', a.dataset)
        export = metrics.export_check(run / 'train' / 'eval', run / 'ply_render.npz', a.dataset)
        crit = crit_for(scored, bases, export)
        hot = {x['name'][:5]: x['abs_peak_error_c'] for x in scored['views']['hot']}
        rows.append(dict(run=run.name, crit=crit))
        print('%-40s fails %-8s hot peak errors %s' % (run.name, fails(crit) or 'none',
                                                     ' '.join('%s %5.1f' % kv for kv in hot.items())))
    for spec in a.info:
        name, path = spec.split('=', 1)
        crit = crit_for(json.loads(Path(path).read_text()), bases)
        print('info %-35s fails %s | %s' % (name, fails(crit) or 'none', '; '.join(
            '%s %s %.2f' % (n, s[0][0], s[0][1]) for _, n, s in crit if n in ('hot PSNR', 'abs peak error hot'))))
    print('\nledger: %d attempts, %d valid (%d scored), %d invalid (allowed %d)' % (
        len(a.runs), len(valid), len(scored_runs), len(invalid), MAX_INVALID))
    print('decision (pass in >= %d of %d runs, median within bound):' % (NEED, RUNS))
    ok_all = len(rows) == RUNS and len(invalid) <= MAX_INVALID
    for idx, (num, name, stats) in enumerate(rows[0]['crit'] if rows else []):
        passes = sum(all(metrics.within(v, b, op) for _, v, b, op in r['crit'][idx][2]) for r in rows)
        meds, med_ok = [], True
        for k, (sname, _, bound, op) in enumerate(stats):
            m = float(np.median([r['crit'][idx][2][k][1] for r in rows]))
            meds.append('%s median %.2f (%s %.2f)' % (sname, m, op, bound))
            med_ok &= metrics.within(m, bound, op)
        ok = passes >= NEED and med_ok
        ok_all &= ok
        print('%d %-20s %d/%d runs, %s: %s' % (num, name, passes, len(rows), '; '.join(meds), 'PASS' if ok else 'FAIL'))
    why = []
    if len(rows) != RUNS:
        why.append('%d valid runs, the policy needs %d' % (len(rows), RUNS))
    if len(invalid) > MAX_INVALID:
        why.append('%d invalid attempts, the policy allows %d' % (len(invalid), MAX_INVALID))
    print('stage 6 on this series: %s%s' % ('PASS' if ok_all else 'FAIL', (' (' + '; '.join(why) + ')') if why else ''))
    sys.exit(0 if ok_all else 1)


if __name__ == '__main__':
    main()
