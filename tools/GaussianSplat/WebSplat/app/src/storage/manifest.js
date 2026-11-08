// Job manifest (CONTRACTS.md "Job manifest"): two alternating slots, highest valid seq wins.
import { hashJson } from './hash.js';
import { list, readFile, writeFile } from './opfs.js';

export const FORMAT = 'websplat.manifest';
export const VERSION = 1;
export const SLOTS = ['manifest.a.json', 'manifest.b.json'];

export class ManifestError extends Error {
  name = 'ManifestError';
  constructor(kind, message) {
    super(message);
    this.kind = kind; // 'corrupt' | 'unsupported'
  }
}

const checksum = (m) => hashJson({ ...m, checksum: '' });

// 'ok' with the manifest, or 'missing' / 'corrupt' / 'unsupported'
export async function validateSlot(path) {
  let text;
  try {
    text = await (await readFile(path)).text();
  } catch (e) {
    if (e.name === 'NotFoundError' || e.name === 'TypeMismatchError') return { status: 'missing' };
    throw e;
  }
  let m;
  try {
    m = JSON.parse(text);
  } catch {
    return { status: 'corrupt' };
  }
  if (m?.format !== FORMAT) return { status: 'corrupt' };
  if (m.version !== VERSION) return { status: 'unsupported', version: m.version };
  if (typeof m.seq !== 'number' || m.checksum !== (await checksum(m))) return { status: 'corrupt' };
  return { status: 'ok', manifest: m };
}

// { manifest, slot } for the newest valid slot; null if the job has no manifest at all;
// throws ManifestError if manifests exist but none is valid (the job's data is left alone)
export async function loadManifest(jobDir) {
  let best = null;
  const bad = [];
  for (const [slot, name] of SLOTS.entries()) {
    const r = await validateSlot(`${jobDir}/${name}`);
    if (r.status === 'ok') {
      if (!best || r.manifest.seq > best.manifest.seq) best = { manifest: r.manifest, slot };
    } else if (r.status !== 'missing') bad.push(r);
  }
  // any slot from another version means a different app has written this job: refuse it rather
  // than recover from an older slot (which would delete the newer version's data)
  if (bad.some((r) => r.status === 'unsupported')) {
    const v = bad.find((r) => r.status === 'unsupported').version;
    throw new ManifestError(
      'unsupported',
      `this job was written by another app version (manifest version ${v})`,
    );
  }
  if (best) return best; // a damaged slot next to a valid one: an interrupted write
  if (bad.length) throw new ManifestError('corrupt', 'this job’s manifest is damaged');
  if ((await list(jobDir)).length)
    throw new ManifestError('corrupt', 'this job has data but no manifest');
  return null;
}

export function newManifest(jobId, fields = {}) {
  return {
    format: FORMAT,
    version: VERSION,
    seq: 0,
    jobId,
    created: new Date().toISOString(),
    stages: {},
    ...fields,
    checksum: '',
  };
}

// writes an immutable snapshot of m with seq + 1 into the slot not holding the current manifest;
// m.seq advances only once the write succeeded. Returns { slot, snapshot }.
export async function saveManifest(jobDir, m, currentSlot) {
  const snap = JSON.parse(JSON.stringify({ ...m, seq: m.seq + 1, checksum: '' }));
  snap.checksum = await checksum(snap);
  const slot = currentSlot === 0 ? 1 : 0;
  await writeFile(`${jobDir}/${SLOTS[slot]}`, JSON.stringify(snap, null, 1));
  m.seq = snap.seq;
  m.checksum = snap.checksum;
  return { slot, snapshot: snap };
}
