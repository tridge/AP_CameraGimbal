import test from 'node:test';
import assert from 'node:assert/strict';
import { validateMapConfiguration, freshMapPosition } from '../src/map/map-config.js';
const base = 'https://viewer.example/viewer/';
test('all map configuration validation happens before provider changes', () => {
  assert.deepEqual(
    validateMapConfiguration(
      { imagery: 'osm', terrainUrl: '/terrain/', amslToEllipsoidMetres: 14 },
      base,
    ),
    {
      imagery: 'osm',
      terrainUrl: '/terrain/',
      googleMapsApiKey: '',
      amslToEllipsoidMetres: 14,
      terrainDatum: 'ellipsoid',
    },
  );
  for (const config of [
    { imagery: 'bad' },
    { imagery: 'google' },
    { terrainUrl: 'https://other.example/terrain' },
    { terrainUrl: 'javascript:void(0)' },
    { terrainUrl: 'http://plot.ardupilot.org/quantized/' },
    { terrainUrl: 'https://plot.ardupilot.org/other/' },
    { terrainUrl: 'https://user:password@plot.ardupilot.org/quantized/' },
    { terrainUrl: 'https://plot.ardupilot.org.evil.example/quantized/' },
    { terrainUrl: 'https://plot.ardupilot.org/quantized/', terrainDatum: 'ellipsoid' },
    { amslToEllipsoidMetres: NaN },
    { amslToEllipsoidMetres: '10' },
    { amslToEllipsoidMetres: null },
    { terrainUrl: null },
    { terrainDatum: 'unknown' },
  ])
    assert.throws(() => validateMapConfiguration(config, base));
});
test('shared plot terrain works across viewer origins while local services remain supported', () => {
  const terrainUrl = 'https://plot.ardupilot.org/quantized/';
  assert.equal(
    validateMapConfiguration({ terrainUrl, amslToEllipsoidMetres: 50 }, base).terrainDatum,
    'amsl',
  );
  for (const baseURL of ['http://127.0.0.1:4178/video/', 'https://plot.ardupilot.org/video/']) {
    assert.equal(
      validateMapConfiguration({ terrainUrl, terrainDatum: 'amsl' }, baseURL).terrainUrl,
      terrainUrl,
    );
    assert.equal(
      validateMapConfiguration({ terrainUrl: '/local-terrain/' }, baseURL).terrainUrl,
      '/local-terrain/',
    );
  }
});
test('fresh map position is independent of absent gimbal attitude and validates age/ranges', () => {
  const position = { lat_e7: -350000000, lon_e7: 1490000000, alt_amsl_m: 820, age_ms: 0 };
  assert.deepEqual(freshMapPosition({ position }), { lat: -35, lon: 149, altMsl: 820 });
  assert.ok(freshMapPosition({ position: { ...position, age_ms: 2000 } }));
  for (const age_ms of [undefined, null, NaN, Infinity, -1, 2001])
    assert.equal(freshMapPosition({ position: { ...position, age_ms } }), null);
  for (const fields of [
    { lat_e7: 910000000 },
    { lon_e7: 1810000000 },
    { alt_amsl_m: NaN },
    { lat_e7: '-350000000' },
  ])
    assert.equal(freshMapPosition({ position: { ...position, ...fields } }), null);
});
