// Stand-in stages: they load prepared files instead of computing them. 'train' stands in for
// stage 6 until it is written (a trained splat: Brush PLY or WSSP params.bin, CONTRACTS.md 6);
// the 'export-test' set also uses them for select and prepare. config.standin[id] =
// {files: {name: url}}: http(s) URLs, or 'opfs:PATH' for a file the page copied into storage.
import { defineStage } from '../../pipeline/registry.js';
import { readFile, readJson, writeFile, writeJson } from '../../storage/opfs.js';
import { parsePlyHeader } from './ply.js';

async function source(url, signal) {
  if (url.startsWith('opfs:')) {
    const f = await readFile(url.slice(5));
    return { size: f.size, stream: f.stream() };
  }
  const r = await fetch(url, { signal });
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return { size: Number(r.headers.get('content-length')) || null, stream: r.body };
}

// the stage 6 record for a stand-in trained splat (the real stage writes its own)
async function trainJson(dir, files) {
  const name = Object.keys(files).find((n) => n === 'splat.ply' || n === 'params.bin');
  if (!name) throw new Error('a trained splat needs splat.ply or params.bin');
  const head = new Uint8Array(
    await (await readFile(`${dir}/${name}`)).slice(0, 1 << 20).arrayBuffer(),
  );
  let gaussians;
  let shDegree;
  if (name === 'params.bin') {
    const u = new Uint32Array(head.slice(0, 16).buffer);
    if (u[0] !== 0x50535357) throw new Error('params.bin: not a WSSP file');
    [gaussians, shDegree] = [u[2], u[3]];
  } else {
    const h = parsePlyHeader(head);
    gaussians = h.count;
    shDegree =
      Math.round(Math.sqrt(h.names.filter((n) => n.startsWith('f_rest_')).length / 3 + 1)) - 1;
  }
  return {
    format: 'websplat.train',
    version: 1,
    splat: name,
    gaussians,
    sh_degree: shDegree,
    rasterize_mode: 'classic',
    eval: null,
    standin: true,
  };
}

export const STANDIN_VERSION = 1;

export function standinStage(id, title, inputs = []) {
  return defineStage({
    id,
    title: `${title} (stand-in: loads prepared files)`,
    // a version no real stage has: when the real stage replaces a stand-in, the job's stand-in
    // outputs (and everything made from them) fail validation and are redone
    version: `standin-${STANDIN_VERSION}`,
    unit: 'bytes',
    inputs,
    configKeys: ['standin'],
    ready: (config) => !!config.standin?.[id]?.files,
    estimate: () => ({ total: null, seconds: 5 }),
    async run(ctx) {
      const files = ctx.config.standin[id].files;
      const opened = [];
      for (const [name, url] of Object.entries(files)) {
        if (!/^[\w.-]+(\/[\w.-]+)?$/.test(name)) throw new Error(`bad file name ${name}`);
        opened.push([name, await source(url, ctx.signal)]);
      }
      const total = opened.every(([, s]) => s.size)
        ? opened.reduce((a, [, s]) => a + s.size, 0)
        : null;
      let done = 0;
      ctx.phase('load');
      for (const [name, s] of opened) {
        const reader = s.stream.getReader();
        await writeFile(
          `${ctx.outDir}/${name}`,
          (async function* () {
            for (;;) {
              if (ctx.signal.aborted) {
                reader.cancel();
                const e = new Error('stopped');
                e.name = 'AbortError';
                throw e;
              }
              const { done: end, value } = await reader.read();
              if (end) return;
              done += value.length;
              ctx.progress(done, total);
              yield value;
            }
          })(),
        );
      }
      ctx.progress(done, done); // the total, also when the server sent no length
      if (id === 'train') {
        let t;
        if (files['train.json']) t = await readJson(`${ctx.outDir}/train.json`);
        else await writeJson(`${ctx.outDir}/train.json`, (t = await trainJson(ctx.outDir, files)));
        return { ...t, standin: true, bytes: done };
      }
      return { files: Object.keys(files), bytes: done };
    },
  });
}
