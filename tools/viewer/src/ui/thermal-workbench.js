import {
  analysisSettings,
  DEFAULT_ANALYSIS,
  METHODS,
  regionOf,
} from '../domain/thermal-analysis.js';
import { ThermalAnalysisClient } from '../domain/thermal-analysis-client.js';
import { calibration, celsius } from '../domain/thermal.js';

export function thermalWorkbenchLayout() {
  const number = (id, label, min, max, step, value) =>
    `<label>${label}<input id="analysis-${id}" type="number" min="${min}" max="${max}" step="${step}" value="${value}"></label>`;
  return `<details id="thermal-analysis" class="panel thermal-analysis"><summary>Thermal analysis <span class="muted">Histogram · weak-source filters · deconvolution</span></summary>
  <p id="analysis-availability" class="note">Open a raw thermal recording to analyse detector samples.</p>
  <fieldset id="analysis-controls" disabled><legend class="sr-only">Thermal image analysis</legend><div class="analysis-options"><label>Method<select id="analysis-method">${Object.entries(
    METHODS,
  )
    .map(
      ([key, value]) =>
        `<option value="${key}"${key === 'matched' ? ' selected' : ''}>${value}</option>`,
    )
    .join('')}</select></label>
  ${number('sigma', 'PSF σ (px)', 0.5, 4, 0.1, 1)}${number('background', 'Background scale (px)', 2, 16, 0.5, 6)}${number('iterations', 'RL iterations', 1, 200, 1, 8)}${number('damping', 'RL damping (noise σ)', 0, 10, 0.1, 1)}${number('regularization', 'Wiener K adjustment', 0.1, 10, 0.1, 1)}${number('clip', 'CLAHE clip factor', 1, 8, 0.1, 2)}
  <label><input id="analysis-destripe" type="checkbox"> Remove row/column offsets</label></div><p id="analysis-method-note" class="note"></p>
  <details class="analysis-region"><summary>Analysis region · raw pixel coordinates</summary><div class="analysis-options">${number('x', 'X', 0, 65535, 1, 0)}${number('y', 'Y', 0, 65535, 1, 0)}${number('width', 'Width', 1, 65536, 1, 640)}${number('height', 'Height', 1, 65536, 1, 512)}<button id="analysis-whole" type="button">Whole frame</button><button id="analysis-cursor" type="button">Around cursor</button></div><p class="note">Region selects statistics and candidates. Filters and noise estimates use the full frame.</p></details>
  <div class="analysis-actions"><button id="analysis-apply" class="primary" type="button">Apply to paused frame</button><button id="analysis-cancel" type="button" disabled>Cancel</button><button id="analysis-reset" type="button">Reset analysis</button><label><input id="analysis-follow" type="checkbox"> Apply to every frame</label><label>Compare<select id="analysis-view"><option value="raw">Original</option><option value="processed" disabled>Processed result</option></select></label></div></fieldset>
  <p id="analysis-status" class="note" role="status">Analysis is off. Original pixels and measured temperatures are preserved.</p>
  <div class="histogram-options"><label>Histogram<select id="analysis-histogram"><option value="raw">Raw samples</option><option value="processed" disabled>Processed values</option></select></label><label><input id="analysis-log" type="checkbox"> Log counts</label><button id="analysis-export" type="button" disabled>Export histogram CSV</button></div>
  <canvas id="analysis-histogram-canvas" width="640" height="160" aria-label="Thermal histogram" role="img"></canvas><p id="analysis-bin" class="note">Hover over the histogram for bin values and counts.</p><p id="analysis-statistics" class="analysis-statistics"></p><p id="analysis-provenance" class="note"></p><button id="analysis-peaks" type="button" disabled>Show analysis candidates</button><p id="analysis-candidate-note" class="note"></p>
  <p class="note">Gaussian PSF is assumed, not measured. Filtered contrast is a review aid. Restored intensities are not temperatures; a feature visible only after deconvolution needs confirmation in original and neighbouring frames.</p></details>`;
}
const NOTES = {
  raw: 'Histogram of original 16-bit detector samples. No image processing.',
  background:
    'Subtract a broad Gaussian background to remove scene gradients. Bright edges can also respond.',
  dog: 'Gaussian bandpass at σ and 1.6σ. Compare scales to check whether a compact source persists.',
  tophat: 'Subtract a square morphological opening. Background scale is its radius in pixels.',
  cfar: 'Centre minus surrounding mean, normalized by ring standard deviation with a robust full-frame noise floor. Guard radius = ceil(3σ); outer radius ≥ guard + 2. This is not a calibrated false-alarm probability.',
  matched:
    'Gaussian PSF filter after broad background suppression, normalized by empirical response MAD / white-noise floor. Scores are heuristic contrast σ, not calibrated detection probabilities.',
  multiscale:
    'Scale-normalized negative LoG at 0.7σ, σ and 1.4σ. Each scale is noise normalized; the maximum is shown. Searching scales increases false responses.',
  clahe:
    'Contrast-limited adaptive histogram equalization, 32-pixel tiles / 1024 bins. Display intensity only; candidate ranking uses the independent matched filter.',
  rl: 'Exploratory Richardson–Lucy with positive floor-subtracted input, early stopping and optional soft-threshold residual damping. Thermal noise need not be Poisson. Candidates use the matched filter, not restored peaks.',
  wiener:
    'Exploratory FFT deconvolution. K = noise variance / estimated signal variance × adjustment, minimum 0.001; inverse-filter gain is capped at 8. Reflected padding ≥4σ. Candidates use the matched filter, not restored peaks.',
};
export class ThermalWorkbench {
  constructor(
    root,
    {
      onChange,
      onCandidates,
      onOverlayChange = () => {},
      onPendingChange = () => {},
      pause,
      cursor,
      playing = () => false,
      loading = () => false,
      preferences = {},
      onPreferences = () => {},
    },
  ) {
    this.root = root;
    this.$ = (id) => root.querySelector(`#analysis-${id}`);
    this.client = new ThermalAnalysisClient();
    Object.assign(this, {
      onChange,
      onCandidates,
      onOverlayChange,
      pause,
      cursor,
      playing,
      loading,
      onPreferences,
    });
    this.settings = analysisSettings(preferences);
    this.result = null;
    this.resultIdentity = '';
    this.frame = null;
    this.decoded = null;
    this.identity = '';
    this.serial = 0;
    this.busy = this.presenting = false;
    this.writeSettings();
    this.methodNote();
    root.addEventListener('toggle', () => {
      this.onOverlayChange();
      if (root.open && this.decoded?.kind === 'gray16' && !this.result && !this.busy)
        this.run(true);
    });
    for (const key of Object.keys(DEFAULT_ANALYSIS))
      this.$(key).addEventListener('change', () => {
        this.settings = this.readSettings();
        this.onPreferences(this.settings);
        this.methodNote();
        this.invalidate();
      });
    for (const key of ['x', 'y', 'width', 'height'])
      this.$(key).addEventListener('change', () => {
        this.writeRegion(this.roi);
        this.onOverlayChange();
        this.invalidate();
        if (root.open) this.run(true);
      });
    this.$('whole').onclick = () => {
      this.writeRegion(null);
      this.onOverlayChange();
      this.invalidate();
      this.run(true);
    };
    this.$('cursor').onclick = () => {
      if (!this.decoded) return;
      const { width, height } = this.decoded,
        p = this.cursor() || { x: width / 2, y: height / 2 };
      this.writeRegion({
        x: Math.max(0, Math.round(p.x) - 48),
        y: Math.max(0, Math.round(p.y) - 48),
        width: Math.min(96, width),
        height: Math.min(96, height),
      });
      this.onOverlayChange();
      this.invalidate();
      this.run(true);
    };
    this.$('apply').onclick = () => {
      this.pause();
      this.run(false);
    };
    this.$('cancel').onclick = () => this.invalidate('Analysis cancelled. Original frame shown.');
    this.$('reset').onclick = () => {
      this.settings = analysisSettings();
      this.writeSettings();
      this.methodNote();
      this.onPreferences(this.settings);
      this.writeRegion(null);
      this.onOverlayChange();
      this.invalidate('Analysis reset. Original frame shown.');
      this.run(true);
    };
    this.$('follow').onchange = () => {
      this.$('availability').textContent = this.availabilityText();
      if (this.following && !this.playing() && !this.loading() && !this.result?.plane)
        this.ready = this.run(false);
    };
    this.ready = Promise.resolve();
    this.$('view').onchange = () => this.onChange();
    this.$('histogram').onchange = this.$('log').onchange = () => this.drawHistogram();
    this.$('export').onclick = () => this.exportHistogram();
    this.$('peaks').onclick = () => {
      if (this.current) {
        this.candidatesShown = true;
        this.onCandidates(
          this.current.candidates.map((p) => ({
            ...p,
            contrast: p.score,
            scoreUnit: 'contrast σ',
            analysis: this.candidateProvenance(p),
          })),
        );
      }
    };
    this.$('histogram-canvas').onpointermove = (event) => {
      const h = this.currentHistogram();
      if (!h) return;
      const rect = event.currentTarget.getBoundingClientRect(),
        i = Math.max(
          0,
          Math.min(255, Math.floor(((event.clientX - rect.left - 6) / (rect.width - 12)) * 256)),
        );
      this.$('bin').textContent =
        `${this.valueText(h.low + ((h.high - h.low) * i) / 256)} to ${this.valueText(h.low + ((h.high - h.low) * (i + 1)) / 256)} · ${h.bins[i].toLocaleString()} samples (last bin includes upper bound)`;
    };
    this.resizeObserver = new ResizeObserver(() => this.drawHistogram());
    this.resizeObserver.observe(this.$('histogram-canvas'));
    this.onPendingChange = () => {};
    this.setFrame(null, null, null);
    this.onPendingChange = onPendingChange;
  }
  readSettings() {
    return analysisSettings(
      Object.fromEntries(
        Object.keys(DEFAULT_ANALYSIS).map((key) => [
          key,
          key === 'method'
            ? this.$(key).value
            : key === 'destripe'
              ? this.$(key).checked
              : Number(this.$(key).value),
        ]),
      ),
    );
  }
  writeSettings() {
    for (const [key, value] of Object.entries(this.settings))
      if (key === 'destripe') this.$(key).checked = value;
      else this.$(key).value = value;
  }
  methodNote() {
    this.$('method-note').textContent =
      `${NOTES[this.settings.method]} PSF FWHM ≈ ${(2.355 * this.settings.sigma).toFixed(2)} raw pixels.`;
    for (const key of ['iterations', 'damping', 'regularization', 'clip'])
      this.$(key).closest('label').hidden =
        key === 'clip'
          ? this.settings.method !== 'clahe'
          : key === 'regularization'
            ? this.settings.method !== 'wiener'
            : this.settings.method !== 'rl';
  }
  get roi() {
    return this.decoded
      ? regionOf(
          Object.fromEntries(
            ['x', 'y', 'width', 'height'].map((k) => [k, Number(this.$(k).value)]),
          ),
          this.decoded.width,
          this.decoded.height,
        )
      : null;
  }
  writeRegion(region) {
    if (!this.decoded) return;
    const r = regionOf(region, this.decoded.width, this.decoded.height);
    for (const key of ['x', 'y', 'width', 'height']) this.$(key).value = r[key];
    this.$('x').max = this.decoded.width - 1;
    this.$('y').max = this.decoded.height - 1;
    this.$('width').max = this.decoded.width;
    this.$('height').max = this.decoded.height;
  }
  setFrame(file, frame, decoded, generation = 0) {
    const previousROI = generation === this.sourceGeneration ? this.roi : null;
    this.frame = frame;
    this.decoded = decoded;
    this.sourceGeneration = generation;
    this.identity = JSON.stringify([
      generation,
      file?.name,
      file?.size,
      file?.lastModified,
      frame?.id,
      frame?.pts,
      frame?.thermalMetadata?.source_sha256,
    ]);
    const thermal = decoded?.kind === 'gray16';
    // Following keeps the previous processed image until this frame's result replaces it,
    // but nothing may record that stale result against this frame.
    if (!(thermal && this.following)) this.invalidate();
    else this.$('export').disabled = this.$('peaks').disabled = true;
    this.$('controls').disabled = this.$('histogram').disabled = this.$('log').disabled = !thermal;
    this.$('availability').textContent = this.availabilityText();
    if (!thermal) {
      this.ready = Promise.resolve();
      return this.onPendingChange();
    }
    this.activity();
    this.writeRegion(previousROI);
    if (this.following) this.ready = this.run(false, true);
    else {
      this.ready = Promise.resolve();
      if (this.root.open && !this.playing()) this.run(true);
    }
    this.onPendingChange();
  }
  // The result for the current frame, or null while it is pending.
  get current() {
    return this.result && this.resultIdentity === this.identity ? this.result : null;
  }
  // The display still shows the previous frame until this frame's result arrives.
  get pending() {
    return this.presenting || (!!this.result && !this.current);
  }
  get following() {
    return this.root.open && this.$('follow').checked && this.decoded?.kind === 'gray16';
  }
  availabilityText() {
    if (this.decoded?.kind !== 'gray16')
      return 'Raw thermal analysis requires a lossless gray16 recording. RGB and colourized thermal videos contain no detector samples.';
    return this.$('follow').checked
      ? 'Raw thermal samples available. Every displayed frame is processed; playback waits for each result.'
      : 'Raw thermal samples available. Processing applies to this paused frame; seeking or playback returns to original.';
  }
  invalidate(message = 'Original frame shown. Apply analysis to this paused frame.') {
    const repaint = !!this.plane || this.candidatesShown;
    this.candidatesShown = false;
    this.serial++;
    this.client.cancel();
    this.busy = this.presenting = false;
    this.result = null;
    this.resultIdentity = '';
    this.$('view').value = 'raw';
    this.$('view').options[1].disabled = true;
    this.$('histogram').value = 'raw';
    this.$('histogram').options[1].disabled = true;
    this.$('cancel').disabled = this.$('export').disabled = this.$('peaks').disabled = true;
    this.$('status').textContent = message;
    this.$('statistics').textContent =
      this.$('provenance').textContent =
      this.$('candidate-note').textContent =
        '';
    const canvas = this.$('histogram-canvas');
    canvas.getContext('2d').clearRect(0, 0, canvas.width, canvas.height);
    if (repaint) this.onChange();
    this.onPendingChange();
  }
  async run(histogramOnly, live = false) {
    if (this.decoded?.kind !== 'gray16') return;
    if (!histogramOnly && !live && (this.playing() || this.loading())) {
      this.$('status').textContent =
        'Pause playback and wait for the current frame before applying analysis.';
      return;
    }
    if (!histogramOnly) {
      const bad = [...this.$('controls').querySelectorAll('input')].find(
        (input) => !input.closest('label').hidden && !input.validity.valid,
      );
      if (bad) {
        // Following must not carry the previous frame's result onto this one.
        if (live) this.invalidate('Correct the highlighted setting. Original frame shown.');
        bad.reportValidity();
        return;
      }
    }
    if (live) {
      // Cancel any stale request but leave the current image displayed.
      this.serial++;
      this.client.cancel();
      this.candidatesShown = false;
    } else this.invalidate();
    this.settings = this.readSettings();
    this.writeSettings();
    this.methodNote();
    const serial = this.serial,
      identity = this.identity,
      settings = histogramOnly ? { ...this.settings, method: 'raw' } : { ...this.settings },
      roi = this.roi;
    this.busy = true;
    this.presenting = live;
    this.$('cancel').disabled = false;
    this.$('status').textContent = histogramOnly
      ? 'Calculating raw histogram…'
      : live
        ? 'Analysing frame…'
        : 'Analysing paused frame…';
    const start = performance.now();
    try {
      const r = await this.client.run(
        this.decoded.data,
        this.decoded.width,
        this.decoded.height,
        settings,
        roi,
        identity,
      );
      if (serial !== this.serial || identity !== this.identity) return;
      this.result = r;
      this.resultIdentity = identity;
      this.busy = this.presenting = false;
      this.$('cancel').disabled = true;
      this.$('export').disabled = false;
      this.$('view').options[1].disabled = this.$('histogram').options[1].disabled = !r.plane;
      this.$('view').value = r.plane ? 'processed' : 'raw';
      this.$('status').textContent = r.plane
        ? `${METHODS[r.settings.method]} · ${Math.round(performance.now() - start)} ms · ${r.unit}`
        : 'Original raw histogram · nearest-rank percentiles';
      this.$('peaks').disabled = !r.candidates.length;
      this.$('candidate-note').textContent = r.plane
        ? `${r.candidates.length} candidate peaks ≥3 contrast σ from ${METHODS[r.candidateMethod]}. Border ${r.border} px excluded. Scores are heuristic; no false-alarm probability. Pixel-scale peaks may be defects or undersampled sources.`
        : '';
      this.$('provenance').textContent = r.plane
        ? `Noise σ ${r.noise.toFixed(3)} raw counts${r.noiseFloorApplied ? ' (1-count floor applied)' : ''}; full-frame estimate. Assumed Gaussian PSF σ ${r.settings.sigma} px.${r.k !== null ? ` Effective Wiener K ${r.k.toPrecision(4)}, gain ≤8.` : ''}${r.settings.destripe ? ' Row/column correction enabled; inspect elongated real sources carefully.' : ''}`
        : '';
      this.drawHistogram();
      this.onChange();
      this.onPendingChange();
    } catch (e) {
      if (serial === this.serial && identity === this.identity && e.name !== 'AbortError')
        this.invalidate(`Analysis failed: ${e.message} Original frame retained.`);
    }
  }
  get plane() {
    return this.$('view').value === 'processed' ? (this.result?.plane ?? null) : null;
  }
  get limits() {
    const s = this.result?.displayStats;
    return s ? [s.p01, s.p99 > s.p01 ? s.p99 : s.p01 + 1] : null;
  }
  currentHistogram() {
    return this.$('histogram').value === 'processed'
      ? this.result?.processedStats
      : this.result?.rawStats;
  }
  valueText(value) {
    if (this.$('histogram').value === 'raw') {
      const c = celsius(value, this.frame?.thermalMetadata);
      return c === null ? `${value.toFixed(2)} raw` : `${c.toFixed(2)} °C`;
    }
    return `${value.toFixed(3)} ${this.result?.unit || ''}`;
  }
  drawHistogram() {
    const h = this.currentHistogram();
    if (!h) return;
    const canvas = this.$('histogram-canvas'),
      rect = canvas.getBoundingClientRect();
    if (rect.width < 1 || rect.height < 1) return;
    const ratio = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(rect.width * ratio));
    canvas.height = Math.max(1, Math.round(rect.height * ratio));
    const context = canvas.getContext('2d'),
      log = this.$('log').checked;
    context.setTransform(ratio, 0, 0, ratio, 0, 0);
    context.clearRect(0, 0, rect.width, rect.height);
    const plotWidth = rect.width - 12,
      plotHeight = rect.height - 30;
    const peak = Math.max(1, ...h.bins),
      transform = (v) => (log ? Math.log1p(v) / Math.log1p(peak) : v / peak);
    context.fillStyle = '#56d8c1';
    for (let i = 0; i < 256; i++) {
      const height = transform(h.bins[i]) * plotHeight;
      context.fillRect(
        6 + (i / 256) * plotWidth,
        10 + plotHeight - height,
        Math.max(0.5, plotWidth / 256 - 0.5),
        height,
      );
    }
    context.fillStyle = '#c9d9e4';
    context.font = '12px sans-serif';
    context.textAlign = 'left';
    context.fillText(this.axisValue(h.low), 6, rect.height - 4);
    context.textAlign = 'right';
    context.fillText(this.axisValue(h.high), rect.width - 6, rect.height - 4);
    this.$('statistics').textContent =
      `${h.count.toLocaleString()} samples · Min ${this.valueText(h.min)} · Max ${this.valueText(h.max)} · Mean ${this.valueText(h.mean)}\nP01 ${this.valueText(h.p01)} · Median ${this.valueText(h.median)} · P99 ${this.valueText(h.p99)}${this.$('histogram').value === 'raw' ? ` · ${h.endpoints} samples at 0/65535 (possible clipping)` : ''}`;
  }
  provenance(point = null) {
    const r = this.current;
    if (!r?.plane || !this.decoded) return null;
    const index = point ? point.y * this.decoded.width + point.x : -1;
    return {
      schema: r.version,
      implementation: 'browser-js-float32-reflect-v1',
      identity: this.identity,
      inputSha256: r.inputSha256,
      method: r.settings.method,
      parameters: { ...r.settings },
      roi: { ...r.roi },
      psf: {
        model: 'assumed Gaussian, unmeasured',
        sigmaPixels: r.settings.sigma,
        fwhmPixels: 2.355 * r.settings.sigma,
      },
      noiseSigmaRaw: r.noise,
      noiseFloorApplied: r.noiseFloorApplied,
      noiseScope: r.noiseScope,
      responseNoise: r.scoreNoise,
      effectiveWienerK: r.k,
      wienerGainLimit: r.wienerGainLimit,
      candidateMethod: r.candidateMethod,
      scoreUnit: 'heuristic contrast sigma, not probability',
      candidateBorderPixels: r.border,
      displayRangeSource: 'full-frame 1–99 percentiles',
      displayLimits: this.limits,
      displayed: this.plane ? 'processed' : 'original',
      processedValue: index >= 0 ? r.plane[index] : null,
      processedUnit: r.unit,
      score: index >= 0 ? r.score[index] : null,
      scoreInsideRegion: point
        ? point.x >= r.roi.x &&
          point.x < r.roi.x + r.roi.width &&
          point.y >= r.roi.y &&
          point.y < r.roi.y + r.roi.height
        : null,
      temperatureFromOriginalOnly: true,
    };
  }
  exportHistogram() {
    const h = this.current && this.currentHistogram();
    if (!h) return;
    const raw = this.$('histogram').value === 'raw',
      cal = raw ? calibration(this.frame?.thermalMetadata) : null;
    const info = {
      schema: 'apcg.thermal-histogram.v1',
      identity: this.identity,
      inputSha256: this.current.inputSha256,
      roi: h.roi,
      count: h.count,
      source: raw ? 'raw' : 'processed',
      unit: cal ? 'Celsius from original calibration' : raw ? 'raw samples' : this.current.unit,
      binConvention: '[lower,upper), last bin closed',
      analysis: this.provenance(),
    };
    const lines = [`# ${JSON.stringify(info)}`, 'lower,upper,count'],
      value = (v) => (cal ? v * cal.scale + cal.offset - 273.15 : v);
    for (let i = 0; i < 256; i++)
      lines.push(
        `${value(h.low + ((h.high - h.low) * i) / 256)},${value(h.low + ((h.high - h.low) * (i + 1)) / 256)},${h.bins[i]}`,
      );
    const url = URL.createObjectURL(new Blob([lines.join('\n') + '\n'], { type: 'text/csv' })),
      a = document.createElement('a');
    a.href = url;
    a.download = 'thermal-histogram.csv';
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  activity() {
    this.$('apply').disabled = this.decoded?.kind !== 'gray16' || this.playing() || this.loading();
  }
  axisValue(value) {
    const c =
      this.$('histogram').value === 'raw' ? celsius(value, this.frame?.thermalMetadata) : null;
    return c === null ? value.toFixed(2) : `${c.toFixed(2)} °C`;
  }
  candidateProvenance(point) {
    const p = this.provenance(point);
    if (p) delete p.displayed;
    return p;
  }
  paused() {
    if (this.root.open && this.decoded?.kind === 'gray16' && !this.result && !this.busy)
      this.run(true);
  }
  close() {
    this.serial++;
    this.client.cancel();
    this.resizeObserver.disconnect();
  }
}
