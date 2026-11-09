// OPFS helpers; paths are '/'-separated and relative to the app root directory 'websplat'.
const ROOT = 'websplat';

export async function root() {
  if (!navigator.storage?.getDirectory)
    throw new StorageError('this browser has no Origin Private File System');
  return (await navigator.storage.getDirectory()).getDirectoryHandle(ROOT, { create: true });
}

export class StorageError extends Error {
  name = 'StorageError';
}

const parts = (path) => path.split('/').filter(Boolean);

export async function dir(path, { create = false } = {}) {
  let d = await root();
  for (const p of parts(path)) d = await d.getDirectoryHandle(p, { create });
  return d;
}

export async function exists(path) {
  try {
    const ps = parts(path);
    const d = await dir(ps.slice(0, -1).join('/'));
    const name = ps.at(-1);
    for await (const k of d.keys()) if (k === name) return true;
    return false;
  } catch (e) {
    if (e.name === 'NotFoundError' || e.name === 'TypeMismatchError') return false;
    throw e;
  }
}

async function fileHandle(path, create) {
  const ps = parts(path);
  const d = await dir(ps.slice(0, -1).join('/'), { create });
  return d.getFileHandle(ps.at(-1), { create });
}

// data: ArrayBuffer, view, Blob or string; or a (sync or async) generator of chunks
export async function writeFile(path, data) {
  const h = await fileHandle(path, true);
  const w = await h.createWritable();
  try {
    if (typeof data?.next === 'function') {
      // a sync or async generator of chunks
      for await (const chunk of data) await w.write(chunk);
    } else {
      await w.write(data);
    }
    await w.close();
  } catch (e) {
    await w.abort().catch(() => {});
    throw quota(e);
  }
}

export const writeJson = (path, v) => writeFile(path, JSON.stringify(v, null, 1));

export async function readFile(path) {
  return (await fileHandle(path, false)).getFile();
}

export async function readJson(path) {
  return JSON.parse(await (await readFile(path)).text());
}

// a worker that was stopped or whose page was reloaded may still hold a file's access handle
// for a moment: removal retries with backoff (about 10 s in all) before giving up
export async function remove(path) {
  const ps = parts(path);
  for (let i = 0, wait = 50; ; i++, wait = Math.min(2 * wait, 1000)) {
    try {
      const d = await dir(ps.slice(0, -1).join('/'));
      await d.removeEntry(ps.at(-1), { recursive: true });
      return;
    } catch (e) {
      if (e.name === 'NotFoundError') return;
      if (e.name !== 'NoModificationAllowedError' || i >= 15) throw e;
      await new Promise((r) => setTimeout(r, wait));
    }
  }
}

export async function list(path) {
  const out = [];
  try {
    const d = await dir(path);
    for await (const [name, h] of d.entries()) out.push({ name, kind: h.kind });
  } catch (e) {
    if (e.name !== 'NotFoundError') throw e;
  }
  return out.sort((a, b) => a.name.localeCompare(b.name));
}

// all files below path, as paths relative to it
export async function walk(path, prefix = '') {
  const out = [];
  for (const e of await list(path)) {
    const rel = prefix ? `${prefix}/${e.name}` : e.name;
    if (e.kind === 'directory') out.push(...(await walk(`${path}/${e.name}`, rel)));
    else out.push(rel);
  }
  return out;
}

function quota(e) {
  if (e?.name === 'QuotaExceededError') {
    const s = new StorageError('browser storage is full (quota exceeded)');
    s.quota = true;
    return s;
  }
  return e;
}

export async function storageInfo() {
  const est = (await navigator.storage?.estimate?.()) ?? {};
  const persisted = (await navigator.storage?.persisted?.()) ?? false;
  return { usage: est.usage ?? null, quota: est.quota ?? null, persisted };
}

// returns true if granted; denial is reported, not fatal
export async function requestPersistence() {
  try {
    if (await navigator.storage.persisted()) return true;
    return await navigator.storage.persist();
  } catch {
    return false;
  }
}
