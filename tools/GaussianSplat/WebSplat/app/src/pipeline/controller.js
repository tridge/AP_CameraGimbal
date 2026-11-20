// Runs a stage set for a job: invalidation, resume, states, checkpoints, progress and ETA.
import { hashJson } from '../storage/hash.js';
import { runWorker, StageAborted } from '../workers/host.js';
import { PipelineEta, StageClock, formatEta } from './eta.js';

const pick = (o, keys) => Object.fromEntries(keys.map((k) => [k, o?.[k] ?? null]));
const durableCkpt = (r) =>
  r?.checkpoint ? { generation: r.checkpoint.generation, done: r.checkpoint.done ?? null } : null;
const isAbort = (e) => e instanceof StageAborted || e?.name === 'AbortError';
const errInfo = (e) => ({
  name: e?.name ?? 'Error',
  message: e?.message ?? String(e),
  quota: !!e?.quota || e?.name === 'QuotaExceededError',
});

export class Controller extends EventTarget {
  constructor({ set, job, config = {}, env = {}, sourceFile = null }) {
    super();
    this.set = set;
    this.stages = set.stages;
    this.job = job;
    this.config = config;
    this.env = env;
    this.sourceFile = sourceFile;
    this.needs = null; // id of a stage waiting for user input (def.ready(config) was false)
    this.clocks = new Map();
    this.eta = new PipelineEta();
    this.events = [];
    this.running = false;
    this.runPromise = null;
    this.abort = null;
    this.run = 0; // identifies the current stage run; stale publications are refused
    this.lastSave = 0;
    this.storageError = null;
    this.channels = new Map(); // stage id -> command channel of its running worker
  }

  // a command to a running stage's worker (e.g. train 'preview'); rejects when none runs
  command(id, name, data) {
    const ch = this.channels.get(id);
    if (!ch?.request) return Promise.reject(new Error(`${id} is not running`));
    return ch.request(name, data);
  }

  // stop and accept: the stage ends with what it has (train: the iterations done so far). Recorded
  // in the manifest (record.accept) before the stage completes, so a reload in between finishes it
  // the same way (from its checkpoint); a stopped stage needs a checkpoint and completes on the
  // next start.
  async acceptEarly(id) {
    const r = this.record(id);
    if (r.state === 'done') throw new Error(`${id} is already done`);
    const ch = this.channels.get(id);
    // starting: the run has already read record.accept but has no worker to stop yet
    if (ch && !ch.request) throw new Error(`${id} is starting; try again in a moment`);
    const running = ch?.request;
    if (!running && !r.checkpoint) throw new Error(`${id} has no checkpoint to accept`);
    // a running worker refuses before training has started
    if (running) await this.command(id, 'finish');
    const iteration = running ? (r.progress?.done ?? 0) : (r.checkpoint.done ?? 0);
    // in the write queue: a checkpoint publication in flight holds an older copy of the record
    await this.job.transact(id, (rec) => {
      rec.accept = { iteration, at: new Date().toISOString() };
    });
    this.emit();
  }

  record(id) {
    return this.job.stage(id);
  }

  setState(id, state, extra = {}) {
    const r = this.record(id);
    Object.assign(r, extra);
    if (r.state !== state) {
      r.state = state;
      this.events.push({ t: Date.now(), stage: id, state });
    }
    this.emit();
  }

  emit() {
    this.dispatchEvent(new Event('update'));
  }

  // results of finished stages (their workers' return values), for estimates and UI
  results() {
    return Object.fromEntries(
      this.stages
        .filter((d) => this.record(d.id).state === 'done')
        .map((d) => [d.id, this.record(d.id).result]),
    );
  }

  // env for estimates: device class plus what finished stages found (e.g. frame counts)
  envNow() {
    const r = this.results();
    return {
      ...this.env,
      frames: r.select?.frames ?? r.ingest?.frames ?? this.env.frames,
      results: r,
    };
  }

  async key(def) {
    const inputs = {};
    for (const i of def.inputs) {
      inputs[i] =
        i === 'source'
          ? (this.job.manifest.source?.hash ?? null)
          : (this.record(i).outputsHash ?? null);
      if (inputs[i] == null) return null;
    }
    return hashJson({
      id: def.id,
      version: def.version,
      config: pick(this.config, def.configKeys),
      inputs,
    });
  }

  // drop outputs whose inputs, config or version changed, and everything downstream of them
  async validate() {
    const reset = new Set();
    for (const def of this.stages) {
      const r = this.record(def.id);
      const upstreamReset = def.inputs.some((i) => reset.has(i));
      // an acceptance is state too: a changed key must drop it even without a checkpoint
      const hasData = r.state === 'done' || r.checkpoint || r.accept;
      if (!hasData && !upstreamReset) continue;
      const stale = upstreamReset || r.version !== def.version || r.key !== (await this.key(def));
      if (stale) {
        reset.add(def.id);
        await this.job.reset(def.id);
        this.events.push({ t: Date.now(), stage: def.id, state: 'waiting', reason: 'invalidated' });
      }
    }
    if (reset.size) await this.job.save();
    // calibrate the time model from stages finished before a reload
    this.eta = new PipelineEta();
    for (const def of this.stages) {
      const r = this.record(def.id);
      const work = r.timings?.workMs;
      if (r.state === 'done' && work != null)
        this.eta.complete(def.estimate(this.config, this.envNow()).seconds ?? 0, work / 1000);
    }
    this.emit();
    return [...reset];
  }

  start() {
    if (this.runPromise) return this.runPromise;
    this.runPromise = this.loop().finally(() => (this.runPromise = null));
    return this.runPromise;
  }

  async loop() {
    this.running = true;
    this.storageError = null;
    this.abort = new AbortController();
    this.emit();
    try {
      await this.validate();
      this.needs = null;
      for (const def of this.stages) {
        if (this.record(def.id).state === 'done') continue;
        if (def.ready && !def.ready(this.config, this.results())) {
          this.needs = def.id;
          this.log(def.id, 'info', 'waiting for input');
          break;
        }
        if (!(await this.runStage(def, this.abort.signal))) break;
      }
    } catch (e) {
      // only storage failures outside a stage get here (validate)
      this.storageError = errInfo(e);
      this.log('pipeline', 'error', `storage: ${e.message}`);
    } finally {
      this.running = false;
      this.abort = null;
      await this.job.save().catch((e) => (this.storageError ??= errInfo(e)));
      this.emit();
    }
  }

  stop() {
    this.abort?.abort();
  }

  // stop and wait until no stage work, publication or manifest write is in flight
  async shutdown() {
    this.stop();
    await this.runPromise?.catch(() => {});
    await this.job.saving;
  }

  // periodic progress saves: a failure (e.g. quota) stops the run as a storage error
  saveSoon() {
    if (performance.now() - this.lastSave < 5000) return;
    this.lastSave = performance.now();
    this.job.save().catch((e) => {
      this.storageError ??= errInfo(e);
      this.abort?.abort();
    });
  }

  async runStage(def, signal) {
    const job = this.job;
    const run = ++this.run;
    const current = () => this.run === run && !signal.aborted;
    const t0 = performance.now();
    let clock = null;
    let generation = null;
    let prevTimings = { elapsedMs: 0, workMs: 0, phases: {} };
    const timings = () => {
      if (!clock) return prevTimings;
      clock.setPhase('done', 'overhead');
      const phases = { ...prevTimings.phases };
      for (const [p, ms] of Object.entries(clock.phaseTime))
        if (p !== 'done') phases[p] = (phases[p] ?? 0) + ms;
      return {
        elapsedMs: prevTimings.elapsedMs + performance.now() - t0,
        workMs: (prevTimings.workMs ?? 0) + clock.workMs(),
        phases,
      };
    };
    try {
      const key = await this.key(def);
      const prev = this.record(def.id);
      const checkpoint = prev.checkpoint && prev.key === key ? prev.checkpoint : null;
      if (!checkpoint && prev.checkpoint) await job.reset(def.id);
      await job.removeUnpublished(def.id); // e.g. a failed earlier attempt's partial output
      const r = this.record(def.id);
      prevTimings = { workMs: 0, ...(r.timings ?? prevTimings) };
      generation = job.nextGeneration(def.id);
      const channel = {};
      this.channels.set(def.id, channel);
      let ckptGeneration = generation;
      const estimate = def.estimate(this.config, this.envNow());
      clock = new StageClock(estimate);
      if (checkpoint) clock.progress(checkpoint.done ?? 0, estimate.total);
      this.clocks.set(def.id, clock);
      this.setState(def.id, 'running', {
        key,
        version: def.version,
        error: null,
        runs: (r.runs ?? 0) + 1,
        startedAt: r.startedAt ?? new Date().toISOString(),
        progress: { unit: def.unit, done: checkpoint?.done ?? 0, total: estimate.total ?? null },
      });
      await job.save();

      const progress = (done, total, extra) => {
        if (!current()) return;
        clock.progress(done, total);
        r.progress = { unit: def.unit, done, total: total ?? clock.total, extra };
        this.emit();
        this.saveSoon();
      };
      const phase = (p, kind) => {
        if (!current()) return;
        clock.setPhase(p, kind);
        r.phase = p;
        this.emit();
      };
      const handlers = {
        progress,
        phase,
        log: (level, text) => this.log(def.id, level, text),
        checkpointBegin: () => {
          if (!current()) return;
          clock.setPhase('checkpoint', 'overhead');
          this.setState(def.id, 'checkpointing');
        },
        checkpoint: async (g, info) => {
          if (!current()) throw new StageAborted('stale checkpoint');
          await job.publishCheckpoint(def.id, g, info, current);
          if (!current()) return;
          clock.setPhase('work');
          this.setState(def.id, 'running');
        },
      };
      const ctx = {
        def,
        config: this.config,
        env: this.env,
        signal,
        accept: r.accept ?? null,
        jobDir: job.dir,
        sourceFile: this.sourceFile,
        outDir: job.path(def.id, 'gen', generation),
        checkpoint: checkpoint
          ? { ...checkpoint, dir: job.path(def.id, 'ckpt', checkpoint.generation) }
          : null,
        input: (id) => {
          const u = this.record(id);
          return { dir: job.path(id, 'gen', u.generation), outputs: u.outputs, result: u.result };
        },
        nextCheckpoint: () => {
          ckptGeneration = Math.max(ckptGeneration, checkpoint?.generation ?? 0) + 1;
          return { generation: ckptGeneration, dir: job.path(def.id, 'ckpt', ckptGeneration) };
        },
        progress,
        phase,
        log: (text, level = 'info') => this.log(def.id, level, text),
        runWorker: (factory, msg = {}, opts = {}) =>
          runWorker(
            factory,
            {
              outDir: ctx.outDir,
              checkpoint: ctx.checkpoint,
              ckptBase: job.path(def.id, 'ckpt', ''),
              firstCheckpointGeneration: Math.max(generation, checkpoint?.generation ?? 0) + 1,
              config: this.config,
              env: this.env,
              ...msg,
            },
            { signal, handlers, channel, ...opts },
          ),
      };

      const result = await def.run(ctx);
      if (signal.aborted) throw new StageAborted('stopped');
      const t = timings();
      // outputs and the done state in one manifest write
      await job.publishOutputs(def.id, generation, {
        state: 'done',
        phase: null,
        result: result ?? null,
        finishedAt: new Date().toISOString(),
        timings: t,
        // reported: the stage's own last count (a check that it reached its total)
        progress: {
          ...r.progress,
          reported: r.progress.done,
          // stopped early (accepted): the count reached, not the planned total
          done: result?.stopped_early ? r.progress.done : (r.progress.total ?? r.progress.done),
        },
      });
      this.events.push({ t: Date.now(), stage: def.id, state: 'done' });
      this.eta.complete(estimate.seconds ?? 0, t.workMs / 1000);
      this.emit();
      return true;
    } catch (e) {
      this.run++; // invalidates anything still in flight for this run
      const t = timings();
      const storage = this.storageError;
      if (isAbort(e) && !storage) {
        this.setState(def.id, 'paused', { timings: t, phase: null });
      } else {
        console.error(`stage ${def.id} failed`, e);
        this.setState(def.id, 'failed', { timings: t, phase: null, error: storage ?? errInfo(e) });
      }
      // release partial output so a retry has the space
      await this.job.removeUnpublished(def.id).catch(() => {});
      await this.job.save().catch((x) => (this.storageError ??= errInfo(x)));
      return false;
    } finally {
      this.clocks.delete(def.id);
      this.channels.delete(def.id);
    }
  }

  log(stage, level, text) {
    this.events.push({ t: Date.now(), stage, log: level, text });
    if (this.events.length > 2000) this.events.splice(0, 1000);
    this.dispatchEvent(new CustomEvent('log', { detail: { stage, level, text } }));
  }

  snapshot() {
    const now = performance.now();
    let current;
    let currentUncertain = false;
    const waiting = [];
    const factor = this.eta.factor();
    const stages = this.stages.map((def) => {
      const r = this.record(def.id);
      const clock = this.clocks.get(def.id);
      const remaining = clock ? clock.remaining(now) : null;
      let priorSec = null; // a waiting stage's estimate as the overall ETA counts it
      if (clock) {
        current = remaining;
        currentUncertain = remaining == null || !!clock.est.uncertain;
      } else if (r.state !== 'done') {
        const est = def.estimate(this.config, this.envNow());
        const ckpts =
          est.checkpointEvery && est.total ? Math.floor((est.total - 1) / est.checkpointEvery) : 0;
        const prior = (est.seconds ?? 60) + (est.overhead ?? 0) + ckpts * (est.checkpointSeconds ?? 0);
        waiting.push(prior);
        priorSec = prior * factor;
      }
      return {
        id: def.id,
        title: def.title,
        unit: def.unit,
        state: r.state,
        phase: r.phase ?? null,
        progress: r.progress ?? null,
        generation: r.generation ?? null,
        // only a checkpoint whose manifest write completed counts as published
        checkpoint: durableCkpt(this.job.durableStage(def.id)),
        runs: r.runs ?? 0,
        elapsedMs: (r.timings?.elapsedMs ?? 0) + (clock ? clock.elapsedMs(now) : 0),
        timings: r.timings ?? null,
        remainingSec: clock ? remaining : r.state === 'done' ? 0 : null,
        priorSec,
        uncertain: clock ? currentUncertain : false,
        error: r.error ?? null,
      };
    });
    const o = this.eta.overall(current, waiting);
    return {
      jobId: this.job.id,
      set: this.set.name,
      running: this.running,
      needs: this.needs,
      storageError: this.storageError,
      calibration: this.eta.factor(),
      stages,
      overall: {
        elapsedMs: stages.reduce((a, s) => a + s.elapsedMs, 0),
        remainingSec: o.seconds,
        low: o.low,
        high: o.high,
        uncertain: currentUncertain,
        text: stages.every((s) => s.state === 'done')
          ? 'done'
          : formatEta(o.seconds, o.low, o.high),
      },
    };
  }
}
