import http from 'node:http';
import crypto from 'node:crypto';
import fs from 'node:fs';
import { test, expect, fixture, start } from './helpers.js';
import { openMedia } from '../../src/media/containers.js';
import { retimePacket } from '../../src/media/mpegts-live.js';

// A stand-in for a SupportProxy H.264 video port: a WebSocket that sends the program tables and
// then one access unit (with its KLV) every 100 ms, looping the MISB fixture with running
// timestamps and continuity counters. A viewer joins at the newest keyframe, as at the relay.
// restart() is a camera that starts again with a new clock; drop() only loses viewers.
let server, port, timer, tables, units, keyframes;
const sockets = new Set();
const stream = { n: 0, base: 0, counters: new Map(), group: [] };
const LOOP_TICKS = 20 * 9000;
function frame(payload) {
  const length = payload.length,
    header =
      length < 126
        ? Buffer.from([0x82, length])
        : length < 65536
          ? Buffer.from([0x82, 126, length >> 8, length & 255])
          : Buffer.concat([Buffer.from([0x82, 127, 0, 0, 0, 0]), Buffer.alloc(4)]);
  if (length >= 65536) header.writeUInt32BE(length, 6);
  return Buffer.concat([header, payload]);
}
function unit(n) {
  const index = n % units.length,
    bytes = Buffer.from(units[index]);
  for (let at = 0; at < bytes.length; at += 188) {
    const packet = bytes.subarray(at, at + 188),
      pid = (packet[1] & 31) * 256 + packet[2];
    retimePacket(packet, stream.base + Math.floor(n / units.length) * LOOP_TICKS);
    if (packet[3] & 0x10) {
      const counter = stream.counters.get(pid) ?? 0;
      packet[3] = (packet[3] & 0xf0) | counter;
      stream.counters.set(pid, (counter + 1) % 16);
    }
  }
  return { bytes, keyframe: keyframes.has(index) };
}
function tick() {
  const next = unit(stream.n++);
  if (next.keyframe) stream.group = [];
  stream.group.push(next.bytes);
  // split mid-packet, as the relay's 16 KiB messages do
  for (const socket of sockets) {
    socket.write(frame(next.bytes.subarray(0, 1000)));
    socket.write(frame(next.bytes.subarray(1000)));
  }
}
const drop = () => {
  for (const socket of sockets) socket.destroy();
  sockets.clear();
};
function restart() {
  drop();
  stream.n = 0;
  stream.group = [];
  // a restarted camera's clock is unrelated to the old one
  stream.base += 12345678;
}

test.beforeAll(async () => {
  const bytes = fs.readFileSync(fixture('misb.ts')),
    source = await openMedia(new Blob([bytes]));
  for await (const batch of source.index()) void batch;
  const starts = source.frames.map((f) => f.transport.packetStart);
  tables = bytes.subarray(0, starts[0]);
  units = starts.map((at, i) => bytes.subarray(at, starts[i + 1] ?? bytes.length));
  keyframes = new Set(source.frames.flatMap((f, i) => (f.keyframe ? [i] : [])));
  server = http.createServer((req, res) => res.writeHead(404).end());
  server.on('upgrade', (req, socket) => {
    const accept = crypto
      .createHash('sha1')
      .update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
      .digest('base64');
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
    );
    socket.on('error', () => {});
    socket.on('close', () => sockets.delete(socket));
    socket.write(frame(Buffer.from(tables)));
    for (const held of stream.group) socket.write(frame(held));
    sockets.add(socket);
  });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  port = server.address().port;
  timer = setInterval(tick, 100);
});
test.afterAll(async () => {
  clearInterval(timer);
  drop();
  server.closeAllConnections();
  await new Promise((resolve) => server.close(resolve));
});
test.beforeEach(() => {
  restart();
  stream.counters.clear();
});

// The shipped CSP lists the deployed proxy only.
test.use({ bypassCSP: true });

const view = (page) =>
  page.evaluate(() => {
    const s = window.fireViewer.state;
    return {
      frames: s.frames.length,
      number: s.number,
      playing: s.playing,
      following: s.live?.following ?? null,
      indexed: s.indexed,
      connection: s.source?.state,
      format: s.source?.format,
      name: s.source?.name,
      codec: s.source?.tracks?.[0]?.codec,
      size: `${s.source?.tracks?.[0]?.width}x${s.source?.tracks?.[0]?.height}`,
      epochs: [...new Set(s.frames.map((f) => f.epoch))],
      pts: s.frames.map((f) => f.pts),
      undecodable: s.frames.filter((f) => f.decodable === false).length,
      located: s.frames.filter((f) => Number.isFinite(f.telemetry?.position?.lat_e7)).length,
      decoded: s.decoded ? [s.decoded.width, s.decoded.height] : null,
    };
  });
const frames = (page, count) =>
  page.waitForFunction((count) => window.fireViewer.state.frames.length >= count, count);
const settled = (page) =>
  page.waitForFunction(() => {
    const s = window.fireViewer.state;
    return s.frame && !s.decoding && s.requestedFrameId === null;
  });
async function openLive(page) {
  await page.goto(`./?live=${encodeURIComponent(`ws://127.0.0.1:${port}/`)}`);
  await page.waitForFunction(() => window.fireViewer?.map?.viewer?.scene?.globe);
}

test('a live H.264 stream plays at its newest frame with its telemetry, and can be paused and resumed', async ({
  page,
}) => {
  await openLive(page);
  await frames(page, 15);
  await page.waitForFunction(() => window.fireViewer.state.decoded);
  let now = await view(page);
  expect(now.format).toBe('mpegts');
  expect(now.name).toMatch(/^live-127\.0\.0\.1-\d+-\d{8}T\d{6}Z\.ts$/);
  expect(now.codec).toBe('avc');
  expect(now.size).toBe('320x240');
  expect(now.decoded).toEqual([320, 240]);
  expect(now.playing && now.following).toBe(true);
  expect(now.connection).toBe('live');
  expect(now.frames - now.number).toBeLessThan(6);
  expect(now.undecodable).toBe(0);
  expect(now.located).toBe(now.frames);
  await expect(page.locator('#video-format')).toContainText('LIVE');
  await expect(page.locator('#source-name')).toContainText('.ts');
  await expect(page.locator('#video-canvas')).toBeVisible();
  await expect(page.locator('#error-text')).toBeEmpty();

  // pause holds the frame while the recording keeps growing; seek back and step
  await page.getByRole('button', { name: 'Pause video' }).click();
  await settled(page);
  const held = await view(page);
  await frames(page, held.frames + 12);
  now = await view(page);
  expect(now.number).toBe(held.number);
  expect(now.following).toBe(false);
  await page.locator('#seek').evaluate((seek) => {
    seek.value = '0.5';
    seek.dispatchEvent(new Event('input', { bubbles: true }));
  });
  await settled(page);
  expect((await view(page)).number).toBe(5);
  await page.locator('#next').click();
  await page.waitForFunction(() => window.fireViewer.state.number === 6);
  await settled(page);

  await page.locator('#go-live').click();
  await page.waitForFunction(() => {
    const s = window.fireViewer.state;
    return s.live.following && s.frames.length - s.number < 6;
  });
  await expect(page.locator('#go-live')).toHaveText('● LIVE');
});

test('a dropped H.264 connection resumes without repeats, and a restarted camera continues the timeline', async ({
  page,
}) => {
  await openLive(page);
  await frames(page, 14);
  drop();
  await page.waitForFunction(() => window.fireViewer.state.source.state === 'reconnecting');
  let before = (await view(page)).frames;
  await frames(page, before + 12);
  let now = await view(page);
  expect(now.epochs).toEqual([0]);
  expect(now.connection).toBe('live');
  for (let i = 1; i < now.pts.length; i++) expect(now.pts[i]).toBeGreaterThan(now.pts[i - 1]);
  // rejoining at the keyframe already held repeats nothing: the frames stay 100 ms apart
  // except across the time the viewer was away
  const steps = now.pts.slice(1).map((t, i) => Math.round((t - now.pts[i]) * 10));
  expect(steps.filter((s) => s < 1).length).toBe(0);

  restart();
  before = now.frames;
  await page.waitForFunction(() => window.fireViewer.state.frames.some((f) => f.epoch === 1));
  await frames(page, before + 12);
  now = await view(page);
  expect(now.epochs).toEqual([0, 1]);
  for (let i = 1; i < now.pts.length; i++) expect(now.pts[i]).toBeGreaterThan(now.pts[i - 1]);
  await page.waitForFunction(() => {
    const s = window.fireViewer.state;
    return s.live.following && s.playing && s.frames.length - s.number < 6;
  });
});

test('End live leaves an indexed H.264 recording, and the saved file reopens and decodes', async ({
  page,
}) => {
  await openLive(page);
  await frames(page, 12);
  restart();
  await page.waitForFunction(() => window.fireViewer.state.frames.some((f) => f.epoch === 1));
  await frames(page, 30);
  await page.locator('#live-end').click();
  await page.waitForFunction(() => window.fireViewer.state.indexed);
  const live = await page.evaluate(() => {
    const s = window.fireViewer.state;
    return {
      file: s.file instanceof File,
      name: s.file.name,
      type: s.file.type,
      size: s.file.size,
      pts: s.frames.map((f) => f.pts),
      state: s.source.state,
    };
  });
  expect(live.file).toBe(true);
  expect(live.name).toMatch(/\.ts$/);
  expect(live.type).toBe('video/mp2t');
  expect(live.state).toBe('ended');
  await expect(page.locator('#go-live')).toBeHidden();
  // an ended recording still seeks and decodes
  await page.evaluate(() => window.fireViewer.showFrame(3));
  await page.waitForFunction(() => window.fireViewer.state.number === 3);
  await settled(page);

  const download = page.waitForEvent('download');
  await page.locator('#live-save').click();
  const saved = await (await download).path();
  expect(fs.statSync(saved).size).toBe(live.size);
  await start(page);
  await page.locator('#video-file').setInputFiles({
    name: 'saved-live.ts',
    mimeType: 'video/mp2t',
    buffer: fs.readFileSync(saved),
  });
  await page.waitForFunction(() => window.fireViewer.state.indexed);
  const reopened = await page.evaluate(() => {
    const s = window.fireViewer.state;
    return { pts: s.frames.map((f) => f.pts), live: s.live, warnings: s.warnings };
  });
  expect(reopened.live).toBe(null);
  // the file has every frame the live index had, on one increasing timeline
  for (let i = 1; i < reopened.pts.length; i++)
    expect(reopened.pts[i]).toBeGreaterThan(reopened.pts[i - 1]);
  for (const t of live.pts) expect(reopened.pts.some((r) => Math.abs(r - t) < 1e-6)).toBe(true);
  await page.evaluate(() => window.fireViewer.showFrame(3));
  await page.waitForFunction(() => window.fireViewer.state.number === 3);
  await settled(page);
  await expect(page.locator('#live-bar')).toBeHidden();
});
