import test from 'node:test';
import assert from 'node:assert/strict';
import {
  packetNals,
  parseSEI,
  parseKLV,
  parseMetadataPES,
  parseThermalMetadata,
  normalizeTelemetry,
} from '../src/media/telemetry.js';

const enc = new TextEncoder();
const bytes = (h) => Uint8Array.from(h.match(/../g) || [], (v) => parseInt(v, 16));
const join = (...parts) => Uint8Array.from(parts.flatMap((p) => [...p]));
// Real ca_misb_klv output in SITL_1_5iNY7M.ts, decoded independently by MAVProxy misb.py.
const golden = bytes(
  '060e2b34020b01010e0103010100000069020800065f1ab5f649f10b044d5431310c0e47656f64657469632057475338340d04cdd211ec0e0469f59be40f0213110502071c06020000070200005a04000000005b04000000001002226b1102138f1204000000001304eaaaaaa114040000000041010801020fc2',
);
function checksum(b) {
  let sum = 0;
  for (let i = 0; i < b.length - 2; i++) sum = (sum + b[i] * (i % 2 ? 1 : 256)) & 65535;
  b[b.length - 2] = sum >> 8;
  b[b.length - 1] = sum & 255;
  return b;
}
function changeTag(b, tag, replacement) {
  b = b.slice();
  for (let i = 17; i < b.length; ) {
    const t = b[i++],
      size = b[i++];
    if (t === tag) b.set(replacement, i);
    i += size;
  }
  return checksum(b);
}
function sei(codec, record) {
  const payload = join(
    bytes('8d646b4e556f4a908b7c35e629510321'),
    enc.encode(JSON.stringify(record)),
  );
  const size = [];
  for (let n = payload.length; n >= 255; n -= 255) size.push(255);
  size.push(payload.length % 255);
  return join(
    Uint8Array.of(0, 0, 0, 1),
    codec === 'hevc' ? Uint8Array.of(78, 1) : Uint8Array.of(6),
    Uint8Array.of(5, ...size),
    payload,
    Uint8Array.of(128),
  );
}

test('real MISB golden vector matches authoritative Python position and composed camera pose', () => {
  const [r] = parseKLV(golden);
  assert.equal(r.utc_us, 1793418186869233);
  assert.deepEqual(r.position, {
    lat_e7: -352824005,
    lon_e7: 1490054203,
    alt_amsl_m: 582.1377889677271,
    age_ms: 0,
  });
  assert.equal(r.vehicle_attitude.yaw_rad, 0.17449297717352327);
  assert.ok(Math.abs(r.gimbal_attitude.pitch_rad - -0.5235987899836599) < 1e-14);
  assert.ok(Math.abs(r.gimbal_attitude.yaw_rad) < 1e-14);
  assert.equal(r.hfov_deg, 24.20050354772259);
  assert.equal(r.misb_version, 8);
});

test('checksum corruption, duplicates, missing timestamp/version do not produce telemetry', () => {
  const errors = [],
    corrupt = golden.slice();
  corrupt[40] ^= 1;
  assert.deepEqual(parseKLV(corrupt, { errors }), []);
  assert.match(errors[0], /checksum/);
  const badTimestamp = golden.slice();
  badTimestamp[17] = 3;
  checksum(badTimestamp);
  assert.deepEqual(parseKLV(badTimestamp), []);
  const duplicate = golden.slice();
  duplicate[17] = 11;
  checksum(duplicate);
  assert.deepEqual(parseKLV(duplicate), []);
  assert.deepEqual(parseKLV(golden.subarray(0, golden.length - 1)), []);
});

test('extended-angle invalid supersedes valid legacy; full-range precision retained', () => {
  const [invalid] = parseKLV(changeTag(golden, 91, bytes('80000000')));
  assert.equal(invalid.vehicle_attitude, null);
  assert.equal(invalid.gimbal_attitude, null);
  const [full] = parseKLV(changeTag(golden, 91, bytes('7fffffff')));
  assert.equal(full.vehicle_attitude.roll_rad, Math.PI / 2);
  const [pitch] = parseKLV(changeTag(golden, 90, bytes('40000000')));
  assert.ok(Math.abs(pitch.vehicle_attitude.pitch_rad - Math.PI / 4) < 1e-9);
});

test('synchronous AU cell and asynchronous raw KLV produce the same valid set', () => {
  const cell = join(Uint8Array.of(0, 4, 0xdf, golden.length >> 8, golden.length & 255), golden);
  assert.deepEqual(parseMetadataPES(cell), parseMetadataPES(golden));
  const nonRandomAccess = cell.slice();
  nonRandomAccess[2] = 0xcf;
  assert.deepEqual(parseMetadataPES(nonRandomAccess), parseMetadataPES(golden));
  const fragmented = cell.slice();
  fragmented[2] = 0xff;
  assert.deepEqual(parseMetadataPES(fragmented), []);
});

test('APCG AVC/HEVC SEI preserve unrestricted angles, named validity, and unknown future fields', () => {
  const record = {
    schema: 'apcg.telemetry.v1',
    utc_us: 1787882912367000,
    vehicle_attitude: { roll_rad: 2.617993878, pitch_rad: 0.7, yaw_rad: 1, age_ms: 123 },
    position: null,
    named_values: { TMAX: { value: 645, valid: false } },
    future: [1, 2, 3],
    extra: 'a'.repeat(600),
  };
  for (const codec of ['avc', 'hevc']) {
    const annexB = sei(codec, record),
      nal = [...packetNals(annexB)][0];
    const prefix = Uint8Array.of(
      nal.length >>> 24,
      (nal.length >>> 16) & 255,
      (nal.length >>> 8) & 255,
      nal.length & 255,
    );
    assert.deepEqual(parseSEI(annexB, codec), [record]);
    if (codec === 'hevc') {
      const suffix = annexB.slice();
      suffix[4] = 80;
      assert.deepEqual(parseSEI(suffix, codec), [record]);
    }
    assert.deepEqual(parseSEI(join(prefix, nal), codec, 4), [record]);
    assert.equal(normalizeTelemetry(record).named_values.TMAX.valid, false);
  }
});

test('invalid SEI/schema and thermal metadata never fabricate telemetry/calibration', () => {
  assert.deepEqual(parseSEI(sei('avc', { schema: 'unknown', position: {} })), []);
  assert.deepEqual(parseSEI(Uint8Array.of(0, 0, 1, 6, 5, 255)), []);
  const thermal = {
    schema: 'apcg.thermal.v1',
    rotation_deg: 180,
    temperature_scale_k: 1 / 64,
    temperature_offset_k: 0,
    telemetry: null,
  };
  assert.deepEqual(parseThermalMetadata(enc.encode(JSON.stringify(thermal))), thermal);
  assert.equal(parseThermalMetadata(enc.encode('{}')), null);
  assert.equal(normalizeTelemetry(null), null);
});

test('JSON uint64 provenance remains exact without altering quoted text or physical floats', () => {
  const raw =
    '{"schema":"apcg.thermal.v1","source_mtime_ns":1787882912367000123,"negative":-9007199254740993,"utc_us":1787882912367000,"text":"a\\\"1787882912367000123","physical":1.234e20}';
  const record = parseThermalMetadata(enc.encode(raw));
  assert.equal(record.source_mtime_ns, '1787882912367000123');
  assert.equal(record.negative, '-9007199254740993');
  assert.equal(record.utc_us, 1787882912367000);
  assert.equal(record.text, 'a"1787882912367000123');
  assert.equal(record.physical, 1.234e20);
});

test('noncommuting platform/sensor rotations match independent Python golden values', () => {
  let packet = golden;
  for (const [tag, raw] of Object.entries({
    5: '8000',
    90: '38e38e38',
    91: '2aaaaaaa',
    18: '40000000',
    19: 'eaaaaaab',
    20: '15555555',
  }))
    packet = changeTag(packet, Number(tag), bytes(raw));
  const camera = parseKLV(packet)[0].gimbal_attitude;
  // MAVProxy misb.py output for this exact encoded vector, rather than Euler-angle sums.
  assert.ok(Math.abs(camera.roll_rad - 1.5570128842416817) < 1e-14);
  assert.ok(Math.abs(camera.pitch_rad - -0.7253721467532284) < 1e-14);
  assert.ok(Math.abs(camera.yaw_rad - 0.731818392241161) < 1e-14);
});
