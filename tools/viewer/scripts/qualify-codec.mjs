#!/usr/bin/env node
/** Visible-Xephyr, local-file qualification for the actual parser/software codec.
 * Usage: DISPLAY=:99 node scripts/qualify-codec.mjs FILE REFERENCES.json OUTPUT.json
 * REFERENCES: {frames:[{pts:seconds, expected:sha256, pixelSampleOffsets?:[],
 *                      pixelSampleValues?:[]}]}. Native raw references use gray16le
 * for thermal or rgba for display video. No reference recording is uploaded.
 */
import { chromium } from '@playwright/test';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
const [filename, referencePath, outputPath] = process.argv.slice(2);
if (!filename || !referencePath || !outputPath)
  throw new Error('Usage: qualify-codec.mjs FILE REFERENCES.json OUTPUT.json');
if (!process.env.DISPLAY || /^:0(?:\.\d+)?$/.test(process.env.DISPLAY))
  throw new Error('Set DISPLAY to a dedicated Xephyr display, for example :99');
const baseURL = new URL(process.env.VIEWER_URL ?? 'http://127.0.0.1:5173/viewer/');
const references = JSON.parse(await readFile(referencePath, 'utf8'));
const browser = await chromium.launch({
  executablePath: process.env.CHROME ?? '/opt/google/chrome/chrome',
  headless: false,
  args: [
    '--use-angle=swiftshader',
    '--enable-unsafe-swiftshader',
    '--disable-features=Vulkan',
    '--disable-dev-shm-usage',
    ...(process.env.CHROME_NO_SANDBOX === '1' ? ['--no-sandbox'] : []),
  ],
});
const requests = [];
const failures = [];
try {
  const context = await browser.newContext();
  await context.route('**/*', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    requests.push({
      method: request.method(),
      url: request.url(),
      bodyBytes: request.postDataBuffer()?.length ?? 0,
    });
    if (url.origin !== baseURL.origin || request.method() !== 'GET') {
      failures.push(request.url());
      await route.abort();
    } else await route.continue();
  });
  const page = await context.newPage();
  await page.goto(new URL('docs/PLAN.md', baseURL).href);
  await page.evaluate(() => {
    document.body.innerHTML =
      '<h1>Local parser / software decoder qualification</h1><input type="file" aria-label="Recording"><pre></pre>';
  });
  await page.locator('input').setInputFiles(path.resolve(filename));
  const result = await page.evaluate(
    async ({ references, baseURL }) => {
      const { openMedia } = await import(new URL('src/media/containers.js', baseURL).href);
      const { SoftwareDecoder } = await import(
        new URL('src/media/software-decoder.js', baseURL).href
      );
      const file = document.querySelector('input').files[0];
      const readSizes = [];
      const originalRead = Blob.prototype.arrayBuffer;
      Blob.prototype.arrayBuffer = function () {
        readSizes.push(this.size);
        if (this.size > 4 * 1024 * 1024)
          throw new Error(`Input Blob read exceeds 4 MiB: ${this.size}`);
        return originalRead.call(this);
      };
      const source = await openMedia(file);
      const status = [];
      const decoder = new SoftwareDecoder({ onStatus: (message) => status.push(message) });
      try {
        const wanted = references.frames.map((frame) => frame.pts);
        for await (const batch of source.index({ batchSize: 16 })) {
          if (
            wanted.every((time) =>
              source.frames.some((frame) => Math.abs(frame.pts - time) < 0.000002),
            )
          )
            break;
        }
        await decoder.open(file, source);
        const frames = [];
        for (const reference of references.frames) {
          const frame = source.frames.find(
            (candidate) => Math.abs(candidate.pts - reference.pts) < 0.000002,
          );
          if (!frame) throw new Error(`Missing indexed frame PTS ${reference.pts}`);
          const start = performance.now();
          const decoded = await decoder.decode(frame);
          const bytes = new Uint8Array(
            decoded.data.buffer,
            decoded.data.byteOffset,
            decoded.data.byteLength,
          );
          const hash = [...new Uint8Array(await crypto.subtle.digest('SHA-256', bytes))]
            .map((value) => value.toString(16).padStart(2, '0'))
            .join('');
          const differences = reference.pixelSampleOffsets?.map((offset, i) =>
            Math.abs(bytes[offset] - reference.pixelSampleValues[i]),
          );
          frames.push({
            id: frame.id,
            pts: frame.pts,
            offset: frame.offset,
            extents: frame.extents,
            kind: decoded.kind,
            width: decoded.width,
            height: decoded.height,
            bytes: bytes.length,
            sha256: hash,
            exact: reference.expected ? hash === reference.expected : null,
            maxAbsoluteSampleError: differences ? Math.max(...differences) : null,
            meanAbsoluteSampleError: differences
              ? differences.reduce((sum, n) => sum + n, 0) / differences.length
              : null,
            elapsedMs: performance.now() - start,
          });
          document.querySelector('pre').textContent = JSON.stringify({ frames, status }, null, 2);
        }
        return {
          fileSize: file.size,
          format: source.format,
          indexedFrames: source.frames.length,
          maxBlobReadBytes: Math.max(0, ...readSizes),
          frames,
          status,
        };
      } finally {
        decoder.close();
        source.close();
        Blob.prototype.arrayBuffer = originalRead;
      }
    },
    { references, baseURL: baseURL.href },
  );
  const failedRaw = result.frames.some((frame) => frame.kind === 'gray16' && frame.exact !== true);
  const failedDisplay = result.frames.some(
    (frame) =>
      frame.kind === 'rgba' &&
      frame.maxAbsoluteSampleError !== null &&
      frame.maxAbsoluteSampleError > 2,
  );
  await writeFile(
    outputPath,
    JSON.stringify({ ...result, requests, blockedRequests: failures }, null, 2) + '\n',
  );
  await page.screenshot({ path: outputPath.replace(/\.json$/, '') + '.png' });
  if (failedRaw || failedDisplay || failures.length)
    throw new Error(`Codec qualification failed; see ${outputPath}`);
  console.log(`Codec qualification passed; see ${outputPath}`);
} finally {
  await browser.close();
}
