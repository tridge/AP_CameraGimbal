import test from 'node:test';
import assert from 'node:assert/strict';
import {
  buildVideoTrack,
  buildVideoTrackAsync,
  nearestTrackTime,
} from '../src/domain/video-track.js';
const frame = (i) => ({
  id: i,
  pts: 600 + i,
  telemetry: {
    utc_us: 1e12 + i * 1e6,
    position: { lat_e7: -350000000 + i * 10, lon_e7: 1490000000, alt_amsl_m: 1000, age_ms: 0 },
  },
});
const point = (x, y, pts, clockSegment = 0) => ({
  lat: 0,
  lon: 0,
  altMsl: 0,
  x,
  y,
  pts,
  clockSegment,
});
const project = (p) => ({ x: p.x, y: p.y });
test('track keeps short bursts and detects missing positions even between decimated samples', () => {
  const frames = Array.from({ length: 100 }, (_, i) => frame(i));
  frames[22].telemetry = null;
  frames[26].telemetry = null;
  const track = buildVideoTrack(frames, undefined, 5);
  assert.ok(track.some((p) => p.frameId === 23));
  assert.ok(track.some((p) => p.frameId === 25));
  const a = track.find((p) => p.frameId === 21),
    b = track.find((p) => p.frameId === 23);
  assert.notEqual(a.clockSegment, b.clockSegment);
  assert.equal(
    nearestTrackTime([point(0, 0, 0, 0), point(100, 0, 1, 1)], project, { x: 50, y: 0 }),
    null,
  );
});
test('display segment identity survives large PTS gaps introduced only by decimation', () => {
  const track = buildVideoTrack(
    Array.from({ length: 10000 }, (_, i) => frame(i)),
    undefined,
    10,
  );
  assert.ok(track.length < 35);
  const selected = track.map((p) => ({ ...p, x: p.pts, y: 0 }));
  const hit = nearestTrackTime(selected, project, { x: 2100, y: 3 });
  assert.equal(hit.pts, 2100);
  assert.equal(hit.distance, 3);
});
test('track breaks actual clock reversals, long recording gaps and stale telemetry', () => {
  const frames = Array.from({ length: 10 }, (_, i) => frame(i));
  frames[3].telemetry.position.age_ms = 3000;
  frames[6].pts += 50;
  frames[7].telemetry.utc_us -= 20e6;
  const track = buildVideoTrack(frames);
  assert.equal(
    track.some((p) => p.frameId === 3),
    false,
  );
  const get = (i) => track.find((p) => p.frameId === i);
  assert.notEqual(get(2).clockSegment, get(4).clockSegment);
  // Isolated invalid-clock bursts do not fabricate a connecting line.
  for (let i = 1; i < track.length; i++)
    if (track[i].clockSegment === track[i - 1].clockSegment)
      assert.ok(track[i].pts - track[i - 1].pts <= 10);
});
test('nearest track selection respects CSS pixel tolerance and prefers the nearby playback pass', () => {
  const track = [
    point(0, 0, 100),
    point(10, 0, 110),
    point(0, 0.2, 1000, 1),
    point(10, 0.2, 1010, 1),
  ];
  assert.equal(nearestTrackTime(track, project, { x: 5, y: 0 }, 8, 1005).pts, 1005);
  assert.equal(nearestTrackTime(track, project, { x: 5, y: 8.3 }, 8, 1005), null);
  const separate = [
    point(0, 0, 100),
    point(10, 0, 110),
    point(0, 1, 1000, 1),
    point(10, 1, 1010, 1),
  ];
  assert.equal(nearestTrackTime(separate, project, { x: 5, y: 0 }, 8, 1005).pts, 105);
  assert.equal(
    nearestTrackTime(track, () => null, { x: 5, y: 0 }),
    null,
  );
});
test('stationary track resolves toward current PTS instead of an arbitrary endpoint', () => {
  const hit = nearestTrackTime([point(4, 8, 10), point(4, 8, 30)], project, { x: 5, y: 8 }, 8, 24);
  assert.equal(hit.pts, 24);
  assert.equal(hit.distance, 1);
});
test('bad-data bursts have a bounded display budget without joining omitted segments', () => {
  const frames = Array.from({ length: 6000 }, (_, i) => frame(i));
  for (let i = 2; i < frames.length; i += 3) frames[i].telemetry = null;
  const track = buildVideoTrack(frames, undefined, 20);
  assert.ok(track.length <= 62);
  assert.ok(track.limited);
  for (let i = 1; i < track.length; i++)
    if (track[i].clockSegment === track[i - 1].clockSegment)
      assert.equal(track[i].pts - track[i - 1].pts, 1);
});
test('track uses the supplied aligned position/pose clock while retaining original video PTS', () => {
  const frames = Array.from({ length: 8 }, (_, i) => frame(i));
  const track = buildVideoTrack(frames, (f) => ({
    ...f.telemetry,
    pose_utc_us: f.telemetry.utc_us - 5e6,
    position: { ...f.telemetry.position, lat_e7: -360000000 },
  }));
  assert.equal(track[0].lat, -36);
  assert.equal(track[0].pts, 600);
  assert.equal(track[0].utcUs, 1e12 - 5e6);
});

test('large track building yields and cancellation prevents stale navigation maps', async () => {
  const frames = Array.from({ length: 10000 }, (_, i) => frame(i));
  const actual = await buildVideoTrackAsync(frames);
  assert.deepEqual(actual, buildVideoTrack(frames));
  const controller = new AbortController();
  let read = 0;
  const pending = buildVideoTrackAsync(
    frames,
    (f) => {
      read++;
      return f.telemetry;
    },
    4000,
    controller.signal,
  );
  controller.abort();
  await assert.rejects(pending, { name: 'AbortError' });
  assert.ok(read <= 2048);
});
