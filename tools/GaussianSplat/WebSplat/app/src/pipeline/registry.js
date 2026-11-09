// Stage definitions and stage sets. See CONTRACTS.md "Stage module interface".
export const STATES = [
  'waiting',
  'running',
  'checkpointing',
  'paused',
  'interrupted',
  'done',
  'failed',
];

const required = ['id', 'title', 'version', 'unit', 'run'];

export function defineStage(def) {
  for (const k of required)
    if (def[k] === undefined) throw new Error(`stage definition lacks ${k}`);
  if (!/^[a-z0-9-]+$/.test(def.id)) throw new Error(`bad stage id ${def.id}`);
  return Object.freeze({
    inputs: [], // ids of upstream stages, or 'source'
    configKeys: [], // job config keys that change this stage's output
    checkpoints: false,
    ready: null, // (config, results) => false makes the pipeline wait for user input first
    estimate: () => ({ total: null, seconds: 60 }),
    ...def,
  });
}

const sets = new Map();

// source: false for a set that works without a recording (the page gives it a placeholder source)
export function registerStageSet(name, stages, { title = name, source = true } = {}) {
  const ids = new Set();
  for (const s of stages) {
    if (ids.has(s.id)) throw new Error(`duplicate stage ${s.id}`);
    for (const i of s.inputs) {
      if (i !== 'source' && !ids.has(i))
        throw new Error(`stage ${s.id} input ${i} must come earlier`);
    }
    ids.add(s.id);
  }
  sets.set(name, { name, title, stages, source });
}

export const stageSet = (name) => sets.get(name);
export const stageSets = () => [...sets.values()];
