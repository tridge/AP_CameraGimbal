/* Classic worker: the pinned single-thread FFmpeg core uses importScripts.
 * Input is a bounded synthetic Matroska Blob, never the complete recording.
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
    if (type !== 'decode' || !core) throw new Error('Software decoder has not loaded');
    if (data.blob.size > 33 * 1024 * 1024)
      throw new Error('Software input exceeds its 33 MiB budget');
    const expected = data.width * data.height * (data.kind === 'gray16' ? 2 : 4);
    if (!Number.isSafeInteger(expected) || expected <= 0 || expected > 64 * 1024 * 1024)
      throw new Error('Software frame exceeds output budget');
    if (!Number.isFinite(data.targetTime) || data.targetTime < 0)
      throw new Error('Invalid decode timestamp');
    const logs = [];
    const started = performance.now();
    core.setLogger(({ message }) => {
      if (logs.length < 160) logs.push(message);
    });
    core.FS.mount(
      core.FS.filesystems.WORKERFS,
      { blobs: [{ name: 'group.mkv', data: data.blob }] },
      '/input',
    );
    try {
      // Preserve assigned packet timestamps. The two-microsecond tolerance
      // handles rounding to Matroska ticks without choosing a preceding frame.
      const filter = `select=gte(t\\,${Math.max(0, data.targetTime - 0.000002).toFixed(6)}),showinfo`;
      core.setTimeout(30000);
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
        filter,
        '-vsync',
        '0',
        '-frames:v',
        '1',
        '-pix_fmt',
        data.kind === 'gray16' ? 'gray16le' : 'rgba',
        '-f',
        'rawvideo',
        '/frame.raw',
      );
      const exitCode = core.ret;
      core.reset();
      if (exitCode !== 0)
        throw new Error(`Software video decode failed (${exitCode}): ${logs.slice(-8).join('\n')}`);
      if (data.kind === 'gray16' && !logs.some((line) => /Video: ffv1.*\bgray16le\b/.test(line)))
        throw new Error('FFV1 source is not lossless gray16; cannot expose calibrated raw pixels');
      const selected = logs
        .map((line) => /\bn:\s*0\s+pts:\s*[-\d]+\s+pts_time:([^\s]+)/.exec(line))
        .find(Boolean);
      if (!selected || Math.abs(Number(selected[1]) - data.targetTime) > 0.00001)
        throw new Error('Software decoder could not identify the requested presentation frame');
      const bytes = core.FS.readFile('/frame.raw');
      if (bytes.byteLength !== expected)
        throw new Error(`Decoded frame has ${bytes.byteLength} bytes; expected ${expected}`);
      self.postMessage(
        {
          id,
          result: { kind: data.kind, buffer: bytes.buffer, decodeMs: performance.now() - started },
        },
        [bytes.buffer],
      );
    } finally {
      try {
        core.FS.unlink('/frame.raw');
      } catch {
        /* No output on failed decode. */
      }
      core.FS.unmount('/input');
    }
  } catch (error) {
    self.postMessage({ id, error: String(error?.message ?? error) });
  }
};
