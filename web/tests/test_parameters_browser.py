"""Check the rendered parameter form in an isolated headless Chrome profile."""
import html
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


CHECKS = r"""
(() => {
  const result = document.createElement('pre');
  result.id = 'browser-test-result';
  document.body.append(result);
  try {
    const form = document.querySelector('form[action^="/parameters"]');
    const field = name => form.elements.namedItem(name);
    const original = Object.fromEntries(new FormData(form));
    const assert = (ok, message) => { if (!ok) throw new Error(message); };
    const tabs = [...document.querySelectorAll('#parameter-tabs [role=tab]')];
    const panels = [...form.querySelectorAll('.parameter-panel')];
    assert(tabs.length === 6 && panels.length === 6, 'six parameter tabs');
    const active = () => tabs.find(tab => tab.getAttribute('aria-selected') === 'true');
    const switchTo = name => document.getElementById('tab-' + name).click();
    assert(active().id === 'tab-system', 'default System tab');
    assert(panels.filter(panel => !panel.hidden).length === 1, 'one visible panel');
    const categories = {timezone: 'system', orientation: 'system', mavlink_system_id: 'system',
      network_vpn_enabled: 'network', network_vpn_provider: 'network', network_vpn_network_id: 'network', network_capture: 'network', mavlink_tcp_port: 'network', network_primary_address: 'network', network_secondary_address: 'network',
      brightness: 'video', autorecord: 'video', main_resolution: 'video',
      proxy_host: 'supportproxy', proxy_video1_port: 'supportproxy', survey_lens: 'survey', survey_overlap: 'survey'};
    for (const [name, category] of Object.entries(categories)) {
      assert(field(name).closest('.parameter-panel').id === 'parameters-' + category, 'wrong category: ' + name);
    }
    switchTo('survey');
    const pattern = field('survey_pattern');
    assert(pattern.value === 'both', 'default survey pattern');
    assert([...pattern.options].map(o => o.value).join(',') === 'both,left_right,fore_aft,fore_only,fore_wide', 'survey pattern choices');
    for (const value of ['left_right','fore_aft','fore_only','fore_wide','both']) {
      pattern.value = value;
      pattern.dispatchEvent(new Event('change', {bubbles: true}));
      assert(document.getElementById('survey-pattern-grid').hidden === (value !== 'both'), 'pattern grid visibility');
      assert(document.getElementById('survey-pattern-description').textContent.length > 10, 'pattern description');
    }
    const uploadButton = document.getElementById('terrain-upload');
    assert(uploadButton && uploadButton.type === 'button', 'terrain upload is independent of parameter save');
    uploadButton.click();
    assert(document.getElementById('terrain-upload-status').textContent.includes('Choose'), 'missing terrain file prompt');
    const transfer = new DataTransfer();
    const terrainFile = new File(['test ZIP'], 'terrain.zip');
    transfer.items.add(terrainFile);
    document.getElementById('terrain-file').files = transfer.files;
    const originalXHR = window.XMLHttpRequest;
    let sent;
    window.XMLHttpRequest = class {
      constructor() { this.upload = {}; this.headers = {}; }
      open(method, path) { assert(method === 'POST' && path === '/survey/terrain', 'terrain upload route'); }
      setRequestHeader(name, value) { this.headers[name] = value; }
      send(body) {
        sent = body;
        assert(this.headers['X-CSRF-Token'] === form.elements.namedItem('csrf').value, 'terrain CSRF token');
        this.responseText = 'Terrain installed'; this.onload(); this.onloadend();
      }
    };
    uploadButton.click();
    window.XMLHttpRequest = originalXHR;
    assert(sent instanceof File && sent.name === 'terrain.zip', 'terrain file must be sent directly');
    assert(!uploadButton.disabled && document.getElementById('terrain-upload-status').textContent === 'Terrain installed', 'terrain completion');
    switchTo('system');
    field('brightness').value = '63';
    for (const name of ['network', 'video', 'supportproxy', 'system']) switchTo(name);
    assert(field('brightness').value === '63', 'tab change discarded edits');
    assert(new FormData(form).get('brightness') === '63', 'hidden fields omitted from save');
    field('brightness').value = original.brightness;
    active().dispatchEvent(new KeyboardEvent('keydown', {key: 'ArrowRight', bubbles: true}));
    assert(active().id === 'tab-network' && document.activeElement === active(), 'arrow navigation');
    active().dispatchEvent(new KeyboardEvent('keydown', {key: 'End', bubbles: true}));
    assert(active().id === 'tab-upload', 'End navigation');
    active().dispatchEvent(new KeyboardEvent('keydown', {key: 'Home', bubbles: true}));
    assert(active().id === 'tab-system', 'Home navigation');
    let blocked;
    // Observe whether the application blocked submission, then prevent actual
    // navigation for valid cases too so this test never writes configuration.
    form.addEventListener('submit', event => {
      blocked = event.defaultPrevented;
      event.preventDefault();
    });
    function submit() {
      blocked = undefined;
      form.requestSubmit(form.querySelector('.actions button[value=save]'));
      assert(blocked !== undefined, 'submit handler did not run');
    }
    function reset() {
      for (const [name, value] of Object.entries(original)) field(name).value = value;
      form.dispatchEvent(new Event('input', {bubbles: true}));
    }
    function rejected(changes, badName) {
      reset();
      for (const [name, value] of Object.entries(changes)) field(name).value = value;
      const before = JSON.stringify([...new FormData(form)]);
      submit();
      assert(blocked, 'invalid form submitted: ' + badName);
      assert(!field(badName).closest('.parameter-panel').hidden, 'invalid tab still hidden');
      assert(active().classList.contains('has-error'), 'invalid tab not marked');
      assert(document.activeElement === field(badName), 'wrong focused field: ' + badName);
      assert(field(badName).getAttribute('aria-invalid') === 'true', 'missing highlight: ' + badName);
      const error = document.getElementById(badName + '-error');
      assert(!error.hidden && error.textContent, 'missing inline error: ' + badName);
      assert(JSON.stringify([...new FormData(form)]) === before, 'entered values lost');
    }
    submit();
    assert(blocked === false, 'valid configuration rejected');
    assert(field('network_vpn_enabled').value === 'false', 'VPN defaults off');
    rejected({network_vpn_enabled: 'true', network_vpn_network_id: ''}, 'network_vpn_network_id');
    rejected({network_vpn_network_id: 'abfd31bd474e3f2z'}, 'network_vpn_network_id');
    rejected({network_vpn_network_id: '0000000000000000'}, 'network_vpn_network_id');
    reset();
    field('network_vpn_enabled').value = 'true';
    field('network_vpn_network_id').value = '0123456789abcdef';
    submit(); assert(blocked === false, 'valid VPN network rejected');
    reset();
    rejected({network_secondary_address: '192.168.2.97'}, 'network_secondary_address');
    assert(document.getElementById('network_secondary_address-error').textContent.includes('/24'), 'missing prefix guidance');
    field('network_secondary_address').value = '192.168.2.97/24';
    field('network_secondary_address').dispatchEvent(new Event('input', {bubbles: true}));
    assert(field('network_secondary_address').getAttribute('aria-invalid') === 'false', 'correction still highlighted');
    submit();
    assert(blocked === false, 'corrected prefix rejected');
    rejected({network_secondary_address: '192.168.2.999/24'}, 'network_secondary_address');
    rejected({network_secondary_address: '192.168.2.97/33'}, 'network_secondary_address');
    rejected({network_gateway: '192.168.2.999'}, 'network_gateway');
    rejected({network_primary_address: '192.0.2.25'}, 'network_primary_address');
    rejected({network_primary_address: '192.0.2.0/24'}, 'network_primary_address');
    rejected({network_primary_address: '192.0.2.255/24'}, 'network_primary_address');
    rejected({network_primary_address: '127.0.0.1/8'}, 'network_primary_address');
    rejected({network_primary_address: original.network_secondary_address}, 'network_secondary_address');
    rejected({network_primary_address: '198.51.100.27/24', network_gateway: '203.0.113.1'}, 'network_gateway');
    reset();
    field('network_primary_address').value = '198.51.100.27/24';
    field('proxy_enabled').value = 'false';
    submit();
    assert(blocked === false, 'network configuration requires SupportProxy');
    assert(!document.getElementById('network-reconnect').hidden, 'reconnect instructions missing');
    assert(document.getElementById('network-link').href.includes('198.51.100.27'), 'reconnect link has wrong address');
    rejected({proxy_host: ''}, 'proxy_host');
    rejected({proxy_host: 'rtsp://localhost'}, 'proxy_host');
    rejected({proxy_signing_passphrase: ''}, 'proxy_signing_passphrase');
    rejected({proxy_video2_port: original.proxy_video1_port}, 'proxy_video2_port');
    rejected({proxy_video1_name: ''}, 'proxy_video1_name');
    rejected({main_alias: 'live/main'}, 'main_alias');
    rejected({sub_alias: 'sub 264'}, 'sub_alias');
    rejected({proxy_publish_password: 'bad"password'}, 'proxy_publish_password');
    rejected({mavlink_system_id: '256'}, 'mavlink_system_id');
    rejected({mavlink_system_id: '1.5'}, 'mavlink_system_id');
    rejected({brightness: '101'}, 'brightness');
    rejected({survey_overlap: '91'}, 'survey_overlap');
    rejected({survey_burst: '0'}, 'survey_burst');
    reset();
    field('proxy_enabled').value = 'false';
    field('proxy_host').value = '';
    field('proxy_signing_passphrase').value = '';
    field('network_secondary_address').value = '';
    field('network_gateway').value = '';
    submit();
    assert(blocked === false, 'disabled proxy requires optional fields');
    field('network_primary_address').value = '198.51.100.27/24';
    let restartRequest;
    window.fetch = (url, options) => {
      restartRequest = {url, options};
      return new Promise(() => {}); // Keep the request pending, without any network access.
    };
    form.requestSubmit(form.querySelector('button[value=save_restart]'));
    assert(blocked === true && restartRequest, 'restart navigated away from reconnect instructions');
    assert(restartRequest.url === '/parameters', 'restart posted to wrong endpoint: ' + restartRequest.url);
    assert(restartRequest.options.method === 'POST', 'restart used wrong HTTP method');
    assert(!field('network_primary_address').hasAttribute('placeholder'), 'primary example looks like a configured value');
    assert(!field('network_secondary_address').hasAttribute('placeholder'), 'secondary example looks like a configured value');
    assert(restartRequest.options.body.get('action') === 'save_restart', 'restart action missing');
    assert(restartRequest.options.body.get('network_primary_address') === '198.51.100.27/24', 'new address missing');
    assert(!document.getElementById('network-reconnect').hidden, 'reconnect link lost during request');
    result.textContent = 'PASS parameter tabs, keyboard navigation, cross-tab validation and input preservation';
  } catch (error) {
    result.textContent = 'FAIL ' + error.stack;
  }
})();
"""


def check_parameters_browser(page, script):
    browser = shutil.which('google-chrome') or shutil.which('chromium')
    if browser is None:
        print('SKIP parameter browser checks: Chrome/Chromium not installed')
        return
    # Chrome helpers can briefly retain profile files after --dump-dom exits.
    with tempfile.TemporaryDirectory(prefix='camera-parameters-browser-', ignore_cleanup_errors=True) as temp:
        root = Path(temp)
        # Use the actual server-rendered form and translated validation script,
        # but remove unrelated network scripts and never submit to the camera.
        document = re.sub(r'<script\b[^>]*>.*?</script>', '', page.decode(), flags=re.S)
        document = document.replace('</body>', '<script>' + script.decode() +
                                    '</script><script>' + CHECKS + '</script></body>')
        fixture = root / 'form.html'
        fixture.write_text(document)
        run = subprocess.run([browser, '--headless', '--no-sandbox', '--disable-gpu',
                              '--disable-dev-shm-usage', '--no-first-run',
                              '--disable-background-networking', '--disable-component-update',
                              '--disable-extensions', '--timeout=10000',
                              '--user-data-dir=' + str(root / 'profile'),
                              '--dump-dom', fixture.as_uri()],
                             capture_output=True, timeout=30)
        match = re.search(r'<pre id="browser-test-result">(.*?)</pre>',
                          run.stdout.decode(), re.S)
        assert run.returncode == 0 and match, run.stderr.decode()[-2000:]
        result = html.unescape(match.group(1))
        assert result.startswith('PASS '), result
        print(result)
