import test from 'node:test';
import assert from 'node:assert/strict';
import { videoInformation, metadataJSON } from '../src/domain/video-info.js';

const pose = {
  schema: 'apcg.telemetry.v1',
  utc_us: 1479310687167137,
  position: { lat_e7: 477023478, lon_e7: 162446310, alt_amsl_m: 1136, age_ms: 0 },
  gimbal_attitude: { roll_rad: 0, pitch_rad: -1, yaw_rad: 0, age_ms: 0 },
  vehicle_attitude: { yaw_rad: 0, age_ms: 0 },
  hfov_deg: 30,
};
function state(times = [100, 100.02, 100.04]) {
  const frames = times.map((pts, id) => ({
    id,
    trackId: 273,
    pts,
    dts: pts,
    duration: 0.02,
    size: 1000,
    keyframe: id === 0,
    telemetry: {
      ...pose,
      sourceSchema: 'misb.st0601',
      provenance: { format: 'misb-klv', association: 'nearest-pts' },
    },
  }));
  return {
    file: { name: 'example.ts', size: 5000, lastModified: 1700000000000, type: 'video/mp2t' },
    source: {
      format: 'mpegts',
      tracks: [
        {
          id: 273,
          codec: 'avc',
          codecString: 'avc1.640028',
          width: 1280,
          height: 720,
          timescale: 90000,
          defaultDuration: 0.02,
        },
      ],
      frames,
    },
    frames,
    frame: frames[0],
    number: 0,
    origin: 100,
    indexed: true,
    indexPercent: 100,
    decoded: {
      kind: 'rgba',
      width: 1280,
      height: 720,
      get data() {
        throw new Error('The info popup must not read pixel data');
      },
    },
    decoder: { mode: 'WebCodecs · native browser' },
    offset: 0,
    warnings: [],
  };
}
function field(report, name) {
  return report.sections.flatMap((s) => s.rows).find(([key]) => key === name)?.[1];
}

test('video information measures rate, dimensions, telemetry coverage and clock provenance without reading payloads', () => {
  const s = state(),
    report = videoInformation(s);
  assert.equal(field(report, 'Frame rate (average)'), '50.000 fps');
  assert.equal(field(report, 'Playback duration'), '0.060 s');
  assert.equal(field(report, 'Encoded dimensions'), '1280 × 720');
  assert.equal(field(report, 'Aspect ratio'), '16:9');
  assert.equal(field(report, 'Frames with fresh camera pose'), '3 / 3 frames');
  assert.equal(field(report, 'Telemetry type'), 'MISB ST0601 KLV');
  assert.equal(field(report, 'Approximate timestamp associations'), '3 / 3 frames');
  assert.equal(report.raw.currentFrame.telemetry.utc_us, pose.utc_us);
  assert.equal(report.raw.statistics.videoPayloadBytes, 3000);
  assert.ok(!('data' in report.raw.decoded));
  metadataJSON(report.raw);
});

test('variable capture timing is measured independently of the nominal frame rate', () => {
  const report = videoInformation(state([100, 100.02, 100.1]));
  assert.equal(field(report, 'Nominal frame rate'), '50.000 fps');
  assert.equal(field(report, 'Frame rate (average)'), '20.000 fps');
  assert.equal(field(report, 'Frame timing'), 'Variable intervals / gaps');
  assert.equal(field(report, 'Frame intervals'), '20.000–80.000 ms');
});

test('empty/incomplete indexes never invent a rate or metadata and retain indexing diagnostics', () => {
  const report = videoInformation({
    file: { name: 'bad.mkv', size: 0 },
    frames: [],
    indexFailed: true,
    indexError: 'Truncated Matroska header',
  });
  assert.equal(field(report, 'Frame rate (average)'), 'Unavailable');
  assert.equal(field(report, 'Aligned camera pose'), 'Unavailable');
  assert.equal(field(report, 'Telemetry type'), 'None found in indexed frames');
  assert.match(field(report, 'Indexing'), /Incomplete/);
  assert.deepEqual(report.raw.diagnostics, ['Truncated Matroska header']);
});

test('co-carried KLV and raw thermal calibration remain visible alongside preferred APCG telemetry', () => {
  const s = state();
  s.frames.forEach((frame, i) => {
    frame.klvTelemetry = frame.telemetry;
    frame.telemetry = { ...pose, provenance: { format: 'apcg-sei' } };
    frame.thermalMetadata = {
      schema: 'apcg.thermal.v1',
      bits_per_sample: 16,
      pixel_format: 'gray16le',
      temperature_scale_k: 1 / 64,
      temperature_offset_k: 0,
      minimum_c: -20,
      maximum_c: [20, -10, 130][i],
    };
  });
  const report = videoInformation(s);
  assert.match(field(report, 'Telemetry type'), /APCG JSON SEI/);
  assert.match(field(report, 'Telemetry type'), /MISB ST0601 KLV/);
  assert.equal(field(report, 'Frames with KLV'), '3 / 3 frames');
  assert.equal(field(report, 'Calibrated raw thermal frames'), '3 / 3 frames');
  assert.equal(field(report, 'Frame-maximum temperature range'), '-10.00–130.00 °C');
  assert.equal(field(report, 'Temperature scale'), '0.01562500 K/count');
});

test('metadata JSON preserves binary codec headers, parameter maps and uint64 values', () => {
  const value = {
    codecPrivate: new Uint8Array([0, 255, 128]),
    parameters: new Map([[33, new Uint8Array([1, 2])]]),
    exact: 18446744073709551615n,
  };
  const json = JSON.parse(metadataJSON(value));
  assert.deepEqual(json.codecPrivate, { type: 'Uint8Array', byteLength: 3, hex: '00ff80' });
  assert.equal(json.parameters.entries[0][1].hex, '0102');
  assert.equal(json.exact, '18446744073709551615');
});

test('out-of-range telemetry UTC remains exportable without breaking the information report', () => {
  const s = state();
  s.frames.forEach((f) => (f.telemetry.utc_us = 1e21));
  const report = videoInformation(s);
  for (const label of [
    'First recorded UTC',
    'Last recorded UTC',
    'Recorded UTC',
    'Aligned pose UTC',
  ])
    assert.equal(field(report, label), 'Unavailable');
  assert.equal(field(report, 'Frame rate (average)'), '50.000 fps');
  assert.equal(JSON.parse(metadataJSON(report.raw)).currentFrame.telemetry.utc_us, 1e21);
});
