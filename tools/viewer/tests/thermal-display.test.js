import test from 'node:test';
import assert from 'node:assert/strict';
import { colorize, adjustRgba, sampleRange, stretch, celsius } from '../src/domain/thermal.js';

const rgb = (rgba, i) => [...rgba.slice(i * 4, i * 4 + 3)];
test('gamma changes the display curve after windowing without changing sample precision or data', () => {
  const pixels = new Uint16Array([0, 1, 4, 16384, 32768, 65535]),
    original = pixels.slice();
  const linear = colorize(pixels, 'White hot', [0, 65535]);
  const dark = colorize(pixels, 'White hot', [0, 65535], { gamma: 2 });
  const bright = colorize(pixels, 'White hot', [0, 65535], { gamma: 0.1 });
  assert.equal(dark[0], 0);
  assert.equal(dark[20], 255);
  assert.ok(dark[16] < linear[16]);
  assert.ok(bright[16] > linear[16]);
  assert.ok(bright[4] > 0, 'faint raw differences must survive until gamma mapping');
  assert.deepEqual(pixels, original);
  for (const gamma of [0, -1, NaN, Infinity, 6])
    assert.throws(() => colorize(pixels, 'White hot', [0, 65535], { gamma }));
});

test('GloryHot keeps every sample at/below Low grayscale and every warmer sample colored', () => {
  const pixels = [-5, 0, 5, 10, 10.01, 15, 20, 100],
    original = [...pixels];
  for (const gamma of [0.5, 1, 2]) {
    const image = colorize(pixels, 'GloryHot', [10, 20], { gamma });
    for (let i = 0; i < 4; i++) assert.equal(new Set(rgb(image, i)).size, 1);
    for (let i = 4; i < pixels.length; i++) assert.ok(new Set(rgb(image, i)).size > 1);
    assert.deepEqual(rgb(image, 6), [255, 0, 0]);
    assert.deepEqual(rgb(image, 7), rgb(image, 6));
    assert.ok(image[0] < image[12], 'grayscale retains cool scene structure');
  }
  const a = colorize(pixels, 'GloryHot', [10, 20]),
    b = colorize(pixels, 'GloryHot', [10, 30]);
  assert.notDeepEqual(rgb(a, 5), rgb(b, 5), 'High controls the hot colour ramp');
  assert.deepEqual(pixels, original);
});

test('display limits work in calibrated Celsius, explicit raw counts and RGB intensity', () => {
  const pixels = new Uint16Array([17536, 18000, 19000]);
  const metadata = { temperature_scale_k: 1 / 64, temperature_offset_k: 0 };
  const range = stretch(pixels, [5, 25], metadata);
  assert.ok(Math.abs(celsius(range[0], metadata) - 5) < 1e-10);
  assert.ok(Math.abs(celsius(range[1], metadata) - 25) < 1e-10);
  assert.deepEqual(stretch(pixels, [17500, 19000], null, { raw: true }), [17500, 19000]);
  assert.throws(() => stretch(pixels, [5, 25], null));
  assert.throws(() => stretch(pixels, [20, 10], null, { raw: true }));
  assert.deepEqual(sampleRange(new Uint16Array([10, 10])), [10, 11]);
  assert.deepEqual(sampleRange(pixels), [17536, 19000]);
});

test('calibrated GloryHot keeps exact Celsius boundaries gray without rounding fractional limits', () => {
  const pixels = new Uint16Array([30000, 30315, 30316]);
  const metadata = { temperature_scale_k: 0.01, temperature_offset_k: 0 };
  // 30 C converts to 30314.999999999996 without roundoff correction.
  const typed = stretch(pixels, [30, 40], metadata);
  const automatic = stretch(pixels, [celsius(30315, metadata), celsius(30316, metadata)], metadata);
  for (const limits of [typed, automatic]) {
    assert.equal(limits[0], 30315);
    const image = colorize(pixels, 'GloryHot', limits);
    assert.equal(new Set(rgb(image, 1)).size, 1);
    assert.ok(new Set(rgb(image, 2)).size > 1);
  }
  const fractional = stretch(pixels, [celsius(30315.25, metadata), 40], metadata);
  assert.ok(Math.abs(fractional[0] - 30315.25) < 1e-9);
  const narrow = stretch(pixels, [30 - 1e-13, 30 + 1e-13], metadata);
  assert.ok(narrow[1] > narrow[0], 'roundoff correction must not collapse a valid window');
  for (const calibration of [
    { temperature_scale_k: 0.01, temperature_offset_k: 300 },
    { temperature_scale_k: 0.03, temperature_offset_k: 0 },
  ]) {
    const limits = stretch(pixels, [celsius(1, calibration), celsius(2, calibration)], calibration);
    assert.deepEqual(limits, [1, 2]);
  }
});

test('Original RGB supports manual levels and gamma, preserving source bytes and alpha', () => {
  const source = new Uint8ClampedArray([20, 80, 160, 130, 255, 128, 0, 255]),
    copy = source.slice();
  assert.deepEqual(adjustRgba(source, [0, 255]), source);
  const levels = adjustRgba(source, [0, 160]),
    gamma = adjustRgba(source, [0, 255], { gamma: 2 });
  assert.equal(levels[2], 255);
  assert.equal(gamma[3], 130);
  assert.ok(gamma[1] < source[1]);
  assert.deepEqual(source, copy);
});
