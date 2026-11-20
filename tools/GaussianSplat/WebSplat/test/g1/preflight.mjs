// Environment preflight for a remote G1 attempt: in the test Chrome (over CDP), requestAdapter()
// in a page and in a worker must return the NVIDIA adapter. Prints the adapter info as JSON;
// exits 0 if both are NVIDIA, 1 otherwise. No training input is sent.
// usage: node preflight.mjs --cdp http://127.0.0.1:9353 --port 8782
import { createServer } from 'node:http';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const { chromium } = await import(join(here, '../../../../viewer/node_modules/playwright-core/index.mjs'));
const opt = { port: '8782' };
for (let i = 2; i < process.argv.length; i += 2) opt[process.argv[i].replace(/^--/, '')] = process.argv[i + 1];

// a secure origin (localhost through the reverse tunnel), as the harness page has
const server = createServer((req, res) => res.writeHead(200, { 'Content-Type': 'text/html' }).end('<!DOCTYPE html><html></html>'))
  .listen(+opt.port, '127.0.0.1');
await new Promise((r) => server.once('listening', r));
let info = { page: null, worker: null, error: null };
let browser;
try {
  browser = await chromium.connectOverCDP(opt.cdp, { timeout: 30000 });
  const page = await browser.contexts()[0].newPage();
  await page.goto(`http://localhost:${opt.port}/preflight`);
  info = await page.evaluate(async () => {
    const describe = (a) => (a ? { vendor: a.info.vendor, architecture: a.info.architecture, description: a.info.description } : null);
    const main = navigator.gpu ? describe(await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' })) : 'no navigator.gpu';
    const src = `onmessage = async () => { const a = navigator.gpu && await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      postMessage(a ? { vendor: a.info.vendor, architecture: a.info.architecture, description: a.info.description } : (navigator.gpu ? null : 'no navigator.gpu')); }`;
    const w = new Worker(URL.createObjectURL(new Blob([src])));
    const worker = await new Promise((res) => { w.onmessage = (e) => res(e.data); w.postMessage(0); });
    w.terminate();
    return { page: main, worker, error: null };
  });
  await page.close();
} catch (e) {
  info.error = e.message.split('\n')[0];
}
await browser?.close().catch(() => {});
server.close();
const ok = info.page?.vendor === 'nvidia' && info.worker?.vendor === 'nvidia';
console.log(JSON.stringify({ ok, ...info }));
process.exit(ok ? 0 : 1);
