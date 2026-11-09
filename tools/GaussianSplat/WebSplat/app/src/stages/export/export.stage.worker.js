// Stage 7 worker (workers/runtime.js protocol), one pass per worker: msg.pass 'ply' | 'spz' with
// {selectDir, prepareDir, trainDir, outDir, title, description} and, for 'spz', the PLY pass's
// result (ply). Files are OPFS sync access handles (stream.js); 'spz' also writes meta.json.
import { serveStage } from '../../workers/runtime.js';
import { dir, readJson } from '../../storage/opfs.js';
import { metaPass, plyPass, spzPass } from './stream.js';

async function handle(path, create) {
  const parts = path.split('/').filter(Boolean);
  const d = await dir(parts.slice(0, -1).join('/'), { create });
  const h = await (await d.getFileHandle(parts.at(-1), { create })).createSyncAccessHandle();
  const read = (offset, length) => {
    const u = new Uint8Array(length);
    if (h.read(u, { at: offset }) !== length) throw new Error(`${path}: short read`);
    return u;
  };
  return {
    size: h.getSize(),
    read,
    write(bytes, offset) {
      if (h.write(bytes, { at: offset }) !== bytes.length) throw new Error(`${path}: short write`);
    },
    truncate: (n) => h.truncate(n),
    flush: () => h.flush(),
    close: () => h.close(),
  };
}

serveStage(async (msg, api) => {
  const t0 = performance.now();
  const open = [];
  const h = async (p, create = false) => {
    const x = await handle(p, create);
    open.push(x);
    return x;
  };
  try {
    if (msg.pass === 'ply') {
      api.phase('load');
      const train = await readJson(`${msg.trainDir}/train.json`);
      if (train.format !== 'websplat.train' || train.version !== 1)
        throw new Error(`train.json: ${train.format} v${train.version} is not supported`);
      const dataset = await readJson(`${msg.prepareDir}/dataset.json`);
      const frames = await readJson(`${msg.selectDir}/frames.json`);
      const input = await h(`${msg.trainDir}/${train.splat}`);
      const ply = await h(`${msg.outDir}/splat.ply`, true);
      api.phase('ply');
      const r = await plyPass({
        input,
        dataset,
        frames,
        title: msg.title,
        description: msg.description,
        rasterizeMode: train.rasterize_mode ?? 'classic',
        ply,
        progress: (d, n) => api.progress(d, 2 * n),
        throwIfStopped: () => api.throwIfStopped(),
      });
      return { ...r, ms: performance.now() - t0 };
    }
    if (msg.pass === 'spz') {
      api.phase('spz');
      const { transcodeSpz } = await import('@sparkjsdev/spark');
      const ply = await h(`${msg.outDir}/splat.ply`);
      const spz = await h(`${msg.outDir}/splat.spz`, true);
      const raw = await h(`${msg.outDir}/spz.raw`, true);
      const n = msg.ply.count;
      const r = await spzPass({
        ply,
        plySize: msg.ply.plySize,
        spz,
        raw,
        rasterizeMode: msg.ply.info.rasterize_mode,
        scale: msg.ply.info.temperature_scale,
        transcodeSpz,
        progress: (d) => api.progress(n + d, 2 * n),
        throwIfStopped: () => api.throwIfStopped(),
      });
      raw.close(); // the stage removes it (stage.js)
      api.phase('write');
      const meta = await h(`${msg.outDir}/meta.json`, true);
      const metaSize = metaPass({
        info: msg.ply.info,
        downloads: { ply: msg.ply.plySize, spz: r.spzSize },
        meta,
      });
      return { ...r, metaSize, ms: performance.now() - t0 };
    }
    throw new Error(`unknown pass ${msg.pass}`);
  } finally {
    for (const x of open) {
      try {
        x.close();
      } catch {
        // already closed
      }
    }
  }
});
