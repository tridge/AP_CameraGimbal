/* Classic worker: decode one H.264/HEVC keyframe group (a small Matroska Blob) with the viewer's
 * pinned FFmpeg core, keeping the wanted presentation times, scaled to the processing size
 * (area averaging, as train_rgb.py's cv2.INTER_AREA), as RGBA. Loading as ffv1-batch.worker.js.
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
    if (type !== 'decode' || !core) throw new Error('RGB batch decoder has not loaded');
    if (data.blob.size > 65 * 1024 * 1024) throw new Error('RGB batch input exceeds its budget');
    const expected = data.width * data.height * 4 * data.times.length;
    if (!Number.isSafeInteger(expected) || expected <= 0 || expected > 512 * 1024 * 1024)
      throw new Error('RGB batch output exceeds its budget');
    const logs = [];
    const shown = [];
    let timeBase = null;
    const started = performance.now();
    // showinfo's integer pts in its input time base (pts_time is rounded to about 6 digits)
    core.setLogger(({ message }) => {
      const m = /\bn:\s*(\d+)\s+pts:\s*(-?\d+)\s/.exec(message);
      const tb = /config in time_base:\s*(\d+)\/(\d+)/.exec(message);
      if (m) shown.push(Number(m[2]));
      else if (tb) timeBase = Number(tb[1]) / Number(tb[2]);
      else if (logs.length < 200) logs.push(message);
    });
    core.FS.mount(
      core.FS.filesystems.WORKERFS,
      { blobs: [{ name: 'group.mkv', data: data.blob }] },
      '/input',
    );
    // half a microsecond-rounded Matroska tick either side of each wanted time
    const pick = data.times.map(
      (t) => `between(t\\,${(t - 2e-6).toFixed(6)}\\,${(t + 2e-6).toFixed(6)})`,
    );
    try {
      core.setTimeout(300000);
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
        `select='${pick.join('+')}',showinfo,scale=${data.width}:${data.height}:flags=area`,
        '-vsync',
        '0',
        '-pix_fmt',
        'rgba',
        '-f',
        'rawvideo',
        '/frames.raw',
      );
      const exitCode = core.ret;
      core.reset();
      if (exitCode !== 0)
        throw new Error(`RGB batch decode failed (${exitCode}): ${logs.slice(-8).join('\n')}`);
      if (
        !timeBase ||
        shown.length !== data.times.length ||
        shown.some((pts, i) => Math.abs(pts * timeBase - data.times[i]) > 2e-6)
      )
        throw new Error(
          `decoded ${shown.length} of ${data.times.length} wanted frames, or out of order`,
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
