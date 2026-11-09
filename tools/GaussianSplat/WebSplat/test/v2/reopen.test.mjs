// A finished request job reopened with other config: which stages the controller invalidates.
// usage (WebSplat directory): node --test test/v2/*.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Controller } from '../../app/src/pipeline/controller.js';
import { stageSet } from '../../app/src/stages/index.js';
import { requestIdentity, validateRequest } from '../../app/src/request.js';

// the Job methods validate() uses, in memory
class MemoryJob {
  constructor(config) {
    this.manifest = { source: { hash: 'sha256c:00' }, config, stages: {} };
    this.resets = [];
  }
  stage(id) {
    return (this.manifest.stages[id] ??= { state: 'waiting' });
  }
  durableStage(id) {
    return this.manifest.stages[id] ?? null;
  }
  async reset(id) {
    this.resets.push(id);
    this.manifest.stages[id] = { state: 'waiting' };
  }
  async save() {}
}

const request = validateRequest({
  format: 'websplat.request',
  version: 1,
  source: { name: 'orbit.mkv', size: 100 },
  range: {
    timescale: 1000,
    start: { pts: 0, offset: 470, size: 156427 },
    end: { pts: 1300, offset: 158502, size: 156226 },
  },
  target: { lat: -35.58, lon: 148.9, alt_amsl: 1010, refine: true },
  view: { camera: { lat: -35.579, lon: 148.9, alt_amsl: 1150 }, fov_deg: 45 },
  title: 'first',
});

// a job as main.js writes it for the request, every stage finished under its current key
async function finishedJob() {
  const id = requestIdentity(request);
  const config = {
    request: { ...id, source: { name: 'orbit.mkv', size: 100 } },
    proposal: { range: id.range },
    select: { lat: -35.58, lon: 148.9, target_alt: 32, start: 0, end: 1, step: 1 },
    export: { title: 'first', view: request.view },
  };
  const job = new MemoryJob(config);
  const set = stageSet('thermal');
  const c = new Controller({ set, job, config });
  for (const def of set.stages) {
    const key = await c.key(def);
    Object.assign(job.stage(def.id), {
      state: 'done',
      version: def.version,
      key,
      outputsHash: `sha256:${def.id}`,
    });
  }
  return { job, c };
}

test('a finished job validates unchanged', async () => {
  const { job, c } = await finishedJob();
  assert.deepEqual(await c.validate(), []);
  assert.deepEqual(job.resets, []);
});

test('a new title on reopen invalidates export only', async () => {
  const { job, c } = await finishedJob();
  // main.js fromViewer on reopen
  job.manifest.config.export = { ...job.manifest.config.export, title: 'renamed' };
  assert.deepEqual(await c.validate(), ['export']);
  const states = Object.fromEntries(
    Object.entries(job.manifest.stages).map(([k, v]) => [k, v.state]),
  );
  assert.deepEqual(states, {
    ingest: 'done',
    select: 'done',
    features: 'done',
    match: 'done',
    map: 'done',
    prepare: 'done',
    train: 'done',
    export: 'waiting',
  });
});

test('config.request is in no stage key', async () => {
  const { c, job } = await finishedJob();
  job.manifest.config.request = { ...job.manifest.config.request, source: { name: 'x', size: 1 } };
  assert.deepEqual(await c.validate(), []);
});

test('another range invalidates everything from ingest', async () => {
  const { c, job } = await finishedJob();
  job.manifest.config.proposal = {
    range: { ...job.manifest.config.proposal.range, timescale: 90000 },
  };
  assert.equal((await c.validate()).length, 8);
});
