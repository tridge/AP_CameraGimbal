import { validPose } from './geometry.js';
import { finite } from './time.js';

/** Sample the raw image plane against the same terrain/rays used by picking.
 * A triangle is visible only when all its vertices have ground intersections.
 * Sky, stale pose and unavailable terrain never acquire a flat fallback.
 */
export function buildVideoProjection(elevation, record, width, height, steps = 16) {
  const vertices = [],
    indices = [],
    reasons = new Set();
  const unavailable = (reason) => ({ vertices, indices, complete: false, reason });
  if (![width, height].every((n) => finite(n) && n > 0))
    return unavailable('Open a video to project it onto terrain.');
  if (!validPose(record))
    return unavailable('Camera pose is missing, stale or outside the time-alignment interval.');
  if (!finite(record.hfov_deg) || record.hfov_deg <= 0 || record.hfov_deg >= 179)
    return unavailable('Camera field of view is unavailable or invalid.');
  if (!Number.isInteger(steps) || steps < 1 || steps > 32)
    throw new RangeError('Video projection grid must have 1–32 subdivisions.');
  const samples = new Int32Array((steps + 1) ** 2).fill(-1);
  for (let row = 0; row <= steps; row++)
    for (let col = 0; col <= steps; col++) {
      const u = col / steps,
        v = row / steps,
        // Pixel centres run 0..width-1; image edges lie half a pixel outside.
        result = elevation.project(record, u * width - 0.5, v * height - 0.5, width, height);
      if (result.point) {
        samples[row * (steps + 1) + col] = vertices.length;
        vertices.push({ ...result.point, u, v: 1 - v });
      } else reasons.add(result.reason);
    }
  const triangle = (...points) => {
    if (points.every((p) => p >= 0)) indices.push(...points);
  };
  for (let row = 0; row < steps; row++)
    for (let col = 0; col < steps; col++) {
      const a = row * (steps + 1) + col,
        b = a + 1,
        c = a + steps + 1,
        d = c + 1;
      triangle(samples[a], samples[c], samples[b]);
      triangle(samples[b], samples[c], samples[d]);
    }
  const complete = indices.length === steps * steps * 6;
  return {
    vertices,
    indices,
    complete,
    reason: complete ? null : [...reasons].join(' ') || 'No ground-visible part of this frame.',
  };
}
