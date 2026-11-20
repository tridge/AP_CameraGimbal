import test from 'node:test';
import assert from 'node:assert/strict';
import { PoseTimeline } from '../src/domain/time.js';
import { projectionPose } from '../src/domain/projection-pose.js';
import { validPose } from '../src/domain/geometry.js';

const wrap = (a) => Math.atan2(Math.sin(a), Math.cos(a));
const rad = (n) => (n * Math.PI) / 180;
function recording() {
  const frames = Array.from({ length: 181 }, (_, i) => ({
    pts: i / 30,
    telemetry: {
      utc_us: 1700000000000000 + (i / 30) * 1e6,
      position: { lat_e7: -350000000, lon_e7: 1490000000, alt_amsl_m: 1000, age_ms: 0 },
      vehicle_attitude: { yaw_rad: wrap(rad(160 + i * 0.2)), age_ms: 0 },
      gimbal_attitude: { roll_rad: 0, pitch_rad: -1, yaw_rad: rad(10), age_ms: 0 },
      hfov_deg: 60,
    },
  }));
  const timeline = new PoseTimeline();
  timeline.rebuild(frames);
  return { frames, timeline };
}

test('projection yaw rejects isolated spikes, handles north wrap and retains steady turns without phase lag', () => {
  const { frames, timeline } = recording();
  for (let i = 15; i < frames.length; i += 15)
    frames[i].telemetry.gimbal_attitude.yaw_rad += rad(25);
  const original = structuredClone(frames);
  for (let i = 10; i < frames.length - 10; i++) {
    const output = projectionPose(frames[i].telemetry, timeline);
    assert.ok(Math.abs(wrap(validPose(output).yaw - rad(170 + i * 0.2))) < rad(0.1));
    assert.equal(output.gimbal_attitude.pitch_rad, -1);
    assert.equal(output.position, frames[i].telemetry.position);
  }
  assert.deepEqual(frames, original, 'raw metadata is immutable');
  const order = [100, 20, 80, 40, 120];
  const yaw = order.map((i) => validPose(projectionPose(frames[i].telemetry, timeline)).yaw);
  assert.deepEqual(
    order
      .slice()
      .reverse()
      .map((i) => validPose(projectionPose(frames[i].telemetry, timeline)).yaw),
    yaw.reverse(),
  );
});

test('yaw stabilisation can be disabled and never fills stale, missing or separated pose samples', () => {
  const { frames, timeline } = recording();
  const raw = frames[90].telemetry;
  assert.equal(projectionPose(raw, timeline, 0), raw);
  assert.equal(projectionPose(null, timeline), null);
  const stale = { ...raw, gimbal_attitude: { ...raw.gimbal_attitude, age_ms: 3000 } };
  assert.equal(projectionPose(stale, timeline), stale);
  frames[89].telemetry = null;
  frames[91].telemetry = null;
  frames[88].telemetry.utc_us = raw.utc_us - 500000;
  frames[92].telemetry.utc_us = raw.utc_us + 500000;
  const separated = new PoseTimeline();
  separated.rebuild([frames[88], frames[90], frames[92]]);
  assert.equal(projectionPose(raw, separated), raw);
  const unavailable = new PoseTimeline();
  assert.equal(projectionPose(raw, unavailable), raw);
});

test('a short sampling outage prevents a new pose segment from borrowing the old heading', () => {
  const { frames } = recording();
  const prior = frames.slice(0, 90);
  const resumed = frames.slice(90, 95);
  for (const f of resumed) {
    f.telemetry.utc_us += 350000;
    f.telemetry.gimbal_attitude.yaw_rad += rad(90);
  }
  const timeline = new PoseTimeline();
  timeline.rebuild([...prior, ...resumed]);
  const raw = resumed[0].telemetry;
  assert.equal(projectionPose(raw, timeline), raw);
});
