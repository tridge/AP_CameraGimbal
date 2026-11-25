import test from 'node:test';
import assert from 'node:assert/strict';
import { PoseTimeline } from '../src/domain/time.js';
import { connectedTrackPoints } from '../src/domain/video-track.js';

test('pose lookup and the map track do not cross publisher epochs', () => {
  const frame = (epoch, utc, lat) => ({ epoch, telemetry: { utc_us: utc, position: { lat } } }),
    timeline = new PoseTimeline(),
    frames = [frame(0, 1e6, 1), frame(0, 2e6, 2), frame(1, 1.5e6, 9), frame(1, 2.5e6, 10)];
  timeline.rebuild(frames);
  assert.equal(timeline.scope(0).at(2e6, null).position.lat, 2);
  assert.equal(timeline.scope(1).at(2e6, null).pose_utc_us, 2e6);
  assert.equal(timeline.scope(0).at(2.4e6, null), null);
  const live = new PoseTimeline();
  assert.equal(live.extend(frames.slice(0, 2)), true);
  assert.equal(live.extend(frames.slice(2)), true);
  assert.deepEqual([live.samples.length, live.scope(1).samples.length], [2, 2]);
  assert.equal(live.extend([frame(1, 2e6, 0)]), false);
  const a = { clockSegment: 0, lat: 1, lon: 1, pts: 1, utcUs: 1e6, epoch: 0 };
  assert.equal(connectedTrackPoints(a, { ...a, pts: 2, utcUs: 2e6 }), true);
  assert.equal(connectedTrackPoints(a, { ...a, pts: 2, utcUs: 2e6, epoch: 1 }), false);
});
