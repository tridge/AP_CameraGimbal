// Stage 1 definition (pipeline/registry.js). Needs the source File as ctx.sourceFile.
// config.proposal: {start, end, hotspot_min_c, hotspot_max_error, hfov} for the proposal; other
// keys are ignored, so editing the target (config.select) does not re-run ingest.
import { defineStage } from '../../pipeline/registry.js';
import { FFMPEG_VERSION } from '../../../../../../viewer/src/versions.js';
import { INGEST_VERSION } from './ingest.js';
import { assetURL } from '../../asset-url.js';

// the viewer's pinned FFmpeg core, served by the app under vendor/
export const coreURL = () => assetURL(`vendor/ffmpeg/${FFMPEG_VERSION}/ffmpeg-core.js`);

export const ingestStage = defineStage({
  id: 'ingest',
  title: 'Read the video',
  version: INGEST_VERSION,
  unit: 'bytes',
  inputs: ['source'],
  // proposal.json depends on these settings (select.js PROPOSAL_KEYS): config.proposal holds them
  configKeys: ['proposal'],
  // measured on the laptop: about 0.2 s for the 32 MB orbit, plus the proposal's hot frames
  estimate: (config, env) => ({ total: (env.sourceSize ?? 0) * 2 || null, seconds: 1 }),
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./ingest.worker.js', import.meta.url), { type: 'module' }),
      {
        file: ctx.sourceFile,
        coreURL: coreURL(),
        proposal: ctx.config.proposal ?? {},
      },
    ),
});
