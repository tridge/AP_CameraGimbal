import http from 'node:http';
import crypto from 'node:crypto';
import fs from 'node:fs';
import { test, expect, fixture, start } from './helpers.js';
import { openMedia } from '../../src/media/containers.js';
import { headBytes, liveCluster, liveHead } from '../helpers/live-stream.js';

// A stand-in for a SupportProxy video port: a WebSocket that sends the Matroska prefix and
// then a Cluster every 100 ms, built from the thermal fixture's FFV1 frames with a moving
// position. restart() is a publisher that starts again from zero; drop() only loses viewers.
let server, port, timer, packets, head;
const sockets = new Set();
const stream = { n: 0, ticks: 0, last: null, paused: false };
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
function tick() {
  if (stream.paused) return;
  stream.last = liveCluster(stream.ticks, stream.n, {
    payload: packets[stream.n % packets.length],
  });
  // split mid-element, as the relay's 16 KiB messages do
  for (const socket of sockets) {
    socket.write(frame(Buffer.from(stream.last.subarray(0, 1000))));
    socket.write(frame(Buffer.from(stream.last.subarray(1000))));
  }
  stream.n++;
  stream.ticks += 100;
}
const drop = () => {
  for (const socket of sockets) socket.destroy();
  sockets.clear();
};
function restart() {
  drop();
  stream.ticks = 0;
  stream.last = null;
}

test.beforeAll(async () => {
  const source = await openMedia(new Blob([fs.readFileSync(fixture('thermal.mkv'))]));
  for await (const batch of source.index()) void batch;
  packets = [];
  for (const f of source.frames) packets.push(await source.readPacket(f));
  head = headBytes(liveHead({ codecPrivate: source.tracks[0].codecPrivate }));
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
    if (new URL(req.url, 'http://x').searchParams.get('pw') === 'wrong') return socket.destroy();
    // a viewer starts at the prefix and the newest complete Cluster
    socket.write(frame(Buffer.from(head)));
    if (stream.last) socket.write(frame(Buffer.from(stream.last)));
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
  stream.n = 0;
  stream.paused = false;
});

// The shipped CSP lists the deployed proxy only.
test.use({ bypassCSP: true });

const view = (page) =>
  page.evaluate(() => {
    const s = window.fireViewer.state;
    return {
      frames: s.frames.length,
      number: s.number,
      shown: s.frame?.thermalMetadata?.frame_id ?? null,
      playing: s.playing,
      following: s.live?.following ?? null,
      indexed: s.indexed,
      connection: s.source?.state,
      track: window.fireViewer.map.videoTrack.length,
      golive: document.getElementById('go-live').textContent,
      epochs: [...new Set(s.frames.map((f) => f.epoch))],
      pts: s.frames.map((f) => f.pts),
    };
  });
const frames = (page, count) =>
  page.waitForFunction((count) => window.fireViewer.state.frames.length >= count, count);
const settled = (page) =>
  page.waitForFunction(() => {
    const s = window.fireViewer.state;
    return s.frame && !s.decoding && s.requestedFrameId === null;
  });
async function openLive(page, query = '') {
  await page.goto(`./?live=${encodeURIComponent(`ws://127.0.0.1:${port}/`)}${query}`);
  await page.waitForFunction(() => window.fireViewer?.map?.viewer?.scene?.globe);
}

test('a live stream plays at its newest frame, grows the map track and can be paused and resumed', async ({
  page,
}) => {
  await openLive(page);
  await frames(page, 15);
  await page.waitForFunction(() => window.fireViewer.map.videoTrack.length >= 8);
  let now = await view(page);
  expect(now.playing && now.following).toBe(true);
  expect(now.connection).toBe('live');
  expect(now.golive).toBe('● LIVE');
  expect(now.frames - now.number).toBeLessThan(6);
  await expect(page.locator('#video-format')).toContainText('LIVE');
  await expect(page.locator('#live-bar')).toBeVisible();
  const firstTrack = now.track;

  // pause holds the frame while the recording and the track keep growing
  await page.getByRole('button', { name: 'Pause video' }).click();
  await settled(page);
  const held = await view(page);
  await frames(page, held.frames + 15);
  await page.waitForFunction((n) => window.fireViewer.map.videoTrack.length > n, firstTrack);
  now = await view(page);
  expect(now.number).toBe(held.number);
  expect(now.shown).toBe(held.shown);
  expect(now.following).toBe(false);
  expect(now.golive).toMatch(/^Go live · −00:0[1-9]/);

  // Play carries on from the held frame; Go live jumps to the newest
  await page.getByRole('button', { name: 'Play video' }).click();
  await page.waitForFunction((n) => window.fireViewer.state.number > n, held.number);
  expect((await view(page)).following).toBe(false);
  await page.locator('#go-live').click();
  await page.waitForFunction(() => {
    const s = window.fireViewer.state;
    return s.live.following && s.frames.length - s.number < 6;
  });
  await expect(page.locator('#go-live')).toHaveText('● LIVE');
});

test('seeking back, clicking the track and frame stepping leave the live edge', async ({
  page,
}) => {
  await openLive(page);
  await frames(page, 30);
  await page.locator('#seek').evaluate((seek) => {
    seek.value = '0.5';
    seek.dispatchEvent(new Event('input', { bubbles: true }));
  });
  await settled(page);
  let now = await view(page);
  expect(now.number).toBe(5);
  expect(now.playing || now.following).toBe(false);
  await page.keyboard.press('ArrowRight');
  await page.waitForFunction(() => window.fireViewer.state.number === 6);
  await settled(page);

  // click the track where frame 12 was recorded
  await page.locator('#go-live').click();
  await page.waitForFunction(() => window.fireViewer.state.live.following);
  await page.locator('#map-follow').uncheck();
  await page.waitForFunction(() => window.fireViewer.map.videoTrack.some((p) => p.frameId >= 20));
  const point = await page.evaluate(() => {
    const map = window.fireViewer.map,
      p = map.videoTrack.find((p) => p.frameId >= 12),
      c = map.viewer.scene.cartesianToCanvasCoordinates(p.world),
      r = map.viewer.scene.canvas.getBoundingClientRect();
    return { x: r.left + c.x, y: r.top + c.y, frameId: p.frameId };
  });
  await page.mouse.click(point.x, point.y);
  await settled(page);
  await page.waitForFunction(() => !window.fireViewer.state.playing);
  now = await view(page);
  expect(Math.abs(now.number - point.frameId)).toBeLessThanOrEqual(3);
  expect(now.following).toBe(false);
});

test('a dropped connection resumes the same timeline and a restarted publisher continues it', async ({
  page,
}) => {
  await openLive(page);
  await frames(page, 10);
  drop();
  await page.waitForFunction(() => window.fireViewer.state.source.state === 'reconnecting');
  let before = (await view(page)).frames;
  await frames(page, before + 10);
  let now = await view(page);
  expect(now.epochs).toEqual([0]);
  expect(now.connection).toBe('live');

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

test('End live leaves an indexed recording, and the saved file reopens with the same frames', async ({
  page,
}) => {
  await openLive(page);
  await frames(page, 12);
  restart();
  await page.waitForFunction(() => window.fireViewer.state.frames.some((f) => f.epoch === 1));
  await frames(page, 30);
  await page.locator('#live-end').click();
  await page.waitForFunction(() => window.fireViewer.state.indexed);
  const live = await page.evaluate(async () => {
    const s = window.fireViewer.state;
    return {
      file: s.file instanceof File,
      size: s.file.size,
      pts: s.frames.map((f) => f.pts),
      ids: s.frames.map((f) => f.thermalMetadata.frame_id),
      state: s.source.state,
    };
  });
  expect(live.file).toBe(true);
  expect(live.state).toBe('ended');
  await expect(page.locator('#go-live')).toBeHidden();
  await expect(page.locator('#live-end')).toBeHidden();
  await expect(page.locator('#video-format')).not.toContainText('LIVE');
  // an ended recording still seeks and decodes
  await page.evaluate(() => window.fireViewer.showFrame(3));
  await page.waitForFunction(() => window.fireViewer.state.number === 3);
  await settled(page);
  const pixel = await page.evaluate(() =>
    Array.from(window.fireViewer.state.decoded.data.slice(0, 64)),
  );

  const download = page.waitForEvent('download');
  await page.locator('#live-save').click();
  const saved = await (await download).path();
  expect(fs.statSync(saved).size).toBe(live.size);
  await start(page);
  await page.locator('#video-file').setInputFiles({
    name: 'saved-live.mkv',
    mimeType: 'video/x-matroska',
    buffer: fs.readFileSync(saved),
  });
  await page.waitForFunction(() => window.fireViewer.state.indexed);
  const reopened = await page.evaluate(() => {
    const s = window.fireViewer.state;
    return {
      pts: s.frames.map((f) => f.pts),
      ids: s.frames.map((f) => f.thermalMetadata.frame_id),
      live: s.live,
    };
  });
  expect(reopened.live).toBe(null);
  expect(reopened.ids).toEqual(live.ids);
  expect(reopened.pts).toEqual(live.pts);
  await page.evaluate(() => window.fireViewer.showFrame(3));
  await page.waitForFunction(() => window.fireViewer.state.number === 3);
  await settled(page);
  expect(
    await page.evaluate(() => Array.from(window.fireViewer.state.decoded.data.slice(0, 64))),
  ).toEqual(pixel);
  await expect(page.locator('#live-bar')).toBeHidden();
});

test('a refused connection is reported as observed and a password can be entered', async ({
  page,
}) => {
  stream.paused = true;
  await openLive(page, '#pw=wrong');
  expect(new URL(page.url()).hash).toBe('');
  await expect(page.locator('#live-status')).toContainText(
    'closed the connection before sending video',
  );
  stream.paused = false;
  await page.getByRole('button', { name: 'Live', exact: true }).click();
  await expect(page.locator('#live-url')).toHaveValue(`ws://127.0.0.1:${port}/`);
  // the close button works under the shipped CSP (no inline handlers)
  await page.getByRole('button', { name: 'Close live stream' }).click();
  await expect(page.locator('#live-dialog')).toBeHidden();
  await page.getByRole('button', { name: 'Live', exact: true }).click();
  await page.locator('#live-password').fill('right');
  await page.getByRole('button', { name: 'Connect' }).click();
  await frames(page, 5);
  expect((await view(page)).connection).toBe('live');

  // a new password for the same stream keeps what has been received
  const session = await page.evaluate(() => window.fireViewer.state.live.started);
  await page.getByRole('button', { name: 'Live', exact: true }).click();
  await page.locator('#live-password').fill('other');
  await page.getByRole('button', { name: 'Connect' }).click();
  const had = (await view(page)).frames;
  await frames(page, had + 5);
  expect(await page.evaluate(() => window.fireViewer.state.live.started)).toBe(session);
});

test('a bad address is explained', async ({ page }) => {
  await page.goto(`./?live=${encodeURIComponent('ftp://example.org/')}`);
  await expect(page.locator('#error-text')).toContainText('wss://');
});

test.describe('with the shipped CSP', () => {
  test.use({ bypassCSP: false });
  test('a proxy the page does not list is refused once, with the reason', async ({ page }) => {
    await openLive(page);
    await expect(page.locator('#error-text')).toContainText(
      'Content-Security-Policy (connect-src)',
    );
    await page.waitForFunction(() => window.fireViewer.state.source.state === 'failed');
  });
});
