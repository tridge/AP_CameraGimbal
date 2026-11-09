// Build the Brush input directory from a stage 5b prepare output on disk, with the stage's own
// module (app/src/stages/train/brush-dataset.js), and print the Brush options.
// usage: node make_brush_dataset.mjs PREPARE_DIR OUT_DIR [--growth G]
import fs from 'node:fs';
import zlib from 'node:zlib';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const { writeBrushDataset, brushConfig } = await import(join(here, '../../app/src/stages/train/brush-dataset.js'));
const [prep, out, ...rest] = process.argv.slice(2);
const growth = rest[0] === '--growth' ? Number(rest[1]) : undefined;
const dataset = JSON.parse(fs.readFileSync(join(prep, 'dataset.json'), 'utf8'));
const fd = fs.openSync(join(prep, dataset.targets.file), 'r');
const px = dataset.width * dataset.height;
const buf = (f) => { const b = fs.readFileSync(join(prep, f)); return b.buffer.slice(b.byteOffset, b.byteOffset + b.length); };
fs.rmSync(out, { recursive: true, force: true });
const t0 = Date.now();
await writeBrushDataset(dataset, {
  points: new Float32Array(buf(dataset.points.file)),
  ceiling: new Float32Array(buf(dataset.bounds.ceiling.file)),
  readTargets: async (i) => {
    const b = Buffer.alloc(px * 2);
    fs.readSync(fd, b, 0, px * 2, i * px * 2);
    return new Uint16Array(b.buffer, b.byteOffset, px);
  },
  write: async (path, data) => {
    fs.mkdirSync(dirname(join(out, path)), { recursive: true });
    fs.writeFileSync(join(out, path), data);
  },
  deflate: async (bytes) => new Uint8Array(zlib.deflateSync(bytes, { level: 6 })),
});
const cfg = brushConfig(dataset, growth === undefined ? {} : { growthGradThreshold: growth });
fs.writeFileSync(join(out, 'brush_config.json'), JSON.stringify(cfg, null, 1));
// the same options as brush-cli arguments (for native runs)
const args = Object.entries(cfg).filter(([k]) => !['eval-every', 'start-iter'].includes(k))
  .flatMap(([k, v]) => (typeof v === 'boolean' ? (v ? [`--${k}`] : []) : [`--${k}`, String(v)]));
fs.writeFileSync(join(out, 'brush_args.txt'), args.join(' ') + '\n');
console.log(`wrote ${out} in ${((Date.now() - t0) / 1000).toFixed(1)} s`);
