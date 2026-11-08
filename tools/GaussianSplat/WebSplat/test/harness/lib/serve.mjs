// Minimal static server for app/dist (plus /__blank for clearing storage on the origin).
import { createReadStream, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize } from 'node:path';

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
};

export function serve(root, port, host = '127.0.0.1') {
  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    if (url.pathname === '/__blank') {
      res.writeHead(200, { 'content-type': 'text/html' });
      return res.end('<!doctype html><link rel="icon" href="data:,"><title>blank</title>');
    }
    let p = normalize(decodeURIComponent(url.pathname)).replace(/^(\.\.[/\\])+/, '');
    if (p.endsWith('/')) p += 'index.html';
    const file = join(root, p);
    try {
      if (!file.startsWith(root) || !statSync(file).isFile()) throw new Error();
    } catch {
      res.writeHead(404);
      return res.end('not found');
    }
    res.writeHead(200, {
      'content-type': TYPES[extname(file)] ?? 'application/octet-stream',
      'cache-control': 'no-store',
    });
    createReadStream(file).pipe(res);
  });
  return new Promise((resolve) => server.listen(port, host, () => resolve(server)));
}
