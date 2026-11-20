// The result view's start camera from a request's captured view (config.export.view).
import { LocalFrame, nearLon } from '../select/geometry.js';

export const VIEW_DISTANCE = [1, 8]; // allowed camera distance from the target, x bounds radius

// view: {camera: {lat, lon, alt_amsl}, fov_deg}; origin: frames.json origin; target: ENU;
// radius: dataset.bounds.radius. Returns {camera, note} (note: what was adjusted, or null).
export function viewCamera(view, origin, target, radius) {
  const local = new LocalFrame(origin.lat, origin.lon, origin.alt_amsl);
  // a view across the antimeridian stays near the origin
  const lon = nearLon(view.camera.lon, origin.lon);
  let position = local.toEnu(view.camera.lat, lon, view.camera.alt_amsl);
  let note = null;
  const d = position.map((x, a) => x - target[a]);
  const dist = Math.hypot(...d);
  if (!(Number.isFinite(radius) && radius > 0)) {
    note = `scene radius ${radius} is not usable: the captured view's distance is not checked`;
  } else if (dist === 0) {
    // no direction to keep: straight above the target at the nearest allowed distance
    position = [target[0], target[1], target[2] + VIEW_DISTANCE[0] * radius];
    note = 'the captured view is at the target; starting above it';
  } else {
    const [lo, hi] = VIEW_DISTANCE.map((k) => k * radius);
    const want = Math.min(hi, Math.max(lo, dist));
    if (want !== dist) {
      position = target.map((t, a) => t + (d[a] * want) / dist);
      note = `the captured view is ${dist.toFixed(0)} m from the target; moved to ${want.toFixed(0)} m`;
    }
  }
  return { camera: { position, target: [...target], fov_deg: view.fov_deg }, note };
}
