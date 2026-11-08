// Headless Chrome: launched on this machine with its own profile, or the win11 machine's
// instance reached over CDP through an ssh tunnel (win11/tunnel.sh).
import { mkdirSync } from 'node:fs';
import { chromium } from '../../../../../viewer/node_modules/playwright-core/index.mjs';

export const LINUX_CHROME = '/opt/google/chrome/chrome';
export const LINUX_FLAGS = [
  '--headless=new',
  '--use-angle=gl',
  '--enable-features=Vulkan',
  '--enable-unsafe-webgpu',
];

// adapter: 'low-power' forces the integrated GPU through Chrome as well as the page parameter
export async function launchLocal({ profile, adapter, extraArgs = [] }) {
  mkdirSync(profile, { recursive: true });
  const args = [...LINUX_FLAGS, '--no-first-run', '--no-default-browser-check', ...extraArgs];
  if (adapter === 'low-power') args.push('--use-webgpu-power-preference=force-low-power');
  const context = await chromium.launchPersistentContext(profile, {
    executablePath: process.env.CHROME ?? LINUX_CHROME,
    headless: false, // --headless=new is in args
    args,
    viewport: { width: 1280, height: 900 },
  });
  return { context, close: () => context.close(), kind: 'local' };
}

export async function connectCdp(endpoint = 'http://127.0.0.1:9333') {
  const browser = await chromium.connectOverCDP(endpoint, { timeout: 20000 });
  const context = browser.contexts()[0] ?? (await browser.newContext());
  return {
    context,
    close: async () => {
      for (const p of context.pages()) await p.close().catch(() => {});
      await browser.close();
    },
    kind: 'cdp',
  };
}

// every console message (warnings matter: Dawn reports allocation trouble as warnings)
export function capture(page, sink, tag) {
  page.on('console', (m) =>
    sink.push({
      t: Date.now(),
      tag,
      type: m.type(),
      text: m.text().slice(0, 2000),
    }),
  );
  page.on('pageerror', (e) =>
    sink.push({ t: Date.now(), tag, type: 'pageerror', text: e.message }),
  );
  page.on('response', (r) => {
    if (r.status() >= 400)
      sink.push({
        t: Date.now(),
        tag,
        type: 'http',
        text: `${r.status()} ${r.url()}`,
      });
  });
  page.on('crash', () => sink.push({ t: Date.now(), tag, type: 'crash', text: 'page crashed' }));
  page.on('worker', (w) =>
    sink.push({
      t: Date.now(),
      tag,
      type: 'worker',
      text: w.url().replace(/^.*\//, ''),
    }),
  );
}
