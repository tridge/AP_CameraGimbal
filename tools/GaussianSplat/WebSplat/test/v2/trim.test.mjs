// The viewer's trim circle in the export metadata (export/meta.js trimInfo, splat_common.trim_info).
// usage (WebSplat directory): node --test test/v2/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { exportInputs } from '../../app/src/stages/export/export.js';
import {
  TRIM_FRACTION,
  apcgComment,
  rgbWebInfo,
  trainInfo,
  trimInfo,
} from '../../app/src/stages/export/meta.js';

test('trimInfo: a fraction of the bounds radius about the bounds centre', () => {
  assert.deepEqual(trimInfo({ centre: [1.5, -2], radius: 200, z_min: 0 }), {
    trim_centre: [1.5, -2],
    trim_radius: TRIM_FRACTION * 200,
  });
  assert.deepEqual(trimInfo(undefined), {}); // older datasets: no fields, the viewer estimates
  assert.deepEqual(trimInfo({ centre: [0, 0], radius: 0 }), {});
});

test('exportInputs: trim from dataset.bounds (thermal: about the target, not the hot spot)', () => {
  const dataset = {
    images: [],
    K: [],
    height: 512,
    hotspot: { enu: [9, 9, 9] },
    scale: {},
    bounds: { centre: [0, 0], radius: 165 },
  };
  const frames = { target_enu: [0, 0, 30], origin: { lat: -35, lon: 149, alt_amsl: 600 } };
  const inputs = exportInputs(dataset, frames);
  assert.deepEqual(inputs.trim, { trim_centre: [0, 0], trim_radius: 0.85 * 165 });
  assert.deepEqual(inputs.target, [9, 9, 9]);
});

test('header text: Python float form, after rasterize_mode, as train.py / export_web.py', () => {
  const trim = trimInfo({ centre: [0, 0], radius: 165 });
  assert.equal(
    apcgComment(trainInfo({ target: [0, 0, 10], scale: { kind: 'log1p' }, origin: null, trim })),
    'apcg_splat {"kind":"thermal","up":[0,0,1],"target":[0.0,0.0,10.0],' +
      '"temperature_scale":{"kind":"log1p"},"origin":null,"rasterize_mode":"classic",' +
      '"trim_centre":[0.0,0.0],"trim_radius":140.25}',
  );
  const rgb = rgbWebInfo({ count: 1, shDegree: 1, title: 't', camera: null, target: [0, 0, 0] });
  assert.ok(!('trim_radius' in rgb)); // without bounds
});
