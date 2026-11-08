// G2 browser harness: serves the app, this directory and the G2/stage 4/reference data, runs
// g2_test.html in headless Chrome and prints the result JSON. Outputs (geometry.bin, model/,
// result.json) go to $WEBSPLAT_DATA/g2/browser/<tag>/.
//   WEBSPLAT_DATA=... node run_browser.mjs [--cdp PORT] [--port HTTPPORT] [--tag T] [query]
// --cdp: use an already running Chrome (e.g. the win11 machine through ssh -L, serving to it
// through ssh -R --port), closing only our tab.
import { createServer } from 'node:http';
import { createReadStream, existsSync, mkdirSync, statSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, extname, join, normalize } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const { chromium } = await import(
  join(here, '../../../../viewer/node_modules/playwright-core/index.mjs')
);
const args = process.argv.slice(2);
const opt = (k, d) => {
  const i = args.indexOf(k);
  return i >= 0 ? args.splice(i, 2)[1] : d;
};
const cdp = opt('--cdp'),
  port = +opt('--port', 0),
  tag = opt('--tag', 'local');
const query = args[0] || '';
const DATA = process.env.WEBSPLAT_DATA;
if (!DATA) throw new Error('set WEBSPLAT_DATA');

const roots = {
  app: join(here, '../../app'),
  test: join(here, '..'),
  g2data: `${DATA}/g2`,
  stage4: `${DATA}/stage4`,
  ref: `${DATA}/ref/work`,
};
const types = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
};
const server = createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method === 'POST' && url.pathname === '/upload') {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      const t = url.searchParams.get('tag').replace(/[^\w.-]/g, '_');
      const name = url.searchParams
        .get('name')
        .replace(/[^\w./-]/g, '_')
        .replace(/\.\./g, '_');
      const file = `${DATA}/g2/browser/${t}/${name}`;
      mkdirSync(dirname(file), { recursive: true });
      writeFileSync(file, Buffer.concat(chunks));
      res.end('ok');
    });
    return;
  }
  const [, root, ...rest] = url.pathname.split('/');
  const file = roots[root] && normalize(join(roots[root], ...rest));
  if (!file || !file.startsWith(roots[root]) || !existsSync(file) || statSync(file).isDirectory()) {
    res.statusCode = 404;
    res.end();
    return;
  }
  res.setHeader('Content-Type', types[extname(file)] || 'application/octet-stream');
  res.setHeader('Content-Length', statSync(file).size);
  createReadStream(file).pipe(res);
}).listen(port, '127.0.0.1');
await new Promise((r) => server.once('listening', r));
const url = `http://127.0.0.1:${server.address().port}/test/g2/g2_test.html?tag=${tag}&${query}`;

let browser, page, ctx;
if (cdp) {
  browser = await chromium.connectOverCDP(`http://127.0.0.1:${cdp}`, {
    timeout: 20000,
  });
  page = await browser.contexts()[0].newPage();
} else {
  const flags =
    process.env.CHROME_FLAGS !== undefined
      ? process.env.CHROME_FLAGS.split(' ').filter(Boolean)
      : ['--use-angle=gl', '--enable-features=Vulkan'];
  ctx = await chromium.launchPersistentContext(`${DATA}/chrome-profiles/g2-${tag}`, {
    executablePath: process.env.CHROME || '/opt/google/chrome/chrome',
    headless: true,
    args: ['--headless=new', ...flags],
  });
  page = await ctx.newPage();
}
page.on('console', (m) => {
  if (m.type() !== 'log')
    console.log(`console.${m.type()}: ${m.text().split('\n')[0].slice(0, 300)}`);
  else if (
    m.text().startsWith('progress') ||
    m.text().startsWith('verified') ||
    m.text().startsWith('mapped') ||
    m.text().startsWith('stop')
  )
    console.log(m.text());
});
page.on('pageerror', (e) => console.log('pageerror', e.message));
const t0 = Date.now();
await page.goto(url, { timeout: 120000 });
await page.waitForFunction(() => window.result, null, {
  timeout: 60 * 60000,
  polling: 1000,
});
const result = await page.evaluate(() => window.result);
console.log(JSON.stringify(result, null, 1));
console.log(`wall ${((Date.now() - t0) / 1000).toFixed(1)} s`);
await page.close();
if (ctx) await ctx.close();
else await browser.close();
server.close();
process.exit(result.pass ? 0 : 1);
