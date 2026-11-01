import { finite } from './time.js';
import { freshMapPosition } from '../map/map-config.js';

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
    selected = [];
  const commit = () => {
    // Retain short valid bursts too. At the hard display budget omit whole
    // segments rather than joining across a missing-data interval.
    const points = [first, ...selected, last].filter((p, i, a) => p && (i === 0 || p !== a[i - 1]));
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
      utcUs: record.pose_utc_us ?? record.utc_us,
      clockSegment: segment,
    };
    if (last && !connectedTrackPoints(last, point)) {
      commit();
      point.clockSegment = segment;
    }
    if (!first) first = point;
    last = point;
    if (i % stride === 0 || i === frames.length - 1) selected.push(point);
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
