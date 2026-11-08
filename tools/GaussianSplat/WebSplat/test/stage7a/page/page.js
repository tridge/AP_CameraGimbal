// Stage 7a browser page: the export module in a worker on the tiny model, then the result in the
// embedded viewer (app/src/stages/export/embed.js). Driven by viewer_check.mjs.
import { embedViewer } from '../../../app/src/stages/export/embed.js';
import { exportInputs } from '../../../app/src/stages/export/export.js';

const get = async (url) => {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r;
};

function exportInWorker(bytes, opts) {
  const worker = new Worker(
    new URL('../../../app/src/stages/export/export.worker.js', import.meta.url),
    {
      type: 'module',
    },
  );
  return new Promise((resolve, reject) => {
    worker.onmessage = ({ data }) => {
      worker.terminate();
      if (data.type === 'exported') resolve(data);
      else reject(new Error(data.message));
    };
    worker.onerror = (e) => reject(new Error(e.message));
    worker.postMessage({ type: 'export', bytes, opts }, [bytes.buffer]);
  });
}

let viewer = null;

window.runExport = async (files, title) => {
  const params = new Uint8Array(await (await get(`${files}/params.bin`)).arrayBuffer());
  const dataset = await (await get(`${files}/dataset.json`)).json();
  const frames = await (await get(`${files}/frames.json`)).json();
  const t0 = performance.now();
  const out = await exportInWorker(params, { ...exportInputs(dataset, frames), title });
  window.result = out;
  return {
    ms: performance.now() - t0,
    sizes: Object.fromEntries(Object.entries(out.files).map(([k, v]) => [k, v.length])),
  };
};

// base64 of an exported file, for the driver to compare
window.fileBase64 = (name) => {
  const b = window.result.files[name];
  let s = '';
  for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode(...b.subarray(i, i + 0x8000));
  return btoa(s);
};

window.openResult = async (name, meta = null) => {
  if (!viewer) viewer = embedViewer(document.body, '../viewer/index.html');
  await viewer.open(window.result.files[name], name, meta);
};

window.resultMeta = () => JSON.parse(new TextDecoder().decode(window.result.files['meta.json']));
