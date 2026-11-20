// Checks priormap.js's telemetry model against sfm.py's (sparse/telemetry/images.txt):
// node check_telemetry.mjs FRAMES.json IMAGES.txt
import { readFileSync } from 'node:fs';
import { telemetryPose } from '../../app/src/stages/map/priormap.js';

const [framesPath, imagesTxt] = process.argv.slice(2);
const frames = JSON.parse(readFileSync(framesPath, 'utf8')).frames;
const lines = readFileSync(imagesTxt, 'utf8')
  .split('\n')
  .filter((l) => l.trim() && !l.startsWith('#'));
let bad = 0;
lines.forEach((l, k) => {
  const v = l.split(/\s+/);
  const ref = v.slice(1, 8).map(Number);
  const got = telemetryPose(frames[k]);
  if (v[9] !== frames[k].name || ref.some((x, i) => x !== got[i])) {
    bad++;
    if (bad < 5) console.log(frames[k].name, ref, got);
  }
});
console.log(`${lines.length - bad}/${lines.length} poses identical to sfm.py's telemetry model`);
process.exit(bad ? 1 : 0);
