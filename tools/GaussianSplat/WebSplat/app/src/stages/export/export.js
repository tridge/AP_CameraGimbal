// Stage 7 export: the downloads export_web.py makes from a thermal splat, from the browser's
// training output. Runs in a worker or in Node (no DOM).
//
//   splat.ply   export_web.py --ply: greyscale SH, apcg_splat header with the temperature scale,
//               start camera and display range (opens with temperatures in the viewer)
//   splat.spz   Spark's SPZ of splat.ply (ply2spz.mjs)
//   meta.json   export_web.py site metadata (the PLY header info plus download sizes)
import { greyscale, readParams, readPly, splatFromPly, writePly } from './ply.js';
import { apcgComment, metaJson, startCamera, trainInfo, webInfo } from './meta.js';
import { decodeSpz, encodeSpz, spzErrors } from './spz.js';

// inputs from the contract files: dataset.json (5b) and frames.json (2)
export function exportInputs(dataset, frames) {
  const images = dataset.images;
  const target = dataset.hotspot?.enu ?? frames.target_enu;
  return {
    target,
    origin: frames.origin,
    scale: dataset.scale,
    hotspot: dataset.hotspot ?? null,
    views: {
      names: images.map((im) => im.name),
      hot: images.filter((im) => im.hot).map((im) => im.name),
      viewmats: images.map((im) => [0, 1, 2, 3].map((r) => im.viewmat.slice(4 * r, 4 * r + 4))),
      K: dataset.K,
      height: dataset.height,
    },
  };
}

// training output: WSSP params.bin or a 3DGS PLY (Brush export)
export function loadSplat(bytes) {
  const b = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  if (b[0] === 0x57 && b[1] === 0x53 && b[2] === 0x53 && b[3] === 0x50) return readParams(b);
  return splatFromPly(readPly(b));
}

const enc = (s) => new TextEncoder().encode(s);

// opts: target, origin, scale, hotspot, views (start camera) or camera, title, description,
// rasterizeMode (default 'classic'; also clears the SPZ antialiased flag),
// transcodeSpz (Spark; without it no SPZ), checkSpz (decode and report the SPZ error)
export async function exportSplat(splat, opts) {
  const camera = opts.camera ?? startCamera(opts.views, opts.target);
  const info = webInfo({
    count: splat.count,
    shDegree: splat.shDegree,
    title: opts.title ?? 'Thermal splat',
    description: opts.description ?? '',
    camera,
    target: opts.target,
    origin: opts.origin,
    scale: opts.scale,
    hotspot: opts.hotspot ?? null,
    rasterizeMode: opts.rasterizeMode,
  });
  const files = {};
  files['splat.ply'] = writePly(greyscale(splat), { comments: [apcgComment(info)] });
  let spzReport = null;
  if (opts.transcodeSpz) {
    files['splat.spz'] = await encodeSpz(files['splat.ply'], opts.transcodeSpz, {
      antialiased: info.rasterize_mode !== 'classic',
    });
    if (opts.checkSpz) {
      const dec = await decodeSpz(files['splat.spz']);
      spzReport = {
        version: dec.version,
        fractionalBits: dec.fractionalBits,
        antialiased: dec.antialiased,
        ...spzErrors(splatFromPly(readPly(files['splat.ply'])), dec.splat, dec.fractionalBits),
      };
    }
  }
  const downloads = {};
  for (const k of ['ply', 'spz'])
    if (files[`splat.${k}`]) downloads[k] = files[`splat.${k}`].length;
  files['meta.json'] = enc(metaJson({ ...info, downloads }));
  return { files, info, spzReport };
}

// train.py's own splat.ply (colour channels kept, the shorter header)
export function trainPly(splat, { target, scale, origin, rasterizeMode }) {
  return writePly(splat, {
    comments: [apcgComment(trainInfo({ target, scale, origin, rasterizeMode }))],
  });
}
