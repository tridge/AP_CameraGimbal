// A job in OPFS: manifest, stage output generations and checkpoints (CONTRACTS.md).
import { hashBlob, hashJson } from './hash.js';
import { acquireJobLock } from './lock.js';
import { loadManifest, newManifest, saveManifest } from './manifest.js';
import { list, readFile, remove, walk } from './opfs.js';

export const jobDir = (jobId) => `jobs/${jobId}`;
const clone = (v) => (v === undefined ? undefined : JSON.parse(JSON.stringify(v)));

export class Job {
  constructor(jobId, lock, manifest, slot) {
    this.id = jobId;
    this.dir = jobDir(jobId);
    this.lock = lock;
    this.manifest = manifest;
    this.slot = slot;
    this.saving = Promise.resolve();
  }

  // throws JobLockedError if another tab has the job open, ManifestError if its manifest is
  // unreadable or from another version (nothing is deleted then)
  static async open(jobId, fields = null) {
    const lock = await acquireJobLock(jobId);
    try {
      const found = await loadManifest(jobDir(jobId));
      if (!found && !fields) return Object.assign(new Job(jobId, lock, null, 1), { missing: true });
      const job = found
        ? new Job(jobId, lock, found.manifest, found.slot)
        : new Job(jobId, lock, newManifest(jobId, fields), 1);
      job.durable = found ? JSON.parse(JSON.stringify(found.manifest)) : null;
      if (!found) await job.save();
      await job.recover();
      return job;
    } catch (e) {
      lock.release();
      e.jobId ??= jobId;
      throw e;
    }
  }

  close() {
    this.lock.release();
  }

  // the stage record as last written to storage (null if never)
  durableStage(id) {
    return this.durable?.stages?.[id] ?? null;
  }

  stage(id) {
    return (this.manifest.stages[id] ??= { state: 'waiting' });
  }

  // test hook: (kind: 'save' | 'transact') => Error to throw instead of writing, or null
  fault(kind) {
    const e = this.injectFault?.(kind);
    if (e) throw e;
  }

  // queue fn after every earlier manifest write; writes never overlap
  enqueue(fn) {
    const p = this.saving.then(fn, fn);
    this.saving = p.catch(() => {});
    return p;
  }

  async write(m) {
    const r = await saveManifest(this.dir, m, this.slot);
    this.slot = r.slot;
    this.durable = r.snapshot; // what a reload would see
  }

  // ordinary save of the live manifest; a failed write rejects its caller, later writes still run
  save() {
    return this.enqueue(async () => {
      this.fault('save');
      await this.write(this.manifest);
    });
  }

  // a change to one stage record that becomes visible only once written: fn(record) runs on a
  // copy, in the write queue (so no earlier save can publish it), and the live record takes the
  // result after the write succeeded. Returns fn's value.
  transact(stageId, fn) {
    return this.enqueue(async () => {
      const next = clone(this.manifest);
      const rec = (next.stages[stageId] ??= { state: 'waiting' });
      const value = fn(rec);
      this.fault('transact');
      await this.write(next);
      this.manifest.seq = next.seq;
      this.manifest.checksum = next.checksum;
      const live = this.stage(stageId); // keep the object identity callers hold
      for (const k of Object.keys(live)) if (!(k in rec)) delete live[k];
      Object.assign(live, rec);
      return value;
    });
  }

  path(stageId, kind, generation) {
    return `${this.dir}/${stageId}/${kind}-${generation}`;
  }

  nextGeneration(stageId) {
    const r = this.stage(stageId);
    const used = Math.max(r.generation ?? 0, r.checkpoint?.generation ?? 0, r.lastGeneration ?? 0);
    r.lastGeneration = used + 1;
    return used + 1;
  }

  // delete generation directories of a stage that the manifest does not reference
  async removeUnpublished(stageId) {
    const r = this.manifest.stages[stageId];
    for (const g of await list(`${this.dir}/${stageId}`)) {
      const keep =
        r && (g.name === `gen-${r.generation}` || g.name === `ckpt-${r.checkpoint?.generation}`);
      if (!keep) await remove(`${this.dir}/${stageId}/${g.name}`);
    }
  }

  // after a reload: interrupted stages, and generations that were never published
  async recover() {
    let changed = false;
    for (const r of Object.values(this.manifest.stages)) {
      if (r.state === 'running' || r.state === 'checkpointing') {
        r.state = 'interrupted';
        changed = true;
      }
    }
    for (const e of await list(this.dir)) {
      if (e.kind === 'directory' && e.name !== 'source') await this.removeUnpublished(e.name);
    }
    if (changed) await this.save();
  }

  async hashDir(path) {
    const outputs = {};
    for (const rel of await walk(path)) {
      const f = await readFile(`${path}/${rel}`);
      outputs[rel] = { path: `${path}/${rel}`, bytes: f.size, hash: await hashBlob(f) };
    }
    return outputs;
  }

  // publish a finished stage generation together with its final state (one manifest write);
  // earlier generations and checkpoints are removed afterwards
  async publishOutputs(stageId, generation, fields = {}) {
    const outputs = await this.hashDir(this.path(stageId, 'gen', generation));
    const outputsHash = await hashJson(Object.values(outputs).map((o) => o.hash));
    const old = await this.transact(stageId, (r) => {
      const was = { generation: r.generation, checkpoint: r.checkpoint?.generation };
      Object.assign(r, fields, { generation, outputs, outputsHash, checkpoint: null });
      return was;
    });
    if (old.generation && old.generation !== generation)
      await remove(this.path(stageId, 'gen', old.generation));
    if (old.checkpoint) await remove(this.path(stageId, 'ckpt', old.checkpoint));
    return outputs;
  }

  // isCurrent(): checked just before the manifest write, so a stale run cannot publish
  async publishCheckpoint(stageId, generation, info, isCurrent = () => true) {
    const outputs = await this.hashDir(this.path(stageId, 'ckpt', generation));
    const old = await this.transact(stageId, (r) => {
      if (!isCurrent()) throw new Error('stale checkpoint publication');
      const was = r.checkpoint?.generation;
      r.checkpoint = { generation, outputs, ...info };
      return was;
    });
    if (old && old !== generation) await remove(this.path(stageId, 'ckpt', old));
  }

  // drop a stage's outputs and checkpoint (invalidated)
  async reset(stageId) {
    await remove(`${this.dir}/${stageId}`);
    this.manifest.stages[stageId] = { state: 'waiting' };
  }

  // delete a job whose manifest cannot be read (explicit user action)
  static async discard(jobId) {
    const lock = await acquireJobLock(jobId);
    try {
      await remove(jobDir(jobId));
    } finally {
      lock.release();
    }
  }

  // the caller must have stopped all work on the job first
  async destroy() {
    await this.saving;
    await remove(this.dir);
    this.close();
  }
}
