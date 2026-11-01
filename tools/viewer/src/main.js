import './ui/style.css';
import { layout } from './ui/layout.js';
import { LocalSource } from './media/local-source.js';
import { FrameDecoder } from './media/native-decoder.js';
import { selectLogAt } from './domain/flight-log.js';
import { ReviewMap } from './map/map-view.js';
import { ElevationModel, validPose } from './domain/geometry.js';
import {
  PoseTimeline,
  correctedUtc,
  validateTiePoints,
  frameAt,
  utcText,
  finite,
  rebaseReviewTime,
} from './domain/time.js';
import {
  calibration,
  celsius,
  stretch,
  colorize,
  thermalPeaks,
  rawPixel,
} from './domain/thermal.js';

document.querySelector('#app').innerHTML = layout();
const $ = (id) => document.getElementById(id);
const elevation = new ElevationModel(),
  poseTimeline = new PoseTimeline();
const state = {
  source: null,
  file: null,
  decoder: null,
  frames: [],
  frame: null,
  decoded: null,
  decoding: false,
  number: 0,
  origin: 0,
  pendingReviewSave: false,
  playing: false,
  playEpoch: 0,
  closed: false,
  generation: 0,
  seekGeneration: 0,
  controller: null,
  dirtyPose: true,
  findings: [],
  peaks: [],
  cursor: null,
  image: null,
  offset: 0,
  anchors: [],
  rotation: 0,
  fovOverride: null,
  indexed: false,
  indexPercent: 0,
  indexFailed: false,
  requestedNumber: null,
  requestedFrameId: null,
  initialFrameId: null,
  warnings: [],
  timeQuality: null,
  log: null,
  visibleLog: null,
  logRenderKey: null,
  logWorker: null,
  lastTrack: 0,
  renderMs: 0,
  config: { ...window.VIEWER_CONFIG },
};
let map = null;
let terrainRefresh = null;
function terrainChanged() {
  if (terrainRefresh !== null) return;
  terrainRefresh = setTimeout(() => {
    terrainRefresh = null;
    if (state.closed) return;
    updateTelemetry();
    if (state.log) refreshLog(true);
  }, 60);
}
function error(value) {
  const message = value?.message || String(value);
  $('error-text').textContent = message;
  $('error-banner').hidden = false;
  console.error(value);
}
function status(text) {
  $('status').textContent = text;
}
function decoderStatus(text) {
  $('decoder-status').textContent = text;
}
try {
  map = new ReviewMap($('map'), error, terrainChanged, seekFromTrack);
  elevation.terrain = map.terrain;
  map
    .configure(state.config)
    .then((applied) => {
      if (applied) {
        state.config = { ...map.configuration };
        updateTelemetry();
      }
    })
    .catch((e) => {
      state.config = { ...map.configuration };
      error(e);
    });
} catch (e) {
  error(new Error(`Map unavailable: ${e.message}. Video review remains available.`));
}
const storage = {
  get(key, fallback) {
    try {
      return JSON.parse(localStorage.getItem(key)) ?? fallback;
    } catch {
      return fallback;
    }
  },
  set(key, value) {
    try {
      localStorage.setItem(key, JSON.stringify(value));
    } catch {
      status('Browser storage unavailable; export findings before closing.');
    }
  },
};
function fileKey() {
  return state.file
    ? `fire-review:${state.file.name}:${state.file.size}:${state.file.lastModified}`
    : '';
}
function nameOf(record, key) {
  const value = record?.[key];
  return finite(value) ? value.toFixed(2) : '—';
}
function currentRecord(frame = state.frame) {
  if (!frame?.telemetry) return null;
  const recordedFov = frame.thermalMetadata
    ? frame.thermalMetadata.hfov_deg
    : frame.telemetry.hfov_deg;
  return {
    ...frame.telemetry,
    hfov_deg:
      state.fovOverride ??
      (finite(recordedFov) && recordedFov > 0 && recordedFov < 179 ? recordedFov : null),
    hfov_source:
      state.fovOverride !== null
        ? 'user override'
        : frame.thermalMetadata
          ? 'thermal metadata'
          : 'video telemetry',
  };
}
function alignedRecord(frame = state.frame) {
  const record = currentRecord(frame);
  if (!record) return null;
  if (state.offset === 0 && !state.anchors.length) return record;
  if (state.dirtyPose) {
    poseTimeline.rebuild(state.frames);
    state.dirtyPose = false;
  }
  const seconds = frame.pts - state.origin,
    utc = correctedUtc(seconds, record.utc_us, state.offset, state.anchors);
  const result = poseTimeline.at(utc, record, state.offset === 0 && !state.anchors.length);
  return result ? { ...result, hfov_deg: record.hfov_deg, hfov_source: record.hfov_source } : null;
}
function updateVideoTrack() {
  if (!map) return;
  const generation = state.generation;
  $('map-track-hint').textContent = 'Updating video track…';
  const pending = map.setVideoTrack(state.frames, (frame) => alignedRecord(frame));
  const controller = map.trackController;
  pending
    .then((applied) => {
      if (!applied || generation !== state.generation || controller !== map.trackController) return;
      $('map-track-hint').textContent = map.trackLimited
        ? 'Track simplified; click visible segments to seek'
        : 'Click the video track to seek';
    })
    .catch(error);
}
async function seekFromTrack(pts) {
  if (!finite(pts) || !state.frames.length) return;
  pause();
  let number = frameAt(state.frames, pts - state.origin, state.origin);
  if (
    state.frames[number + 1] &&
    Math.abs(state.frames[number + 1].pts - pts) < Math.abs(state.frames[number].pts - pts)
  )
    number++;
  const generation = state.generation,
    frameId = state.frames[number].id;
  await showFrame(number);
  if (generation === state.generation && state.frame?.id === frameId)
    status(`Video track · ${clock(state.frame.pts - state.origin)}`);
}
function field(name, value) {
  const dt = document.createElement('dt'),
    dd = document.createElement('dd');
  dt.textContent = name;
  dd.textContent = value;
  return [dt, dd];
}
function clock(seconds) {
  if (!finite(seconds) || seconds < 0) return '00:00.000';
  const h = Math.floor(seconds / 3600),
    m = Math.floor(seconds / 60) % 60,
    s = (seconds % 60).toFixed(3).padStart(6, '0');
  return `${h ? `${h}:` : ''}${String(m).padStart(2, '0')}:${s}`;
}
function frameDuration(frame) {
  return finite(frame?.duration) && frame.duration > 0
    ? frame.duration
    : state.frames[1]
      ? Math.max(0.001, state.frames[1].pts - state.frames[0].pts)
      : 1 / 30;
}
function extent() {
  const last = state.frames.at(-1);
  return last ? last.pts - state.origin + frameDuration(last) : 0;
}
function videoIndexSummary() {
  const format = state.source?.format?.toUpperCase() || 'VIDEO';
  const percent = !state.indexed || state.indexFailed ? `${state.indexPercent}% · ` : '';
  $('video-format').textContent =
    `${format} · ${percent}${state.frames.length.toLocaleString()} frames${state.indexFailed ? ' · incomplete' : ''}`;
}
function progress(batch) {
  const p = batch.progress || {};
  const total = p.fileSize || p.totalBytes || state.file?.size || 1;
  // Readers skip compressed payloads and may reread cached metadata. Their
  // cumulative I/O byte count is not the position reached through the file.
  const position = finite(p.position) ? p.position : finite(p.bytesRead) ? p.bytesRead : 0;
  state.indexPercent = Math.max(
    state.indexPercent,
    Math.min(99, Math.max(0, Math.floor((position / total) * 100))),
  );
  $('index-progress').hidden = false;
  $('index-progress').value = state.indexPercent;
}
function inspectClockLabels(frames) {
  const warn = (text) => {
    if (!state.warnings.includes(text)) state.warnings.push(text);
  };
  for (const frame of [...frames].sort((a, b) => a.pts - b.pts)) {
    const utc = frame.telemetry?.utc_us;
    if (!finite(utc)) continue;
    const old = state.timeQuality;
    if (old && frame.pts <= old.pts) continue;
    if (old && utc < old.utc) warn('Recorded UTC moves backwards; check time alignment');
    const since = old && utc === old.utc ? old.since : frame.pts;
    if (frame.pts - since > 1.5)
      warn('UTC labels repeat across multiple playback seconds; time anchors may be needed');
    state.timeQuality = { pts: frame.pts, utc, since };
  }
}
function saveReview() {
  if (state.file && !finite(state.origin)) {
    state.pendingReviewSave = true;
    return;
  }
  state.pendingReviewSave = false;
  if (state.file)
    storage.set(fileKey(), {
      schema: 'fire.review.session.v1',
      findings: state.findings,
      offset: state.offset,
      anchors: state.anchors,
      rotation: state.rotation,
      fovOverride: state.fovOverride,
      timelineVersion: 2,
      timelineOrigin: state.origin,
    });
}
function pause() {
  state.playEpoch++;
  state.playing = false;
  $('play').textContent = 'Play';
  $('play').setAttribute('aria-label', 'Play video');
}
async function openVideo(file) {
  if (!file) return;
  pause();
  state.generation++;
  const own = state.generation;
  state.controller?.abort();
  state.decoder?.close();
  state.source?.close();
  state.decoder = null;
  state.source = null;
  state.file = file;
  state.origin = null;
  state.pendingReviewSave = false;
  $('anchors-button').disabled = true;
  state.frames = [];
  state.frame = null;
  state.decoded = null;
  state.number = 0;
  state.cursor = null;
  state.peaks = [];
  state.image = null;
  state.indexed = false;
  state.indexPercent = 0;
  state.indexFailed = false;
  state.requestedNumber = null;
  state.requestedFrameId = null;
  state.initialFrameId = null;
  state.decoding = false;
  state.warnings = [];
  state.timeQuality = null;
  state.dirtyPose = true;
  $('palette').value = 'Original';
  $('stretch').value = 'auto';
  $('hover-reading').textContent = 'Hover over a frame to inspect a pixel.';
  $('peak-list').replaceChildren();
  $('telemetry-fields').replaceChildren(...field('Recorded UTC', '—'), ...field('Position', '—'));
  state.seekGeneration++;
  map?.clear();
  $('video-canvas').hidden = true;
  $('empty-video').hidden = false;
  $('source-name').textContent = file.name;
  $('video-format').textContent = `${(file.size / 1024 / 1024).toFixed(1)} MB · 0% · 0 frames`;
  $('error-banner').hidden = true;
  for (const id of ['seek', 'previous', 'next', 'play', 'mark-finding', 'scan-peaks'])
    $(id).disabled = true;
  const saved = storage.get(fileKey(), {});
  state.findings = Array.isArray(saved.findings)
    ? saved.findings
        .filter(
          (f) =>
            f &&
            typeof f.id === 'string' &&
            typeof f.label === 'string' &&
            finite(f.videoSeconds) &&
            (f.videoSeconds >= 0 || f.presentationUnavailable === true) &&
            finite(f.rawPixel?.x) &&
            finite(f.rawPixel?.y) &&
            Number.isInteger(f.rawPixel.x) &&
            f.rawPixel.x >= 0 &&
            Number.isInteger(f.rawPixel.y) &&
            f.rawPixel.y >= 0 &&
            (!f.ground || [f.ground.lat, f.ground.lon, f.ground.altMsl].every(finite)),
        )
        .map((f) => ({ ...f, temperatureC: finite(f.temperatureC) ? f.temperatureC : null }))
    : [];
  state.offset = finite(saved.offset) ? saved.offset : 0;
  try {
    state.anchors = validateTiePoints(saved.anchors || []);
  } catch {
    state.anchors = [];
  }
  state.fovOverride =
    finite(saved.fovOverride) && saved.fovOverride > 0 && saved.fovOverride < 179
      ? saved.fovOverride
      : null;
  state.rotation = saved.rotation === 180 ? 180 : 0;
  $('rotation').value = String(state.rotation);
  $('offset').value = state.offset;
  renderFindings();
  status(`Opening ${file.name} locally…`);
  let restoredOrigin = false;
  $('index-progress').hidden = false;
  $('index-progress').value = 0;
  state.source = new LocalSource(file, {
    onStatus: (text) => {
      if (own === state.generation) status(text);
    },
    onBatch: (batch) => {
      if (own !== state.generation) return;
      state.frames.push(...batch.frames);
      state.frames.sort((a, b) => a.pts - b.pts || a.id - b.id);
      if (state.frame)
        state.number = Math.max(
          0,
          state.frames.findIndex((f) => f.id === state.frame.id),
        );
      if (state.requestedFrameId !== null)
        state.requestedNumber = state.frames.findIndex((f) => f.id === state.requestedFrameId);
      state.dirtyPose = true;
      if (!state.decoder && state.frames.length) {
        const track = state.source.tracks.find((t) => t.id === state.frames[0].trackId);
        state.origin = finite(track?.presentationStart) ? 0 : state.frames[0].pts;
        if (!restoredOrigin && finite(track?.presentationStart)) {
          restoredOrigin = true;
          const oldOrigin =
            saved.timelineVersion === 2 && finite(saved.timelineOrigin)
              ? saved.timelineOrigin
              : state.source.frames.reduce((min, frame) => Math.min(min, frame.pts), Infinity);
          if (finite(oldOrigin) && (state.findings.length || state.anchors.length)) {
            const review = rebaseReviewTime(
              state.findings,
              state.anchors,
              oldOrigin,
              state.origin,
              track.presentationEnd,
              track.presentationStart,
            );
            state.findings = review.findings;
            state.anchors = review.anchors;
            if (Math.abs(oldOrigin - state.origin) > 1e-8)
              state.warnings.push('Saved review times updated to the MP4 presentation timeline');
            saveReview();
            renderFindings();
          }
        }
        $('anchors-button').disabled = false;
        if (state.pendingReviewSave) saveReview();
      }
      for (const warning of batch.warnings || [])
        if (!state.warnings.includes(warning)) state.warnings.push(warning);
      inspectClockLabels(batch.frames);
      progress(batch);
      $('seek').max = extent();
      $('duration').textContent = clock(extent());
      if (state.frame)
        $('frame-number').textContent =
          `Frame ${(state.number + 1).toLocaleString()} / ${state.frames.length.toLocaleString()}${state.indexed ? '' : '+'}`;
      videoIndexSummary();
      if (!state.decoder && state.frames.length) {
        state.decoder = new FrameDecoder(file, state.source, decoderStatus);
      }
      for (const id of ['seek', 'previous', 'next']) $(id).disabled = !state.frames.length;
      if (state.decoder && state.initialFrameId === null && !state.frame && !state.decoding) {
        const first = state.frames.findIndex((frame) => frame.decodable !== false);
        if (first >= 0) {
          state.initialFrameId = state.frames[first].id;
          if (
            first > 0 &&
            !state.warnings.includes(
              'Opening at the first decodable frame; earlier frames lack a keyframe',
            )
          )
            state.warnings.push(
              'Opening at the first decodable frame; earlier frames lack a keyframe',
            );
          showFrame(first).catch(error);
        }
      }
      if (performance.now() - state.lastTrack > 1200) {
        state.lastTrack = performance.now();
        updateVideoTrack();
      }
      status(
        `Indexing locally · ${state.indexPercent}% · ${state.frames.length.toLocaleString()} frames available${state.warnings.length ? ' · ' + state.warnings.at(-1) : ''}`,
      );
    },
    onDone: () => {
      if (own !== state.generation) return;
      state.indexed = true;
      state.indexPercent = 100;
      $('index-progress').value = 100;
      $('index-progress').hidden = true;
      videoIndexSummary();
      if (!state.frames.length)
        error(new Error('No video frames are visible in the recording’s presentation interval.'));
      else if (!state.frames.some((frame) => frame.decodable !== false))
        error(new Error('No decodable keyframe was found in this recording.'));
      updateVideoTrack();
      $('seek').max = extent();
      $('duration').textContent = clock(extent());
      if (state.frame)
        $('frame-number').textContent =
          `Frame ${(state.number + 1).toLocaleString()} / ${state.frames.length.toLocaleString()}${state.indexed ? '' : '+'}`;
      status(
        `100% · ${state.frames.length.toLocaleString()} frames indexed · ${state.warnings.length ? state.warnings.join('; ') : 'ready for review'}`,
      );
    },
    onError: (e) => {
      if (own === state.generation) {
        $('index-progress').hidden = true;
        state.indexed = true;
        state.indexFailed = true;
        videoIndexSummary();
        if (state.frame)
          $('frame-number').textContent =
            `Frame ${state.number + 1} / ${state.frames.length} · incomplete recording`;
        error(e);
        if (state.frames.length) status('Playable prefix retained; file indexing stopped.');
      }
    },
  });
}
async function showFrame(number) {
  if (!state.decoder || !state.frames.length) return;
  number = Math.max(0, Math.min(state.frames.length - 1, number));
  if (state.frames[number].decodable === false) {
    const next = state.frames.findIndex((frame, i) => i >= number && frame.decodable !== false);
    if (next < 0) {
      status('Waiting for a keyframe; choose a later frame when indexing advances.');
      return;
    }
    number = next;
  }
  state.requestedNumber = number;
  state.requestedFrameId = state.frames[number].id;
  const own = state.generation,
    seek = ++state.seekGeneration,
    frame = state.frames[number];
  state.controller?.abort();
  const controller = new AbortController();
  state.controller = controller;
  state.decoding = true;
  const started = performance.now();
  try {
    const decoded = await state.decoder.decode(frame, { signal: controller.signal });
    if (own !== state.generation || seek !== state.seekGeneration) return;
    state.number = Math.max(
      0,
      state.frames.findIndex((f) => f.id === frame.id),
    );
    state.requestedNumber = null;
    state.requestedFrameId = null;
    state.frame = frame;
    state.decoded = decoded;
    state.renderMs = performance.now() - started;
    state.peaks = [];
    if (decoded.kind === 'gray16' && $('palette').value === 'Original')
      $('palette').value = 'White hot';
    const seconds = frame.pts - state.origin;
    $('seek').value = seconds;
    $('elapsed').textContent = clock(seconds);
    $('frame-number').textContent =
      `Frame ${(state.number + 1).toLocaleString()} / ${state.frames.length.toLocaleString()}${state.indexed ? '' : '+'}`;
    $('empty-video').hidden = true;
    $('video-canvas').hidden = false;
    for (const id of ['seek', 'previous', 'next', 'play', 'mark-finding']) $(id).disabled = false;
    $('scan-peaks').disabled = decoded.kind !== 'gray16';
    $('stretch').disabled = decoded.kind !== 'gray16' || !calibration(frame.thermalMetadata);
    syncContrastControls();
    if (state.cursor && (state.cursor.x >= decoded.width || state.cursor.y >= decoded.height))
      state.cursor = null;
    for (const [id, max] of [
      ['pixel-x', decoded.width - 1],
      ['pixel-y', decoded.height - 1],
    ]) {
      $(id).disabled = false;
      $(id).max = max;
    }
    $('pixel-x').value = state.cursor?.x ?? Math.floor(decoded.width / 2);
    $('pixel-y').value = state.cursor?.y ?? Math.floor(decoded.height / 2);
    state.decoding = false;
    decoderStatus(`${state.decoder.mode} · ${Math.round(state.renderMs)} ms/frame`);
    renderImageSafely();
    updateTelemetry();
    renderPeaks();
  } catch (e) {
    if (e.name !== 'AbortError' && own === state.generation && seek === state.seekGeneration) {
      pause();
      state.requestedNumber = null;
      state.requestedFrameId = null;
      state.decoding = false;
      error(e);
    }
  }
}
function syncContrastControls() {
  const enabled =
    state.decoded?.kind === 'gray16' &&
    !!calibration(state.frame?.thermalMetadata) &&
    $('stretch').value === 'fixed';
  $('stretch-low').disabled = $('stretch-high').disabled = !enabled;
}
function renderImageSafely() {
  try {
    makeImage();
  } catch (e) {
    $('stretch').value = 'auto';
    $('palette').value = state.decoded?.kind === 'gray16' ? 'White hot' : 'Original';
    syncContrastControls();
    try {
      makeImage();
    } catch (fatal) {
      state.image = null;
      $('video-canvas').hidden = true;
      throw fatal;
    }
    error(new Error(`Display range reset to auto: ${e.message}`));
  }
}
function makeImage() {
  const d = state.decoded;
  if (!d) return;
  let rgba;
  if (d.kind === 'gray16') {
    const fixed =
      $('stretch').value === 'fixed'
        ? [Number($('stretch-low').value), Number($('stretch-high').value)]
        : null;
    rgba = colorize(
      d.data,
      $('palette').value === 'Original' ? 'White hot' : $('palette').value,
      stretch(d.data, fixed, state.frame.thermalMetadata),
    );
  } else if ($('palette').value !== 'Original') {
    const intensity = new Uint16Array(d.width * d.height);
    for (let i = 0; i < intensity.length; i++)
      intensity[i] = Math.round(
        0.2126 * d.data[i * 4] + 0.7152 * d.data[i * 4 + 1] + 0.0722 * d.data[i * 4 + 2],
      );
    rgba = colorize(intensity, $('palette').value, [0, 255]);
  } else rgba = d.data;
  const image = new OffscreenCanvas(d.width, d.height);
  image
    .getContext('2d')
    .putImageData(new ImageData(new Uint8ClampedArray(rgba), d.width, d.height), 0, 0);
  state.image = image;
  paint();
}
function paint() {
  if (!state.image) return;
  const canvas = $('video-canvas'),
    d = state.decoded;
  if (canvas.width !== d.width || canvas.height !== d.height) {
    canvas.width = d.width;
    canvas.height = d.height;
  }
  const ctx = canvas.getContext('2d');
  ctx.save();
  if (state.rotation === 180) {
    ctx.translate(d.width, d.height);
    ctx.rotate(Math.PI);
  }
  ctx.drawImage(state.image, 0, 0);
  for (const [i, p] of state.peaks.entries()) {
    ctx.strokeStyle = '#ffe66d';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(p.x, p.y, 7, 0, Math.PI * 2);
    ctx.stroke();
    ctx.fillStyle = '#ffe66d';
    ctx.font = '12px sans-serif';
    ctx.fillText(String(i + 1), p.x + 9, p.y - 5);
  }
  if (state.cursor) {
    const p = state.cursor;
    ctx.strokeStyle = '#67ffdf';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(p.x - 7, p.y);
    ctx.lineTo(p.x + 7, p.y);
    ctx.moveTo(p.x, p.y - 7);
    ctx.lineTo(p.x, p.y + 7);
    ctx.stroke();
  }
  ctx.restore();
}
function namedValue(record, name) {
  const item = record?.named_values?.[name];
  if (finite(item)) return item;
  if (finite(item?.value) && item.valid !== false) return item.value;
  return null;
}
function updateTelemetry() {
  const record = currentRecord(),
    pose = alignedRecord(),
    p = pose?.position,
    g = pose?.gimbal_attitude,
    a = pose?.vehicle_attitude;
  const rows = [];
  const add = (k, v) => rows.push(...field(k, v));
  add('Recorded UTC', utcText(record?.utc_us));
  const selectedUtc =
    state.offset === 0 && !state.anchors.length
      ? (record?.pose_utc_us ?? record?.utc_us)
      : correctedUtc(state.frame?.pts - state.origin, record?.utc_us, state.offset, state.anchors);
  add('Pose UTC', utcText(selectedUtc));
  add(
    'Position',
    p ? `${(p.lat_e7 / 1e7).toFixed(7)}, ${(p.lon_e7 / 1e7).toFixed(7)}` : 'Unavailable',
  );
  add('Altitude AMSL', finite(p?.alt_amsl_m) ? `${p.alt_amsl_m.toFixed(1)} m` : '—');
  add(
    'Camera R / P / Y',
    g
      ? [g.roll_rad, g.pitch_rad, g.yaw_rad]
          .map((x) => (finite(x) ? ((x * 180) / Math.PI).toFixed(1) : '—'))
          .join(' / ') + '°'
      : 'Unavailable',
  );
  add(
    'Aircraft R / P / Y',
    a
      ? [a.roll_rad, a.pitch_rad, a.yaw_rad]
          .map((x) => (finite(x) ? ((x * 180) / Math.PI).toFixed(1) : '—'))
          .join(' / ') + '°'
      : 'Unavailable',
  );
  add(
    'Horizontal FOV',
    finite(record?.hfov_deg) ? `${record.hfov_deg.toFixed(2)}°` : 'Unavailable',
  );
  let low = namedValue(record, 'TMIN'),
    high = namedValue(record, 'TMAX');
  if (state.decoded?.kind === 'gray16') {
    let min = 65535,
      max = 0;
    for (const x of state.decoded.data) {
      min = Math.min(min, x);
      max = Math.max(max, x);
    }
    const lo = celsius(min, state.frame.thermalMetadata),
      hi = celsius(max, state.frame.thermalMetadata);
    add(
      'Frame min / max',
      lo !== null ? `${lo.toFixed(2)} / ${hi.toFixed(2)} °C` : `${min} / ${max} raw`,
    );
  }
  if (low !== null || high !== null)
    add(
      'TMIN / TMAX (log)',
      `${low === null ? '—' : low.toFixed(1)} / ${high === null ? '—' : high.toFixed(1)} °C`,
    );
  const lidar = namedValue(record, 'SIYI_LIDR');
  if (lidar !== null) add('Lidar range', `${lidar.toFixed(1)} m`);
  $('telemetry-fields').replaceChildren(...rows);
  const basis =
    record?.time_alignment?.exposure_alignment ||
    record?.time_alignment?.basis ||
    record?.provenance?.format;
  $('timing-note').textContent = state.anchors.length
    ? `Time anchors active (${state.anchors.length}) plus telemetry offset ${state.offset.toFixed(1)}s; outside the anchor interval pose is unavailable.`
    : basis
      ? `Timing: ${basis}`
      : record
        ? 'Embedded telemetry; verify camera alignment before interpreting ground coordinates.'
        : 'No embedded telemetry in this frame.';
  let footprint = null;
  if (state.decoded) {
    const { width, height } = state.decoded;
    footprint = elevation.footprint(pose, width, height);
    map?.update(pose, footprint, state.frame.pts);
  }
  $('map-note').textContent =
    `${footprint?.reason ? footprint.reason + ' ' : ''}Ground picking: ${elevation.description}. Nominal FOV, pose and timing limit accuracy.`;
  if (state.cursor) inspectPixel(state.cursor);
  refreshLog();
}
function displayPoint(event) {
  const d = state.decoded;
  if (!d) return null;
  const rect = $('video-canvas').getBoundingClientRect(),
    scale = Math.min(rect.width / d.width, rect.height / d.height),
    x = (event.clientX - rect.left - (rect.width - d.width * scale) / 2) / scale,
    y = (event.clientY - rect.top - (rect.height - d.height * scale) / 2) / scale;
  return rawPixel(x, y, d.width, d.height, state.rotation);
}
function inspectPixel(point) {
  if (!point || !state.decoded) return;
  const d = state.decoded,
    raw = d.kind === 'gray16' ? d.data[point.y * d.width + point.x] : null,
    temp = raw !== null ? celsius(raw, state.frame.thermalMetadata) : null;
  const projection = elevation.project(alignedRecord(), point.x, point.y, d.width, d.height);
  const ground = projection.point;
  map?.setHover(ground);
  const level = ground?.terrain?.terrainLevel;
  const detail = Number.isInteger(level)
    ? ` · terrain L${level}${level < 12 ? ' (coarse)' : ''}`
    : '';
  $('hover-reading').textContent =
    `Raw pixel ${point.x}, ${point.y}${temp !== null ? ` · ${temp.toFixed(2)} °C` : raw !== null ? ` · ${raw} raw` : ' · display image (no calibrated temperature)'}${ground ? `\n${ground.lat.toFixed(7)}, ${ground.lon.toFixed(7)} (approx.)${detail}` : `\n${projection.status === 'loading' ? projection.reason : `Ground position unavailable: ${projection.reason}`}`}`;
}
function renderPeaks() {
  const nodes = state.peaks.map((p, i) => {
    const li = document.createElement('li'),
      button = document.createElement('button'),
      temp = celsius(p.value, state.frame?.thermalMetadata);
    button.textContent = `${i + 1}. ${p.x}, ${p.y} · ${temp === null ? p.value + ' raw' : temp.toFixed(2) + ' °C'} · contrast +${p.contrast.toFixed(1)} raw`;
    button.onclick = () => {
      state.cursor = { x: p.x, y: p.y };
      paint();
      inspectPixel(state.cursor);
    };
    li.append(button);
    return li;
  });
  $('peak-list').replaceChildren(...nodes);
}
async function markFinding() {
  if (!state.frame || !state.decoded || state.markingGeneration === state.generation) return;
  const generation = state.generation;
  const d = state.decoded,
    point = state.cursor || { x: Math.floor(d.width / 2), y: Math.floor(d.height / 2) },
    raw = d.kind === 'gray16' ? d.data[point.y * d.width + point.x] : null,
    record = currentRecord(),
    pose = alignedRecord();
  const finding = {
    id: crypto.randomUUID(),
    label: `F${String(1 + Math.max(0, ...state.findings.map((f) => Number(/^F(\d+)$/.exec(f.label)?.[1]) || 0))).padStart(3, '0')}`,
    source: { name: state.file.name, size: state.file.size, lastModified: state.file.lastModified },
    frameId: state.frame.id,
    frameNumber: state.number,
    originalPts: state.frame.pts,
    videoSeconds: state.frame.pts - state.origin,
    recordedUtcUs: record?.utc_us ?? null,
    poseUtcUs:
      state.offset === 0 && !state.anchors.length
        ? (record?.pose_utc_us ?? record?.utc_us)
        : correctedUtc(state.frame.pts - state.origin, record?.utc_us, state.offset, state.anchors),
    rawPixel: { ...point },
    rawValue: raw,
    temperatureC: raw === null ? null : celsius(raw, state.frame.thermalMetadata),
    displayRotationDegrees: state.rotation,
    projectionRawRotationDegrees: 0,
    clockQualityWarnings: [...state.warnings],
    metadataRotationDegrees:
      state.frame.thermalMetadata?.rotation_deg ?? record?.rotation_deg ?? null,
    alignment: { offsetSeconds: state.offset, tiePoints: state.anchors.map((p) => ({ ...p })) },
    ground: null,
    groundUnavailableReason: null,
    calibration: state.frame.thermalMetadata
      ? {
          scaleK: state.frame.thermalMetadata.temperature_scale_k,
          offsetK: state.frame.thermalMetadata.temperature_offset_k,
          sourceSha256: state.frame.thermalMetadata.source_sha256,
        }
      : null,
    cameraPose: pose
      ? {
          position: pose.position,
          vehicle_attitude: pose.vehicle_attitude,
          gimbal_attitude: pose.gimbal_attitude,
          hfov_deg: pose.hfov_deg,
        }
      : null,
    fovSource: record?.hfov_source ?? 'unavailable',
    elevation: elevation.terrain?.provenance?.() ?? { source: 'Terrain not configured' },
    quality: 'Unconfirmed observation; nominal geometry and clock/elevation uncertainty apply',
    createdAt: new Date().toISOString(),
  };
  state.markingGeneration = generation;
  $('mark-finding').disabled = true;
  try {
    let projection = elevation.project(pose, point.x, point.y, d.width, d.height);
    if (projection.status === 'loading') {
      status('Resolving terrain for the marked pixel…');
      projection = await elevation.resolve(pose, point.x, point.y, d.width, d.height);
    }
    if (state.generation !== generation) return;
    finding.ground = projection.point;
    finding.groundUnavailableReason = projection.reason;
    finding.label = `F${String(1 + Math.max(0, ...state.findings.map((f) => Number(/^F(\d+)$/.exec(f.label)?.[1]) || 0))).padStart(3, '0')}`;
    state.findings.push(finding);
  } finally {
    if (state.generation === generation) {
      state.markingGeneration = null;
      $('mark-finding').disabled = false;
    }
  }
  saveReview();
  renderFindings();
  status(`Marked ${finding.label} at raw pixel ${point.x}, ${point.y}`);
}
function renderFindings() {
  $('finding-count').textContent = state.findings.length;
  $('export-findings').disabled = !state.findings.length;
  const nodes = state.findings.map((f) => {
    const li = document.createElement('li'),
      button = document.createElement('button'),
      remove = document.createElement('button');
    button.textContent = `${f.label} · ${f.presentationUnavailable ? 'outside clip' : clock(f.videoSeconds)} · ${f.temperatureC === null ? 'no pixel temperature' : f.temperatureC.toFixed(2) + ' °C'}`;
    button.disabled = f.presentationUnavailable === true;
    button.title = f.note || 'Review this observation';
    button.onclick = () => {
      pause();
      const generation = state.generation;
      showFrame(frameAt(state.frames, f.videoSeconds, state.origin))
        .then(() => {
          if (generation !== state.generation || !state.decoded) return;
          if (f.rawPixel.x >= state.decoded.width || f.rawPixel.y >= state.decoded.height) return;
          state.cursor = { ...f.rawPixel };
          paint();
          inspectPixel(state.cursor);
        })
        .catch(error);
    };
    remove.textContent = '×';
    remove.className = 'delete';
    remove.setAttribute('aria-label', `Delete ${f.label}`);
    remove.onclick = () => {
      state.findings = state.findings.filter((x) => x.id !== f.id);
      saveReview();
      renderFindings();
    };
    const note = document.createElement('input');
    note.className = 'finding-note';
    note.type = 'text';
    note.maxLength = 2000;
    note.placeholder = 'Add a note';
    note.value = f.note || '';
    note.setAttribute('aria-label', `Note for ${f.label}`);
    note.onchange = () => {
      f.note = note.value;
      saveReview();
      button.title = f.note || 'Review this observation';
    };
    li.append(button, remove, note);
    return li;
  });
  if (!nodes.length) {
    const li = document.createElement('li');
    li.className = 'muted';
    li.textContent = 'Marked observations will appear here.';
    nodes.push(li);
  }
  $('finding-list').replaceChildren(...nodes);
  map?.setFindings(state.findings);
}
function download(name, data) {
  const blob = new Blob([JSON.stringify(data, null, 2) + '\n'], { type: 'application/json' }),
    url = URL.createObjectURL(blob),
    a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
function exportFindings(geojson) {
  const common = {
    schema: 'fire.review.findings.v1',
    source: state.file
      ? { name: state.file.name, size: state.file.size, lastModified: state.file.lastModified }
      : null,
    exportedAt: new Date().toISOString(),
    quality: 'Review observations; not confirmed ignition detections',
  };
  const data = geojson
    ? {
        type: 'FeatureCollection',
        ...common,
        features: state.findings
          .filter((f) => f.ground)
          .map((f) => ({
            type: 'Feature',
            id: f.id,
            geometry: { type: 'Point', coordinates: [f.ground.lon, f.ground.lat] },
            properties: f,
          })),
        unmappedFindings: state.findings.filter((f) => !f.ground),
      }
    : { ...common, findings: state.findings };
  download(`fire-findings.${geojson ? 'geojson' : 'json'}`, data);
  $('export-dialog').close();
}
async function play() {
  if (!state.frames.length) return;
  if (state.playing) {
    pause();
    return;
  }
  state.playing = true;
  const playEpoch = ++state.playEpoch;
  $('play').textContent = 'Pause';
  $('play').setAttribute('aria-label', 'Pause video');
  const own = state.generation;
  while (state.playing && own === state.generation && playEpoch === state.playEpoch) {
    const start = performance.now(),
      duration = frameDuration(state.frame) / Number($('speed').value);
    if (state.number >= state.frames.length - 1) {
      if (state.indexed) {
        pause();
        break;
      }
      await new Promise((r) => setTimeout(r, 100));
      continue;
    }
    await showFrame((state.requestedNumber ?? state.number) + 1);
    if (!state.playing || playEpoch !== state.playEpoch) break;
    await new Promise((r) =>
      setTimeout(r, Math.max(0, duration * 1000 - (performance.now() - start))),
    );
  }
}
function refreshLog(force = false) {
  if (!state.log) return;
  const record = currentRecord(),
    utc = state.frame
      ? correctedUtc(state.frame.pts - state.origin, record?.utc_us, state.offset, state.anchors)
      : null;
  const visible =
    $('log-time-mode').value === 'latest'
      ? state.log
      : !finite(utc)
        ? {
            ...state.log,
            mission: { id: null, items: [], segments: [], complete: false, confirmed: false },
            fences: [],
            selectedFenceVersion: null,
            home: null,
            selection: {
              mode: 'at-time',
              utcUs: null,
              uncertain: ['Video time unavailable; choose Latest in log for an overview'],
            },
          }
        : selectLogAt(state.log, utc);
  state.visibleLog = visible;
  const key = [
    visible.mission?.id,
    visible.mission?.items?.length,
    visible.mission?.confirmed,
    visible.selectedFenceVersion?.id,
    visible.home?.sourceRef?.byteOffset,
  ].join('|');
  if (force || key !== state.logRenderKey) {
    map?.setLog(visible, elevation);
    state.logRenderKey = key;
  }
  const confidence = visible.mission?.items?.length
    ? visible.mission.complete
      ? visible.mission.confirmed
        ? 'confirmed'
        : 'unconfirmed'
      : 'partial'
    : 'no applicable plan';
  const uncertain =
    visible.selection?.utcUs === null
      ? ' · time unavailable'
      : visible.selection?.uncertain?.length
        ? ' · uncertain time'
        : '';
  $('log-summary').textContent =
    `${state.log.source?.name || 'Flight log'} · ${visible.mission?.items?.length || 0} mission items · ${visible.fences?.length || 0} fences · ${confidence}${uncertain}`;
  $('log-summary').title = $('log-summary').textContent;
}
function loadLog(file) {
  if (!file) return;
  state.logWorker?.terminate();
  const worker = new Worker(new URL('./domain/log.worker.js', import.meta.url), { type: 'module' });
  state.logWorker = worker;
  status(`Reading ${file.name} locally…`);
  worker.onmessage = ({ data }) => {
    if (worker !== state.logWorker) return;
    if (data.type === 'progress') {
      const p = data.progress;
      status(`Reading log · ${Math.round((100 * p.bytesRead) / p.totalBytes)}%`);
    }
    if (data.type === 'log') {
      state.log = data.log;
      state.logRenderKey = null;
      $('log-time-mode').disabled = false;
      refreshLog(true);
      status(
        `Flight log loaded${state.log.diagnostics?.length ? ' · ' + state.log.diagnostics.length + ' diagnostic types (available in Settings)' : ''}`,
      );
      worker.terminate();
      state.logWorker = null;
    }
    if (data.type === 'error') {
      error(data.message);
      worker.terminate();
      state.logWorker = null;
    }
  };
  worker.onerror = (e) => {
    if (worker === state.logWorker) error(new Error(e.message));
  };
  worker.postMessage({ file });
}
$('open-video').onclick = $('empty-open').onclick = () => $('video-file').click();
$('video-file').onchange = (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  openVideo(file).catch(error);
};
$('open-log').onclick = () => $('log-file').click();
$('log-file').onchange = (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  loadLog(file);
};
$('play').onclick = play;
$('previous').onclick = () => {
  pause();
  showFrame((state.requestedNumber ?? state.number) - 1);
};
$('next').onclick = () => {
  pause();
  showFrame((state.requestedNumber ?? state.number) + 1);
};
$('seek').oninput = (e) => {
  pause();
  showFrame(frameAt(state.frames, Number(e.target.value), state.origin));
};
for (const id of ['palette', 'stretch', 'stretch-low', 'stretch-high'])
  $(id).onchange = () => {
    try {
      syncContrastControls();
      renderImageSafely();
    } catch (e) {
      error(e);
    }
  };
$('rotation').onchange = (e) => {
  state.rotation = Number(e.target.value);
  saveReview();
  paint();
};
$('video-canvas').onpointermove = (e) => {
  const point = displayPoint(e);
  if (point) {
    $('pixel-x').value = point.x;
    $('pixel-y').value = point.y;
    state.cursor = point;
    inspectPixel(point);
    paint();
  } else map?.setHover(null);
};
$('video-canvas').onpointerleave = () => map?.setHover(null);
for (const id of ['pixel-x', 'pixel-y'])
  $(id).oninput = () => {
    if (!state.decoded) return;
    const x = Number($('pixel-x').value),
      y = Number($('pixel-y').value);
    if (
      !Number.isInteger(x) ||
      !Number.isInteger(y) ||
      x < 0 ||
      y < 0 ||
      x >= state.decoded.width ||
      y >= state.decoded.height
    )
      return;
    state.cursor = { x, y };
    paint();
    inspectPixel(state.cursor);
  };
$('mark-finding').onclick = markFinding;
$('scan-peaks').onclick = () => {
  if (state.decoded?.kind !== 'gray16') return;
  state.peaks = thermalPeaks(state.decoded.data, state.decoded.width, state.decoded.height);
  paint();
  renderPeaks();
};
$('offset').oninput = (e) => {
  const value = Number(e.target.value);
  if (!finite(value)) return;
  state.offset = value;
  updateVideoTrack();
  saveReview();
  updateTelemetry();
};
$('offset-reset').onclick = () => {
  state.offset = 0;
  $('offset').value = 0;
  updateVideoTrack();
  saveReview();
  updateTelemetry();
};
for (const mode of ['2d', '3d'])
  $(`map-${mode}`).onclick = () => {
    map?.setMode(mode);
    for (const m of ['2d', '3d']) {
      $(`map-${m}`).classList.toggle('selected', m === mode);
      $(`map-${m}`).setAttribute('aria-pressed', String(m === mode));
    }
  };
$('log-time-mode').onchange = () => refreshLog(true);
$('map-follow').onchange = (e) => map?.setFollow(e.target.checked);
$('map-center').onclick = () => map?.centerCurrent();
$('map-zoom-in').onclick = () => map?.zoom(true);
$('map-zoom-out').onclick = () => map?.zoom(false);
$('dismiss-error').onclick = () => ($('error-banner').hidden = true);
$('settings-button').onclick = () => {
  for (const [id, key] of [
    ['imagery', 'imagery'],
    ['google-key', 'googleMapsApiKey'],
    ['terrain-url', 'terrainUrl'],
    ['terrain-datum', 'terrainDatum'],
    ['geoid-offset', 'amslToEllipsoidMetres'],
  ])
    $(id).value = state.config[key] ?? '';
  $('fov-override').value = state.fovOverride ?? '';
  $('log-diagnostics').textContent = state.visibleLog
    ? JSON.stringify(
        {
          source: state.log.source,
          selection: state.visibleLog.selection,
          mission: {
            id: state.visibleLog.mission?.id,
            complete: state.visibleLog.mission?.complete,
            confirmed: state.visibleLog.mission?.confirmed,
          },
          home: state.visibleLog.home,
          diagnostics: state.visibleLog.diagnostics,
        },
        null,
        2,
      )
    : 'No flight log loaded.';
  $('settings-dialog').showModal();
};
$('apply-settings').onclick = async () => {
  const generation = state.generation;
  try {
    const config = {
      imagery: $('imagery').value,
      googleMapsApiKey: $('google-key').value,
      terrainUrl: $('terrain-url').value,
      terrainDatum: $('terrain-datum').value,
      amslToEllipsoidMetres: Number($('geoid-offset').value),
    };
    if (!finite(config.amslToEllipsoidMetres)) throw new Error('Geoid offset must be finite.');
    const fov = $('fov-override').value === '' ? null : Number($('fov-override').value);
    if (fov !== null && (!finite(fov) || fov <= 0 || fov >= 179))
      throw new Error('FOV must be between 0 and179 degrees.');
    $('apply-settings').disabled = true;
    const applied = await map?.configure(config);
    if (map && applied === false) return;
    state.config = map ? { ...map.configuration } : config;
    if (generation === state.generation) state.fovOverride = fov;
    saveReview();
    $('settings-dialog').close();
    updateVideoTrack();
    renderFindings();
    if (state.log) refreshLog(true);
    updateTelemetry();
    if (generation !== state.generation)
      status('Map settings applied; the FOV override was not copied to the replacement video.');
  } catch (e) {
    error(e);
  } finally {
    $('apply-settings').disabled = false;
  }
};
$('anchors-button').onclick = () => {
  $('anchors-json').value = JSON.stringify(
    state.anchors.map((p) => ({
      videoSeconds: p.videoSeconds,
      utc: new Date(p.utcUs / 1000).toISOString(),
    })),
    null,
    2,
  );
  $('anchors-dialog').showModal();
};
$('apply-anchors').onclick = () => {
  try {
    state.anchors = validateTiePoints(JSON.parse($('anchors-json').value));
    saveReview();
    $('anchors-dialog').close();
    updateVideoTrack();
    updateTelemetry();
  } catch (e) {
    error(e);
  }
};
$('import-anchors').onclick = () => $('anchors-file').click();
$('anchors-file').onchange = async (e) => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file) return;
  try {
    if (file.size > 1048576) throw new Error('Anchor file exceeds 1 MiB.');
    const data = JSON.parse(await file.text());
    validateTiePoints(data);
    $('anchors-json').value = JSON.stringify(data, null, 2);
  } catch (e) {
    error(e);
  }
};
$('export-anchors').onclick = () => {
  try {
    const points = validateTiePoints(JSON.parse($('anchors-json').value));
    download(
      'fire-time-anchors.json',
      points.map((p) => ({
        videoSeconds: p.videoSeconds,
        utc: new Date(p.utcUs / 1000).toISOString(),
      })),
    );
  } catch (e) {
    error(e);
  }
};
$('export-findings').onclick = () => $('export-dialog').showModal();
$('export-json').onclick = () => exportFindings(false);
$('export-geojson').onclick = () => exportFindings(true);
$('video-stage').ondragover = (e) => e.preventDefault();
$('video-stage').ondrop = (e) => {
  e.preventDefault();
  openVideo(e.dataTransfer.files[0]).catch(error);
};
document.addEventListener('keydown', (e) => {
  if (
    e.target.closest('textarea,select,[contenteditable="true"]') ||
    document.querySelector('dialog[open]')
  )
    return;
  const input = e.target.closest('input');
  if (input && !['range', 'checkbox', 'radio', 'button', 'submit'].includes(input.type)) return;
  if (!e.ctrlKey && !e.metaKey && !e.altKey && !e.defaultPrevented) {
    const inward = ['+', '='].includes(e.key) || e.code === 'NumpadAdd';
    const outward = ['-', '−'].includes(e.key) || e.code === 'NumpadSubtract';
    if (inward || outward) {
      e.preventDefault();
      map?.zoom(inward);
      return;
    }
  }
  if (e.target.matches('input,button')) return;
  if (e.code === 'Space') {
    e.preventDefault();
    play();
  }
  if (e.code === 'ArrowRight') {
    e.preventDefault();
    pause();
    showFrame((state.requestedNumber ?? state.number) + 1);
  }
  if (e.code === 'ArrowLeft') {
    e.preventDefault();
    pause();
    showFrame((state.requestedNumber ?? state.number) - 1);
  }
  if (e.code === 'KeyM') markFinding();
});
window.addEventListener('beforeunload', () => {
  state.closed = true;
  clearTimeout(terrainRefresh);
  state.generation++;
  state.controller?.abort();
  state.decoder?.close();
  state.source?.close();
  state.logWorker?.terminate();
  map?.destroy();
});
// A small diagnostics surface for qualification and future source adapters.
window.fireViewer = {
  state,
  openVideo,
  showFrame,
  loadLog,
  markFinding,
  elevation,
  get map() {
    return map;
  },
  get pose() {
    return alignedRecord();
  },
};
