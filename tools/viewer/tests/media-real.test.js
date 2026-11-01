/** Opt-in local-file qualification; no flight recordings or absolute paths enter git.
 * VIEWER_MEDIA_REAL_FILES='["/path/video.ts","/path/thermal.mkv"]' npm test
 * VIEWER_MEDIA_REAL_LIMIT=0 scans completely; default 128 verifies early startup.
 */
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import { openMedia } from '../src/media/containers.js';

const paths = JSON.parse(process.env.VIEWER_MEDIA_REAL_FILES || '[]');
test(
  'opt-in real recording bounded startup/index/packet read',
  { skip: !paths.length },
  async () => {
    const limit = Number(process.env.VIEWER_MEDIA_REAL_LIMIT ?? 128);
    for (const path of paths) {
      const handle = await fs.open(path),
        { size } = await handle.stat();
      let bytesRead = 0,
        maximum = 0,
        firstBatchBytes;
      const file = {
        size,
        slice(start, end) {
          maximum = Math.max(maximum, end - start);
          return {
            async arrayBuffer() {
              const b = Buffer.alloc(end - start);
              const read = await handle.read(b, 0, b.length, start);
              bytesRead += read.bytesRead;
              return b.buffer.slice(b.byteOffset, b.byteOffset + read.bytesRead);
            },
          };
        },
      };
      try {
        const source = await openMedia(file, { batchSize: 8 });
        for await (const batch of source.index()) {
          if (batch.frames.length) firstBatchBytes ??= bytesRead;
          if (limit && source.frames.length >= limit) break;
        }
        assert.ok(source.frames.length > 0, path);
        assert.ok(maximum <= 1048576, `${path}: ${maximum}`);
        if (size > 16 * 1048576)
          assert.ok(
            firstBatchBytes < size / 2,
            `${path}: startup scanned ${firstBatchBytes}/${size}`,
          );
        const candidates = [source.frames[0], source.frames.at(-1)];
        for (const frame of candidates) {
          const data = await source.readPacket(frame);
          assert.equal(data.length, frame.size);
          assert.ok(Number.isFinite(frame.pts));
        }
        console.log(
          JSON.stringify({
            path,
            size,
            frames: source.frames.length,
            firstBatchBytes,
            maximum,
            bytesRead,
            firstPTS: source.frames[0].pts,
            lastPTS: source.frames.at(-1).pts,
            warnings: source.warnings,
          }),
        );
        source.close();
      } finally {
        await handle.close();
      }
    }
  },
);
