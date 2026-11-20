// Stage 1 for RGB recordings (pipeline/registry.js); config.proposal: {range} from a request.
import { defineStage } from '../../pipeline/registry.js';
import { RGB_INGEST_VERSION } from './rgb.js';

export const rgbIngestStage = defineStage({
  id: 'ingest',
  title: 'Read the video',
  version: `rgb-${RGB_INGEST_VERSION}`,
  unit: 'bytes',
  inputs: ['source'],
  configKeys: ['proposal'],
  // indexing reads the transport packets (about 3 s for 470 MB), then hashing
  estimate: (config, env) => ({ total: (env.sourceSize ?? 0) * 2 || null, seconds: 8 }),
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./rgb.ingest.worker.js', import.meta.url), { type: 'module' }),
      { file: ctx.sourceFile, proposal: ctx.config.proposal ?? {} },
    ),
});
