#!/usr/bin/env python3
"""Serve a site of thermal splats: an index page plus one interactive viewer per splat.

SITE is a directory laid out as SITE/DATE/FLIGHT/RUN/ with splat.pt and scale.json from
train.py in each RUN directory (an optional description.txt is shown in the index). URLs:

  /                      index of all splats
  /DATE/, /DATE/FLIGHT/  index of that subset
  /DATE/FLIGHT/RUN/      the view_splat.py viewer for that splat

Each viewer runs as its own view_splat.py process on a local port, started on the first
visit and stopped after it has been idle, so many splats can be published without holding
them all on the GPU. New RUN directories are picked up without a restart. Put this behind a
TLS reverse proxy that passes websockets, e.g. Apache:

  ProxyPass /splat/ http://HOST:8088/ upgrade=websocket
"""
import argparse
import asyncio
import html
import os
from pathlib import Path
import re
import socket
import sys
import time

from aiohttp import ClientError, ClientSession, ClientTimeout, WSMsgType, web

HERE = Path(__file__).resolve().parent
NAME_RE = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')
# a viewer may be replaced by another once its visitors have left for IDLE_GRACE seconds, or sent nothing
# for STALE seconds (browsers keep websockets of pages in their back/forward cache open)
IDLE_GRACE = 20
MAX_CLIENT_MSG = 1 << 20
STALE = 120
HOP_HEADERS = {'connection', 'keep-alive', 'proxy-authenticate', 'proxy-authorization', 'te', 'trailers',
               'transfer-encoding', 'upgrade', 'host', 'content-length', 'content-encoding'}


def find_splats(site):
    """{(date, flight, run): directory} for every complete splat under site."""
    out = {}
    for pt in site.glob('*/*/*/splat.pt'):
        d = pt.parent
        key = tuple(d.relative_to(site).parts)
        if all(NAME_RE.match(k) for k in key) and (d / 'scale.json').exists():
            out[key] = d
    return out


def die_with_parent():
    """Run in the child: get SIGTERM if the site server dies, so viewers are never orphaned."""
    try:
        import ctypes
        import signal
        ctypes.CDLL(None).prctl(1, signal.SIGTERM)  # PR_SET_PDEATHSIG
    except Exception:
        pass


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


class Viewer:
    """One view_splat.py process for one splat. Start and stop are serialised by self.lock. A
    viewer is running (routable) once it has reported its port, and alive (holding GPU memory,
    counted against --max-running) until its process has exited."""

    def __init__(self, key, path, args):
        self.key, self.path, self.args = key, path, args
        self.proc = None
        self.port = None
        self.sockets = 0
        self.last_used = time.monotonic()
        self.lock = asyncio.Lock()

    @property
    def alive(self):
        return self.proc is not None and self.proc.returncode is None

    @property
    def running(self):
        return self.port is not None and self.alive

    def title(self):
        t = '**%s**' % ' / '.join(self.key)
        desc = self.path / 'description.txt'
        if desc.exists() and desc.read_text().strip():
            t += '\n\n' + desc.read_text().strip()
        return t

    async def ensure(self):
        async with self.lock:
            if self.running:
                return
            await self._stop_locked()  # a crashed process may linger
            port = free_port()
            cmd = [sys.executable, str(HERE / 'view_splat.py'), str(self.path), '--host', '127.0.0.1',
                   '--port', str(port), '--max-clients', str(self.args.max_clients),
                   '--max-width', str(self.args.max_width), '--title', self.title()]
            name = '/'.join(self.key)
            print('starting %s' % name, flush=True)
            proc = await asyncio.create_subprocess_exec(*cmd, stdout=asyncio.subprocess.PIPE,
                                                        stderr=asyncio.subprocess.STDOUT,
                                                        preexec_fn=die_with_parent)
            self.proc = proc
            ready = re.compile(r'viewer at http://[^:]+:(\d+)')
            deadline = time.monotonic() + self.args.start_timeout
            try:
                while True:
                    try:
                        line = await asyncio.wait_for(proc.stdout.readline(), max(0.1, deadline - time.monotonic()))
                    except asyncio.TimeoutError:
                        line = b''
                    if not line:
                        await self._stop_locked()
                        raise web.HTTPServiceUnavailable(text='viewer for %s failed to start' % name)
                    m = ready.search(line.decode(errors='replace'))
                    if m:
                        break
            except asyncio.CancelledError:
                proc.kill()  # cancelled mid-start: never leave a half-started viewer behind
                raise
            asyncio.create_task(self._drain(name, proc))
            self.port = int(m.group(1))  # viser moves on if the port was taken meanwhile
            self.last_used = time.monotonic()
            print('%s ready on port %d' % (name, self.port), flush=True)

    @staticmethod
    async def _drain(name, proc):
        while True:
            line = await proc.stdout.readline()
            if not line:
                break
            print('[%s] %s' % (name, line.decode(errors='replace').rstrip()), flush=True)

    async def stop(self):
        async with self.lock:
            await self._stop_locked()

    async def _stop_locked(self):
        # unroutable at once, but self.proc is kept until the process has exited so it stays
        # counted against --max-running and a cancelled stop can be retried
        self.port = None
        proc = self.proc
        if proc is not None and proc.returncode is None:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), 10)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
            except asyncio.CancelledError:
                proc.kill()
                raise
            print('stopped %s' % '/'.join(self.key), flush=True)
        if self.proc is proc:
            self.proc = None


class Site:
    def __init__(self, args):
        self.args = args
        self.viewers = {}
        self.session = None
        self.start_lock = asyncio.Lock()

    def refresh(self):
        splats = find_splats(self.args.site)
        for key, path in splats.items():
            if key not in self.viewers:
                self.viewers[key] = Viewer(key, path, self.args)
        return splats

    async def viewer(self, key, start=True):
        self.refresh()
        v = self.viewers.get(key)
        if v is None:
            raise web.HTTPNotFound(text='no such splat')
        if not v.running and not start:
            raise web.HTTPServiceUnavailable(text='viewer not running, reload the page')
        if not v.running:
            # one start at a time, so concurrent first visits can't exceed max_running
            async with self.start_lock:
                deadline = time.monotonic() + 30
                while not v.running:
                    now = time.monotonic()
                    alive = [x for x in self.viewers.values() if x.alive]
                    if len(alive) < self.args.max_running:
                        break
                    # keep viewers in use or just left (the browser may be reconnecting)
                    idle = sorted((x for x in alive if x.running and now - x.last_used > (IDLE_GRACE if x.sockets == 0 else STALE)),
                                  key=lambda x: x.last_used)
                    if idle:
                        await idle[0].stop()
                        continue
                    if now > deadline:
                        raise web.HTTPServiceUnavailable(text='all viewers are busy, please try again later')
                    await asyncio.sleep(1)
                await v.ensure()
        v.last_used = time.monotonic()
        return v

    async def reaper(self):
        while True:
            await asyncio.sleep(30)
            now = time.monotonic()
            for v in list(self.viewers.values()):
                if v.running and 0 < self.args.idle_timeout < now - v.last_used:
                    await v.stop()
                elif v.alive and not v.running and not v.lock.locked():
                    await v.stop()  # left over from a cancelled start

    def index(self, prefix):
        splats = self.refresh()
        def natural(k):  # run9 before run12
            return [[int(t) if t.isdigit() else t for t in re.split(r'(\d+)', p)] for p in k]
        keys = sorted((k for k in splats if k[:len(prefix)] == prefix), key=natural)
        depth = len(prefix)
        up = '../' * depth
        rows = []
        for k in keys:
            desc = splats[k] / 'description.txt'
            lines = desc.read_text().strip().splitlines() if desc.exists() else []
            text = html.escape(lines[0]) if lines else ''
            cells = ''.join('<td>%s</td>' % html.escape(p) for p in k)
            rows.append('<tr>%s<td><a href="%s%s/">view</a></td><td>%s</td></tr>' % (
                cells, up, '/'.join(html.escape(p) for p in k), text))
        crumbs = ['<a href="%s">all</a>' % (up or './')]
        for i, p in enumerate(prefix):
            crumbs.append('<a href="%s">%s</a>' % ('../' * (depth - i - 1) or './', html.escape(p)))
        title = html.escape(self.args.title)
        body = ('<table><tr><th>date</th><th>flight</th><th>run</th><th></th><th></th></tr>%s</table>' % ''.join(rows)
                if rows else '<p>No splats here.</p>')
        return web.Response(content_type='text/html', text='''<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="robots" content="noindex"><title>%s</title>
<style>body{font-family:system-ui,sans-serif;margin:2em;max-width:60em}table{border-collapse:collapse}
td,th{padding:.3em .8em;text-align:left;border-bottom:1px solid #ddd}th{background:#f4f4f4}</style></head>
<body><h1>%s</h1><p>%s</p>%s
<p>Each viewer renders on a GPU server; the first visit may take up to a minute to start. Drag to orbit,
right-drag to pan, scroll to zoom.</p>%s</body></html>
''' % (title, title, ' / '.join(crumbs), body, self.args.footer))

    async def handle(self, request):
        parts = [p for p in request.path.split('/') if p]
        if len(parts) < 3 or any(not NAME_RE.match(p) for p in parts[:3]):
            if any(not NAME_RE.match(p) for p in parts):
                raise web.HTTPNotFound()
            if not request.path.endswith('/'):
                raise web.HTTPMovedPermanently(parts[-1] + '/')
            return self.index(tuple(parts))
        key = tuple(parts[:3])
        rest = '/'.join(parts[3:])
        is_ws = request.headers.get('Upgrade', '').lower() == 'websocket'
        if not rest and not request.path.endswith('/') and not is_ws:
            # the viser client opens its websocket on the path without the trailing slash
            raise web.HTTPMovedPermanently(parts[2] + '/' + ('?' + request.query_string if request.query_string else ''))
        # only page loads start viewers: pages left in a browser's back/forward cache keep trying to
        # reconnect their websocket and would otherwise restart viewers that were stopped to free a slot
        v = await self.viewer(key, start=not is_ws)
        port = v.port  # may be cleared by a concurrent stop
        if port is None:
            raise web.HTTPServiceUnavailable(text='viewer restarting, reload the page')
        target = 'http://127.0.0.1:%d/%s' % (port, rest)
        if is_ws:
            return await self.proxy_ws(request, v, target)
        headers = {k: val for k, val in request.headers.items() if k.lower() not in HOP_HEADERS}
        try:
            async with self.session.request(request.method, target, params=request.query, headers=headers,
                                            data=await request.read(), allow_redirects=False) as r:
                body = await r.read()
                out = {k: val for k, val in r.headers.items() if k.lower() not in HOP_HEADERS}
                return web.Response(status=r.status, headers=out, body=body)
        except ClientError:
            raise web.HTTPBadGateway(text='viewer not reachable, reload the page')

    async def proxy_ws(self, request, v, target):
        if self.args.max_clients and v.sockets >= self.args.max_clients:
            raise web.HTTPServiceUnavailable(text='viewer is busy, please try again later')
        protocols = [p.strip() for p in request.headers.get('Sec-WebSocket-Protocol', '').split(',') if p.strip()]
        v.sockets += 1
        try:
            async with self.session.ws_connect(target.replace('http://', 'ws://'), protocols=protocols,
                                               max_msg_size=0, autoping=False) as back:
                # visitors only send small control messages; the viewer's own output is trusted
                front = web.WebSocketResponse(protocols=[back.protocol] if back.protocol else (),
                                              max_msg_size=MAX_CLIENT_MSG, autoping=False)
                try:
                    await front.prepare(request)
                except ConnectionResetError:  # visitor already gone
                    return front

                async def pump(src, dst, from_client):
                    async for msg in src:
                        if msg.type == WSMsgType.TEXT:
                            await dst.send_str(msg.data)
                        elif msg.type == WSMsgType.BINARY:
                            await dst.send_bytes(msg.data)
                        elif msg.type == WSMsgType.PING:
                            await dst.ping(msg.data)
                        elif msg.type == WSMsgType.PONG:
                            await dst.pong(msg.data)
                        else:
                            break
                        if from_client and msg.type in (WSMsgType.TEXT, WSMsgType.BINARY):
                            v.last_used = time.monotonic()  # visitor activity, not heartbeats
                done, pending = await asyncio.wait([asyncio.create_task(pump(front, back, True)),
                                                    asyncio.create_task(pump(back, front, False))],
                                                   return_when=asyncio.FIRST_COMPLETED)
                for t in pending:
                    t.cancel()
                # let cancelled reads finish first: aiohttp's close() otherwise waits for them forever
                await asyncio.gather(*pending, return_exceptions=True)
                await front.close()
                return front
        except ClientError:
            raise web.HTTPBadGateway(text='viewer not reachable, reload the page')
        finally:
            v.sockets -= 1
            v.last_used = time.monotonic()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('site', type=Path, help='site directory laid out as DATE/FLIGHT/RUN/')
    ap.add_argument('--host', default='127.0.0.1', help='bind address (0.0.0.0 for a proxy on another machine)')
    ap.add_argument('--port', type=int, default=8088)
    ap.add_argument('--title', default='Thermal Gaussian splats')
    ap.add_argument('--footer', default='', help='trusted HTML appended to index pages, e.g. related links')
    ap.add_argument('--max-running', type=int, default=4, help='most viewer processes at once')
    ap.add_argument('--idle-timeout', type=float, default=900, help='stop a viewer when its visitors have sent nothing for this many seconds (0: never)')
    ap.add_argument('--start-timeout', type=float, default=180)
    ap.add_argument('--max-clients', type=int, default=6, help='per viewer')
    ap.add_argument('--max-width', type=int, default=1280, help='largest render width a visitor may choose')
    args = ap.parse_args()
    args.site = args.site.resolve()
    if not args.site.is_dir():
        sys.exit('no site directory %s' % args.site)

    site = Site(args)

    async def startup(app):
        site.session = ClientSession(timeout=ClientTimeout(total=None, sock_connect=10))
        app['reaper'] = asyncio.create_task(site.reaper())

    async def cleanup(app):
        app['reaper'].cancel()
        for v in site.viewers.values():
            await v.stop()
        await site.session.close()

    app = web.Application()
    app.router.add_route('*', '/{tail:.*}', site.handle)
    app.on_startup.append(startup)
    app.on_cleanup.append(cleanup)
    print('serving %d splats from %s on http://%s:%d/' % (len(find_splats(args.site)), args.site, args.host, args.port),
          flush=True)
    web.run_app(app, host=args.host, port=args.port, print=None, shutdown_timeout=5)


if __name__ == '__main__':
    os.environ.setdefault('PYTHONUNBUFFERED', '1')
    main()
