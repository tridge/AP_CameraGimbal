// G1 browser run: serve the harness page, the Brush worker, the wasm package and the dataset,
// drive Chrome (a local headless instance with the laptop flags, or a remote one over CDP),
// collect the held-out renders, the exported PLY, console counts and GPU memory samples.
//
// usage: node run_browser.mjs --dataset DIR --pkg DIR --out DIR [--iters N] [--port 8781]
//            [--cdp http://127.0.0.1:9333 --gpu-ssh win11] [--viewport 1280x720] [--shaders 1]
//            [--steps N per worker report] [--debug-port P (for profile_worker.mjs)]
//            [--trace 1 GPU buffer accounting] [--stress 1 explicit memory cleanup every report]
//            [--tasks N queue submit after at most N GPU tasks (stress)]
// Exits 0 only for a valid run: the worker disposed cleanly with no GPU error, shader error or
// device loss (its verdict, posted by the page to OUT/verdict.json) and all 13 renders and the PLY
// arrived. The CDP link to a remote page can drop while the page carries on; then the driver
// waits for the verdict file instead (connection_lost in run.json). Otherwise the renders and PLY are moved to OUT/rejected/,
// run.json has valid: false, and the exit status is 1.
// Local Chrome profiles go under $WEBSPLAT_DATA/chrome-profiles.
import { createServer } from 'node:http';
import { createReadStream, createWriteStream, mkdirSync, readdirSync, statSync, appendFileSync, writeFileSync, renameSync, existsSync, readFileSync } from 'node:fs';
import { execFile } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { dirname, join, normalize, extname } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const appSrc = join(here, '../../app/src');
const { chromium } = await import(join(here, '../../../../viewer/node_modules/playwright-core/index.mjs'));

const opt = { port: '8781', viewport: '1280x720' };
for (let i = 2; i < process.argv.length; i += 2) opt[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];
for (const k of ['dataset', 'pkg', 'out']) if (!opt[k]) throw new Error(`--${k} is required`);
// fail closed: every attempt gets a fresh output directory and an id that its uploads and its
// verdict must carry, so nothing from an earlier attempt can stand in for this one
if (existsSync(opt.out) && readdirSync(opt.out).length) {
  console.error(`${opt.out} is not empty: use a fresh output directory per attempt`);
  process.exit(2);
}
const attempt = randomUUID();
mkdirSync(join(opt.out, 'eval'), { recursive: true });
const meta = JSON.parse(readFileSync(join(opt.dataset, 'g1.json'), 'utf8'));
const evalNames = readFileSync(join(opt.dataset, 'eval_views.txt'), 'utf8').split('\n').map((l) => l.trim()).filter(Boolean);
const logFile = join(opt.out, 'log.jsonl');
const record = (o) => appendFileSync(logFile, JSON.stringify({ wall: Date.now(), ...o }) + '\n');

const types = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.wasm': 'application/wasm',
  '.json': 'application/json' };
function listFiles(dir, prefix = '') {
  return readdirSync(join(dir, prefix)).flatMap((f) => {
    const rel = prefix ? `${prefix}/${f}` : f;
    return statSync(join(dir, rel)).isDirectory() ? listFiles(dir, rel) : [rel];
  });
}
const datasetFiles = listFiles(opt.dataset).filter((f) => f !== 'gt.npy');
function serveFile(res, path) {
  try {
    statSync(path);
  } catch {
    res.writeHead(404).end();
    return;
  }
  res.writeHead(200, { 'Content-Type': types[extname(path)] || 'application/octet-stream' });
  createReadStream(path).pipe(res);
}
const server = createServer((req, res) => {
  const url = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  if (url.includes('..')) return res.writeHead(400).end();
  if (req.method === 'POST' && url.startsWith('/result/')) {
    // chunked uploads to /result/ATTEMPT/NAME?offset=N (0 starts the file); other attempts refused
    const prefix = `/result/${attempt}/`;
    if (!url.startsWith(prefix)) return res.writeHead(403).end();
    const path = normalize(join(opt.out, url.slice(prefix.length)));
    const offset = Number(new URL(req.url, 'http://x').searchParams.get('offset') || 0);
    mkdirSync(dirname(path), { recursive: true });
    const out = createWriteStream(path, offset ? { flags: 'r+', start: offset } : {});
    req.pipe(out).on('finish', () => res.writeHead(200).end());
    return;
  }
  if (url === '/') return serveFile(res, join(here, 'web/index.html'));
  if (url === '/manifest') return res.writeHead(200, { 'Content-Type': 'application/json' }).end(JSON.stringify(datasetFiles));
  if (url.startsWith('/app/')) return serveFile(res, join(appSrc, url.slice(5)));
  if (url.startsWith('/pkg/')) return serveFile(res, join(opt.pkg, url.slice(5)));
  if (url.startsWith('/dataset/')) return serveFile(res, join(opt.dataset, url.slice(9)));
  res.writeHead(404).end();
}).listen(+opt.port, '127.0.0.1');

// GPU memory: nvidia-smi locally or on the remote machine
const gpu = [];
const smi = ['nvidia-smi', '--query-gpu=memory.used,utilization.gpu', '--format=csv,noheader,nounits'];
const sample = () => execFile(opt['gpu-ssh'] ? 'ssh' : smi[0], opt['gpu-ssh'] ? ['-o', 'LogLevel=ERROR', opt['gpu-ssh'], smi.join(' ')] : smi.slice(1),
  (err, out) => {
    if (err) return;
    const [mem, util] = out.trim().split(',').map(Number);
    gpu.push({ t: Date.now(), mem, util });
    record({ gpu_mib: mem, util });
  });
sample();
const timer = setInterval(sample, 5000);

const [vw, vh] = opt.viewport.split('x').map(Number);
let browser;
let context;
if (opt.cdp) {
  browser = await chromium.connectOverCDP(opt.cdp, { timeout: 30000 });
  context = browser.contexts()[0];
} else {
  context = await chromium.launchPersistentContext(`${process.env.WEBSPLAT_DATA}/chrome-profiles/g1-train`, {
    executablePath: process.env.CHROME || '/opt/google/chrome/chrome', headless: true, viewport: { width: vw, height: vh },
    args: ['--headless=new', '--use-angle=gl', '--enable-features=Vulkan', '--enable-unsafe-webgpu',
      ...(opt['debug-port'] ? [`--remote-debugging-port=${opt['debug-port']}`] : [])],
  });
}
const page = await context.newPage();
await page.setViewportSize({ width: vw, height: vh });
const counts = {};
const verdict = { disposed: null, finished: false, fatal: null };
page.on('console', (m) => {
  const text = m.text();
  if (text.startsWith('G1 ')) {
    const o = JSON.parse(text.slice(3));
    record(o);
    if (o.type === 'disposed') verdict.disposed = o;
    if (o.stage === 'finished') verdict.finished = true;
    if (o.stage === 'fatal') verdict.fatal = o.message;
    if (o.type !== 'progress' || o.iter % 2000 < 200) console.log(text.slice(0, 300));
    return;
  }
  counts[m.type()] = (counts[m.type()] || 0) + 1;
  if (m.type() !== 'log' || counts[m.type()] <= 20) record({ console: m.type(), text: text.slice(0, 20000) });
});
page.on('pageerror', (e) => { counts.pageerror = (counts.pageerror || 0) + 1; record({ pageerror: e.message }); });
page.on('worker', (w) => w.on('console', (m) => {
  counts['worker.' + m.type()] = (counts['worker.' + m.type()] || 0) + 1;
  record({ worker_console: m.type(), text: m.text().slice(0, 20000) });
}));
const t0 = Date.now();
let base = gpu.length ? gpu[0].mem : null;
await page.goto(`http://localhost:${opt.port}/?iters=${opt.iters || ''}&shaders=${opt.shaders || ''}&steps=${opt.steps || ''}&trace=${opt.trace || ''}&stress=${opt.stress || ''}&tasks=${opt.tasks || ''}&attempt=${attempt}`);
let ending = 'finished';
page.on('crash', () => { ending = 'page crashed'; record({ ending }); });
page.on('close', () => record({ page: 'closed' }));
let connectionLost = false;
try {
  await page.waitForFunction(() => window.g1done, null, { timeout: 4 * 3600 * 1000, polling: 5000 });
} catch (e) {
  if (ending === 'finished') ending = 'driver: ' + e.message.split('\n')[0];
  connectionLost = ending !== 'page crashed';
}
const verdictFile = join(opt.out, 'verdict.json');
if (connectionLost) {
  record({ driver: 'connection to the page lost; waiting for the verdict file' });
  const until = Date.now() + 30 * 60 * 1000;
  while (!existsSync(verdictFile) && Date.now() < until) await new Promise((r) => setTimeout(r, 5000));
  await new Promise((r) => setTimeout(r, 2000)); // let the upload finish writing
}
let fileVerdict = null;
try {
  fileVerdict = existsSync(verdictFile) ? JSON.parse(readFileSync(verdictFile, 'utf8')) : null;
} catch { /* truncated: treated as absent */ }
if (fileVerdict && fileVerdict.attempt !== attempt) fileVerdict = null;
if (verdict.disposed && verdict.disposed.attempt !== attempt) verdict.disposed = null;
if (fileVerdict?.type === 'disposed') verdict.disposed = verdict.disposed || fileVerdict;
if (connectionLost && fileVerdict?.ok) verdict.finished = true;
const artifacts = checkArtifacts();
const complete = artifacts.ok;
if (!complete) record({ driver: 'artifacts rejected: ' + artifacts.problems.join('; ') });

// exactly the frozen eval views as NAME.f32 of W x H finite values in [0, 1], and a PLY whose
// binary payload matches its header, with the thermal apcg_splat metadata
function checkArtifacts() {
  const problems = [];
  const evalDir = join(opt.out, 'eval');
  const want = evalNames.map((n) => n + '.f32').sort();
  const have = existsSync(evalDir) ? readdirSync(evalDir).sort() : [];
  if (JSON.stringify(have) !== JSON.stringify(want)) problems.push(`renders ${have.join(',')} != ${want.join(',')}`);
  for (const f of want.filter((x) => have.includes(x))) {
    const buf = readFileSync(join(evalDir, f));
    if (buf.length !== meta.width * meta.height * 4) { problems.push(`${f}: ${buf.length} bytes`); continue; }
    const v = new Float32Array(buf.buffer, buf.byteOffset, buf.length / 4);
    if (!v.every((x) => Number.isFinite(x) && x >= 0 && x <= 1)) problems.push(`${f}: values not finite in [0, 1]`);
  }
  const plyPath = join(opt.out, 'export.ply');
  if (!existsSync(plyPath)) {
    problems.push('no export.ply');
  } else {
    const ply = readFileSync(plyPath);
    const end = ply.indexOf('end_header\n');
    const head = end > 0 ? ply.subarray(0, end).toString('latin1') : '';
    const lines = head.split('\n');
    const vertex = lines.find((l) => l.startsWith('element vertex '));
    const n = vertex ? Number(vertex.split(' ')[2]) : 0;
    const props = lines.filter((l) => l.startsWith('property float ')).length;
    const comment = lines.find((l) => l.startsWith('comment apcg_splat '));
    let apcg = null;
    try { apcg = comment && JSON.parse(comment.slice('comment apcg_splat '.length)); } catch { /* bad */ }
    if (!head.startsWith('ply\nformat binary_little_endian 1.0')) problems.push('PLY header');
    else if (!(n > 0) || !props) problems.push('PLY has no vertices');
    else if (ply.length !== end + 'end_header\n'.length + n * props * 4) problems.push(`PLY size ${ply.length} for ${n} x ${props} floats`);
    if (apcg?.kind !== 'thermal' || JSON.stringify(apcg.temperature_scale) !== JSON.stringify(meta.temperature_scale)) {
      problems.push('PLY apcg_splat metadata');
    }
  }
  return { ok: !problems.length, problems };
}
const wall = (Date.now() - t0) / 1000;
clearInterval(timer);
const peak = gpu.reduce((m, s) => Math.max(m, s.mem), 0);
if (base === null && gpu.length) base = gpu[0].mem; // the first remote sample arrives late
const valid = !!(verdict.finished && verdict.disposed?.ok && !verdict.fatal && complete
  && verdict.disposed.gpuErrors === 0 && verdict.disposed.shaderErrors === 0 && !counts.pageerror);
const reason = valid ? null : (verdict.fatal || verdict.disposed?.reason || (counts.pageerror ? 'page error' : null)
  || (!complete ? 'artifacts: ' + artifacts.problems.join('; ') : ending));
if (!valid) {
  // keep, but out of the way of anything scoring this directory
  mkdirSync(join(opt.out, 'rejected'), { recursive: true });
  for (const f of ['eval', 'export.ply']) {
    if (existsSync(join(opt.out, f))) renameSync(join(opt.out, f), join(opt.out, 'rejected', f));
  }
}
const summary = { attempt, valid, reason, ending, connection_lost: connectionLost, artifacts: artifacts.problems, wall_s: wall, console: counts, gpu_mem_before_mib: base, gpu_mem_peak_mib: peak,
  viewport: opt.viewport, remote: !!opt.cdp, worker: verdict.disposed };
writeFileSync(join(opt.out, 'gpu.json'), JSON.stringify(gpu));
writeFileSync(join(opt.out, 'run.json'), JSON.stringify(summary, null, 1));
console.log(JSON.stringify(summary));
await page.close().catch(() => {});
if (!opt.cdp) await context.close().catch(() => {});
else await browser.close().catch(() => {});
server.close();
process.exit(valid ? 0 : 1);
