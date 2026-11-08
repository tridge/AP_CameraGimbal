// colmap.js heap handling with a mock module: a failing allocation (array or string) frees
// what was allocated before it and never calls the C function.
import assert from 'node:assert/strict';
import { Colmap } from '../../app/src/stages/map/colmap.js';

function mock(failAt) {
  let n = 0;
  const live = new Set();
  const M = {
    HEAPU8: new Uint8Array(1 << 16),
    _malloc: () => (++n === failAt ? 0 : (live.add(n * 64), n * 64)),
    _free: (p) => live.delete(p),
    stringToNewUTF8: () => (++n === failAt ? 0 : (live.add(n * 64), n * 64)),
  };
  return { M, live };
}
for (let failAt = 1; failAt <= 3; failAt++) {
  const { M, live } = mock(failAt);
  const cw = new Colmap(M);
  let called = false;
  assert.throws(
    () => cw.with([new Uint8Array(8), 'name', new Float64Array(2)], () => (called = true)),
    /out of wasm memory/,
  );
  assert.equal(called, false);
  assert.equal(live.size, 0, `allocation ${failAt} failing leaks ${live.size}`);
}
{
  const { M, live } = mock(0);
  const cw = new Colmap(M);
  assert.throws(
    () =>
      cw.with([new Uint8Array(8), 'x'], () => {
        throw new Error('boom');
      }),
    /boom/,
  );
  assert.equal(live.size, 0);
  assert.equal(
    cw.with([new Uint8Array(8), 'x'], (a, b) => b - a),
    64,
  );
  assert.equal(live.size, 0);
}
console.log('PASS colmap.js allocation handling');
