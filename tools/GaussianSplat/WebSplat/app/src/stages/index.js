// Stage sets known to the app. Real stages are defined in app/src/stages/<stage>/stage.js
// (CONTRACTS.md "Stage module interface") and listed here in pipeline order.
import { registerStageSet } from '../pipeline/registry.js';
import { ingestStage } from './ingest/stage.js';
import { selectStage } from './select/stage.js';
import { featuresStage } from './features/stage.js';
import { matchStage } from './match/stage.js';
import { mapStage } from './map/stage.js';
import { prepareStage } from './prepare/stage.js';
import { exportStage } from './export/stage.js';
import { standinStage } from './export/standin.js';
import { trainStage } from './train/stage.js';
import { rgbIngestStage } from './ingest/rgb.stage.js';
import { rgbSelectStage } from './select/rgb.stage.js';
import { rgbPrepareStage } from './prepare/rgb.stage.js';
import './dummy/stages.js';

// selection waits for the user to confirm ingest's proposal (config.select)
const select = { ...selectStage, ready: (config) => !!config.select };

// a stand-in loads a trained splat (CONTRACTS.md 6) for the export test set
const trainStandin = standinStage('train', 'Training', ['prepare']);

registerStageSet(
  'thermal',
  [ingestStage, select, featuresStage, matchStage, mapStage, prepareStage, trainStage, exportStage],
  {
    title: 'Thermal splat',
  },
);

// RGB recordings (MPEG-TS/MP4 with KLV or APCG SEI): features, matching, mapping, training and
// export are the thermal stages on grey images and RGB targets
registerStageSet(
  'rgb',
  [
    rgbIngestStage,
    rgbSelectStage,
    featuresStage,
    matchStage,
    mapStage,
    rgbPrepareStage,
    trainStage,
    exportStage,
  ],
  { title: 'RGB splat' },
);

// export and result view on prepared inputs (frames.json, dataset.json, a trained splat): tests
registerStageSet(
  'export-test',
  [
    standinStage('select', 'Frames'),
    standinStage('map', 'COLMAP model'),
    standinStage('prepare', 'Training data'),
    trainStandin,
    exportStage,
  ],
  { title: 'Export from prepared files (test)', source: false },
);

export { stageSet, stageSets } from '../pipeline/registry.js';
