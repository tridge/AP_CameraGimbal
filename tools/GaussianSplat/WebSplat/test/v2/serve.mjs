// Same-origin development server for the viewer-to-WebSplat handover, laid out as on
// plot.ardupilot.org: /video/ -> tools/viewer/dist, /websplat/ -> the WebSplat site build
// (app/dist, or WEBSPLAT_APP_DIST: e.g. a deploy.sh release root). Plain static files, as both
// sites are deployed (no special headers); no-store so rebuilds show at once.
// usage: node serve.mjs [--port 8800] [--host 127.0.0.1]
// As a module: serveV2({port, host, video, websplat, extra: {'/url/path/': 'directory'}}).
import { createReadStream, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { APP_DIST, args } from '../harness/lib/env.mjs';

export const VIEWER_DIST = fileURLToPath(new URL('../../../../viewer/dist/', import.meta.url));

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript',
  '.mjs': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.mkv': 'video/x-matroska',
  '.spz': 'application/octet-stream',
  '.ply': 'application/octet-stream',
};

export function serveV2({
  port = 8800,
  host = '127.0.0.1',
  video = VIEWER_DIST,
  websplat = APP_DIST,
  extra = {},
} = {}) {
  // the longest prefix wins (extra mounts may lie below /websplat/)
  const mounts = Object.entries({ '/video/': video, '/websplat/': websplat, ...extra })
    .map(([prefix, dir]) => [prefix, resolve(dir)])
    .sort((x, y) => y[0].length - x[0].length);
  const server = createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    const path = decodeURIComponent(url.pathname);
    if (path === '/__blank') {
      res.writeHead(200, { 'content-type': 'text/html' });
      return res.end('<!doctype html><link rel="icon" href="data:,"><title>blank</title>');
    }
    // /video -> /video/, as the nginx config does
    const bare = mounts.find(([prefix]) => path === prefix.slice(0, -1));
    if (bare) {
      res.writeHead(301, { location: bare[0] + url.search });
      return res.end();
    }
    const mount = mounts.find(([prefix]) => path.startsWith(prefix));
    let file = null;
    let size = 0;
    if (mount) {
      let rel = normalize(path.slice(mount[0].length)).replace(/^(\.\.[/\\])+/, '');
      if (rel === '.' || rel.endsWith(sep)) rel = join(rel, 'index.html');
      file = join(mount[1], rel);
      try {
        if (!file.startsWith(mount[1] + sep) && file !== mount[1]) throw new Error();
        const st = statSync(file);
        if (!st.isFile()) throw new Error();
        size = st.size;
      } catch {
        file = null;
      }
    }
    if (!file) {
      res.writeHead(404);
      return res.end('not found');
    }
    res.writeHead(200, {
      'content-type': TYPES[extname(file)] ?? 'application/octet-stream',
      'cache-control': 'no-store',
      'content-length': size,
    });
    createReadStream(file).pipe(res);
  });
  return new Promise((ok, fail) => {
    server.once('error', fail);
    server.listen(port, host, () => ok(server));
  });
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const a = args();
  const port = +(a.port ?? 8800);
  const host = a.host ?? '127.0.0.1';
  await serveV2({ port, host });
  console.log(`video viewer: http://${host}:${port}/video/  (${VIEWER_DIST})`);
  console.log(`WebSplat:     http://${host}:${port}/websplat/  (${APP_DIST})`);
}
