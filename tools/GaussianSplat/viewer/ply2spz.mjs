// Compress a 3DGS PLY to SPZ with Spark's encoder: node ply2spz.mjs IN.ply OUT.spz
import { readFileSync, writeFileSync } from 'node:fs';
import { transcodeSpz } from '@sparkjsdev/spark';

const [, , src, dst] = process.argv;
if (!src || !dst) {
  console.error('usage: node ply2spz.mjs IN.ply OUT.spz');
  process.exit(1);
}
const { fileBytes } = await transcodeSpz({
  inputs: [{ fileBytes: new Uint8Array(readFileSync(src)), pathOrUrl: src }],
  maxSh: 3,
});
writeFileSync(dst, fileBytes);
console.log('wrote %s (%d bytes)', dst, fileBytes.length);
