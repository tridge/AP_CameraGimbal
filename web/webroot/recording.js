/* Range-backed MPEG-TS recording playback. No whole-file fetch or scan.
 * mpegts.js handles video transmuxing; this adapter owns bounded HTTP reads,
 * sparse PTS/keyframe lookup, and a recording-wide seek control.
 */
(function () {
  'use strict';
  const root = document.getElementById('recording-player');
  if (!root) return;
  const video = document.getElementById('recording-video');
  const playButton = document.getElementById('recording-play');
  const seek = document.getElementById('recording-seek');
  const time = document.getElementById('recording-time');
  const status = document.getElementById('recording-status');
  const PACKET = 188, CHUNK = PACKET * 2048, PROBE = PACKET * 1024, WRAP = 2 ** 33;
  let player = null, generation = 0, lookup = null, windowStart = 0;
  let drag = false, pendingTime = null, pendingPlay = false, seeking = false, requestedTime = 0, closed = false;
  const file = { url: root.dataset.url, size: 0, pid: null, prefix: null, first: null, duration: 0, samples: [] };
  const clock = (seconds) => {
    seconds = Math.max(0, Math.floor(seconds || 0));
    return (seconds >= 3600 ? Math.floor(seconds / 3600) + ':' : '') +
      String(Math.floor(seconds / 60) % 60).padStart(2, '0') + ':' + String(seconds % 60).padStart(2, '0');
  };
  function message(text) { status.textContent = text || ''; }
  function failure(error) {
    if (closed) return;
    console.error('Recording playback:', error);
    generation++; lookup?.abort(); pendingTime = null; pendingPlay = false; seeking = false;
    destroy(); playButton.disabled = true;
    message(root.dataset.error);
  }
  function startPlayback() {
    const owner = player;
    video.play().catch((error) => {
      // Replacing a seek window or pausing can cancel an in-flight play().
      if (player === owner && error.name !== 'AbortError') failure(error);
    });
  }
  function abortError() { return new DOMException('Cancelled', 'AbortError'); }
  async function read(start, length, signal) {
    start = Math.max(0, Math.floor(start));
    const end = Math.min(file.size, start + length) - 1;
    if (end < start) return new Uint8Array();
    const response = await fetch(file.url, { headers: { Range: `bytes=${start}-${end}` },
      credentials: 'same-origin', redirect: 'error', cache: 'no-store', signal });
    const range = /^bytes (\d+)-(\d+)\/(\d+)$/.exec(response.headers.get('Content-Range') || '');
    if (response.status !== 206 || !range || Number(range[1]) !== start || Number(range[2]) !== end ||
        Number(range[3]) < file.size || Number(response.headers.get('Content-Length')) !== end - start + 1) {
      if (response.body) await response.body.cancel();
      throw new Error('Recording server did not honour the bounded byte range');
    }
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (signal && signal.aborted) throw abortError();
    if (bytes.length !== end - start + 1) throw new Error('Short recording read');
    return bytes;
  }
  function payload(bytes, at) {
    if (bytes[at] !== 0x47 || (bytes[at + 1] & 0x80) || !(bytes[at + 3] & 0x10)) return -1;
    const off = 4 + (bytes[at + 3] & 0x20 ? 1 + bytes[at + 4] : 0);
    return off < PACKET ? at + off : -1;
  }
  function program(bytes) {
    let pmt = null, pat = null, table = null;
    for (let at = 0; at + PACKET <= bytes.length; at += PACKET) {
      const pid = (bytes[at + 1] & 31) * 256 + bytes[at + 2], off = payload(bytes, at);
      if (off < 0 || !(bytes[at + 1] & 64)) continue;
      const section = off + 1 + bytes[off];
      const end = section + 3 + ((bytes[section + 1] & 15) * 256 + bytes[section + 2]);
      if (section + 12 > at + PACKET || end > at + PACKET || end < section + 12) continue;
      if (pid === 0 && bytes[section] === 0) {
        for (let i = section + 8; i + 4 <= end - 4; i += 4) {
          if (bytes[i] || bytes[i + 1]) { pmt = (bytes[i + 2] & 31) * 256 + bytes[i + 3]; break; }
        }
        pat = bytes.slice(at, at + PACKET);
      } else if (pid === pmt && bytes[section] === 2) {
        for (let i = section + 12 + ((bytes[section + 10] & 15) * 256 + bytes[section + 11]); i + 5 <= end - 4;) {
          const size = (bytes[i + 3] & 15) * 256 + bytes[i + 4];
          if (i + 5 + size > end - 4) break;
          if ([0x1b, 0x24].includes(bytes[i])) {
            file.pid = (bytes[i + 1] & 31) * 256 + bytes[i + 2];
            table = bytes.slice(at, at + PACKET);
            break;
          }
          i += 5 + size;
        }
      }
      if (pat && table) { file.prefix = new Uint8Array(PACKET * 2); file.prefix.set(pat); file.prefix.set(table, PACKET); return; }
    }
    throw new Error('No supported recording program found');
  }
  function scan(bytes, start) {
    const samples = [];
    const aligned = (PACKET - start % PACKET) % PACKET;
    for (let at = aligned; at + PACKET <= bytes.length; at += PACKET) {
      if (bytes[at] !== 0x47 || ((bytes[at + 1] & 31) * 256 + bytes[at + 2]) !== file.pid || !(bytes[at + 1] & 64)) continue;
      const off = payload(bytes, at);
      if (off < 0 || off + 14 > at + PACKET || bytes[off] || bytes[off + 1] || bytes[off + 2] !== 1 || !(bytes[off + 7] & 0x80)) continue;
      const b = bytes.subarray(off + 9, off + 14);
      if (!(b[0] & b[2] & b[4] & 1)) continue;
      const pts = ((b[0] >> 1) & 7) * 2 ** 30 + b[1] * 2 ** 22 + (b[2] >> 1) * 2 ** 15 + b[3] * 128 + (b[4] >> 1);
      file.first ??= pts;
      const seconds = ((pts - file.first + WRAP) % WRAP) / 90000;
      const key = !!(bytes[at + 3] & 0x20) && bytes[at + 4] > 0 && !!(bytes[at + 5] & 64);
      samples.push({ offset: start + at, seconds, key });
    }
    const known = new Set(file.samples.map((p) => p.offset));
    for (const sample of samples) if (!known.has(sample.offset)) { file.samples.push(sample); known.add(sample.offset); }
    file.samples.sort((a, b) => a.offset - b.offset);
    // Retain a sparse, bounded index; probes are cheap to repeat when needed.
    while (file.samples.length > 4096) {
      const sparse = file.samples.filter((p, i, all) => p.key || i === 0 || i === all.length - 1 || i % 2 === 0);
      file.samples = sparse.length < file.samples.length ? sparse
        : sparse.filter((p, i, all) => i === 0 || i === all.length - 1 || i % 2 === 0);
    }
    return samples;
  }
  async function initialise(signal) {
    const response = await fetch(file.url, { method: 'HEAD', credentials: 'same-origin', redirect: 'error', cache: 'no-store', signal });
    file.size = Number(response.headers.get('Content-Length'));
    if (!response.ok || !Number.isSafeInteger(file.size) || file.size < PACKET * 3) throw new Error('Invalid recording length');
    const first = await read(0, PROBE, signal);
    program(first);
    const head = scan(first, 0);
    if (!head.length) throw new Error('No recording timestamps');
    let tail = [];
    for (let n = PROBE; !tail.length && n <= 4 * 1024 * 1024; n *= 2) {
      const start = Math.max(0, Math.floor((file.size - n) / PACKET) * PACKET);
      tail = scan(await read(start, file.size - start, signal), start);
    }
    if (!tail.length) throw new Error('No recording end timestamp');
    const times = [...head, ...tail].map((p) => p.seconds).sort((a, b) => a - b);
    const gaps = times.slice(1).map((t, i) => t - times[i]).filter((g) => g > 0 && g < 1).sort((a, b) => a - b);
    const period = gaps.length ? gaps[Math.floor(gaps.length / 2)] : 1 / 30;
    file.duration = Math.max(...times) + period;
    if (!(file.duration > 0 && file.duration < WRAP / 90000 / 2)) throw new Error('Unsupported recording clock discontinuity');
    seek.max = file.duration; seek.disabled = false;
  }
  async function locate(seconds, signal) {
    if (seconds < 0.1) return { offset: 0, seconds: 0 };
    let budget = Math.min(file.size, 64 * 1024 * 1024, Math.max(8 * 1024 * 1024, Math.floor(file.size / 4)));
    const probe = async (start, length) => {
      const count = Math.min(length, file.size - start, budget);
      if (count <= 0) throw new Error('Recording seek probe budget exhausted');
      budget -= count;
      return read(start, count, signal);
    };
    const near = () => file.samples.filter((p) => p.key && p.seconds <= seconds && seconds - p.seconds <= 3).at(-1);
    for (let i = 0; i < 12; i++) {
      const cached = near(); if (cached) return cached;
      const before = file.samples.filter((p) => p.seconds <= seconds).at(-1) || file.samples[0];
      const after = file.samples.find((p) => p.seconds > seconds) || file.samples.at(-1);
      const fraction = after.seconds > before.seconds ? (seconds - before.seconds) / (after.seconds - before.seconds) : 0;
      const guess = before.offset + (after.offset - before.offset) * fraction;
      let points = [];
      // A high-bitrate frame can be larger than the initial probe. Expand
      // around the estimate until a PES timestamp is found; never repeatedly
      // probe the same empty window or fall back to a distant cached keyframe.
      for (let length = PROBE; length <= PROBE * 16; length *= 2) {
        const start = Math.max(0, Math.floor((guess - length / 2) / PACKET) * PACKET);
        points = scan(await probe(start, length), start);
        if (points.length >= 2) break;
      }
      if (!points.length) throw new Error('No timestamp near requested position');
      const key = near(); if (key) return key;
      if (points[0].seconds <= seconds && points.at(-1).seconds >= seconds) break;
    }
    const before = file.samples.filter((p) => p.seconds <= seconds).at(-1);
    if (!before || seconds - before.seconds > 3) throw new Error('No timestamp near requested position');
    // Scan backwards in disjoint bounded blocks. Very high-bitrate SITL
    // recordings can have more than 8 MiB between successive keyframes.
    const floor = Math.max(0, Math.floor((before.offset - 64 * 1024 * 1024) / PACKET) * PACKET);
    for (let end = before.offset + PACKET; end > floor;) {
      const start = Math.max(floor, end - Math.floor(Math.min(CHUNK * 4, budget) / PACKET) * PACKET);
      if (start === end) throw new Error('Recording seek probe budget exhausted');
      const points = scan(await probe(start, end - start), start);
      const key = points.filter((p) => p.key && p.seconds <= seconds).at(-1);
      if (key) return key;
      end = start;
    }
    throw new Error('No nearby keyframe found');
  }
  function destroy() {
    video.pause();
    const current = player; player = null;
    if (!current) return;
    for (const method of ['unload', 'detachMediaElement', 'destroy']) {
      try { current[method](); } catch (error) { console.warn(error); }
    }
  }
  function loaderFor(point) {
    const prefix = point.offset ? file.prefix : new Uint8Array();
    return class extends mpegts.BaseLoader {
      constructor() { super('camera-range-loader'); this._needStash = false; this.controller = null; }
      abort() { this.controller?.abort(); this._status = mpegts.LoaderStatus.kComplete; }
      destroy() { this.abort(); super.destroy(); }
      open(source, range) {
        this.controller = new AbortController();
        const signal = this.controller.signal;
        const total = prefix.length + file.size - point.offset;
        const end = range.to >= 0 ? Math.min(range.to + 1, total) : total;
        this._status = mpegts.LoaderStatus.kBuffering;
        this._onContentLengthKnown?.(total);
        (async () => {
          let at = range.from, received = 0, unbuffered = 0, lastEnd = 0;
          while (at < end && !signal.aborted) {
            const horizon = pendingTime ?? video.currentTime;
            const bufferedEnd = video.buffered.length ? video.buffered.end(video.buffered.length - 1) : 0;
            if (bufferedEnd > lastEnd) { lastEnd = bufferedEnd; unbuffered = 0; }
            if (unbuffered > 32 * 1024 * 1024) throw new Error('No playable video produced within the read budget');
            if (bufferedEnd > horizon + (video.paused ? 2 : 12)) { await new Promise((r) => setTimeout(r, 100)); continue; }
            let bytes;
            if (at < prefix.length) bytes = prefix.slice(at, Math.min(prefix.length, end));
            else bytes = await read(point.offset + at - prefix.length, Math.min(CHUNK, end - at), signal);
            if (signal.aborted) return;
            const offset = at; at += bytes.length; received += bytes.length; unbuffered += bytes.length;
            this._onDataArrival?.(bytes.buffer, offset, received);
            // Yield so MSE appends and cancellation run before another bounded read.
            await new Promise((r) => setTimeout(r, 0));
          }
          if (!signal.aborted) { this._status = mpegts.LoaderStatus.kComplete; this._onComplete?.(range.from, end - 1); }
        })().catch((error) => {
          if (signal.aborted) return;
          this._status = mpegts.LoaderStatus.kError;
          this._onError?.(mpegts.LoaderErrors.EXCEPTION, { code: -1, msg: error.message });
        });
      }
    };
  }
  async function load(seconds, resume) {
    const own = ++generation, previous = player;
    lookup?.abort(); lookup = new AbortController();
    pendingTime = null; pendingPlay = resume; seeking = true; requestedTime = seconds;
    video.pause(); message(root.dataset.loading);
    playButton.disabled = true;
    try {
      const point = await locate(seconds, lookup.signal);
      if (closed || own !== generation) return;
      destroy();
      windowStart = point.seconds;
      pendingTime = Math.max(0, seconds - windowStart);
      const next = mpegts.createPlayer({ type: 'mpegts', isLive: false, url: file.url, hasAudio: false, hasVideo: true },
        { enableWorker: false, customLoader: loaderFor(point), enableStashBuffer: false,
          lazyLoad: false,
          autoCleanupSourceBuffer: true, autoCleanupMaxBackwardDuration: 20, autoCleanupMinBackwardDuration: 10 });
      player = next;
      next.on(mpegts.Events.ERROR, (type, detail, info) => { if (player === next) failure(new Error(`${type}: ${detail}: ${JSON.stringify(info)}`)); });
      next.attachMediaElement(video); next.load();
      playButton.disabled = false;
    } catch (error) {
      if (error.name === 'AbortError' || own !== generation) return;
      if (previous && player === previous) {
        // A failed probe must not discard the existing playable window.
        seeking = false; pendingTime = null; playButton.disabled = false;
        console.warn('Recording seek unavailable:', error); message(root.dataset.seekError);
        if (pendingPlay) startPlayback();
        update();
      } else failure(error);
    }
  }
  function update() {
    const seconds = Math.min(file.duration, seeking ? requestedTime : windowStart + video.currentTime);
    time.textContent = clock(seconds) + ' / ' + clock(file.duration);
    if (!drag) seek.value = seconds;
    playButton.textContent = (seeking ? pendingPlay : !video.paused) ? playButton.dataset.pause : playButton.dataset.play;
  }
  function finishSeek() {
    if (pendingTime === null) return;
    // At EOF the final decoded sample can end before the estimated frame
    // period. Keep an end-of-recording seek inside the playable buffer.
    const target = Number.isFinite(video.duration) && video.duration > 0
      ? Math.min(pendingTime, Math.max(0, video.duration - 0.01)) : pendingTime;
    for (let i = 0; i < video.buffered.length; i++) {
      if (video.buffered.start(i) <= target + 0.05 && video.buffered.end(i) > target) {
        pendingTime = null; seeking = false; video.currentTime = target;
        message(''); if (pendingPlay) startPlayback();
        update(); break;
      }
    }
  }
  for (const event of ['progress', 'durationchange', 'loadeddata']) video.addEventListener(event, finishSeek);
  video.addEventListener('timeupdate', update);
  video.addEventListener('play', update); video.addEventListener('pause', update);
  video.addEventListener('error', () => { if (video.error) failure(video.error); });
  playButton.onclick = () => {
    if (!player) return;
    if (seeking) { pendingPlay = !pendingPlay; update(); return; }
    if (video.paused) startPlayback(); else video.pause();
  };
  seek.oninput = () => { drag = true; time.textContent = clock(Number(seek.value)) + ' / ' + clock(file.duration); };
  seek.onchange = () => {
    const seconds = Math.min(file.duration - 0.01, Number(seek.value)), resume = seeking ? pendingPlay : !video.paused;
    drag = false;
    const local = seconds - windowStart;
    for (let i = 0; !seeking && i < video.buffered.length; i++) {
      if (local >= video.buffered.start(i) && local < video.buffered.end(i)) { video.currentTime = local; message(''); update(); return; }
    }
    load(seconds, resume);
  };
  document.getElementById('recording-fullscreen').onclick = async () => {
    try {
      if (root.requestFullscreen) await root.requestFullscreen();
      else video.webkitEnterFullscreen?.();
    } catch (error) { console.warn('Fullscreen unavailable:', error); }
  };
  root.addEventListener('keydown', (e) => {
    if (e.target.matches('input,button') || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.code === 'Space') { e.preventDefault(); playButton.click(); }
  });
  const close = () => { closed = true; generation++; lookup?.abort(); destroy(); };
  window.addEventListener('pagehide', close);
  window.addEventListener('pageshow', (event) => { if (event.persisted) window.location.reload(); });
  (async () => {
    try {
      if (!window.mpegts?.isSupported()) {
        if (video.canPlayType('video/mp2t')) { video.controls = true; video.src = file.url; document.getElementById('recording-controls').hidden = true; message(''); }
        else message(root.dataset.unavailable);
        return;
      }
      lookup = new AbortController();
      await initialise(lookup.signal);
      if (!closed) { await load(0, false); update(); }
    } catch (error) { if (!closed) failure(error); }
  })();
})();
