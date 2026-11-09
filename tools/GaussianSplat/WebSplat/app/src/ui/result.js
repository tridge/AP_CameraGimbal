// The result view (stage 7): the exported splat in the embedded GaussianSplat viewer with
// temperatures, the downloads, and a summary (Gaussians, temperature range, timings).
import { embedViewer } from '../stages/export/embed.js';
import { assetURL } from '../asset-url.js';
import { formatDuration } from '../pipeline/eta.js';
import { readFile } from '../storage/opfs.js';
import { tarBlob } from './tar.js';

const VIEWER = assetURL('vendor/splat-viewer/index.html');
const LARGE_PLY = 250e6;
const mb = (b) => `${(b / 1e6).toFixed(b < 1e7 ? 1 : 0)} MB`;
const el = (tag, props = {}, ...kids) => {
  const e = Object.assign(document.createElement(tag), props);
  e.append(...kids);
  return e;
};

// values outside SPZ's ranges (SH beyond +-1, colours beyond its byte) that the format clamped
const clampedValues = (spz) => (spz.clamped.sh0 ?? 0) + (spz.clamped.sh ?? 0);

// what the SPZ loses: quantisation always; clamping can take much more off small hot sources.
// spz.temperature: the largest drop of a Gaussian's emitted temperature (measured by the export)
function spzNote(spz, ratio) {
  const t = spz.temperature;
  return (
    `The SPZ is ${(ratio * 100).toFixed(0)}% of the PLY's size but lossy: sampled at 32 view ` +
    `directions, its Gaussians emit as much as ${t ? t.maxDropC.toFixed(1) : '?'} °C less than ` +
    `in the PLY (an estimate, not a bound)` +
    (clampedValues(spz)
      ? ` (${clampedValues(spz)} colour values outside its range were clamped; rendered hot ` +
        `sources can lose more)`
      : '') +
    `. The PLY holds the exact values and the temperature scale; the SPZ needs meta.json for ` +
    `temperatures.`
  );
}

let viewer = null;
let urls = [];

// the map stage's final COLMAP model (cameras.bin, images.bin, points3D.bin) as a tar Blob, or
// null when the job has no map stage
async function colmapArchive(mapRecord) {
  const outs = Object.entries(mapRecord?.outputs ?? {}).filter(([n]) => n.startsWith('final/'));
  if (mapRecord?.state !== 'done' || !outs.length) return null;
  const entries = [];
  for (const [n, o] of outs.sort())
    entries.push({ name: `sparse/0/${n.slice(6)}`, file: await readFile(o.path) });
  return tarBlob(entries);
}

// section: the #result element; record: the export stage's manifest record; mapRecord: the map
// stage's (COLMAP model); snapshot: the controller's (stage timings).
// Returns {files, meta, opened: Promise (viewer showing it)}.
export async function showResult(section, record, snapshot, mapRecord = null) {
  const r = record.result;
  const files = {};
  for (const name of ['splat.ply', 'splat.spz', 'meta.json']) {
    const o = record.outputs?.[name];
    if (!o) throw new Error(`export output ${name} is missing`);
    files[name] = await readFile(o.path);
  }
  const meta = JSON.parse(await files['meta.json'].text());
  for (const u of urls) URL.revokeObjectURL(u);
  urls = [];
  const base = (r.title || 'splat').replace(/[^\w.-]+/g, '_');
  const spz = r.spz;
  const lossy = spz.temperature ? spz.temperature.maxDropC.toFixed(1) : '?';
  const link = (blob, download, id, text) => {
    const href = URL.createObjectURL(blob);
    urls.push(href);
    return el('a', { href, download, className: 'button', id }, text);
  };
  const links = [
    link(
      files['splat.ply'],
      `${base}.ply`,
      'download-ply',
      `PLY, exact (${mb(files['splat.ply'].size)})`,
    ),
    link(
      files['meta.json'],
      `${base}.meta.json`,
      'download-json',
      `meta.json (${mb(files['meta.json'].size)})`,
    ),
    link(
      files['splat.spz'],
      `${base}.spz`,
      'download-spz',
      `SPZ, compressed, lossy: about ${lossy} °C lower at worst, sampled (${mb(files['splat.spz'].size)})`,
    ),
  ];
  links[2].classList.add('secondary');
  const colmap = await colmapArchive(mapRecord);
  if (colmap)
    links.push(
      link(colmap, `${base}.colmap.tar`, 'download-colmap', `COLMAP model (${mb(colmap.size)})`),
    );
  section.querySelector('#result-downloads').replaceChildren(...links);

  const stageRows = snapshot.stages.map((s) =>
    el(
      'tr',
      {},
      el('td', { textContent: s.title }),
      el('td', { textContent: formatDuration(s.elapsedMs) }),
    ),
  );
  const total = snapshot.stages.reduce((a, s) => a + s.elapsedMs, 0);
  section
    .querySelector('#result-summary')
    .replaceChildren(
      el(
        'p',
        {},
        `${r.gaussians.toLocaleString('en')} Gaussians, SH degree ${r.shDegree}; temperature scale ` +
          `${r.temperatureC[0].toFixed(1)} to ${r.temperatureC[1].toFixed(1)} °C (display ` +
          `${r.displayRangeC[0]} to ${r.displayRangeC[1]} °C); rendered as trained (${r.rasterizeMode}).`,
      ),
      el(
        'p',
        { className: 'note' },
        spzNote(spz, files['splat.spz'].size / files['splat.ply'].size),
      ),
      el(
        'table',
        { className: 'timings' },
        el(
          'tbody',
          {},
          ...stageRows,
          el(
            'tr',
            {},
            el('th', { textContent: 'Total' }),
            el('th', { textContent: formatDuration(total) }),
          ),
        ),
      ),
    );
  section.hidden = false;

  const holder = section.querySelector('#result-viewer');
  if (!viewer) viewer = embedViewer(holder, VIEWER);
  const select = section.querySelector('#result-view');
  select.querySelector('option[value=spz]').textContent =
    `SPZ (compressed, lossy: about ${lossy} °C lower at worst, sampled)`;
  // the PLY, exact; only a large PLY without clamped values opens as the SPZ first (it is slow
  // to open and needs a lot of memory in the viewer)
  select.value = files['splat.ply'].size > LARGE_PLY && !clampedValues(spz) ? 'spz' : 'ply';
  const open = () =>
    select.value === 'spz'
      ? viewer.open(files['splat.spz'], 'splat.spz', meta)
      : viewer.open(files['splat.ply'], 'splat.ply');
  select.onchange = () => open().catch((e) => console.error(e));
  return { files, meta, opened: open() };
}
