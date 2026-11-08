/** Stages 1-2 browser harness: a static server for the repo's tools/ tree, the viewer's pinned
 * FFmpeg core and $WEBSPLAT_DATA, and a separate headless Chrome driven by Playwright.
 */
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';

const here = path.dirname(fileURLToPath(import.meta.url));
export const TOOLS = path.resolve(here, '../../../..');
export const WEBSPLAT = path.resolve(here, '../..');
const VIEWER = path.join(TOOLS, 'viewer');

export function dataDir() {
  if (process.env.WEBSPLAT_DATA) return process.env.WEBSPLAT_DATA;
  const env = path.join(WEBSPLAT, 'local.env');
  if (fs.existsSync(env)) {
    const m = /^WEBSPLAT_DATA=(.*)$/m.exec(fs.readFileSync(env, 'utf8'));
    if (m) return m[1].trim();
  }
  throw new Error('set WEBSPLAT_DATA (or local.env)');
}

const TYPES = {
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.html': 'text/html',
  '.mkv': 'video/x-matroska',
};

export function serve({ port = 0, data = dataDir() } = {}) {
  const ffmpegVersion = JSON.parse(fs.readFileSync(path.join(VIEWER, 'package.json'), 'utf8'))
    .dependencies['@ffmpeg/core'];
  const routes = [
    ['/tools/', TOOLS],
    [`/vendor/ffmpeg/${ffmpegVersion}/`, path.join(VIEWER, 'node_modules/@ffmpeg/core/dist/umd')],
    ['/websplat-data/', data],
  ];
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    let file = null;
    if (url.pathname === '/') {
      res.writeHead(200, { 'content-type': 'text/html' });
      res.end('<!doctype html><title>stage12</title><body></body>');
      return;
    }
    for (const [prefix, root] of routes) {
      if (!url.pathname.startsWith(prefix)) continue;
      const p = path.resolve(root, decodeURIComponent(url.pathname.slice(prefix.length)));
      if (p.startsWith(root + path.sep) || p === root) file = p;
    }
    if (!file || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
      res.writeHead(404);
      res.end();
      return;
    }
    res.writeHead(200, {
      'content-type': TYPES[path.extname(file)] ?? 'application/octet-stream',
      'content-length': fs.statSync(file).size,
      'cache-control': 'no-store',
    });
    fs.createReadStream(file).pipe(res);
  });
  return new Promise((resolve) =>
    server.listen(port, '127.0.0.1', () =>
      resolve({ server, url: `http://127.0.0.1:${server.address().port}/` }),
    ),
  );
}

export async function launchChrome(profile = 'stage12') {
  const require = createRequire(path.join(VIEWER, 'package.json'));
  const { chromium } = require('playwright-core');
  const dir = path.join(dataDir(), 'chrome-profiles', profile);
  fs.mkdirSync(dir, { recursive: true });
  const context = await chromium.launchPersistentContext(dir, {
    executablePath: process.env.CHROME ?? '/opt/google/chrome/chrome',
    headless: true,
    args: ['--headless=new', '--use-angle=gl', '--enable-features=Vulkan'],
  });
  const page = context.pages()[0] ?? (await context.newPage());
  page.on('console', (m) => process.env.STAGE12_VERBOSE && console.log('[page]', m.text()));
  page.on('pageerror', (e) => console.log('[pageerror]', e.message));
  return { context, page };
}

/** Start server and Chrome; returns {page, url, close}. */
export async function openHarness(profile) {
  const { server, url } = await serve();
  const { context, page } = await launchChrome(profile);
  await page.goto(url);
  return {
    page,
    url,
    async close() {
      await context.close();
      server.close();
    },
  };
}
