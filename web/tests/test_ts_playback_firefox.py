#!/usr/bin/env python3
"""Headed Firefox check of range-backed TS playback against a running camera web UI.

Run inside a dedicated Xephyr display. Supply a completed MISB recording of at
least 32 MiB and two minutes; no full recording is downloaded by this test.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from urllib.parse import urlencode

from test_live_firefox import Marionette


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8081')
    parser.add_argument('--recording', required=True, help='absolute path on the web server')
    parser.add_argument('--screenshot', type=Path)
    args = parser.parse_args()
    assert os.environ.get('DISPLAY'), 'Run in a dedicated Xephyr display'
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='camera-ts-firefox-') as directory:
        root = Path(directory)
        (root / 'user.js').write_text(
            f'user_pref("marionette.port", {port});\n'
            'user_pref("media.autoplay.default", 0);\n'
            'user_pref("browser.shell.checkDefaultBrowser", false);\n')
        with (root / 'firefox.log').open('w') as log:
            process = subprocess.Popen(['firefox', '--no-remote', '--profile', directory,
                                        '--marionette'], stdout=log, stderr=log)
            connection = None
            try:
                deadline = time.monotonic() + 25
                while connection is None:
                    try:
                        connection = socket.create_connection(('127.0.0.1', port), timeout=5)
                    except OSError:
                        assert process.poll() is None and time.monotonic() < deadline
                        time.sleep(.1)
                browser = Marionette(connection)
                browser.command('WebDriver:NewSession', {'capabilities': {
                    'alwaysMatch': {'pageLoadStrategy': 'eager'}}})
                target = args.url.rstrip('/') + '/view?' + urlencode({'path': args.recording})
                browser.command('WebDriver:Navigate', {'url': target})
                if browser.js('return !!document.querySelector("#password")'):
                    browser.js('document.querySelector("#password").value=arguments[0];'
                               'document.querySelector("form").submit();',
                               os.environ.get('CAMERA_WEB_PASSWORD', 'ardupilot'))
                    time.sleep(1)
                    browser.command('WebDriver:Navigate', {'url': target})

                browser.js('performance.setResourceTimingBufferSize(100000);')

                def state():
                    return browser.js('''
                        const v = document.querySelector('#recording-video');
                        const s = document.querySelector('#recording-seek');
                        const reads = performance.getEntriesByType('resource')
                            .filter(e => new URL(e.name).pathname === '/file')
                            .map(e => e.encodedBodySize);
                        return {ready:v.readyState, paused:v.paused,
                            time:Number(s.value), duration:Number(s.max),
                            status:document.querySelector('#recording-status').textContent,
                            frames:v.getVideoPlaybackQuality().totalVideoFrames,
                            bytes:reads.reduce((a,b) => a+b, 0), reads};
                    ''')

                def wait(predicate):
                    deadline = time.monotonic() + 25
                    while True:
                        current = state()
                        if predicate(current):
                            return current
                        assert time.monotonic() < deadline, current
                        time.sleep(.1)

                first = wait(lambda s: s['ready'] >= 2 and not s['status'])
                assert first['duration'] > 120, first
                # HEAD has no response body and does not read the recording.
                browser.js('''
                    window.recordingSize = null;
                    fetch(document.querySelector('#recording-player').dataset.url,
                          {method:'HEAD'}).then(r => {
                        window.recordingSize = Number(r.headers.get('Content-Length'));
                    });
                ''')
                time.sleep(.5)
                size = browser.js('return window.recordingSize;')
                assert size and size > 32 * 1024 * 1024, size
                # Let the short buffer fill, then prove pausing stops prefetch.
                time.sleep(2)
                settled = state()
                assert settled['paused'] and 0 < settled['bytes'] < size / 4, settled
                time.sleep(3)
                assert state()['bytes'] == settled['bytes'], state()
                results = {'size': size, 'startup': settled}
                for label, seconds in [('forward', first['duration'] * .8 + .37),
                                       ('backward', first['duration'] * .2 + .37),
                                       ('end', first['duration'] - .01),
                                       ('start', .37)]:
                    browser.js('''
                        const s = document.querySelector('#recording-seek');
                        s.value = arguments[0];
                        s.dispatchEvent(new Event('input'));
                        s.dispatchEvent(new Event('change'));
                    ''', seconds)
                    sought = wait(lambda s: s['ready'] >= 2 and not s['status'] and
                                  abs(s['time'] - seconds) < (.3 if label == 'end' else .2))
                    assert sought['paused'], sought
                    browser.js('document.querySelector("#recording-play").click();')
                    played = wait(lambda s: s['frames'] > sought['frames'] + (0 if label == 'end' else 3) and
                                  (label == 'end' or s['time'] > seconds + .2))
                    if not state()['paused']:
                        browser.js('document.querySelector("#recording-play").click();')
                    assert state()['paused']
                    results[f'seek_{label}'] = played
                last = state()
                assert last['bytes'] < size / 2, last
                assert all(n <= 8 * 1024 * 1024 + 188 for n in last['reads']), last
                if args.screenshot:
                    import base64
                    screenshot = browser.command('WebDriver:TakeScreenshot', {})['value']
                    args.screenshot.write_bytes(base64.b64decode(screenshot))
                print(json.dumps(results, indent=2))
                print('PASS startup, bounded reads, paused prefetch, forward/backward/end '
                      'seeks and decoded playback without a full download')
            except Exception:
                print((root / 'firefox.log').read_text())
                raise
            finally:
                if connection:
                    connection.close()
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
