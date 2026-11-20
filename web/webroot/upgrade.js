(() => {
  const script = document.currentScript;
  const form = document.getElementById('firmware-upload');
  if (!form || !script) return;
  const input = document.getElementById('firmware');
  const button = document.getElementById('select-firmware');
  const progress = document.getElementById('firmware-progress');
  const status = document.getElementById('firmware-status');
  if (!input || !button || !progress || !status) return;
  const waitTimeout = 120000;
  const pollDelay = 1000;
  const waitForRestart = expectedSlot => {
    const started = Date.now();
    let disconnected = false;
    const poll = () => {
      const elapsed = Date.now() - started;
      progress.value = Math.min(95, 50 + elapsed * 45 / waitTimeout);
      if (elapsed >= waitTimeout) {
        status.textContent = L.timeout;
        alert(L.timeoutAlert);
        return;
      }
      const check = new XMLHttpRequest();
      check.open('GET', (L.applicationUpdate === 'true' ? '/upgrade-result?t=' : '/upgrade-status?t=') + Date.now());
      check.timeout = 2000;
      check.onload = () => {
        if (check.status === 401) {
          progress.hidden = true;
          status.textContent = L.backLogin;
          alert(L.backLogin);
          window.location.replace('/login');
          return;
        }
        if (check.status === 200 && L.applicationUpdate === 'true') {
          let result;
          try { result = JSON.parse(check.responseText); } catch (_) {}
          if (result && result.candidate === expectedSlot) {
            if (result.state === 'rolled-back' || result.state === 'abandoned') {
              status.textContent = L.appFailed;
              alert(L.appFailed);
              window.location.reload();
              return;
            }
            if (result.state === 'confirmed' && result.csrf !== script.dataset.csrf) {
              progress.value = 100;
              status.textContent = L.back;
              window.location.reload();
              return;
            }
          }
          status.textContent = L.uploaded;
          setTimeout(poll, pollDelay);
          return;
        }
        if (check.status === 200 && /^[0-9a-f]{64}$/.test(check.responseText.trim()) && check.responseText.trim() !== script.dataset.csrf) {
          progress.value = 100;
          status.textContent = L.back;
          alert(L.backAlert);
          window.location.reload();
          return;
        }
        status.textContent = disconnected ? L.rebooting : L.waitingUpdater;
        setTimeout(poll, pollDelay);
      };
      check.onerror = check.ontimeout = () => {
        disconnected = true;
        status.textContent = L.rebooting;
        setTimeout(poll, pollDelay);
      };
      check.send();
    };
    poll();
  };
  const rollback = document.getElementById('application-rollback');
  if (rollback && L.applicationUpdate === 'true') rollback.addEventListener('submit', event => {
    event.preventDefault();
    if (!confirm(L.confirmRollback)) return;
    const request = new XMLHttpRequest();
    request.open('POST', '/upgrade/rollback');
    request.setRequestHeader('Content-Type', 'application/x-www-form-urlencoded');
    button.disabled = true; input.disabled = true; progress.hidden = false; progress.value = 50;
    status.textContent = L.uploaded;
    request.onload = () => {
      if (request.status === 201) {
        let result;
        try { result = JSON.parse(request.responseText); } catch (_) {}
        if (result && /^[0-9a-f]{64}$/.test(result.slot)) { waitForRestart(result.slot); return; }
      }
      status.textContent = request.responseText.trim() || (L.httpStatus + request.status);
      button.disabled = false; input.disabled = false;
    };
    request.onerror = () => {status.textContent = L.closed; button.disabled = false; input.disabled = false;};
    request.send('csrf=' + encodeURIComponent(script.dataset.csrf));
  });
  button.addEventListener('click', () => input.click());
  input.addEventListener('change', () => {
    const file = input.files && input.files[0];
    if (!file) return;
    const pattern = new RegExp('^' + L.prefix + '[A-Za-z0-9._-]+' + L.suffix.replace(/\./g, '\\.') + '$');
    if (!(pattern.test(file.name) || (L.installName && file.name === L.installName))) {
      status.textContent = L.badName; return;
    }
    if (file.size < 1 || file.size > (Number(L.maxSize) || 134217728)) {
      status.textContent = L.badSize; return;
    }
    if (!confirm(L.confirmUpload.replace('%s', file.name))) return;
    const request = new XMLHttpRequest();
    request.open('POST', '/upgrade');
    request.setRequestHeader('Content-Type', 'application/octet-stream');
    request.setRequestHeader('X-CSRF-Token', script.dataset.csrf);
    request.setRequestHeader('X-Firmware-Name', file.name);
    request.upload.onprogress = event => {
      if (event.lengthComputable) { progress.hidden = false; progress.value = event.loaded * 50 / event.total; }
      status.textContent = L.writing.replace('%s', L.mediaRoot + '/' + file.name + '.tmp');
    };
    request.onload = () => {
      status.textContent = request.responseText.trim() || (L.httpStatus + request.status);
      if (request.status === 201 && L.applicationUpdate === 'true') {
        let result;
        try { result = JSON.parse(request.responseText); } catch (_) {}
        if (!result || !/^[0-9a-f]{64}$/.test(result.slot)) {
          status.textContent = L.closed;
          button.disabled = false; input.disabled = false;
          return;
        }
        progress.value = 50;
        status.textContent = L.uploaded + (result.thermal ? '' : ' Thermal video is currently unavailable.');
        waitForRestart(result.slot);
      } else if (request.status === 201 && L.reboots === 'true') {
        progress.value = 50;
        status.textContent = L.uploaded;
        waitForRestart();
      } else if (request.status === 201) {
        progress.value = 100;
      } else { button.disabled = false; input.disabled = false; input.value = ''; }
    };
    request.onerror = () => {
      status.textContent = L.closed;
      button.disabled = false; input.disabled = false;
    };
    button.disabled = true; input.disabled = true; progress.hidden = false; progress.value = 0;
    request.send(file);
  });
})();
