#!/usr/bin/env python3
"""A stage 5b prepare output as the G1 scorer's dataset directory (test/g1/metrics.py): g1.json
(names, splits, hot set, scale, v_hot, K, viewmats) and gt.npy = the float16 targets as float32
(the values train.py and stage 6 train on), plus eval_views.txt.

  g1_view.py PREPARE_DIR OUT_DIR"""
import json
from pathlib import Path
import sys

import numpy as np

prep, out = Path(sys.argv[1]), Path(sys.argv[2])
d = json.loads((prep / 'dataset.json').read_text())
n, H, W = len(d['images']), d['height'], d['width']
out.mkdir(parents=True, exist_ok=True)
t = np.fromfile(prep / d['targets']['file'], '<f2').reshape(n, H, W)
np.save(out / 'gt.npy', t.astype(np.float32))
names = [im['name'] for im in d['images']]
test = [im['name'] for im in d['images'] if im['split'] == 'test']
meta = dict(names=names, test=test, hot=[im['name'] for im in d['images'] if im['hot']], hot_c=d['hot_c'],
            v_hot=d['v_hot'], temperature_scale={k: d['scale'][k] for k in ('kind', 't0_c', 't1_c', 'k_c')},
            width=W, height=H, K=d['K'], viewmats=[np.array(im['viewmat']).reshape(4, 4).tolist() for im in d['images']],
            source='stage 5b prepare output (float16 targets)')
(out / 'g1.json').write_text(json.dumps(meta, indent=1))
(out / 'eval_views.txt').write_text('\n'.join(test) + '\n')
print('%d views, %d held out -> %s' % (n, len(test), out))
