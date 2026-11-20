// Create splat: range, target and view selection, and the handover to WebSplat (PLAN-v2.md).
import { finite } from '../domain/time.js';
import {
  buildRequest,
  deepFreeze,
  defaultRgbWidth,
  defaultTitle,
  footprintRadius,
  handoverMessage,
  handoverUrl,
  lookPoints,
  LONG_RANGE_FRAMES,
  MAX_RADIUS,
  medianPoint,
  MIN_RADIUS,
  newNonce,
  rangeFrames,
  READY_TIMEOUT_MS,
  requestStep,
  rgbWidthOptions,
  rgbWorkload,
  RGB_DEFAULT_WIDTH,
} from '../domain/splat-request.js';

export function splatPanelLayout() {
  return `<section id="splat-panel" class="splat-panel" aria-label="Create a splat" hidden>
<div class="splat-row splat-title-row"><strong>Create a splat</strong><span id="splat-range-summary" class="muted"></span></div>
<div class="splat-row" role="group" aria-label="Splat time range"><span class="splat-label">Range</span><span id="splat-start" class="splat-value">—</span><span class="muted">to</span><span id="splat-end" class="splat-value">—</span><span class="muted">(drag the brackets on the timeline)</span><label><input id="splat-whole" type="checkbox"> Show whole flight</label></div>
<div class="splat-row" role="group" aria-label="Splat target"><span class="splat-label">Target</span><label>Lat <input id="splat-lat" type="number" step="0.0000001" min="-90" max="90"></label><label>Lon <input id="splat-lon" type="number" step="0.0000001" min="-180" max="180"></label><label>Height <input id="splat-alt" type="number" step="0.1"> m AMSL</label><button id="splat-alt-down" type="button" aria-label="Lower target 1 m (shift: 10 m)" title="Lower 1 m (shift: 10 m)">−</button><button id="splat-alt-up" type="button" aria-label="Raise target 1 m (shift: 10 m)" title="Raise 1 m (shift: 10 m)">+</button><button id="splat-pick" type="button" aria-pressed="false">Pick on map</button><label id="splat-refine-label"><input id="splat-refine" type="checkbox" checked> Refine to the hot spot</label></div>
<div class="splat-row" role="group" aria-label="Splat area"><span class="splat-label">Area</span><label id="splat-radius-label">Radius <input id="splat-radius" type="number" min="10" max="10000" step="10"> m</label><label class="splat-height-label">Max height <input id="splat-max-height" type="range" min="${MAX_HEIGHT_MIN}" max="${MAX_HEIGHT_AUTO}" step="5" value="${MAX_HEIGHT_AUTO}" aria-describedby="splat-max-height-value"></label><output id="splat-max-height-value" class="splat-value"></output></div>
<div id="splat-rgb-row" class="splat-row" role="group" aria-label="RGB processing width" hidden><span class="splat-label">Width</span><select id="splat-rgb-width" aria-label="Processing width"></select><span id="splat-rgb-estimate" class="muted"></span></div>
<div class="splat-row"><span class="splat-label">View</span><span id="splat-view" class="muted">The splat opens from the 3D map's view when you press Go.</span></div>
<div class="splat-row"><label class="splat-title-label">Title <input id="splat-title" type="text" maxlength="120"></label></div>
<p id="splat-warnings" class="note splat-warnings"></p>
<p id="splat-status" class="note splat-status" role="status"></p>
<div class="splat-row splat-actions"><button id="splat-go" class="primary" type="button" disabled>Go</button><button id="splat-cancel" type="button">Cancel</button></div>
</section>`;
}

export function splatBandLayout() {
  return '<div id="splat-band" class="splat-band" hidden><span class="splat-band-fill"></span><span id="splat-handle-start" class="splat-handle splat-handle-start" role="slider" tabindex="0" aria-label="Splat range start"></span><span id="splat-handle-end" class="splat-handle splat-handle-end" role="slider" tabindex="0" aria-label="Splat range end"></span></div>';
}

// max height slider: 5-300 m above the target; its top end is Auto (WebSplat's canopy ceiling)
export const MAX_HEIGHT_MIN = 5;
export const MAX_HEIGHT_MAX = 300;
const MAX_HEIGHT_AUTO = MAX_HEIGHT_MAX + 5;
// track and look points follow a dragged handle after this pause
const DRAG_SETTLE_MS = 300;
// request view camera heights (m AMSL) WebSplat accepts
const VIEW_HEIGHT = [-500, 9000];

const STATUS = {
  blocked: 'The browser blocked the WebSplat tab; allow pop-ups for this site and press Go again.',
  waiting: 'Waiting for WebSplat to open…',
  sent: 'Request sent; WebSplat is copying the recording. Keep this tab open until it accepts.',
  closed: 'The WebSplat tab was closed before it accepted the request.',
};

/** One Go: opens WebSplat, answers its ready message once and waits for its verdict. */
export function handOver({
  url,
  request,
  file,
  win = window,
  hold,
  release,
  onState,
  pollMs = 500,
}) {
  const popup = win.open(url.href, '_blank');
  if (!popup) {
    onState('blocked', STATUS.blocked);
    return null;
  }
  const nonce = url.searchParams.get('request'),
    origin = url.origin,
    held = hold?.(file) ?? null;
  let answered = false,
    done = false;
  const finish = (state, text) => {
    if (done) return;
    done = true;
    win.removeEventListener('message', listen);
    clearTimeout(timer);
    clearInterval(poll);
    release?.(file, held);
    onState(state, text);
  };
  const listen = (event) => {
    const data = handoverMessage(event, { popup, origin, nonce });
    if (!data) return;
    if (data.type === 'websplat.ready' && !answered) {
      answered = true;
      clearTimeout(timer);
      popup.postMessage({ type: 'websplat.request', nonce, request, file }, origin);
      onState('sent', STATUS.sent);
    } else if (data.type === 'websplat.accepted')
      finish(
        'accepted',
        `WebSplat accepted the job${typeof data.job === 'string' ? ` ${data.job}` : ''}.`,
      );
    else if (data.type === 'websplat.rejected')
      finish(
        'rejected',
        `WebSplat rejected the request: ${typeof data.reason === 'string' ? data.reason : 'no reason given'}`,
      );
  };
  win.addEventListener('message', listen);
  // the timeout covers only the wait for ready; the copy that follows can take minutes
  const timer = setTimeout(
    () =>
      finish(
        'timeout',
        `WebSplat did not answer within ${READY_TIMEOUT_MS / 1000} s; check that it is available at ${url.origin}${url.pathname}.`,
      ),
    READY_TIMEOUT_MS,
  );
  const poll = setInterval(() => {
    if (popup.closed) finish('closed', STATUS.closed);
  }, pollMs);
  onState('waiting', STATUS.waiting);
  return {
    nonce,
    popup,
    finish,
    get done() {
      return done;
    },
  };
}

export class SplatPanel {
  constructor(env) {
    this.env = env;
    this.$ = (id) => document.getElementById(id);
    this.active = false;
    this.handovers = new Set();
    this.lookTimer = null;
    this.clearSelection();
    const $ = this.$;
    $('splat-create').onclick = () => (this.active ? this.close() : this.open());
    $('splat-cancel').onclick = () => this.close();
    for (const which of ['start', 'end']) this.bindHandle(which, $(`splat-handle-${which}`));
    $('splat-max-height').oninput = () => this.render();
    $('splat-whole').onchange = () => this.refreshTrack();
    for (const id of ['splat-lat', 'splat-lon', 'splat-alt'])
      $(id).onchange = () => this.editTarget();
    $('splat-radius').onchange = () => this.editRadius();
    $('splat-alt-down').onclick = (e) => this.nudge(e.shiftKey ? -10 : -1);
    $('splat-alt-up').onclick = (e) => this.nudge(e.shiftKey ? 10 : 1);
    $('splat-pick').onclick = () => this.setPicking(!this.picking);
    $('splat-go').onclick = () => this.go();
    $('splat-rgb-width').onchange = () => this.render();
  }
  clearSelection() {
    this.startPts = this.endPts = null;
    this.range = null;
    this.target = null;
    this.targetAuto = true;
    this.radius = null;
    this.radiusAuto = true;
    this.picking = false;
    this.points = [];
    this.loading = false;
    this.heightPending = false;
    this.title = null;
    this.source = null;
  }
  get map() {
    return this.env.map();
  }
  /** Enables Create splat, or disables it with the reason as its tooltip. */
  setAvailability(reason) {
    const button = this.$('splat-create');
    button.disabled = !!reason && !this.active;
    button.title = reason || 'Select a time range, target and view for WebSplat';
  }
  open() {
    const frames = this.env.frames();
    if (!frames.length) return;
    this.active = true;
    this.$('splat-panel').hidden = false;
    this.$('splat-create').setAttribute('aria-pressed', 'true');
    if (this.startPts === null) {
      this.startPts = frames[0].pts;
      this.endPts = frames.at(-1).pts;
    }
    if (this.title === null) this.title = defaultTitle(this.env.file()?.name);
    if (!this.source) this.setSource(this.env.source());
    this.$('splat-title').value = this.title;
    this.$('splat-title').oninput = () => (this.title = this.$('splat-title').value);
    this.env.setMapMode('3d');
    this.update();
  }
  /** kind and size of the open recording; RGB shows the width selector instead of refine. */
  setSource(source) {
    this.source = source;
    const rgb = source?.kind === 'rgb',
      select = this.$('splat-rgb-width');
    this.$('splat-rgb-row').hidden = !rgb;
    this.$('splat-refine-label').hidden = rgb;
    select.replaceChildren(
      ...(rgb ? rgbWidthOptions(source.width) : []).map(({ width, label }) => {
        const option = document.createElement('option');
        option.value = String(width);
        option.textContent = label;
        return option;
      }),
    );
    if (rgb) select.value = String(defaultRgbWidth(source.width));
  }
  /** Leaves splat mode and restores the normal track. */
  close() {
    if (!this.active) return;
    this.deactivate();
    this.env.restoreTrack();
  }
  /** A new video: leave splat mode and forget the selection (pending handovers continue). */
  reset() {
    this.deactivate();
    this.clearSelection();
    this.$('splat-status').textContent = '';
  }
  deactivate() {
    this.active = false;
    clearTimeout(this.lookTimer);
    this.lookTimer = null;
    this.setPicking(false);
    this.$('splat-panel').hidden = true;
    clearTimeout(this.settleTimer);
    this.settleTimer = null;
    this.$('splat-band').hidden = true;
    this.$('splat-create').setAttribute('aria-pressed', 'false');
    this.map?.setLookPoints(null);
    this.map?.setSplatTarget(null);
  }
  /** Moves a range end to the frame nearest pts (the other end follows if crossed). */
  setEndpoint(which, pts) {
    const frames = this.env.frames();
    if (!frames.length || !finite(pts)) return false;
    let lo = 0,
      hi = frames.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (frames[mid].pts < pts) lo = mid + 1;
      else hi = mid;
    }
    if (lo > 0 && pts - frames[lo - 1].pts < frames[lo].pts - pts) lo--;
    const at = frames[lo].pts;
    if ((which === 'start' ? this.startPts : this.endPts) === at) return false;
    if (which === 'start') {
      this.startPts = at;
      if (this.endPts < at) this.endPts = at;
    } else {
      this.endPts = at;
      if (this.startPts > at) this.startPts = at;
    }
    return true;
  }
  /** Range ends moved: range text and band at once, track and look points once settled. */
  moved(settleMs = DRAG_SETTLE_MS) {
    this.range = rangeFrames(this.env.frames(), this.startPts, this.endPts);
    // the automatic target belongs to the old range: Go waits for the new one
    if (this.targetAuto) this.target = null;
    this.render();
    clearTimeout(this.settleTimer);
    this.settleTimer = setTimeout(() => {
      this.settleTimer = null;
      this.update();
    }, settleMs);
  }
  /** A timeline bracket: drag it, or focus it and use the arrow keys (shift: 10 frames). */
  bindHandle(which, handle) {
    const ptsAt = (clientX) => {
      const rect = this.$('splat-band').getBoundingClientRect();
      const fraction = Math.max(0, Math.min(1, (clientX - rect.left) / Math.max(1, rect.width)));
      return this.env.origin() + fraction * this.env.extent();
    };
    handle.onpointerdown = (e) => {
      if (!this.active || e.button !== 0) return;
      e.preventDefault();
      handle.setPointerCapture(e.pointerId);
      handle.classList.add('dragging');
      handle.focus();
    };
    handle.onpointermove = (e) => {
      if (handle.hasPointerCapture(e.pointerId) && this.setEndpoint(which, ptsAt(e.clientX)))
        this.moved();
    };
    const release = (e) => {
      if (!handle.hasPointerCapture(e.pointerId)) return;
      handle.releasePointerCapture(e.pointerId);
      handle.classList.remove('dragging');
      if (this.settleTimer !== null) this.moved(0);
    };
    handle.onpointerup = release;
    handle.onpointercancel = release;
    handle.onkeydown = (e) => {
      const step = { ArrowLeft: -1, ArrowDown: -1, ArrowRight: 1, ArrowUp: 1 }[e.key];
      if (!this.active || !step || !this.range) return;
      e.preventDefault();
      e.stopPropagation(); // not also a playback step
      const frames = this.env.frames(),
        index = which === 'start' ? this.range.start : this.range.end,
        next = Math.max(0, Math.min(frames.length - 1, index + step * (e.shiftKey ? 10 : 1)));
      if (this.setEndpoint(which, frames[next].pts)) this.moved();
    };
  }
  /** Range changed: track, look points, default target and display. */
  update() {
    this.range = rangeFrames(this.env.frames(), this.startPts, this.endPts);
    this.poseChanged();
  }
  /** Range, telemetry offset or anchors changed: the automatic target waits for new points. */
  poseChanged() {
    if (!this.active) return;
    if (this.targetAuto) this.target = null;
    this.refreshTrack();
    this.refreshLookPoints();
    this.render();
  }
  refreshTrack() {
    const map = this.map;
    if (!this.active || !map) return;
    const frames = this.env.frames(),
      selected = this.range ? frames.slice(this.range.start, this.range.end + 1) : [];
    this.env.trackHint('Updating video track…');
    const controller = map.setTrackRange(selected, this.env.record, {
      dimOthers: this.$('splat-whole').checked,
      allFrames: frames,
    });
    controller
      .then((applied) => {
        if (applied && this.active) {
          this.env.trackHint('Splat range shown; click the track to seek');
          this.env.trackChanged?.();
        }
      })
      .catch(this.env.onError);
  }
  refreshLookPoints() {
    if (!this.active || this.lookTimer !== null) return;
    this.lookTimer = setTimeout(() => {
      this.lookTimer = null;
      if (!this.active) return;
      const result = lookPoints(this.env.frames(), this.range, this.env.record, this.env.elevation);
      this.points = result.points;
      this.loading = result.loading;
      this.map?.setLookPoints(this.points);
      const centre = medianPoint(this.points);
      if (this.targetAuto)
        this.target = centre ? { lat: centre.lat, lon: centre.lon, alt_amsl: centre.altMsl } : null;
      this.render();
    }, 100);
  }
  /** Terrain tiles arrived: ground points and the target line may now resolve. */
  terrainChanged() {
    if (!this.active) return;
    if (this.heightPending && this.target) {
      const height = this.env.elevation.at(this.target.lat, this.target.lon);
      if (finite(height)) {
        this.target = { ...this.target, alt_amsl: height };
        this.heightPending = false;
        this.status('');
      }
    }
    this.refreshLookPoints();
  }
  editTarget() {
    const lat = this.$('splat-lat').valueAsNumber,
      lon = this.$('splat-lon').valueAsNumber,
      alt = this.$('splat-alt').valueAsNumber;
    if (![lat, lon, alt].every(finite)) return this.render();
    this.target = { lat, lon, alt_amsl: alt };
    this.targetAuto = false;
    this.heightPending = false;
    this.render();
  }
  /** A typed radius is kept; an empty field goes back to the footprint default. */
  editRadius() {
    const value = this.$('splat-radius').valueAsNumber;
    this.radiusAuto = !finite(value);
    if (!this.radiusAuto) this.radius = Math.min(MAX_RADIUS, Math.max(MIN_RADIUS, value));
    this.render();
  }
  nudge(metres) {
    if (!this.target) return;
    this.target = { ...this.target, alt_amsl: this.target.alt_amsl + metres };
    this.targetAuto = false;
    this.render();
  }
  setPicking(enabled) {
    this.picking = !!enabled && this.active;
    this.$('splat-pick').setAttribute('aria-pressed', String(this.picking));
    this.$('splat-pick').classList.toggle('selected', this.picking);
    this.map?.setPickHandler(this.picking ? (pick) => this.picked(pick) : null);
    if (this.active) this.status(this.picking ? 'Click the map to set the target.' : '');
  }
  picked(pick) {
    this.picking = false;
    this.$('splat-pick').setAttribute('aria-pressed', 'false');
    this.$('splat-pick').classList.remove('selected');
    if (!this.active) return;
    if (!finite(pick?.lat) || !finite(pick?.lon))
      return this.status(`No target picked: ${pick?.reason || 'outside the globe'}.`);
    // no height until terrain resolves it or the user enters one
    const height = this.env.elevation.at(pick.lat, pick.lon) ?? pick.heightAmslM;
    this.target = { lat: pick.lat, lon: pick.lon, alt_amsl: finite(height) ? height : NaN };
    this.targetAuto = false;
    this.heightPending = !finite(height);
    this.status(
      finite(height) ? '' : `Terrain height unavailable (${pick.reason}); enter the target height.`,
    );
    this.render();
  }
  /** The 3D map's camera as the splat's start view; null in 2D (WebSplat's default view). */
  currentView() {
    const pose = this.map?.cameraPose();
    return pose
      ? { camera: { lat: pose.lat, lon: pose.lon, alt_amsl: pose.alt_amsl }, fov_deg: pose.fov_deg }
      : null;
  }
  /** The start view for Go, or null with a note when the map's view cannot be one. */
  viewForGo() {
    const view = this.currentView();
    if (!view) return { view: null, note: '' };
    const { alt_amsl } = view.camera;
    if (
      alt_amsl >= VIEW_HEIGHT[0] &&
      alt_amsl <= VIEW_HEIGHT[1] &&
      view.fov_deg >= 10 &&
      view.fov_deg <= 120
    )
      return { view, note: '' };
    return {
      view: null,
      note: `The 3D view (camera at ${Math.round(alt_amsl).toLocaleString()} m) cannot be a start view; WebSplat opens its default view. `,
    };
  }
  maxHeight() {
    const value = Number(this.$('splat-max-height').value);
    return value > MAX_HEIGHT_MAX ? null : value;
  }
  status(text) {
    this.$('splat-status').textContent = text;
  }
  validTarget() {
    return !!this.target && [this.target.lat, this.target.lon, this.target.alt_amsl].every(finite);
  }
  render() {
    if (!this.active) return;
    const $ = this.$,
      frames = this.env.frames();
    this.renderRangeText();
    const range = this.range;
    if (range) {
      const first = frames[range.start],
        last = frames[range.end],
        duration = last.pts - first.pts + (finite(last.duration) ? last.duration : 0);
      $('splat-range-summary').textContent =
        `${range.count.toLocaleString()} frames · ${duration.toFixed(1)} s · ${this.rgb ? `~${this.workload(range, duration).frames.toLocaleString()} used` : `step ${requestStep(range.count)}`}`;
      if (this.rgb) {
        const w = this.workload(range, duration);
        $('splat-rgb-estimate').textContent =
          `${w.frames.toLocaleString()} frames × ${w.width}×${w.height} ≈ ${Math.round(w.megapixels).toLocaleString()} Mpx`;
      }
    }
    const focused = document.activeElement;
    const show = (id, value, digits) => {
      if (focused !== $(id)) $(id).value = finite(value) ? value.toFixed(digits) : '';
    };
    show('splat-lat', this.target?.lat, 7);
    show('splat-lon', this.target?.lon, 7);
    show('splat-alt', this.target?.alt_amsl, 1);
    if (this.radiusAuto) this.radius = footprintRadius(frames, range, this.env.record, this.target);
    show('splat-radius', this.radius, 0);
    $('splat-radius').title = this.radiusAuto
      ? 'Area kept around the target (m): the ground width the camera sees, from its distance and FOV'
      : 'Area kept around the target (m); clear to use the ground width the camera sees';
    $('splat-alt-down').disabled = $('splat-alt-up').disabled = !this.validTarget();
    const maxHeight = this.maxHeight();
    $('splat-max-height-value').textContent =
      maxHeight === null ? 'Auto (above the tree tops)' : `${maxHeight} m above the target`;
    const warnings = [];
    if (!this.points.length)
      warnings.push(
        this.loading
          ? 'Loading terrain for the ground points…'
          : this.targetAuto && !this.target && this.lookTimer !== null
            ? 'Finding where the camera looked…'
            : 'No ground points: the camera pose or terrain is unavailable in this range; pick the target on the map.',
      );
    if (this.rgb && this.rgbWidth() > RGB_DEFAULT_WIDTH)
      warnings.push(
        `Widths above ${RGB_DEFAULT_WIDTH} may exceed GPU memory on small GPUs and take much longer.`,
      );
    if (!this.rgb && range?.count > LONG_RANGE_FRAMES)
      warnings.push(
        `${range.count.toLocaleString()} frames in range; the job will take longer (every ${requestStep(range.count)}th frame is used).`,
      );
    if (this.env.timingCorrected())
      warnings.push(
        'Telemetry offset or time anchors are set here; WebSplat uses the recorded telemetry without them.',
      );
    $('splat-warnings').textContent = warnings.join(' ');
    $('splat-go').disabled = !range || !this.validTarget();
    const target = this.validTarget()
      ? {
          lat: this.target.lat,
          lon: this.target.lon,
          altAmsl: this.target.alt_amsl,
          groundAmsl: this.env.elevation.at(this.target.lat, this.target.lon),
          radiusM: this.radius,
          maxHeightM: maxHeight,
        }
      : null;
    this.map?.setSplatTarget(target);
    this.renderBand();
  }
  renderRangeText() {
    const $ = this.$,
      frames = this.env.frames(),
      origin = this.env.origin();
    const at = (pts) => {
      const range = rangeFrames(frames, pts, pts);
      return `${this.env.clock(pts - origin)}${range ? ` · #${range.start + 1}` : ''}`;
    };
    $('splat-start').textContent = finite(this.startPts) ? at(this.startPts) : '—';
    $('splat-end').textContent = finite(this.endPts) ? at(this.endPts) : '—';
    if (!this.range) $('splat-range-summary').textContent = 'No frames in range';
    for (const which of ['start', 'end']) {
      const handle = $(`splat-handle-${which}`),
        pts = which === 'start' ? this.startPts : this.endPts;
      handle.setAttribute('aria-valuetext', finite(pts) ? at(pts) : '');
    }
  }
  get rgb() {
    return this.source?.kind === 'rgb';
  }
  rgbWidth() {
    return Number(this.$('splat-rgb-width').value);
  }
  workload(range, durationS) {
    return rgbWorkload({
      count: range.count,
      durationS,
      width: this.rgbWidth(),
      sourceWidth: this.source.width,
      sourceHeight: this.source.height,
    });
  }
  renderBand() {
    const band = this.$('splat-band'),
      extent = this.env.extent(),
      origin = this.env.origin();
    band.hidden = !this.active || !this.range || !(extent > 0);
    if (band.hidden) return;
    const percent = (pts) => Math.max(0, Math.min(100, ((pts - origin) / extent) * 100));
    const frames = this.env.frames(),
      left = percent(frames[this.range.start].pts),
      right = percent(frames[this.range.end].pts);
    const [fill, start, end] = band.children;
    fill.style.left = `${left}%`;
    fill.style.width = `${Math.max(0, right - left)}%`;
    start.style.left = `${left}%`;
    end.style.left = `${right}%`;
  }
  go() {
    let request;
    const file = this.env.file(),
      { view, note } = this.viewForGo();
    try {
      if (!file) throw new Error('No recording is open.');
      request = deepFreeze(
        buildRequest({
          kind: this.source?.kind,
          rgbWidth: this.rgb ? this.rgbWidth() : undefined,
          source: file,
          frames: this.env.frames(),
          range: this.range,
          target: this.target,
          refine: this.$('splat-refine').checked,
          hfov: this.env.hfov(),
          radius: this.radius,
          maxHeight: this.maxHeight(),
          view,
          title: this.$('splat-title').value,
        }),
      );
    } catch (e) {
      return this.status(`Cannot create the splat: ${e.message}`);
    }
    const url = handoverUrl(this.env.websplatUrl(), location.href, newNonce());
    let handover = null;
    handover = handOver({
      url,
      request,
      file,
      hold: this.env.holdDownload,
      release: this.env.releaseDownload,
      onState: (state, text) => {
        // the latest handover owns the panel status; all report to the status bar
        if (!handover || [...this.handovers].at(-1) === handover) this.status(note + text);
        this.env.onStatus?.(note + text);
        if (handover?.done) this.handovers.delete(handover);
      },
    });
    if (handover) this.handovers.add(handover);
  }
}
