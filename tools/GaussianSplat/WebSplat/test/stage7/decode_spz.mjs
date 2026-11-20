// An SPZ decoded to a PLY (train.py layout) for rendering, and optionally the source PLY with
// only SPZ's range limits applied (values saturated, no quantisation), to tell the format's
// clamping from its quantisation:
//   node decode_spz.mjs IN.spz OUT.ply [SOURCE.ply CLAMPED.ply]
import { readFileSync, writeFileSync } from 'node:fs';
import { decodeSpz } from '../../app/src/stages/export/spz.js';
import { readPly, splatFromPly, writePly } from '../../app/src/stages/export/ply.js';

const [src, dst, plyIn, clampedOut] = process.argv.slice(2);
const d = await decodeSpz(new Uint8Array(readFileSync(src)));
writeFileSync(dst, writePly(d.splat));
console.log(`decoded ${src}: ${d.count} Gaussians, SH ${d.shDegree} -> ${dst}`);
if (clampedOut) {
  const s = splatFromPly(readPly(new Uint8Array(readFileSync(plyIn))));
  const sat = (a, lo, hi) => {
    let n = 0;
    for (let i = 0; i < a.length; i++) {
      if (a[i] < lo || a[i] > hi) {
        a[i] = Math.min(hi, Math.max(lo, a[i]));
        n++;
      }
    }
    return n;
  };
  const lim = ((1 << 23) - 1) / (1 << d.fractionalBits);
  const counts = {
    means: sat(s.means, -lim, lim),
    scales: sat(s.scales, -10, 255 / 16 - 10),
    sh0: sat(s.sh0, -0.5 / 0.15, 0.5 / 0.15),
    shN: sat(s.shN, -1, 127 / 128),
  };
  writeFileSync(clampedOut, writePly(s, { normalised: true }));
  console.log(`saturated to SPZ's ranges: ${JSON.stringify(counts)} -> ${clampedOut}`);
}
