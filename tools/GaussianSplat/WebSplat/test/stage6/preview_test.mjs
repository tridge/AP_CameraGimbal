// stage-list.js showPreview (codex stage 6 round 1, finding 7): a training preview stays shown
// through ordinary progress messages (no extra.preview) while the stage is active, and goes when
// it stops. A minimal fake DOM row; the OPFS read fails in Node and is ignored.
// usage: node preview_test.mjs
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

globalThis.navigator ??= {};
globalThis.document = {
  createElement: () => ({ dataset: {}, style: {}, className: '', remove() { this.removed = true; } }),
};
const { showPreview } = await import(join(dirname(fileURLToPath(import.meta.url)), '../../app/src/ui/stage-list.js'));
const bar = { children: [], append(x) { this.children.push(x); x.parent = this; } };
const tr = {
  querySelector: (q) => (q === '.bar' ? bar : q === 'img.preview' ? bar.children.find((c) => c.className === 'preview' && !c.removed) ?? null : null),
};
let failures = 0;
const check = (name, ok) => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}`); if (!ok) failures++; };
const preview = { path: 'p.png', seq: 1, view: '00009.png', iteration: 2000 };
showPreview(tr, { state: 'running', progress: { extra: { gaussians: 5, preview } } });
check('preview shown', !!tr.querySelector('img.preview'));
showPreview(tr, { state: 'running', progress: { extra: { gaussians: 6 } } });
check('kept through ordinary progress', !!tr.querySelector('img.preview'));
showPreview(tr, { state: 'checkpointing', progress: { extra: { gaussians: 6 } } });
check('kept while checkpointing', !!tr.querySelector('img.preview'));
showPreview(tr, { state: 'done', progress: { extra: { gaussians: 7 } } });
check('removed when the stage stops', !tr.querySelector('img.preview'));
console.log(failures ? `${failures} check(s) failed` : 'preview: PASS');
process.exit(failures ? 1 : 0);
