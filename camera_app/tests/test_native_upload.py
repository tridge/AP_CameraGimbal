#!/usr/bin/env python3
"""Exercise the native worker against local HTTPS Drive/OAuth, never a real account."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse

BINARY = str(Path(sys.argv[1]).resolve())
sys.argv = [sys.argv[0]]


class DriveServer(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *args):
        pass

    def response(self, status, body=None, **headers):
        payload = json.dumps(body or {}).encode()
        self.send_response(status)
        self.send_header('Content-Length', str(len(payload)))
        for key, value in headers.items():
            self.send_header(key.replace('_', '-'), value)
        self.end_headers()
        self.wfile.write(payload)

    def body(self):
        return self.rfile.read(int(self.headers.get('Content-Length', 0)))

    def authorized(self):
        assert self.headers.get("Authorization") == "Bearer ACCESS_SECRET"

    def do_GET(self):
        self.authorized()
        state = self.server.state
        query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)['q'][0]
        quoted = re.findall(r"'((?:[^'\\]|\\.)*)'", query)
        parent, name = [re.sub(r'\\(.)', r'\1', s) for s in quoted[:2]]
        found = [f for f in state['files'].values() if f['parent'] == parent and f['name'] == name]
        if 'mimeType' in query:
            found = [f for f in found if f['mimeType'] == 'application/vnd.google-apps.folder']
        self.response(200, {'files': [{k: v for k, v in f.items() if k not in ('data', 'name', 'parent')} for f in found]})

    def do_POST(self):
        state = self.server.state
        body = self.body()
        if self.path == '/token':
            form = urllib.parse.parse_qs(body.decode())
            assert form['refresh_token'] == ['REFRESH_SECRET']
            assert form['client_secret'] == ['CLIENT&SECRET']
            state['refreshes'] += 1
            return self.response(200, {'access_token': 'ACCESS_SECRET', 'expires_in': 3600})
        self.create(body)

    def do_PATCH(self):
        self.create(self.body())

    def create(self, body):
        self.authorized()
        state = self.server.state
        metadata = json.loads(body)
        if 'uploadType=resumable' not in self.path:
            fid = 'folder' + str(len(state['files']))
            state['files'][fid] = dict(metadata, id=fid, parent=metadata['parents'][0])
            return self.response(200, {'id': fid})
        sid = str(len(state['sessions']))
        if self.command == 'PATCH':
            fid = self.path.split('?')[0].rsplit('/', 1)[1]
            parent = state['files'][fid]['parent']
        else:
            fid = 'file' + sid
            parent = metadata['parents'][0]
        state['sessions'][sid] = dict(data=bytearray(), id=fid, name=metadata['name'], parent=parent,
                                     size=int(self.headers['X-Upload-Content-Length']))
        location = state['rogue'] or self.server.base + '/session/' + sid
        self.response(200, Location=location)

    def do_PUT(self):
        self.authorized()
        state = self.server.state
        state['upload_started'].set()
        sid = self.path.rsplit('/', 1)[1]
        upload = state['sessions'][sid]
        body = self.body()
        if sid in state['expired']:
            return self.response(410)
        if len(body) != int(self.headers.get('Content-Length', 0)):
            return  # Deliberately terminated worker.
        if state['unauthorized_once']:
            state['unauthorized_once'] = False
            return self.response(401)
        content_range = self.headers['Content-Range']
        if not body:
            if len(upload['data']) == upload['size']:
                return self.response(200, state['files'][upload['id']])
            headers = {'Range': 'bytes=0-' + str(len(upload['data']) - 1)} if upload['data'] else {}
            return self.response(308, **headers)
        start, end, total = map(int, re.fullmatch(r'bytes (\d+)-(\d+)/(\d+)', content_range).groups())
        assert start == len(upload['data']) and len(body) == end - start + 1 and total == upload['size']
        state['offsets'].append(start)
        upload['data'].extend(body)
        if state['mutate']:
            state['mutate']()
            state['mutate'] = None
        if state['fail_once']:
            state['fail_once'] = False
            return self.response(503)
        if len(upload['data']) < total:
            return self.response(308, Range='bytes=0-' + str(len(upload['data']) - 1))
        file = dict(id=upload['id'], name=upload['name'], parent=upload['parent'], size=str(total),
                    md5Checksum='0'*32 if state['bad_hash'] else hashlib.md5(upload['data']).hexdigest(),
                    mimeType='application/octet-stream')
        state['files'][file['id']] = file
        self.response(200, file)


class NativeUpload(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        cls.cert = cls.root/'cert.pem'
        cls.key = cls.root/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                        '-addext', 'basicConstraints=critical,CA:TRUE',
                        '-keyout', str(cls.key), '-out', str(cls.cert)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), DriveServer)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cls.cert, cls.key)
        cls.server.socket = context.wrap_socket(cls.server.socket, server_side=True)
        cls.server.base = f'https://localhost:{cls.server.server_port}'
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.temp.cleanup()

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(dir=self.root)
        self.addCleanup(self.case.cleanup)
        self.path = Path(self.case.name)
        self.source = self.path/'photos'
        self.source.mkdir()
        self.config = self.path/'upload.conf'
        self.config.write_text('[gdrive]\ntype = drive\nclient_id = CLIENT\nclient_secret = CLIENT&SECRET\n'
                               'token = {"refresh_token":"REFRESH_SECRET"}\n')
        self.config.chmod(0o600)
        self.server.state = dict(files={}, sessions={}, refreshes=0, offsets=[], fail_once=False,
                                 bad_hash=False, mutate=None, rogue='', expired=set(), unauthorized_once=False, upload_started=threading.Event())

    def media(self, name='source.jpg', size=700000):
        file = self.source/name
        file.write_bytes((bytes(range(251)) * (size//251 + 1))[:size])
        os.utime(file, (time.time()-10, time.time()-10))
        return file

    def run_worker(self, move=False, good=True, trust=True, *extra):
        args = [BINARY, 'move' if move else 'copy', str(self.source), 'gdrive:flight/photos',
                '--config', str(self.config), '--test-base', self.server.base,
                '--transfers', '1', '--checkers', '2', '--stats', '5s', '--use-json-log',
                '--stats-log-level', 'NOTICE', '--exclude', '*.part', '--exclude', '*.tmp', *extra]
        env = dict(os.environ, CAMERA_APP_UPLOAD_CA_FILE=str(self.cert if trust else '/etc/ssl/certs/ca-certificates.crt'))
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, good, result.stderr)
        for secret in ['CLIENT&SECRET', 'REFRESH_SECRET', 'ACCESS_SECRET']:
            self.assertNotIn(secret, result.stdout + result.stderr)
        for line in result.stderr.splitlines():
            self.assertIsInstance(json.loads(line), dict)
        return result

    def test_upload_verify_and_no_duplicate_on_repeat(self):
        file = self.media()
        self.run_worker()
        self.assertTrue(file.exists())
        self.assertEqual(self.server.state['offsets'], [0, 262144, 524288])
        self.run_worker(move=True)
        self.assertFalse(file.exists())
        self.assertEqual(len(self.server.state['sessions']), 1)
        self.assertEqual(self.server.state['refreshes'], 2)

    def test_resume_after_committed_chunk_and_lost_response(self):
        self.media()
        self.server.state['fail_once'] = True
        self.run_worker(good=False)
        self.assertEqual(len(list(Path(str(self.config)+'.state').iterdir())), 1)
        self.run_worker()
        self.assertEqual(self.server.state['offsets'], [0, 262144, 524288])
        self.assertEqual(len(self.server.state['sessions']), 1)
        self.assertEqual(list(Path(str(self.config)+'.state').iterdir()), [])

    def test_expired_session_restarts_without_deleting_early(self):
        file = self.media()
        self.server.state['fail_once'] = True
        self.run_worker(move=True, good=False)
        self.assertTrue(file.exists())
        self.server.state['expired'].add('0')
        self.run_worker(move=True)
        self.assertFalse(file.exists())
        self.assertEqual(len(self.server.state['sessions']), 2)
        self.assertEqual(self.server.state['offsets'], [0, 0, 262144, 524288])

    def test_bad_checksum_never_deletes(self):
        file = self.media()
        self.server.state['bad_hash'] = True
        self.run_worker(move=True, good=False)
        self.assertTrue(file.exists())
        self.assertEqual(list(Path(str(self.config)+'.state').iterdir()), [])
        self.server.state['bad_hash'] = False
        self.run_worker(move=True)
        self.assertFalse(file.exists())
        self.assertEqual(len(self.server.state['files']), 3)  # two folders, one replaced file

    def test_changed_source_never_deletes(self):
        file = self.media()
        self.server.state['mutate'] = lambda: file.write_bytes(b'changed')
        self.run_worker(move=True, good=False)
        self.assertTrue(file.exists())

    def test_refresh_after_access_token_rejection(self):
        self.media()
        self.server.state['unauthorized_once'] = True
        self.run_worker()
        self.assertEqual(self.server.state['refreshes'], 2)
        self.assertEqual(self.server.state['offsets'], [0, 262144, 524288])

    def test_just_closed_file_uploads_in_the_same_scan(self):
        file = self.media()
        os.utime(file, None)
        self.run_worker()
        self.assertEqual(len(self.server.state['sessions']), 1)

    def test_termination_preserves_file_and_resumable_state(self):
        file = self.media()
        env = dict(os.environ, CAMERA_APP_UPLOAD_CA_FILE=str(self.cert))
        process = subprocess.Popen([BINARY, 'move', str(self.source), 'gdrive:flight/photos',
                                   '--config', str(self.config), '--test-base', self.server.base,
                                   '--bwlimit', '64k'], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            self.assertTrue(self.server.state['upload_started'].wait(5))
            process.terminate()
            process.communicate(timeout=5)
            self.assertTrue(file.exists())
            self.assertEqual(len(list(Path(str(self.config)+'.state').iterdir())), 1)
            self.run_worker(move=True)
            self.assertFalse(file.exists())
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate()

    def test_temporary_hidden_and_symlink_files_are_excluded(self):
        self.media('good.jpg')
        self.media('incomplete.part')
        self.media('incomplete.tmp')
        self.media('.hidden.jpg')
        (self.source/'linked.jpg').symlink_to(self.source/'good.jpg')
        (self.source/'linked-dir').symlink_to(self.source, target_is_directory=True)
        self.run_worker()
        self.assertEqual(len(self.server.state['sessions']), 1)

    def test_untrusted_tls_never_sends_oauth(self):
        self.media()
        self.run_worker(good=False, trust=False)
        self.assertEqual(self.server.state['refreshes'], 0)

    def test_untrusted_session_location_is_rejected(self):
        self.media()
        self.server.state['rogue'] = 'https://attacker.invalid/session'
        self.run_worker(good=False)
        self.assertEqual(self.server.state['offsets'], [])

    def test_duplicate_remote_names_fail_closed(self):
        self.media()
        for fid in ['a', 'b']:
            self.server.state['files'][fid] = dict(id=fid, name='flight', parent='root', mimeType='application/vnd.google-apps.folder')
        self.run_worker(good=False)
        self.assertEqual(len(self.server.state['sessions']), 0)

    def test_private_credentials_required(self):
        self.media()
        self.config.chmod(0o644)
        self.run_worker(good=False)
        self.assertEqual(self.server.state['refreshes'], 0)

    def test_bandwidth_limit_and_nested_folders(self):
        (self.source/'nested').mkdir()
        self.media('nested/source.jpg', size=300000)
        began = time.monotonic()
        self.run_worker(False, True, True, '--bwlimit', '256k')
        self.assertGreater(time.monotonic()-began, 0.7)
        self.assertEqual(len(self.server.state['sessions']), 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
