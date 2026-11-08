// $WEBSPLAT_DATA etc. from the environment or WebSplat/local.env (untracked)
import { existsSync, readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

export const WEBSPLAT = fileURLToPath(new URL('../../../', import.meta.url));
// WEBSPLAT_APP_DIST: serve another build (e.g. one made with vite build --outDir)
export const APP_DIST = process.env.WEBSPLAT_APP_DIST
  ? resolve(process.env.WEBSPLAT_APP_DIST)
  : `${WEBSPLAT}app/dist`;

export function env(name) {
  if (process.env[name]) return process.env[name];
  const f = `${WEBSPLAT}local.env`;
  if (existsSync(f)) {
    for (const line of readFileSync(f, 'utf8').split('\n')) {
      const m = line.match(/^\s*([A-Z_]+)=(.*)$/);
      if (m && m[1] === name) return m[2].trim();
    }
  }
  throw new Error(`${name} is not set (environment or local.env)`);
}

export function args(argv = process.argv.slice(2)) {
  const out = { _: [] };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) out._.push(a);
    else if (a.includes('=')) out[a.slice(2, a.indexOf('='))] = a.slice(a.indexOf('=') + 1);
    else if (i + 1 < argv.length && !argv[i + 1].startsWith('--')) out[a.slice(2)] = argv[++i];
    else out[a.slice(2)] = true;
  }
  return out;
}
