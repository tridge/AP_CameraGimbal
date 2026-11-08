// Progress and remaining-time model (PLAN.md "User experience").
// A stage estimate: { total, seconds, overhead, cumCost(u), checkpointEvery, checkpointSeconds }.
// seconds is the prior work time for the detected GPU class, overhead the prior non-work time
// (compile, load, evaluation); checkpoints are counted separately at their measured duration.
// cumCost maps units done to cost done (default linear); training uses it to slow with Gaussians.

const linear = (u) => u;

// phases whose time is not throughput; any other label (e.g. 'index', 'select') is work unless
// the stage says otherwise with phase(name, 'overhead')
export const OVERHEAD_PHASES = new Set([
  'compile',
  'load',
  'startup',
  'eval',
  'checkpoint',
  'done',
]);
export const phaseKind = (phase, kind) =>
  kind ?? (OVERHEAD_PHASES.has(phase) ? 'overhead' : 'work');

export class StageClock {
  constructor(estimate, now = performance.now()) {
    this.est = { overhead: 0, cumCost: linear, ...estimate };
    this.phase = 'work';
    this.kind = 'work';
    this.phaseStart = now;
    this.phaseTime = {}; // ms per phase label
    this.workAcc = 0; // ms in finished work-kind phases
    this.samples = []; // [workMs, cost]
    this.rate = null; // cost per ms, smoothed
    this.done = 0;
    this.total = estimate.total ?? null;
  }

  workMs(now = performance.now()) {
    return this.workAcc + (this.kind === 'work' ? now - this.phaseStart : 0);
  }

  elapsedMs(now = performance.now()) {
    return Object.values(this.phaseTime).reduce((a, b) => a + b, 0) + (now - this.phaseStart);
  }

  setPhase(phase, kind, now = performance.now()) {
    kind = phaseKind(phase, kind);
    if (phase === this.phase && kind === this.kind) return;
    if (phase === 'checkpoint') this.checkpoints = (this.checkpoints ?? 0) + 1;
    this.phaseTime[this.phase] = (this.phaseTime[this.phase] ?? 0) + now - this.phaseStart;
    if (this.kind === 'work') this.workAcc += now - this.phaseStart;
    this.phase = phase;
    this.kind = kind;
    this.phaseStart = now;
  }

  progress(done, total, now = performance.now()) {
    if (total != null) this.total = total;
    this.done = done;
    const w = this.workMs(now);
    const c = this.est.cumCost(done);
    const last = this.samples.at(-1);
    if (!last || w - last[0] >= 250) {
      this.samples.push([w, c]);
      if (this.samples.length > 400) this.samples.splice(0, 200);
      // rate over the last ~20% of work time (at least 2 s): adapts, but not to single ticks
      const span = Math.max(2000, w * 0.2);
      const first = this.samples.find((s) => s[0] >= w - span) ?? this.samples[0];
      if (w - first[0] >= 500 && c > first[1]) this.rate = (c - first[1]) / (w - first[0]);
    }
  }

  // seconds of work left, or null if unknown
  remaining(now = performance.now()) {
    const { seconds, overhead, cumCost, checkpointEvery, checkpointSeconds } = this.est;
    const ckptMs =
      (this.phaseTime.checkpoint ?? 0) + (this.phase === 'checkpoint' ? now - this.phaseStart : 0);
    const elapsedOverhead = (this.elapsedMs(now) - this.workMs(now) - ckptMs) / 1000; // compile, load, eval
    let over = Math.max(0, overhead - elapsedOverhead);
    if (checkpointEvery && this.total) {
      // checkpoints still to write, at their measured (else prior) duration
      const future =
        Math.floor((this.total - 1) / checkpointEvery) - Math.floor(this.done / checkpointEvery);
      const per = this.checkpoints ? ckptMs / 1000 / this.checkpoints : (checkpointSeconds ?? 0);
      over += Math.max(0, future) * per;
    }
    if (!this.total) {
      // unknown amount of work: the prior while it lasts, then unknown (never counts to zero)
      const left = seconds != null ? seconds - this.workMs(now) / 1000 : -1;
      return left > 0 ? left + over : null;
    }
    const left = cumCost(this.total) - cumCost(this.done);
    const frac = cumCost(this.done) / cumCost(this.total);
    const prior = seconds != null ? seconds * (left / cumCost(this.total)) : null;
    if (this.rate == null) return prior != null ? prior + over : null;
    const measured = left / this.rate / 1000;
    if (prior == null) return measured + over;
    const trust = Math.min(1, frac * 4 + this.workMs(now) / 20000); // measured dominates quickly
    return trust * measured + (1 - trust) * prior + over;
  }
}

export class PipelineEta {
  constructor() {
    this.completed = []; // { prior, actual } seconds
  }

  // calibration of priors from completed stages, clamped
  factor() {
    const p = this.completed.reduce((a, c) => a + c.prior, 0);
    const a = this.completed.reduce((s, c) => s + c.actual, 0);
    return p > 0 ? Math.min(4, Math.max(0.25, a / p)) : 1;
  }

  complete(prior, actual) {
    if (prior > 0) this.completed.push({ prior, actual });
  }

  // current: remaining seconds of the running stage (or null), waiting: prior seconds list
  // current === null: the running stage's remaining time is unknown, so only a lower bound
  overall(current, waiting) {
    const f = this.factor();
    const rest = waiting.reduce((a, s) => a + s * f, 0);
    if (current === null) return { seconds: null, low: rest, high: null };
    const mid = (current ?? 0) + rest;
    const spread = 1 / (1 + this.completed.length); // 100% before anything finished, narrowing
    return {
      seconds: mid,
      low: (current ?? 0) + rest / (1 + spread),
      high: (current ?? 0) + rest * (1 + spread),
    };
  }
}

// "about 3 min", "about 2-6 min", "under a minute"
export function formatEta(sec, low, high) {
  if (sec == null || !isFinite(sec))
    return low >= 45 ? `more than ${formatEta(low).replace('about ', '')}` : 'unknown';
  const r = (s) => {
    if (s < 60) return null;
    if (s < 600) return Math.round(s / 60);
    if (s < 3600) return Math.round(s / 300) * 5;
    return Math.round(s / 360) / 10;
  };
  const unit = (s) => (s < 3600 ? 'min' : 'h');
  if (sec < 45) return 'under a minute';
  if (
    low != null &&
    high != null &&
    high > low * 1.5 &&
    unit(low) === unit(high) &&
    r(low) &&
    r(high) !== r(low)
  ) {
    return `about ${r(low)}-${r(high)} ${unit(high)}`;
  }
  return `about ${r(Math.max(sec, 60))} ${unit(sec)}`;
}

export function formatDuration(ms) {
  const s = Math.round(ms / 1000);
  if (s < 60) return `${s} s`;
  if (s < 3600) return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  return `${Math.floor(s / 3600)}:${String(Math.floor(s / 60) % 60).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}
