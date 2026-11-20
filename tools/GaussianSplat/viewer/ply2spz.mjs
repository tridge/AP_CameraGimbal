// Compress a 3DGS PLY to SPZ with Spark's encoder: node ply2spz.mjs IN.ply OUT.spz
// Spark sets the SPZ antialiased flag; it is cleared when the PLY's apcg_splat header says
// rasterize_mode classic (re-gzipped as WebSplat's export does, so both give the same bytes).
// The apcg_splat JSON is embedded in the gzip header (spz-meta.mjs).
import { readFileSync, writeFileSync } from 'node:fs';
import { transcodeSpz } from '@sparkjsdev/spark';
import { setAntialiased } from './spz-flags.mjs';
import { withSpzMeta } from './spz-meta.mjs';

const [, , src, dst] = process.argv;
if (!src || !dst) {
  console.error('usage: node ply2spz.mjs IN.ply OUT.spz');
  process.exit(1);
}
const ply = new Uint8Array(readFileSync(src));
let { fileBytes } = await transcodeSpz({
  inputs: [{ fileBytes: ply, pathOrUrl: src }],
  maxSh: 3,
});
const head = new TextDecoder('latin1').decode(ply.subarray(0, Math.min(ply.length, 1 << 20)));
const line = head
  .slice(0, Math.max(0, head.search(/\nend_header\r?\n/)))
  .split('\n')
  .find((l) => l.startsWith('comment apcg_splat '));
const text = line ? line.slice('comment apcg_splat '.length).replace(/\r$/, '') : null;
const info = text ? JSON.parse(text) : {};
if (text) fileBytes = withSpzMeta(fileBytes, text);
if (info.rasterize_mode === 'classic') fileBytes = setAntialiased(fileBytes, false);
writeFileSync(dst, fileBytes);
console.log('wrote %s (%d bytes)', dst, fileBytes.length);
