// Run probe.html in a separate headless Chrome (laptop flags by default) and print results.
// usage: WEBSPLAT_DATA=... node run_probe.mjs TAG [query] [--extra-chrome-flag ...]
import { createServer } from 'node:http';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const { chromium } = await import(join(here, '../../../../../viewer/node_modules/playwright-core/index.mjs'));
const [tag = 'probe', query = '', ...extra] = process.argv.slice(2);
const base = process.env.CHROME_FLAGS !== undefined ? process.env.CHROME_FLAGS.split(' ').filter(Boolean)
  : ['--use-angle=gl', '--enable-features=Vulkan', '--enable-unsafe-webgpu'];

const server = createServer((req, res) => {
  res.setHeader('Content-Type', 'text/html');
  res.end(readFileSync(join(here, 'probe.html')));
}).listen(0, '127.0.0.1');
await new Promise((r) => server.once('listening', r));

const browser = await chromium.launchPersistentContext(`${process.env.WEBSPLAT_DATA}/chrome-profiles/g1-${tag}`, {
  executablePath: process.env.CHROME || '/opt/google/chrome/chrome', headless: true,
  viewport: { width: 1280, height: 720 },
  args: ['--headless=new', ...base, ...extra],
});
const page = await browser.newPage();
const consoleCounts = {};
page.on('console', (m) => {
  consoleCounts[m.type()] = (consoleCounts[m.type()] || 0) + 1;
  if (m.type() !== 'log') console.log(`console.${m.type()}: ${m.text().split('\n')[0].slice(0, 200)}`);
});
page.on('pageerror', (e) => console.log('pageerror', e.message));
await page.goto(`http://127.0.0.1:${server.address().port}/probe.html?${query}`);
await page.waitForFunction(() => window.done, null, { timeout: 120000 });
for (const [k, v, t] of await page.evaluate(() => window.results)) console.log(String(t).padStart(6), k.padEnd(28), typeof v === 'string' ? v : JSON.stringify(v));
console.log('console messages', JSON.stringify(consoleCounts));
await browser.close();
server.close();
