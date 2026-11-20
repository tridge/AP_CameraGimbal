// Export in a module worker: {type: 'export', bytes (params.bin or PLY), opts (exportSplat
// options without transcodeSpz)} -> {type: 'exported', files, info, spzReport} (file buffers
// transferred) or {type: 'error', message}.
import { transcodeSpz } from '@sparkjsdev/spark';
import { exportSplat, loadSplat } from './export.js';

self.onmessage = async ({ data: m }) => {
  if (m.type !== 'export') return;
  try {
    const out = await exportSplat(loadSplat(m.bytes), { ...m.opts, transcodeSpz });
    const buffers = Object.values(out.files).map((b) => b.buffer);
    self.postMessage({ type: 'exported', ...out }, buffers);
  } catch (e) {
    self.postMessage({ type: 'error', message: e?.message ?? String(e) });
  }
};
