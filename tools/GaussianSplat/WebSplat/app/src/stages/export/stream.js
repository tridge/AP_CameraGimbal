// Stage 7 streaming export: the downloads of export.js (splat.ply, splat.spz, meta.json, the same
// bytes) without holding the splat. The trained splat is read and the PLY written in chunks
// (greyscale and quaternions per chunk); the SPZ is made by Spark a chunk at a time, assembled in
// a scratch file and gzipped as a stream, and each chunk is decoded and checked against its PLY
// rows. No DOM: runs in a worker (OPFS sync access handles) or in Node (files).
//
// Files are {size, read(offset, length) -> Uint8Array} (input) and sinks {write(bytes, offset),
// read(offset, length), truncate(size), flush()} (synchronous, as FileSystemSyncAccessHandle).
import { exportInputs } from './export.js';
import { apcgComment, hottestView, metaJson, rgbWebInfo, startCamera, webInfo } from './meta.js';
import {
  emptySplat,
  greyscale,
  parsePlyHeader,
  plyBody,
  plyHeader,
  plyPropertyNames,
  readPlyRows,
  shCoeffs,
  splatFromPly,
  checkComments,
} from './ply.js';
import {
  decompressSpz,
  decodeSpzRange,
  mergeSpzErrors,
  gzipFixedStream,
  SPZ_MAX_SH,
  spzErrors,
  spzHeader,
} from './spz.js';

export const CHUNK = 65536;

// a reader of chunks of the trained splat: PLY (any property order, e.g. Brush) or WSSP params.bin
export function splatSource(file) {
  const head = file.read(0, Math.min(file.size, 1 << 20));
  if (head[0] === 0x57 && head[1] === 0x53 && head[2] === 0x53 && head[3] === 0x50) {
    const u = new Uint32Array(head.slice(0, 16).buffer);
    if (u[1] !== 1) throw new Error(`params.bin version ${u[1]}`);
    const count = u[2];
    const shDegree = u[3];
    const k = shCoeffs(shDegree);
    const widths = [
      ['means', 3],
      ['scales', 3],
      ['quats', 4],
      ['opacities', 1],
      ['sh0', 3],
      ['shN', 3 * k],
    ];
    const offsets = {};
    let at = 16;
    for (const [name, w] of widths) {
      offsets[name] = at;
      at += 4 * w * count;
    }
    if (at !== file.size) throw new Error(`params.bin: ${file.size} bytes, expected ${at}`);
    return {
      kind: 'wssp',
      count,
      shDegree,
      read(a, b) {
        const s = emptySplat(b - a, shDegree);
        for (const [name, w] of widths) {
          s[name].set(
            new Float32Array(file.read(offsets[name] + 4 * w * a, 4 * w * (b - a)).buffer),
          );
        }
        return s;
      },
    };
  }
  const h = parsePlyHeader(head);
  if (h.start + h.stride * h.count > file.size) throw new Error('PLY is truncated');
  let rest = 0;
  while (h.names.includes(`f_rest_${rest}`)) rest++;
  const shDegree = Math.round(Math.sqrt(rest / 3 + 1)) - 1;
  return {
    kind: 'ply',
    count: h.count,
    shDegree,
    comments: h.comments,
    read: (a, b) =>
      splatFromPly(readPlyRows(h, file.read(h.start + a * h.stride, (b - a) * h.stride), b - a)),
  };
}

const enc = (s) => new TextEncoder().encode(s);
// a macrotask turn, so a worker sees messages (Stop) between chunks
const turn = () => new Promise((r) => setTimeout(r, 0));

// Pass 1: the PLY. opts: input (file), dataset (5b dataset.json), frames (stage 2 frames.json),
// title, description, rasterizeMode, targetPolicy (exportInputs), camera (default: the start
// camera), training (an early stop's {iterations, iterations_planned}, recorded in the info as
// stopped_early), ply (sink), chunk,
// progress(done, count), throwIfStopped(). Returns {info, count, plySize, input}.
export async function plyPass(opts) {
  const chunk = opts.chunk ?? CHUNK;
  const progress = opts.progress ?? (() => {});
  const stop = opts.throwIfStopped ?? (() => {});
  const src = splatSource(opts.input);
  const n = src.count;
  if (!n) throw new Error('the trained splat has no Gaussians');
  const inputs = exportInputs(opts.dataset, opts.frames, opts.targetPolicy);
  // RGB (dataset kind rgb): colours as trained, export_web.py's rgb metadata
  const rgb = opts.dataset.kind === 'rgb';
  const common = {
    count: n,
    shDegree: src.shDegree,
    title: opts.title ?? (rgb ? 'RGB splat' : 'Thermal splat'),
    description: opts.description ?? '',
    camera:
      opts.camera ??
      startCamera(inputs.views, inputs.target, hottestView(inputs.views, inputs.frameMax)),
    target: inputs.target,
    origin: inputs.origin,
    rasterizeMode: opts.rasterizeMode,
  };
  const info = rgb
    ? rgbWebInfo(common)
    : webInfo({ ...common, scale: inputs.scale, hotspot: inputs.hotspot });
  if (opts.training) info.stopped_early = opts.training;
  const comments = [apcgComment(info)];
  checkComments(comments);
  const names = plyPropertyNames(src.shDegree);
  const head = plyHeader(n, names, comments);
  const rowBytes = 4 * names.length;
  opts.ply.truncate(0);
  opts.ply.write(head, 0);
  for (let a = 0; a < n; a += chunk) {
    stop();
    const b = Math.min(n, a + chunk);
    const part = src.read(a, b);
    opts.ply.write(plyBody(rgb ? part : greyscale(part)), head.length + a * rowBytes);
    progress(b, n);
    await turn(); // lets a Stop message in
  }
  opts.ply.flush();
  return { info, count: n, plySize: head.length + n * rowBytes, input: src.kind };
}

// Pass 2: the SPZ, Spark-exact in chunks. Spark quantises each Gaussian on its own, so it encodes
// SPZ_CHUNK Gaussians of the written PLY at a time; each chunk's planar sections go to their
// places in the uncompressed file (raw sink), each chunk is decoded and checked against its PLY
// rows (spzErrors), and the raw file is then gzipped as a stream (fixed gzip header with the PLY's
// apcg_splat JSON, as ply2spz.mjs) and read back to verify. Memory: one chunk, not the splat.
// Returns {spzSize, spz (merged spzErrors with version, fractionalBits, antialiased)}.
export const SPZ_CHUNK = 131072;

// directions (Fibonacci sphere) at which emitted temperatures are compared
const DIRS = Array.from({ length: 32 }, (_, i) => {
  const z = 1 - (2 * (i + 0.5)) / 32;
  const r = Math.sqrt(1 - z * z);
  const t = Math.PI * (1 + Math.sqrt(5)) * i;
  return [r * Math.cos(t), r * Math.sin(t), z];
});
const C1 = 0.4886025119029199;
const C2 = [
  1.0925484305920792, -1.0925484305920792, 0.31539156525252005, -1.0925484305920792,
  0.5462742152960396,
];
const C3 = [
  -0.5900435899266435, 2.890611442640554, -0.4570457994644658, 0.3731763325901154,
  -0.4570457994644658, 1.445305721320277, -0.5900435899266435,
];
const BASIS = DIRS.map(([x, y, z]) => {
  const [xx, yy, zz] = [x * x, y * y, z * z];
  return [
    -C1 * y,
    C1 * z,
    -C1 * x,
    C2[0] * x * y,
    C2[1] * y * z,
    C2[2] * (2 * zz - xx - yy),
    C2[3] * x * z,
    C2[4] * (xx - yy),
    C3[0] * y * (3 * xx - yy),
    C3[1] * x * y * z,
    C3[2] * y * (4 * zz - xx - yy),
    C3[3] * z * (2 * zz - 3 * xx - 3 * yy),
    C3[4] * x * (4 * zz - xx - yy),
    C3[5] * z * (xx - yy),
    C3[6] * x * (xx - 3 * yy),
  ];
});

// what the SPZ does to the emitted temperature, a sampled estimate (not a bound): for every
// Gaussian, the largest drop and rise over the 32 DIRS of the decoded value against the PLY's
// (greyscale: channel 0), in C; directions between samples can lose more
function temperatureLoss(ref, dec, scale, acc) {
  const norm = Math.log1p((scale.t1_c - scale.t0_c) / scale.k_c);
  const toC = (v) => scale.t0_c + scale.k_c * Math.expm1(Math.min(1, Math.max(0, v)) * norm);
  const k = shCoeffs(ref.shDegree);
  const kd = shCoeffs(dec.shDegree);
  const m = Math.min(k, kd, 15);
  for (let i = 0; i < ref.count; i++) {
    let drop = 0;
    let rise = 0;
    for (const b of BASIS) {
      let a = 0.28209479177387814 * ref.sh0[3 * i] + 0.5;
      let d = 0.28209479177387814 * dec.sh0[3 * i] + 0.5;
      for (let j = 0; j < m; j++) {
        a += b[j] * ref.shN[(i * k + j) * 3];
        d += b[j] * dec.shN[(i * kd + j) * 3];
      }
      const diff = toC(d) - toC(a);
      if (-diff > drop) drop = -diff;
      if (diff > rise) rise = diff;
    }
    acc.maxDropC = Math.max(acc.maxDropC, drop);
    acc.maxRiseC = Math.max(acc.maxRiseC, rise);
    if (drop > 2) acc.over2C++;
  }
  return acc;
}

export async function spzPass(opts) {
  const { ply, plySize, spz, raw, rasterizeMode, transcodeSpz } = opts;
  const stop = opts.throwIfStopped ?? (() => {});
  const chunk = opts.chunk ?? SPZ_CHUNK;
  const ph = parsePlyHeader(ply.read(0, Math.min(plySize, 1 << 20)));
  const n = ph.count;
  const k = shCoeffs(
    Math.min(
      SPZ_MAX_SH,
      Math.round(Math.sqrt(ph.names.filter((x) => x.startsWith('f_rest_')).length / 3 + 1)) - 1,
    ),
  );
  const rot = 4; // SPZ v3
  const sections = [9, 1, 3, 3, rot, 3 * k]; // bytes per Gaussian: positions, alpha, colour, scales, rotation, SH
  const size = 16 + n * sections.reduce((x, y) => x + y, 0);
  raw.truncate(0);
  const headNames = ph.props.map((p) => `property ${p.type} ${p.name}`).join('\n');
  let acc = null;
  let header = null;
  const loss = { maxDropC: 0, maxRiseC: 0, over2C: 0 };
  for (let a = 0; a < n; a += chunk) {
    stop();
    const b = Math.min(n, a + chunk);
    const m = b - a;
    const rows = ply.read(ph.start + a * ph.stride, m * ph.stride);
    const h = new TextEncoder().encode(
      `ply\nformat binary_little_endian 1.0\nelement vertex ${m}\n${headNames}\nend_header\n`,
    );
    const part = new Uint8Array(h.length + rows.length);
    part.set(h);
    part.set(rows, h.length);
    const { fileBytes } = await transcodeSpz({
      inputs: [{ fileBytes: part, pathOrUrl: 'chunk.ply', fileType: 'ply' }],
      maxSh: SPZ_MAX_SH,
    });
    const u = await decompressSpz(fileBytes);
    const hh = spzHeader(u);
    if (hh.count !== m || hh.k !== k || hh.version !== 3)
      throw new Error(`Spark's SPZ of ${m} Gaussians: ${JSON.stringify(hh)}`);
    if (!header) header = u.slice(0, 16);
    // scatter the chunk's sections
    let src = 16;
    let dst = 16;
    for (const w of sections) {
      raw.write(u.subarray(src, src + m * w), dst + a * w);
      src += m * w;
      dst += n * w;
    }
    const ref = splatFromPly(readPlyRows(ph, rows, m));
    const dec = decodeSpzRange(u, hh, 0, m);
    acc = mergeSpzErrors(acc, spzErrors(ref, dec, hh.fractionalBits));
    if (opts.scale) temperatureLoss(ref, dec, opts.scale, loss);
    opts.progress?.(b, n);
    await turn();
  }
  // the whole file's header: Spark's, with the total count and the antialiased flag of the mode
  const dv = new DataView(header.buffer);
  dv.setUint32(8, n, true);
  header[14] = rasterizeMode === 'classic' ? header[14] & ~1 : header[14] | 1;
  raw.write(header, 0);
  raw.flush();
  const pieces = function* () {
    for (let at = 0; at < size; at += 1 << 20) yield raw.read(at, Math.min(1 << 20, size - at));
  };
  stop();
  spz.truncate(0);
  let spzSize = 0;
  const metaText = ph.comments.find((c) => c.startsWith('apcg_splat '));
  for await (const p of gzipFixedStream(pieces(), metaText?.slice('apcg_splat '.length) ?? null)) {
    stop();
    spz.write(p, spzSize);
    spzSize += p.length;
  }
  spz.flush();
  // read back from bounded reads: the gzip decompresses to the raw file
  let rd = 0;
  const r = new ReadableStream({
    pull(c) {
      if (rd >= spzSize) return c.close();
      const n = Math.min(1 << 20, spzSize - rd);
      c.enqueue(spz.read(rd, n));
      rd += n;
    },
  })
    .pipeThrough(new DecompressionStream('gzip'))
    .getReader();
  let at = 0;
  for (;;) {
    const { done, value } = await r.read();
    if (done) break;
    const want = raw.read(at, value.length);
    for (let i = 0; i < value.length; i++)
      if (value[i] !== want[i]) throw new Error('SPZ gzip does not read back');
    at += value.length;
  }
  if (at !== size) throw new Error('SPZ gzip read back short');
  return {
    spzSize,
    spz: {
      version: 3,
      fractionalBits: header[13],
      antialiased: !!(header[14] & 1),
      ...acc,
      temperature: opts.scale ? loss : null,
    },
  };
}

// meta.json (export_web.py site metadata with the download sizes); returns its size
export function metaPass({ info, downloads, meta }) {
  const bytes = enc(metaJson({ ...info, downloads }));
  meta.truncate(0);
  meta.write(bytes, 0);
  meta.flush();
  return bytes.length;
}

// both passes in one thread (Node, tests); the stage runs them in separate workers. opts as
// plyPass plus sinks {ply, spz, raw (scratch), meta}, transcodeSpz, spzChunk, phase(name),
// progress(done, total)
export async function exportStream(opts) {
  const { sinks, transcodeSpz } = opts;
  const phase = opts.phase ?? (() => {});
  const times = {};
  let t = performance.now();
  const lap = (name) => {
    const now = performance.now();
    times[name] = now - t;
    t = now;
  };
  const n = splatSource(opts.input).count;
  const total = transcodeSpz ? 2 * n : n;
  phase('ply');
  const p = await plyPass({ ...opts, ply: sinks.ply, progress: (d) => opts.progress?.(d, total) });
  lap('ply');
  const downloads = { ply: p.plySize };
  let spzReport = null;
  if (transcodeSpz) {
    phase('spz');
    const r = await spzPass({
      ply: sinks.ply,
      plySize: p.plySize,
      spz: sinks.spz,
      raw: sinks.raw,
      rasterizeMode: p.info.rasterize_mode,
      scale: p.info.temperature_scale,
      transcodeSpz,
      chunk: opts.spzChunk,
      progress: (d) => opts.progress?.(n + d, total),
      throwIfStopped: opts.throwIfStopped,
    });
    downloads.spz = r.spzSize;
    spzReport = r.spz;
    lap('spz');
  }
  const metaSize = metaPass({ info: p.info, downloads, meta: sinks.meta });
  return {
    info: p.info,
    downloads: { ...downloads, meta: metaSize },
    spzReport,
    timesMs: times,
    input: p.input,
  };
}
