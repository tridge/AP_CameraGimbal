// Stage 4 browser harness: serves the app + match_test.html + the exported native data and
// runs the page in headless Chrome, printing the result JSON.
//   WEBSPLAT_DATA=... node run_match.mjs [--cdp PORT] [--port HTTPPORT] [--tag T] [query]
// Default: launch a local headless Chrome (laptop flags, own profile). --cdp: open a tab in an
// already running Chrome (e.g. win11 through ssh -L; serve to it through ssh -R --port) and
// close only that tab. query is passed to the page, e.g. "mode=edge" or "pairs=200&power=low-power".
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
  test: here,
  native: `${DATA}/stage4/native`,
  ref: `${DATA}/ref/work`,
};
const types = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.json': 'application/json',
};
const server = createServer((req, res) => {
  const url = new URL(req.url, 'http://x');
  if (req.method === 'POST' && url.pathname === '/upload') {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      mkdirSync(`${DATA}/stage4/out`, { recursive: true });
      writeFileSync(
        `${DATA}/stage4/out/${url.searchParams.get('name').replace(/[^\w.-]/g, '_')}.bin`,
        Buffer.concat(chunks),
      );
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
const url = `http://127.0.0.1:${server.address().port}/test/match_test.html?${query}`;

let browser, page, ctx;
if (cdp) {
  browser = await chromium.connectOverCDP(`http://127.0.0.1:${cdp}`, { timeout: 20000 });
  page = await browser.contexts()[0].newPage();
} else {
  const flags =
    process.env.CHROME_FLAGS !== undefined
      ? process.env.CHROME_FLAGS.split(' ').filter(Boolean)
      : ['--use-angle=gl', '--enable-features=Vulkan'];
  ctx = await chromium.launchPersistentContext(`${DATA}/chrome-profiles/stage4-${tag}`, {
    executablePath: process.env.CHROME || '/opt/google/chrome/chrome',
    headless: true,
    args: ['--headless=new', ...flags],
  });
  page = await ctx.newPage();
}
const consoleCounts = {};
page.on('console', (m) => {
  consoleCounts[m.type()] = (consoleCounts[m.type()] || 0) + 1;
  if (m.type() !== 'log')
    console.log(`console.${m.type()}: ${m.text().split('\n')[0].slice(0, 300)}`);
  else if (m.text().startsWith('progress')) console.log(m.text());
});
page.on('pageerror', (e) => console.log('pageerror', e.message));
const t0 = Date.now();
await page.goto(url, { timeout: 120000 });
await page.waitForFunction(() => window.result, null, { timeout: 30 * 60000, polling: 1000 });
const result = await page.evaluate(() => window.result);
console.log(JSON.stringify(result, null, 1));
console.log(
  `wall ${((Date.now() - t0) / 1000).toFixed(1)} s, console messages ${JSON.stringify(consoleCounts)}`,
);
await page.close();
if (ctx) await ctx.close();
else await browser.close();
server.close();
const pass = result.pass ?? result.cmp?.pass;
process.exit(pass ? 0 : 1);
