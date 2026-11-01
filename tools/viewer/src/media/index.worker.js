import { openMedia } from './containers.js';
let source = null,
  generation = 0;
self.onmessage = async ({ data }) => {
  if (data.type !== 'open') return;
  const own = ++generation;
  try {
    source?.close();
    source = await openMedia(data.file);
    self.postMessage({ type: 'opened', tracks: source.tracks, format: source.format });
    for await (const batch of source.index()) {
      if (own !== generation) return;
      self.postMessage({ type: 'batch', ...batch });
    }
    self.postMessage({ type: 'done', tracks: source.tracks });
  } catch (error) {
    if (own === generation) self.postMessage({ type: 'error', message: error.message });
  }
};
