(() => {
  const form = document.querySelector('form[action="/parameters"]');
  if (!form) return;
  const tablist = document.getElementById('parameter-tabs');
  const tabs = [...tablist.querySelectorAll('[role=tab]')];
  const panels = tabs.map(tab => document.getElementById(tab.getAttribute('aria-controls')));
  function selectTab(tab, focus = false) {
    if (!tabs.includes(tab)) tab = tabs[0];
    tabs.forEach((item, i) => {
      const selected = item === tab;
      item.setAttribute('aria-selected', String(selected));
      item.tabIndex = selected ? 0 : -1;
      panels[i].hidden = !selected;
    });
    const name = tab.id.slice(4);
    form.setAttribute('action', '/parameters#' + name);
    try { history.replaceState(null, '', '#' + name); } catch (_) {}
    if (focus) tab.focus();
  }
  function reveal(field) {
    const panel = field.closest('.parameter-panel');
    selectTab(tabs[panels.indexOf(panel)]);
  }
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => selectTab(tab));
    tab.addEventListener('keydown', event => {
      let next;
      if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
      else if (event.key === 'ArrowLeft') next = (index + tabs.length - 1) % tabs.length;
      else if (event.key === 'Home') next = 0;
      else if (event.key === 'End') next = tabs.length - 1;
      else return;
      event.preventDefault(); selectTab(tabs[next], true);
    });
  });
  panels.forEach(panel => panel.setAttribute('role', 'tabpanel'));
  tablist.hidden = false;
  const fromHash = () => selectTab(tabs.find(tab => tab.id === 'tab-' + location.hash.slice(1)));
  fromHash();
  window.addEventListener('hashchange', fromHash);
  const fields = [...form.querySelectorAll('.field input, .field select')];
  const touched = new Set();
  let attempted = Boolean(document.querySelector('.notice.error'));
  const get = name => form.elements.namedItem(name);
  const value = name => get(name)?.value || '';
  for (const field of fields) {
    const error = document.createElement('div');
    error.id = field.id + '-error';
    error.className = 'field-error';
    error.hidden = true;
    field.setAttribute('aria-describedby', error.id);
    field.closest('.field').append(error);
  }
  const ipv4 = text => {
    const parts = text.split('.');
    return parts.length === 4 && parts.every(p => /^(0|[1-9][0-9]{0,2})$/.test(p) && Number(p) <= 255);
  };
  // Keep in step with apcam_video_uri_valid() in include/apcam/network.h.
  const videoUri = text => {
    const m = /^rtsps?:\/\/[a-zA-Z0-9._-]+(?::([0-9]{1,5}))?(?:\/.*)?$/.exec(text);
    return Boolean(m) && text.length < 160 && !/[^\x21-\x7e]|"/.test(text) &&
      (!m[1] || (Number(m[1]) >= 1 && Number(m[1]) <= 65535));
  };
  // Keep in step with apcam_uart_device_override_valid() in include/apcam/network.h.
  const uartDeviceOverride = text => text === '' || (text.length < 160 && (
    (/^\/dev\/[a-zA-Z0-9._\/-]+$/.test(text) && !text.includes('..')) ||
    (m => Boolean(m) && (m[1] === 'localhost' || ipv4(m[1])) && Number(m[2]) >= 1 && Number(m[2]) <= 65535)(/^(?:udp|tcp):\/\/([^:]+):([0-9]{1,5})$/.exec(text))));
  const number = text => text.split('.').reduce((v, n) => ((v << 8) | Number(n)) >>> 0, 0);
  const host = text => ipv4(text) && Number(text.split('.')[0]) > 0 && Number(text.split('.')[0]) !== 127 && Number(text.split('.')[0]) < 224;
  function invalid(field, message) {
    if (field) field.setCustomValidity(message || L.invalid.replace('%s', field.closest('.field').querySelector('label').textContent));
  }
  function validate() {
    for (const field of fields) {
      field.setCustomValidity('');
      if (field.name.startsWith('proxy_') && ['text', 'password'].includes(field.type)) {
        if (/[^\x20-\x7e]|"/.test(field.value) || field.value.length > field.maxLength) invalid(field);
      }
      if (field.name === 'timezone' && /[\s\x00-\x1f\x7f]/.test(field.value)) invalid(field);
    }
    for (const name of ['proxy_host', 'network_interface', 'main_alias', 'sub_alias']) {
      if (!/^[a-zA-Z0-9_.-]*$/.test(value(name))) invalid(get(name));
    }
    if (get('network_uart_device_override') && !uartDeviceOverride(value('network_uart_device_override'))) invalid(get('network_uart_device_override'));
    if (value('network_video1_uri') && !videoUri(value('network_video1_uri'))) invalid(get('network_video1_uri'));
    const parseAddress = text => {
      const parts = text.split('/');
      if (parts.length !== 2 || !host(parts[0]) || !/^([1-9]|[12][0-9]|3[0-2])$/.test(parts[1])) return null;
      const ip = number(parts[0]), prefix = Number(parts[1]), mask = (0xffffffff << (32 - prefix)) >>> 0;
      if (prefix < 31 && ((ip & ~mask) === 0 || (ip & ~mask) === (~mask >>> 0))) return null;
      return {ip, mask, prefix};
    };
    for (const name of ['network_primary_address', 'network_secondary_address']) {
      if (value(name) && !parseAddress(value(name))) invalid(get(name), L.address);
    }
    const primary = parseAddress(value('network_primary_address'));
    const secondary = parseAddress(value('network_secondary_address'));
    const gateway = value('network_gateway');
    if (primary && secondary && primary.ip === secondary.ip) invalid(get('network_secondary_address'), L.network);
    if (gateway) {
      if (!host(gateway)) invalid(get('network_gateway'));
      else {
        const g = number(gateway);
        const reachable = address => address && !((g ^ address.ip) & address.mask) &&
          (address.prefix >= 31 || ((g & ~address.mask) !== 0 && (g & ~address.mask) !== (~address.mask >>> 0)));
        if ((primary && g === primary.ip) || (secondary && g === secondary.ip) ||
            (primary && !reachable(primary) && !reachable(secondary)))
          invalid(get('network_gateway'), L.network);
      }
    }
    const reconnect = document.getElementById('network-reconnect');
    reconnect.hidden = !primary || reconnect.dataset.sitl === 'true';
    if (primary) {
      const link = document.getElementById('network-link');
      const url = new URL(location.href);
      url.hostname = value('network_primary_address').split('/')[0];
      url.pathname = '/parameters'; url.search = ''; url.hash = 'network';
      link.href = url.href; link.textContent = url.href;
    }
    if (value('proxy_enabled') === 'true') {
      if (!value('proxy_host')) invalid(get('proxy_host'));
      if (value('proxy_signing') === 'true' && !value('proxy_signing_passphrase')) invalid(get('proxy_signing_passphrase'));
      const usedPorts = new Set();
      for (const n of [1, 2, 3]) {
        if (!get('proxy_video' + n + '_port')) continue;
        const port = Number(value('proxy_video' + n + '_port'));
        if (port && usedPorts.has(port)) invalid(get('proxy_video' + n + '_port'), L.ports);
        if (port) usedPorts.add(port);
        if (Number(value('proxy_video' + n + '_port')) && !value('proxy_video' + n + '_name')) invalid(get('proxy_video' + n + '_name'));
      }
    }
    for (const field of fields) {
      const bad = !field.validity.valid && (attempted || touched.has(field));
      const error = document.getElementById(field.id + '-error');
      field.setAttribute('aria-invalid', String(bad));
      error.hidden = !bad;
      error.textContent = bad ? field.validationMessage : '';
    }
    tabs.forEach((tab, i) => tab.classList.toggle('has-error', Boolean(panels[i].querySelector('[aria-invalid=true]'))));
    return fields.find(field => !field.validity.valid);
  }
  for (const name of ['input', 'change']) form.addEventListener(name, event => {
    touched.add(event.target);
    validate();
  });
  form.addEventListener('submit', event => {
    if (event.submitter && event.submitter.name === 'survey_mode') return;
    attempted = true;
    const bad = validate();
    if (bad) {
      event.preventDefault();
      reveal(bad); bad.focus();
      bad.reportValidity();
      return;
    }
    const reconnect = document.getElementById('network-reconnect');
    const link = document.getElementById('network-link');
    if (event.submitter && event.submitter.value === 'save_restart' && !reconnect.hidden &&
        new URL(link.href).hostname !== location.hostname) {
      // Keep the reconnect link visible if removing our address breaks the response.
      event.preventDefault();
      reveal(get('network_primary_address'));
      const body = new URLSearchParams(new FormData(form));
      body.set('action', 'save_restart');
      const buttons = [...form.querySelectorAll('button[type=submit]')];
      buttons.forEach(button => { button.disabled = true; });
      // Named submit buttons shadow form.action; read the HTML attribute.
      const endpoint = form.getAttribute('action').split('#')[0];
      fetch(endpoint, {method: 'POST', body, signal: AbortSignal.timeout(45000)})
        .then(response => response.text()).then(page => {
          document.open(); document.write(page); document.close();
        }).catch(() => {
          const notice = document.createElement('p');
          notice.className = 'notice error'; notice.textContent = L.connectionLost;
          reconnect.before(notice);
          buttons.forEach(button => { button.disabled = false; });
        });
    }
  });
  // Run our checks before native submission so dependent fields are checked too.
  form.noValidate = true;
  const bad = validate();
  if (attempted && bad) { reveal(bad); bad.focus(); }
  const captureStatus = document.getElementById('network-capture-status');
  async function refreshCapture() {
    if (captureStatus && !document.getElementById('parameters-network').hidden && !document.hidden) {
      try {
        const response = await fetch('/network-capture-status', {
          cache: 'no-store', headers: {Accept: 'application/json'}, signal: AbortSignal.timeout(5000)
        });
        if (response.status === 401) { captureStatus.hidden = true; return; }
        if (!response.ok) throw new Error('capture status unavailable');
        const status = await response.json();
        captureStatus.hidden = !status.available;
        captureStatus.classList.toggle('error', status.error);
        captureStatus.querySelector('span').textContent = status.message;
      } catch (_) {
        captureStatus.hidden = true;
      }
    }
    if (captureStatus) setTimeout(refreshCapture, 2000);
  }
  refreshCapture();
})();

(() => {
  const status = document.getElementById('survey-status');
  if (!status) return;
  async function poll() {
    try {
      const response = await fetch('/survey/status', {cache: 'no-store'});
      if (!response.ok) throw new Error('Status unavailable');
      const s = await response.json();
      status.textContent = `${s.state}\nCaptured: ${s.captured || 0}  Skipped: ${s.skipped || 0}  Unsettled: ${s.unsettled || 0}\nLeg: ${s.leg || 0}  Cycle: ${s.cycle || 0}  Position: ${s.slot || 0}/${s.positions || 9}\nSweep: ${s.cycle_seconds || 0}s  Row spacing: ${s.row_spacing_m || 0}m\nNominal along-track overlap: ${s.nominal_overlap_pct || 0}%  Path: ${s.path_source || "ground track"}  Terrain: ${s.terrain_source || "flight controller"}  Report age: ${s.terrain_age_ms || 0}ms`;
    } catch (e) { status.textContent = e.message; }
    setTimeout(poll, 1000);
  }
  poll();
})();

(() => {
  const button = document.getElementById('terrain-upload');
  if (!button) return;
  button.addEventListener('click', () => {
    const file = document.getElementById('terrain-file').files[0];
    const status = document.getElementById('terrain-upload-status');
    if (!file) { status.textContent = 'Choose a terrain ZIP first.'; return; }
    if (file.size > 512 * 1024 * 1024) { status.textContent = 'ZIP exceeds 512 MiB.'; return; }
    const request = new XMLHttpRequest();
    request.open('POST', '/survey/terrain');
    request.setRequestHeader('Content-Type', 'application/octet-stream');
    request.setRequestHeader('X-CSRF-Token', document.querySelector('input[name=csrf]').value);
    button.disabled = true;
    request.upload.onprogress = event => {
      if (event.lengthComputable) status.textContent = `Uploading terrain: ${Math.round(100 * event.loaded / event.total)}%`;
    };
    request.upload.onload = () => { status.textContent = 'Validating and installing terrain on SD card…'; };
    request.onload = () => { status.textContent = request.responseText; };
    request.onerror = () => { status.textContent = 'Terrain upload connection failed.'; };
    request.onloadend = () => { button.disabled = false; };
    status.textContent = 'Uploading terrain…';
    // Send the File directly; never read it into a browser-sized ArrayBuffer.
    request.send(file);
  });
})();

(() => {
  const pattern = document.getElementById('survey_pattern');
  if (!pattern) return;
  const descriptions = {
    both: 'Front-left → front-centre → front-right → down-right → down-centre → down-left → rear-left → rear-centre → rear-right.',
    left_right: 'Left → right, repeating. No centre or fore/aft views. Forward and rear distance settings are unused.',
    fore_aft: 'Front-centre → rear-centre, repeating. No centre or lateral views.',
    fore_only: 'Straight ahead: far → middle → near, at 3×, 2× and 1× forward distance, rounded to ground rows. Three pitch angles with no lateral sweep. Rear distance is unused.'
  };
  function update() {
    document.getElementById('survey-pattern-description').textContent = descriptions[pattern.value];
    document.getElementById('survey-pattern-grid').hidden = pattern.value !== 'both';
  }
  pattern.addEventListener('change', update);
  update();
})();
