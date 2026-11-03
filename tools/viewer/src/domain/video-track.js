import { finite } from './time.js';
import { freshMapPosition } from '../map/map-config.js';

// Shared by the map and its legend, independently of the video display palette.
export const TRACK_TEMPERATURE_STOPS = [
  [40, 120, 255],
  [32, 217, 232],
  [255, 227, 77],
  [255, 138, 36],
  [237, 37, 37],
];

export function frameMaximumC(frame) {
  // Raw archives carry the maximum of this image outside the nested snapshot.
  // Read temperatures from the image's metadata, never the time-aligned pose.
  if (frame.thermalMetadata)
    return finite(frame.thermalMetadata.maximum_c) ? frame.thermalMetadata.maximum_c : null;
  const maximum = frame.telemetry?.thermal?.maximum_c;
  if (finite(maximum)) return maximum;
  const item = frame.telemetry?.named_values?.TMAX;
  if (finite(item)) return item;
  return finite(item?.value) &&
    item.valid !== false &&
    (!finite(item.age_ms) || item.age_ms <= 2000)
    ? item.value
    : null;
}

export function trackTemperatureRange(points) {
  let min = Infinity,
    max = -Infinity;
  for (const p of points) {
    if (!finite(p.tmaxC)) continue;
    min = Math.min(min, p.tmaxC);
    max = Math.max(max, p.tmaxC);
  }
  return min <= max ? { min, max } : null;
}

export function trackTemperatureColor(value, range) {
  if (!finite(value) || !range) return null;
  const fraction = range.max === range.min ? 0.5 : (value - range.min) / (range.max - range.min),
    t = Math.max(0, Math.min(1, fraction)) * (TRACK_TEMPERATURE_STOPS.length - 1),
    i = Math.min(TRACK_TEMPERATURE_STOPS.length - 2, Math.floor(t)),
    f = t - i;
  return TRACK_TEMPERATURE_STOPS[i].map((v, c) =>
    Math.round(v * (1 - f) + TRACK_TEMPERATURE_STOPS[i + 1][c] * f),
  );
}

export function connectedTrackPoints(a, b) {
  return (
    !!a &&
    !!b &&
    a.clockSegment === b.clockSegment &&
    Math.abs(a.lat - b.lat) <= 0.1 &&
    Math.abs(a.lon - b.lon) <= 0.1 &&
    (!finite(a.pts) || !finite(b.pts) || (b.pts >= a.pts && b.pts - a.pts <= 10)) &&
    (!finite(a.utcUs) || !finite(b.utcUs) || (b.utcUs >= a.utcUs && b.utcUs - a.utcUs <= 1e7))
  );
}

/** Decimate while checking every input for gaps. Short bursts keep endpoints;
 * at the hard display budget entire segments are omitted, never joined.
 */
function* trackBuilder(frames, recordForFrame, budget) {
  const stride = Math.max(1, Math.ceil(frames.length / budget)),
    out = [];
  let segment = 0,
    first = null,
    last = null,
    selected = [],
    bucketMin = null,
    bucketMax = null,
    bucketMissing = null;
  const flushBucket = () => {
    // Up to three points per stride: its first point, a low/missing value and
    // its peak. A brief hot spot must survive spatial display simplification.
    selected.push(...[bucketMissing || bucketMin, bucketMax].filter(Boolean));
    bucketMin = bucketMax = bucketMissing = null;
  };
  const commit = () => {
    flushBucket();
    // Retain short valid bursts too. At the hard display budget omit whole
    // segments rather than joining across a missing-data interval.
    const points = [...new Set([first, ...selected, last].filter(Boolean))].sort(
      (a, b) => a.pts - b.pts,
    );
    if (first && first !== last) {
      if (out.length + points.length <= budget * 3 + 2) out.push(...points);
      else out.limited = true;
    }
    first = last = null;
    selected = [];
    segment++;
  };
  for (let i = 0; i < frames.length; i++) {
    if (i && i % 2048 === 0) yield;
    const frame = frames[i],
      record = recordForFrame(frame),
      position = freshMapPosition(record);
    if (!position || !finite(frame.pts) || frame.decodable === false) {
      commit();
      continue;
    }
    const point = {
      ...position,
      pts: frame.pts,
      frameId: frame.id,
      tmaxC: frameMaximumC(frame),
      utcUs: record.pose_utc_us ?? record.utc_us,
      clockSegment: segment,
    };
    if (last && !connectedTrackPoints(last, point)) {
      commit();
      point.clockSegment = segment;
    }
    if (!first) first = point;
    last = point;
    if (i % stride === 0) {
      flushBucket();
      selected.push(point);
    }
    if (finite(point.tmaxC)) {
      if (!bucketMin || point.tmaxC < bucketMin.tmaxC) bucketMin = point;
      if (!bucketMax || point.tmaxC > bucketMax.tmaxC) bucketMax = point;
    } else bucketMissing ||= point;
  }
  commit();
  return out;
}

export function buildVideoTrack(frames, recordForFrame = (f) => f.telemetry, budget = 4000) {
  const iterator = trackBuilder(frames, recordForFrame, budget);
  let next;
  do {
    next = iterator.next();
  } while (!next.done);
  return next.value;
}
export async function buildVideoTrackAsync(frames, recordForFrame, budget = 4000, signal) {
  const iterator = trackBuilder(frames, recordForFrame || ((f) => f.telemetry), budget);
  while (true) {
    if (signal?.aborted) throw new DOMException('Track build canceled', 'AbortError');
    const next = iterator.next();
    if (next.done) return next.value;
    await new Promise((resolve) => setTimeout(resolve, 0));
  }
}

/** Nearest point on the displayed track, in CSS canvas pixels. Repeated passes
 * within half a pixel resolve toward the current playback time.
 */
export function nearestTrackTime(points, project, click, tolerance = 8, currentPts = 0) {
  if (![click?.x, click?.y, tolerance].every(finite) || tolerance < 0) return null;
  const candidates = [];
  let previous = null,
    previousScreen = null;
  for (const point of points) {
    const screen = project(point);
    if (!screen || ![screen.x, screen.y].every(finite)) {
      previous = previousScreen = null;
      continue;
    }
    if (previous && previous.clockSegment === point.clockSegment) {
      const dx = screen.x - previousScreen.x,
        dy = screen.y - previousScreen.y,
        length = dx * dx + dy * dy;
      const t = length
        ? Math.max(
            0,
            Math.min(
              1,
              ((click.x - previousScreen.x) * dx + (click.y - previousScreen.y) * dy) / length,
            ),
          )
        : point.pts === previous.pts
          ? 0
          : Math.max(0, Math.min(1, (currentPts - previous.pts) / (point.pts - previous.pts)));
      const x = previousScreen.x + t * dx,
        y = previousScreen.y + t * dy,
        distance = Math.hypot(click.x - x, click.y - y);
      const pts = previous.pts + t * (point.pts - previous.pts),
        timeDistance = Math.abs(pts - currentPts);
      if (distance <= tolerance)
        candidates.push({
          pts,
          distance,
          timeDistance,
          x,
          y,
          start: previous,
          end: point,
          fraction: t,
        });
    }
    previous = point;
    previousScreen = screen;
  }
  const closest = candidates.reduce((distance, c) => Math.min(distance, c.distance), Infinity);
  return candidates
    .filter((c) => c.distance <= closest + 0.5)
    .reduce((best, c) => (!best || c.timeDistance < best.timeDistance ? c : best), null);
}
