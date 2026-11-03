(() => {
  const script = document.currentScript;
  const video = document.getElementById('live-video');
  const stream = document.getElementById('live-stream');
  const status = document.getElementById('live-status');
  const enable = document.getElementById('manual-enable');
  const controlStatus = document.getElementById('control-status');
  const rate = document.getElementById('live-rate');
  const rateValue = document.getElementById('live-rate-value');
  const zoom = document.getElementById('live-zoom');
  const zoomValue = document.getElementById('live-zoom-value');
  const attitudeDial = document.getElementById('attitude-dial');
  const attitudeWorld = document.getElementById('attitude-world');
  const attitudeHeading = document.getElementById('attitude-heading');
  const attitudeRoll = document.getElementById('attitude-roll');
  const attitudePitch = document.getElementById('attitude-pitch');
  const attitudeYaw = document.getElementById('attitude-yaw');
  const attitudeRollRate = document.getElementById('attitude-roll-rate');
  const attitudePitchRate = document.getElementById('attitude-pitch-rate');
  const attitudeYawRate = document.getElementById('attitude-yaw-rate');
  const attitudeStatus = document.getElementById('attitude-status');
  if (!script || !video || !stream || !status || !enable || !controlStatus || !rate || !rateValue || !zoom || !zoomValue || !attitudeDial || !attitudeWorld || !attitudeHeading || !attitudeRoll || !attitudePitch || !attitudeYaw || !attitudeRollRate || !attitudePitchRate || !attitudeYawRate || !attitudeStatus) return;
  let retry = null;
  let startedAt = 0;
  const start = () => {
    if (retry !== null) { window.clearTimeout(retry); retry = null; }
    startedAt = performance.now();
    status.textContent = L.connecting;
    video.src = '/live/video' + (Number(stream.value) + 1) + '.mp4?start=' + Date.now();
    video.load(); video.play().catch(() => {});
  };
  const reconnect = message => {
    status.textContent = message + L.retrying;
    if (retry === null) retry = window.setTimeout(start, 1500);
  };
  video.addEventListener('loadedmetadata', () => { status.textContent = L.livePrefix + video.videoWidth + '×' + video.videoHeight; });
  video.addEventListener('playing', () => { status.textContent = L.livePrefix + video.videoWidth + '×' + video.videoHeight; });
  video.addEventListener('ended', () => reconnect(L.streamEnded));
  video.addEventListener('error', () => {
    const names = ['', L.errorAborted, L.errorNetwork, L.errorDecode, L.errorUnsupported];
    const detail = video.error ? (names[video.error.code] || (L.errorCode + video.error.code)) + (video.error.message ? ': ' + video.error.message : '') : L.unknownError;
    reconnect(L.liveUnavailable + detail);
  });
  // Native players can keep playing an old buffer after an underrun.
  // Reopen at a fresh IDR: seeking a growing HTTP MP4 can fail or decode
  // the old backlog again. Leave an intentional pause alone.
  const catchUp = () => {
    if (video.paused || video.seeking || video.readyState < 2 || !video.buffered.length) return;
    if (performance.now() - startedAt < 5000) return; // let a new decoder settle
    const last = video.buffered.length - 1;
    const end = video.buffered.end(last);
    if (end - video.currentTime > 1.5) start();
  };
  window.setInterval(catchUp, 250);
  try {
    const selected = sessionStorage.getItem('camera.live.stream');
    if ([...stream.options].some(option => option.value === selected)) stream.value = selected;
    const selectedRate = Number(sessionStorage.getItem('camera.live.rate'));
    if (selectedRate >= Number(rate.min) && selectedRate <= Number(rate.max)) {
      rate.value = selectedRate;
      rateValue.textContent = Number(rate.value).toFixed(0) + '°/s';
    }
  } catch (_) {}
  stream.addEventListener('change', () => {
    try { sessionStorage.setItem('camera.live.stream', stream.value); } catch (_) {}
    start();
  }); start();
  let lease = '', held = null, renewing = false, leaving = false;
  let zoomEditing = false, zoomPending = false, zoomWanted = null, zoomGeneration = 0;
  zoomValue.textContent = '—';
  const controls = [...document.querySelectorAll('[data-direction]'), document.getElementById('live-center'), rate, zoom];
  const refreshControls = () => { controls.forEach(control => { control.disabled = !lease; }); };
  enable.checked = false; refreshControls();
  const controlRequest = async (action, token, value) => {
    const body = new URLSearchParams({csrf: script.dataset.csrf, action, lease: token});
    if (value !== undefined) body.set('value', value);
    const controller = new AbortController();
    // The web endpoint waits up to five seconds for the camera. Optical lens
    // motion can also delay a renewal queued behind the zoom operation.
    const timeout = window.setTimeout(() => controller.abort(), 6500);
    try {
      const response = await fetch('/live/control', {method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded'}, body, signal: controller.signal});
      const message = (await response.text()).trim();
      if (!response.ok) throw new Error(message || ('HTTP ' + response.status));
      return message;
    } finally { window.clearTimeout(timeout); }
  };
  const dropControl = () => {
    const previous = lease; lease = ''; held = null; enable.checked = false; refreshControls();
    if (previous) fetch('/live/control', {method: 'POST', keepalive: true,
      body: new URLSearchParams({csrf: script.dataset.csrf, action: 'release', lease: previous})}).catch(() => {});
  };
  enable.addEventListener('change', async () => {
    enable.disabled = true;
    try {
      if (enable.checked) {
        const token = await controlRequest('acquire', '');
        if (!/^[0-9a-f]{32}$/.test(token)) throw new Error(L.unknownError);
        lease = token;
        if (leaving) dropControl();
        else controlStatus.textContent = L.manualEnabled;
      } else {
        const previous = lease; lease = ''; held = null; refreshControls();
        if (previous) await controlRequest('release', previous);
        controlStatus.textContent = L.manualDisabled;
      }
    } catch (error) {
      dropControl(); controlStatus.textContent = L.controlFailed + error.message;
    } finally { enable.checked = !!lease; enable.disabled = false; refreshControls(); }
  });
  window.setInterval(async () => {
    if (!lease || renewing) return;
    const previous = lease; renewing = true;
    try { await controlRequest('renew', previous); }
    catch (error) { if (lease === previous) { dropControl(); controlStatus.textContent = L.controlFailed + error.message; } }
    finally { renewing = false; }
  }, 1000);
  window.addEventListener('pagehide', () => { leaving = true; dropControl(); });
  window.addEventListener('pageshow', () => { leaving = false; });
  const sendControl = async (action, value) => {
    if (!lease) { controlStatus.textContent = L.enableFirst; return false; }
    const previous = lease;
    try {
      const message = await controlRequest(action, previous, value);
      controlStatus.textContent = message || L.commandSent;
      // Refresh before the 180 ms manual pulse and 150 ms native watchdog.
      // Keep requests serialized; a slow or throttled browser stops safely.
      await new Promise(resolve => window.setTimeout(resolve, 50));
      return lease === previous;
    } catch (error) {
      // A rejected zoom/command is not a lost lease. Stop any held direction;
      // the independent renewal detects expiry, restart or disconnection.
      held = null;
      controlStatus.textContent = L.controlFailed + error.message; return false;
    }
  };
  const release = () => { held = null; };
  document.querySelectorAll('[data-direction]').forEach(button => button.addEventListener('pointerdown', async event => {
    event.preventDefault(); if (!enable.checked || held) return; held = button.dataset.direction; button.setPointerCapture(event.pointerId);
    const direction = held; while (held === direction && await sendControl(direction, Number(rate.value).toFixed(0))) {}
  }));
  window.addEventListener('pointerup', release); window.addEventListener('pointercancel', release); window.addEventListener('blur', release);
  document.getElementById('live-center').addEventListener('click', () => sendControl('center'));
  rate.addEventListener('input', () => {
    rateValue.textContent = Number(rate.value).toFixed(0) + '°/s';
    try { sessionStorage.setItem('camera.live.rate', rate.value); } catch (_) {}
  });
  zoom.addEventListener('pointerdown', () => { zoomEditing = true; });
  window.addEventListener('pointerup', () => { zoomEditing = false; });
  window.addEventListener('pointercancel', () => { zoomEditing = false; });
  zoom.addEventListener('blur', () => { zoomEditing = false; });
  zoom.addEventListener('input', () => {
    ++zoomGeneration;
    zoomValue.textContent = Number(zoom.value).toFixed(1) + 'x';
  });
  zoom.addEventListener('change', async () => {
    ++zoomGeneration;
    zoomWanted = Number(zoom.value).toFixed(1);
    if (zoomPending) return;
    zoomPending = true;
    try {
      // Coalesce quick keyboard/slider changes while a physical move is busy.
      while (zoomWanted !== null && lease) {
        const wanted = zoomWanted; zoomWanted = null;
        if (!await sendControl('zoom', wanted)) { zoomWanted = null; break; }
      }
    } finally {
      // Invalidate polls started during the move as well as those started
      // before it: a delayed response may arrive after zoomPending clears.
      ++zoomGeneration;
      zoomPending = false;
    }
  });
  let attitudePending = false;
  const attitudeUnavailable = message => {
    attitudeRoll.textContent = attitudePitch.textContent = attitudeYaw.textContent = '—';
    attitudeRollRate.textContent = attitudePitchRate.textContent = attitudeYawRate.textContent = '—';
    attitudeWorld.style.transform = 'translateY(0) rotate(0)';
    attitudeHeading.style.transform = 'rotate(0)';
    attitudeDial.setAttribute('aria-label', L.attitudeUnavailable);
    attitudeStatus.textContent = message;
  };
  const refreshAttitude = async () => {
    if (attitudePending) return; attitudePending = true;
    const generation = zoomGeneration;
    try {
      const response = await fetch('/live/attitude.json', {cache: 'no-store'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const data = await response.json();
      if (data.zoom !== null && Number.isFinite(data.zoom) && data.zoom >= 1 &&
          generation === zoomGeneration && !zoomEditing && !zoomPending) {
        // The encoder may report a quantized optical ratio just beyond the
        // command limit. Show the actual ratio and clamp only the slider thumb.
        zoom.value = Math.max(Number(zoom.min), Math.min(Number(zoom.max), data.zoom));
        zoomValue.textContent = data.zoom.toFixed(1) + 'x';
      }
      if (data.roll_deg === null || data.pitch_deg === null || data.yaw_deg === null) { attitudeUnavailable(L.attitudeUnavailable); return; }
      const roll = Number(data.roll_deg), pitch = Number(data.pitch_deg), yaw = Number(data.yaw_deg);
      attitudeRoll.textContent = roll.toFixed(1) + '°'; attitudePitch.textContent = pitch.toFixed(1) + '°'; attitudeYaw.textContent = yaw.toFixed(1) + '°';
      attitudeRollRate.textContent = data.roll_rate_dps === null ? '—' : Number(data.roll_rate_dps).toFixed(1) + '°/s';
      attitudePitchRate.textContent = data.pitch_rate_dps === null ? '—' : Number(data.pitch_rate_dps).toFixed(1) + '°/s';
      attitudeYawRate.textContent = data.yaw_rate_dps === null ? '—' : Number(data.yaw_rate_dps).toFixed(1) + '°/s';
      const pitchPixels = Math.max(-55, Math.min(55, pitch * 1.15));
      attitudeWorld.style.transform = 'translateY(' + pitchPixels + 'px) rotate(' + (-roll) + 'deg)';
      attitudeHeading.style.transform = 'rotate(' + yaw + 'deg)';
      attitudeDial.setAttribute('aria-label', L.attitudeLabel.replace('%s', roll.toFixed(1)).replace('%s', pitch.toFixed(1)).replace('%s', yaw.toFixed(1)));
      attitudeStatus.textContent = L.liveAttitude;
    } catch (error) { attitudeUnavailable(L.attitudeFailed + (error && error.message ? error.message : L.unknownError)); }
    finally { attitudePending = false; }
  };
  refreshAttitude(); window.setInterval(refreshAttitude, 250);
})();
