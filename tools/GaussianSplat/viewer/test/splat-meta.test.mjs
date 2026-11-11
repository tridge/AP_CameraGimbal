import { test } from 'node:test';
import assert from 'node:assert/strict';
import { gzipSync, gunzipSync } from 'node:zlib';
import { embeddedMeta, plyMeta } from '../src/splat-meta.js';
import { spzMetaText, withSpzMeta } from '../spz-meta.mjs';
import { clipPly } from '../ply-clip.mjs';
import { trimFromMeta } from '../src/view-math.js';

// export_web.py's info with the trim fields, as Python json.dumps(separators=(',', ':')) writes it
const INFO =
  '{"kind":"rgb","up":[0,0,1],"target":[1.5,-2.0,30.0],"origin":{"lat":-35.1,"lon":149.2,' +
  '"alt_amsl":600.0},"rasterize_mode":"classic","trim_centre":[0.0,0.0],"trim_radius":140.25}';

function ply(comment) {
  const head =
    'ply\nformat binary_little_endian 1.0\n' +
    (comment ? `comment apcg_splat ${comment}\n` : '') +
    'element vertex 2\nproperty float x\nproperty float y\nproperty float z\nend_header\n';
  const body = new Float32Array([0, 0, 1, 5, 5, 50]);
  const out = new Uint8Array(head.length + body.byteLength);
  out.set(new TextEncoder().encode(head));
  out.set(new Uint8Array(body.buffer), head.length);
  return out;
}

test('PLY header: trim fields read back, and kept by the ceiling download', () => {
  const m = plyMeta(ply(INFO));
  assert.deepEqual(trimFromMeta(m), { centre: [0, 0], radius: 140.25 });
  const clipped = clipPly(ply(INFO), [0, 0, 1], 10);
  assert.ok(clipped);
  assert.deepEqual(plyMeta(clipped.bytes), { ...m, clipped_above: 10 });
});

test('SPZ gzip header: the same object, payload unchanged', () => {
  const raw = new Uint8Array([1, 2, 3, 4, 5]);
  const spz = withSpzMeta(gzipSync(raw), INFO);
  assert.equal(spzMetaText(spz), INFO);
  assert.deepEqual(embeddedMeta(spz), JSON.parse(INFO));
  assert.deepEqual(gunzipSync(spz), Buffer.from(raw));
});

test('older files without the fields: no trim from the file (the viewer estimates one)', () => {
  const old = '{"kind":"thermal","up":[0,0,1],"target":[0.0,0.0,31.5]}';
  assert.equal(trimFromMeta(plyMeta(ply(old))), null);
  assert.equal(trimFromMeta(embeddedMeta(withSpzMeta(gzipSync(new Uint8Array(4)), old))), null);
  assert.equal(embeddedMeta(ply(null)), null);
});
