/* Classic worker: batch FFV1 gray16 decode with the viewer's pinned FFmpeg core (the core uses
 * importScripts). Loading follows tools/viewer/src/media/software-decoder.worker.js; the decode
 * converts a whole small Matroska group in one exec instead of selecting one frame.
 */
let core;
self.onmessage = async ({ data }) => {
  const { id, type } = data;
  try {
    if (type === 'load') {
      importScripts(data.coreURL);
      core = await self.createFFmpegCore({
        mainScriptUrlOrBlob: `${data.coreURL}#${btoa(JSON.stringify({ wasmURL: data.coreURL.replace(/\.js$/, '.wasm') }))}`,
      });
      core.FS.mkdir('/input');
      self.postMessage({ id, result: { loaded: true } });
      return;
    }
    if (type !== 'decode' || !core) throw new Error('batch decoder has not loaded');
    if (data.blob.size > 65 * 1024 * 1024) throw new Error('batch input exceeds its budget');
    const frameBytes = data.width * data.height * 2;
    const expected = frameBytes * data.times.length;
    if (!Number.isSafeInteger(expected) || expected <= 0 || expected > 256 * 1024 * 1024)
      throw new Error('batch output exceeds its budget');
    const logs = [];
    const shown = [];
    const started = performance.now();
    core.setLogger(({ message }) => {
      const m = /\bn:\s*(\d+)\s+pts:\s*[-\d]+\s+pts_time:([^\s]+)/.exec(message);
      if (m) shown.push([Number(m[1]), Number(m[2])]);
      else if (logs.length < 200) logs.push(message);
    });
    core.FS.mount(
      core.FS.filesystems.WORKERFS,
      { blobs: [{ name: 'group.mkv', data: data.blob }] },
      '/input',
    );
    try {
      core.setTimeout(120000);
      core.exec(
        '-hide_banner',
        '-nostdin',
        '-y',
        '-copyts',
        '-i',
        '/input/group.mkv',
        '-map',
        '0:v:0',
        '-vf',
        'showinfo',
        '-vsync',
        '0',
        '-pix_fmt',
        'gray16le',
        '-f',
        'rawvideo',
        '/frames.raw',
      );
      const exitCode = core.ret;
      core.reset();
      if (exitCode !== 0)
        throw new Error(`FFV1 batch decode failed (${exitCode}): ${logs.slice(-8).join('\n')}`);
      if (!logs.some((line) => /Video: ffv1.*\bgray16le\b/.test(line)))
        throw new Error('FFV1 source is not lossless gray16');
      // one output frame per input packet, in order, at the packet's timestamp
      if (
        shown.length !== data.times.length ||
        shown.some(([n, t], i) => n !== i || Math.abs(t - data.times[i]) > 0.00001)
      )
        throw new Error(
          `decoded ${shown.length} frames for ${data.times.length} packets, or out of order`,
        );
      const bytes = core.FS.readFile('/frames.raw');
      if (bytes.byteLength !== expected)
        throw new Error(`decoded ${bytes.byteLength} bytes; expected ${expected}`);
      self.postMessage(
        { id, result: { buffer: bytes.buffer, decodeMs: performance.now() - started } },
        [bytes.buffer],
      );
    } finally {
      try {
        core.FS.unlink('/frames.raw');
      } catch {
        /* no output after a failed decode */
      }
      core.FS.unmount('/input');
    }
  } catch (error) {
    self.postMessage({ id, error: String(error?.message ?? error) });
  }
};
