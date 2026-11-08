// Stage sets known to the app. Real stages are defined in app/src/stages/<stage>/stage.js
// (CONTRACTS.md "Stage module interface") and listed here in pipeline order.
import './dummy/stages.js';

export { stageSet, stageSets } from '../pipeline/registry.js';
