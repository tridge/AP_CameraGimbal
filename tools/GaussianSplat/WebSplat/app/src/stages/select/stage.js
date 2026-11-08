// Stage 2 definition (pipeline/registry.js). Options are config.select (CONTRACTS.md names);
// the UI fills them from ingest's proposal.json. Needs the source File as ctx.sourceFile.
import { defineStage } from '../../pipeline/registry.js';
import { coreURL } from '../ingest/stage.js';
import { SELECT_VERSION } from './select.js';

export const selectStage = defineStage({
  id: 'select',
  title: 'Select frames',
  version: SELECT_VERSION,
  unit: 'frames',
  inputs: ['ingest'],
  configKeys: ['select'],
  // measured on the laptop: about 3.5 s for 201 frames (decode, CLAHE, OPFS writes)
  estimate: (config, env) => ({ total: env.frames ?? null, seconds: 0.02 * (env.frames ?? 200) }),
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./select.worker.js', import.meta.url), { type: 'module' }),
      {
        file: ctx.sourceFile,
        coreURL: coreURL(),
        ingestDir: ctx.input('ingest').dir,
        select: ctx.config.select,
        mode: ctx.config.selectMode ?? 'single',
      },
    ),
});
