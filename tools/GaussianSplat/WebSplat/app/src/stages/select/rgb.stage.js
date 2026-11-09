// Stage 2 for RGB recordings (pipeline/registry.js): config.select (RGB_DEFAULTS names).
import { defineStage } from '../../pipeline/registry.js';
import { coreURL } from '../ingest/stage.js';
import { RGB_SELECT_VERSION } from './rgb-select.js';

export const rgbSelectStage = defineStage({
  id: 'select',
  title: 'Select frames',
  version: `rgb-${RGB_SELECT_VERSION}`,
  unit: 'frames',
  inputs: ['ingest'],
  configKeys: ['select'],
  ready: (config) => !!config.select,
  // decoding dominates: WebCodecs is fast, FFmpeg (e.g. HEVC on Linux) much slower
  estimate: (config, env) => {
    const n = Math.min(
      config.select?.budget ?? 1000,
      env.results?.ingest?.range?.frames / 10 || 500,
    );
    const px = (config.select?.width ?? 1280) / 1280;
    return { total: null, seconds: n * 0.15 * px * px, uncertain: true };
  },
  run: (ctx) =>
    ctx.runWorker(
      () => new Worker(new URL('./rgb.select.worker.js', import.meta.url), { type: 'module' }),
      {
        file: ctx.sourceFile,
        coreURL: coreURL(),
        ingestDir: ctx.input('ingest').dir,
        homeAltitude: ctx.input('ingest').result?.homeAltitude,
        select: ctx.config.select,
        decoder: ctx.config.rgbDecoder,
      },
    ),
});
