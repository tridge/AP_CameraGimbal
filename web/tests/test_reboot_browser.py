"""Run the served reboot monitor with deterministic network and timer events."""
import html
import http.server
from pathlib import Path
import json
import re
import subprocess
import shutil
import tempfile
import threading


def check_reboot_browser(page, script, policy, auth_headers):
    document = page.decode()
    match = re.search(r'<script id=reboot-monitor src=/reboot.js data-csrf="([0-9a-f]{64})" defer></script>', document, re.S)
    assert match, 'missing reboot monitor/token'
    tag = re.search(r'<p id=reboot-status\b[^>]*>', document).group(0)
    messages = {key: html.unescape(value) for key, value in re.findall(r'data-(\w+)="([^"]*)"', tag)}
    assert all(messages[key] for key in ('wait', 'back', 'login', 'timeout'))
    assert '<a id=reboot-home href=/' in document
    assert '<form' not in document
    harness = r'''
const assert = require('assert');
const vm = require('vm');
const fixture = JSON.parse(require('fs').readFileSync(0, 'utf8'));
function run(events, expected) {
  const requests = [], timers = [], navigation = [];
  let now = 0;
  const status = {dataset: fixture.messages, textContent: fixture.messages.wait};
  const heading = {};
  const home = {href: '/', hidden: false};
  const context = {
    window: {location: {replace: path => navigation.push(path)}},
    document: {
      currentScript: {dataset: {csrf: fixture.token}},
      getElementById: id => ({'reboot-status': status, 'reboot-heading': heading, 'reboot-home': home})[id],
    },
    Date: {now: () => now},
    setTimeout: (fn, delay) => timers.push({fn, delay}),
    XMLHttpRequest: function() {
      this.open = (method, path) => {
        assert.equal(method, 'GET');
        assert.ok(path.startsWith('/upgrade-status?t='));
      };
      this.send = () => {assert.equal(this.timeout, 2000); requests.push(this);};
    },
  };
  vm.runInNewContext(fixture.script, context);
  for (const [index, event] of events.entries()) {
    assert.equal(requests.length, 1, 'requests must not overlap');
    const request = requests.shift();
    if (event === 'error') request.onerror();
    else if (event === 'timeout') request.ontimeout();
    else {
      request.status = event[0];
      request.responseText = event[1];
      request.onload();
    }
    assert.equal(home.href, '/');
    assert.equal(home.hidden, false);
    if (index < events.length - 1 || expected === 'timeout') {
      assert.equal(status.textContent, fixture.messages.wait);
      assert.deepEqual(navigation, [], "must not navigate before the server returns");
      assert.equal(timers.length, 1);
      const timer = timers.shift();
      assert.equal(timer.delay, 1000);
      now += expected === 'timeout' ? 120000 : timer.delay;
      timer.fn();
    }
  }
  assert.equal(status.textContent, fixture.messages[expected]);
  assert.deepEqual(navigation, expected === 'timeout' ? [] : ['/']);
  assert.equal(requests.length, 0);
  assert.equal(timers.length, 0, 'monitor must stop after recovery or timeout');
}
const changed = (fixture.token[0] === 'a' ? 'b' : 'a').repeat(64);
// The old server, transient HTTP errors and invalid responses are not recovery.
run([[200, fixture.token], [503, 'busy'], 'error', 'timeout',
     [200, '<html>login</html>'], [200, changed]], 'back');
// Cookie sessions expire on reboot; Basic authentication sees the new token.
run([[200, fixture.token], 'error', [401, 'login required']], 'login');
run([[200, changed]], 'back'); // A quick restart need not produce a failed poll.
// A firmware flash taking 90 seconds must still recover automatically.
run([...Array(90).fill([200, fixture.token]), [200, changed]], 'back');
run(['timeout'], 'timeout');
run([[200, fixture.token]], 'timeout'); // No reboot occurred (e.g. SITL).
'''
    subprocess.run(['node', '-e', harness], input=json.dumps(dict(
        token=match.group(1), script=script.decode(), messages=messages)), text=True, check=True)
    print('PASS reboot monitor: old server, disconnects, restart, login expiry and timeout')

    check_reboot_navigation(page, script, policy, match.group(1), auth_headers)


def check_reboot_navigation(page, script, policy, token, auth_headers):
    """Use real HTTP/CSP and browser script loading, not extracted JavaScript."""
    browser = shutil.which('google-chrome') or shutil.which('chromium')
    if browser is None:
        print('SKIP reboot CSP browser check: Chrome/Chromium not installed')
        return
    assert "script-src 'self';" in policy
    new_token = ('b' if token[0] == 'a' else 'a') * 64
    for recovered_status in (200, 401):
        polls = []
        scripts = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                path = self.path.split('?')[0]
                status, content_type = 200, 'text/html; charset=utf-8'
                if path == '/rebooting':
                    body = page
                elif path == '/reboot.js':
                    scripts.append(path)
                    body, content_type = script, 'application/javascript'
                elif path == '/upgrade-status':
                    polls.append(path)
                    # A response from the still-running old server must not
                    # navigate; a transient outage must not abort monitoring.
                    status = 200 if len(polls) == 1 else 503 if len(polls) == 2 else recovered_status
                    body = (token if len(polls) == 1 else new_token).encode()
                    content_type = 'text/plain'
                elif path == '/':
                    body = b'<html><body id="main-page-reached">Camera main page</body></html>'
                else:
                    status, body = 404, b'not found'
                self.send_response(status)
                if status == 401:
                    # Keep authentication behavior faithful to the real endpoint:
                    # a Basic challenge here stalls the browser in a login dialog.
                    for key, value in auth_headers.items():
                        if key.lower() == 'www-authenticate':
                            self.send_header(key, value)
                self.send_header('Content-Type', content_type)
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Content-Security-Policy', policy)
                self.end_headers()
                self.wfile.write(body)

        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                with tempfile.TemporaryDirectory(prefix='reboot-browser-', ignore_cleanup_errors=True) as temp:
                    result = subprocess.run([
                        browser, '--headless', '--no-sandbox', '--disable-gpu',
                        '--disable-dev-shm-usage', '--no-first-run', '--disable-extensions',
                        '--disable-background-networking', '--disable-component-update',
                        '--user-data-dir=' + str(Path(temp) / 'profile'),
                        '--virtual-time-budget=15000', '--dump-dom',
                        f'http://127.0.0.1:{server.server_port}/rebooting'],
                        capture_output=True, timeout=30)
                assert result.returncode == 0, result.stderr.decode()[-2000:]
                assert scripts and len(polls) >= 3, (scripts, polls, result.stderr.decode()[-2000:])
                assert b'id="main-page-reached"' in result.stdout, result.stdout.decode()
            finally:
                server.shutdown()
                thread.join()
    print('PASS Chrome reboot navigation with production CSP and expired login session')
