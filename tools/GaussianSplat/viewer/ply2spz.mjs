// Compress a 3DGS PLY to SPZ with Spark's encoder: node ply2spz.mjs IN.ply OUT.spz
// Spark sets the SPZ antialiased flag; it is cleared when the PLY's apcg_splat header says
// rasterize_mode classic (re-gzipped as WebSplat's export does, so both give the same bytes).
import { readFileSync, writeFileSync } from 'node:fs';
import { crc32, deflateRawSync, gunzipSync } from 'node:zlib';
import { transcodeSpz } from '@sparkjsdev/spark';

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
const info = line ? JSON.parse(line.slice('comment apcg_splat '.length)) : {};
if (info.rasterize_mode === 'classic') {
  const u = gunzipSync(fileBytes);
  u[14] &= ~1;
  const body = deflateRawSync(u);
  const out = Buffer.alloc(10 + body.length + 8);
  out.set([0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255]);
  body.copy(out, 10);
  out.writeUInt32LE(crc32(u), 10 + body.length);
  out.writeUInt32LE(u.length >>> 0, 14 + body.length);
  fileBytes = out;
}
writeFileSync(dst, fileBytes);
console.log('wrote %s (%d bytes)', dst, fileBytes.length);
