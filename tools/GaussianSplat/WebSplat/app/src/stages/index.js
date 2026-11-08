// Stage sets known to the app. Real stages are defined in app/src/stages/<stage>/stage.js
// (CONTRACTS.md "Stage module interface") and listed here in pipeline order.
import { registerStageSet } from '../pipeline/registry.js';
import { ingestStage } from './ingest/stage.js';
import { selectStage } from './select/stage.js';
import './dummy/stages.js';

// selection waits for the user to confirm ingest's proposal (config.select)
const select = { ...selectStage, ready: (config) => !!config.select };

registerStageSet('thermal', [ingestStage, select], { title: 'Thermal splat (stages so far)' });

export { stageSet, stageSets } from '../pipeline/registry.js';
